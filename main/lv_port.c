#include <string.h>
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "bsp_board.h"
#include "bsp_lcd.h"
#include "esp_lcd_panel_rgb.h"
#include "indev/indev.h"
#include "lv_port.h"
#include "sdkconfig.h"

#define LV_PORT_BUFFER_HEIGHT        (brd->LCD_HEIGHT)
#define LV_PORT_TASK_DELAY_MS        (5)
#define LV_PORT_TASK_MAX_SLEEP_MS    (500)
#define LV_PORT_TASK_STACK_SIZE      (8192)

static const char *TAG = "lvgl_port";
static lv_display_t *s_display = NULL;
static lv_indev_t *s_indev_touchpad = NULL;
static lv_indev_t *s_indev_button = NULL;

#if LV_PORT_RENDER_DIAG
/* Responsiveness diagnostics, reset with each [RENDER] line:
 *  - longest gap between two touch reads while the finger is down (a stall of the LVGL task)
 *  - longest single touch read (I2C bus shared with the IO expander / SX1262 chip select)
 *  - longest LVGL lock hold by another task through lv_port_sem_take()/give() */
static int64_t s_diag_touch_gap_max_us = 0;
static int64_t s_diag_touch_read_max_us = 0;
static uint32_t s_diag_touch_err = 0;      /* failed touch reads in the period (I2C) */
static int64_t s_diag_lock_hold_max_us = 0;
static char    s_diag_lock_holder[16] = "-";
static int64_t s_lock_take_us = 0;
static int     s_lock_depth = 0;
#endif

#ifndef CONFIG_LCD_TASK_PRIORITY
#define CONFIG_LCD_TASK_PRIORITY    5
#endif

static void lv_port_disp_init(void);
static void lv_port_indev_init(void);
static void button_read(lv_indev_t *indev, lv_indev_data_t *data);
static void touchpad_read(lv_indev_t *indev, lv_indev_data_t *data);

void lv_port_init(void)
{
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_priority = CONFIG_LCD_TASK_PRIORITY;
    lvgl_cfg.task_stack = LV_PORT_TASK_STACK_SIZE;
    lvgl_cfg.task_affinity = 1;
    lvgl_cfg.task_priority = 6;
    lvgl_cfg.task_max_sleep_ms = LV_PORT_TASK_MAX_SLEEP_MS;
    lvgl_cfg.timer_period_ms = LV_PORT_TASK_DELAY_MS;

    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));
    lv_port_disp_init();
    lv_port_indev_init();
}

void lv_port_sem_take(void)
{
    lvgl_port_lock(portMAX_DELAY);
#if LV_PORT_RENDER_DIAG
    /* The (recursive) LVGL lock protects these variables */
    if (s_lock_depth++ == 0) {
        s_lock_take_us = esp_timer_get_time();
    }
#endif
}

void lv_port_sem_give(void)
{
#if LV_PORT_RENDER_DIAG
    if (s_lock_depth > 0 && --s_lock_depth == 0) {
        int64_t held = esp_timer_get_time() - s_lock_take_us;
        if (held > s_diag_lock_hold_max_us) {
            s_diag_lock_hold_max_us = held;
            strlcpy(s_diag_lock_holder, pcTaskGetName(NULL), sizeof(s_diag_lock_holder));
        }
    }
#endif
    lvgl_port_unlock();
}

static void button_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    static uint32_t last_key = 0;

    indev_data_t indev_data;
    if (ESP_OK != indev_get_major_value(&indev_data)) {
        ESP_LOGE(TAG, "Failed read input device value");
        return;
    }

    if (indev_data.btn_val & 0x02) {
        last_key = LV_KEY_ENTER;
        data->state = LV_INDEV_STATE_PRESSED;
        ESP_LOGD(TAG, "ok");
    } else if (indev_data.btn_val & 0x04) {
        last_key = LV_KEY_PREV;
        data->state = LV_INDEV_STATE_PRESSED;
        ESP_LOGD(TAG, "prev");
    } else if (indev_data.btn_val & 0x01) {
        last_key = LV_KEY_NEXT;
        data->state = LV_INDEV_STATE_PRESSED;
        ESP_LOGD(TAG, "next");
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->key = last_key;
}


static void touchpad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    static int32_t x = 0;
    static int32_t y = 0;
    static bool    s_pressed = false;   /* state of the last successful read */

    indev_data_t indev_data = {0};
#if LV_PORT_RENDER_DIAG
    static int64_t s_last_read_us = 0;
    static bool s_was_pressed = false;
    int64_t t0 = esp_timer_get_time();
    if (s_was_pressed && s_last_read_us != 0 && (t0 - s_last_read_us) > s_diag_touch_gap_max_us) {
        s_diag_touch_gap_max_us = t0 - s_last_read_us;
    }
    s_last_read_us = t0;
#endif
    esp_err_t read_err = indev_get_major_value(&indev_data);
#if LV_PORT_RENDER_DIAG
    int64_t read_us = esp_timer_get_time() - t0;
    if (read_us > s_diag_touch_read_max_us) s_diag_touch_read_max_us = read_us;
    s_was_pressed = (read_err == ESP_OK) && indev_data.pressed;
#endif
    if (ESP_OK != read_err) {
        /* Keep the state of the last valid read: an I2C error never creates a press */
#if LV_PORT_RENDER_DIAG
        s_diag_touch_err++;
#endif
        data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->point.x = x;
        data->point.y = y;
        return;
    }

    if (indev_data.pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = CONFIG_LCD_EVB_SCREEN_WIDTH - indev_data.x;
        data->point.y = CONFIG_LCD_EVB_SCREEN_HEIGHT - indev_data.y;
        x = data->point.x;
        y = data->point.y;
        s_pressed = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = x;
        data->point.y = y;
        s_pressed = false;
    }
}

#if LV_PORT_RENDER_DIAG
/* Runs in the LVGL task (display event); RENDER_START/READY only fire for frames with
 * invalidated areas, so the numbers describe real drawing work in PSRAM */
static void render_diag_cb(lv_event_t *e)
{
    static int64_t s_render_start_us = 0;
    static int64_t s_period_start_us = 0;
    static uint32_t s_frames = 0;
    static int64_t s_sum_us = 0;
    static int64_t s_max_us = 0;

    int64_t now = esp_timer_get_time();
    if (lv_event_get_code(e) == LV_EVENT_RENDER_START) {
        s_render_start_us = now;
        return;
    }
    if (s_render_start_us == 0) return;

    int64_t took = now - s_render_start_us;
    s_render_start_us = 0;
    s_frames++;
    s_sum_us += took;
    if (took > s_max_us) s_max_us = took;

    if (s_period_start_us == 0) s_period_start_us = now;
    if (now - s_period_start_us >= (int64_t)LV_PORT_RENDER_DIAG_PERIOD_MS * 1000) {
        /* RGB stream restarts after a bounce buffer desync (patched driver, components/esp_lcd) */
        uint32_t restarts = esp_lcd_rgb_panel_get_restart_count(bsp_lcd_get_panel_handle());
        ESP_LOGI(TAG, "[RENDER] %us: %lu frames, render avg %.1f ms, max %.1f ms, RGB restarts total %lu | "
                      "touch gap max %.0f ms, touch read max %.1f ms, touch read errors %lu, lock hold max %.1f ms (%s)",
                 (unsigned)(LV_PORT_RENDER_DIAG_PERIOD_MS / 1000), (unsigned long)s_frames,
                 (double)s_sum_us / (double)s_frames / 1000.0, (double)s_max_us / 1000.0,
                 (unsigned long)restarts,
                 (double)s_diag_touch_gap_max_us / 1000.0, (double)s_diag_touch_read_max_us / 1000.0,
                 (unsigned long)s_diag_touch_err, (double)s_diag_lock_hold_max_us / 1000.0, s_diag_lock_holder);
        s_diag_touch_gap_max_us = 0;
        s_diag_touch_err = 0;
        s_diag_touch_read_max_us = 0;
        s_diag_lock_hold_max_us = 0;
        strlcpy(s_diag_lock_holder, "-", sizeof(s_diag_lock_holder));
        s_period_start_us = now;
        s_frames = 0;
        s_sum_us = 0;
        s_max_us = 0;
    }
}
#endif

#if LV_PORT_HIDE_COVERED_SCREEN
/**
 * @brief true if a visible, fully opaque child of the top layer covers the whole display.
 */
static bool top_layer_covers_display(lv_display_t *disp)
{
    lv_obj_t *top = lv_display_get_layer_top(disp);
    if (!top) return false;

    int32_t x_max = lv_display_get_horizontal_resolution(disp) - 1;
    int32_t y_max = lv_display_get_vertical_resolution(disp) - 1;
    uint32_t cnt = lv_obj_get_child_count(top);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(top, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_get_style_opa(child, LV_PART_MAIN) < LV_OPA_COVER) continue;
        if (lv_obj_get_style_bg_opa(child, LV_PART_MAIN) < LV_OPA_COVER) continue;
        if (lv_obj_get_style_radius(child, LV_PART_MAIN) > 0) continue; /* corners show the page */
        lv_area_t coords;
        lv_obj_get_coords(child, &coords);
        if (coords.x1 <= 0 && coords.y1 <= 0 && coords.x2 >= x_max && coords.y2 >= y_max) return true;
    }
    return false;
}

/* LV_EVENT_REFR_START (LVGL task, before layout and rendering): hide the active screen while a
 * full-screen modal covers it and show it again in the same refresh in which the modal goes away,
 * so no frame without the page is ever displayed */
static void covered_screen_cb(lv_event_t *e)
{
    static lv_obj_t *s_hidden_scr = NULL;
    lv_display_t *disp = lv_event_get_target(e);
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    bool covered = top_layer_covers_display(disp);

    /* Screen changed while hidden: restore the old one */
    if (s_hidden_scr && s_hidden_scr != scr) {
        lv_obj_remove_flag(s_hidden_scr, LV_OBJ_FLAG_HIDDEN);
        s_hidden_scr = NULL;
    }
    if (!scr) return;

    if (covered && !s_hidden_scr) {
        lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN);
        s_hidden_scr = scr;
    } else if (!covered && s_hidden_scr) {
        lv_obj_remove_flag(s_hidden_scr, LV_OBJ_FLAG_HIDDEN);
        s_hidden_scr = NULL;
    }
}
#endif

static void lv_port_disp_init(void)
{
    const board_res_desc_t *brd = bsp_board_get_description();
    const uint32_t buffer_size = brd->LCD_WIDTH * LV_PORT_BUFFER_HEIGHT;

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = bsp_lcd_get_io_handle(),
        .panel_handle = bsp_lcd_get_panel_handle(),
        .buffer_size = buffer_size,
        .double_buffer =
#if CONFIG_LCD_AVOID_TEAR
            true,
#else
            false,
#endif
        .hres = brd->LCD_WIDTH,
        .vres = brd->LCD_HEIGHT,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = brd->LCD_SWAP_XY,
            .mirror_x = brd->LCD_MIRROR_X,
            .mirror_y = brd->LCD_MIRROR_Y,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
#if CONFIG_LCD_LVGL_FULL_REFRESH
            .full_refresh = true,
#endif
#if CONFIG_LCD_LVGL_DIRECT_MODE
            .direct_mode = true,
#endif
            .swap_bytes = false,
        },
    };

    if (LCD_IFACE_RGB == brd->LCD_IFACE) {
        const lvgl_port_display_rgb_cfg_t rgb_cfg = {
            .flags = {
                .bb_mode = true,
#if CONFIG_LCD_AVOID_TEAR
                .avoid_tearing = true,
#else
                .avoid_tearing = false,
#endif
            },
        };
        s_display = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    } else {
        s_display = lvgl_port_add_disp(&disp_cfg);
    }

    ESP_ERROR_CHECK(s_display == NULL ? ESP_FAIL : ESP_OK);

#if LV_PORT_RENDER_DIAG
    if (lvgl_port_lock(0)) {
        lv_display_add_event_cb(s_display, render_diag_cb, LV_EVENT_RENDER_START, NULL);
        lv_display_add_event_cb(s_display, render_diag_cb, LV_EVENT_RENDER_READY, NULL);
        lvgl_port_unlock();
    }
#endif
#if LV_PORT_HIDE_COVERED_SCREEN
    if (lvgl_port_lock(0)) {
        lv_display_add_event_cb(s_display, covered_screen_cb, LV_EVENT_REFR_START, NULL);
        lvgl_port_unlock();
    }
#endif
}

static void lv_port_indev_init(void)
{
    const board_res_desc_t *brd = bsp_board_get_description();

    if (brd->BSP_INDEV_IS_TP) {
        ESP_LOGI(TAG, "Add TP input device to LVGL");
        s_indev_touchpad = lv_indev_create();
        ESP_ERROR_CHECK(s_indev_touchpad == NULL ? ESP_FAIL : ESP_OK);
        lv_indev_set_type(s_indev_touchpad, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(s_indev_touchpad, s_display);
        lv_indev_set_read_cb(s_indev_touchpad, touchpad_read);
    } else {
        ESP_LOGI(TAG, "Add KEYPAD input device to LVGL");
        s_indev_button = lv_indev_create();
        ESP_ERROR_CHECK(s_indev_button == NULL ? ESP_FAIL : ESP_OK);
        lv_indev_set_type(s_indev_button, LV_INDEV_TYPE_KEYPAD);
        lv_indev_set_display(s_indev_button, s_display);
        lv_indev_set_read_cb(s_indev_button, button_read);
    }

#if CONFIG_LV_PORT_SHOW_MOUSE_CURSOR
    LV_IMAGE_DECLARE(mouse_cursor_icon)
    lv_obj_t *cursor_obj = lv_image_create(lv_screen_active());
    lv_image_set_src(cursor_obj, &mouse_cursor_icon);
    lv_indev_set_cursor(s_indev_touchpad, cursor_obj);
#endif
}
