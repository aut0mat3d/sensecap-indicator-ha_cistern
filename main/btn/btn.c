#include "btn.h"
#include "bsp_board.h"
#include "bsp_btn.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <nvs.h>
#include <nvs_flash.h>

static uint32_t hold_cnt = 0;
static bool sleep_flag = false;
static bool sleep_start_flag = false;

static esp_timer_handle_t factory_reset_timer_handle;

static const char *TAG = "btn";

__attribute__((weak)) int indicator_display_off(void) {
    ESP_LOGI(TAG, "indicator_display_off");
}

__attribute__((weak)) int indicator_display_on(void) {
    ESP_LOGI(TAG, "indicator_display_on");
}

__attribute__((weak)) bool indicator_display_st_get(void) {
    ESP_LOGI(TAG, "indicator_display_st_get");
    static bool st = false;
    return st;
}

static void _factory_reset_callback(void *arg) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    fflush(stdout);
    esp_restart();
}

static void _btn_click_callback(void *arg) {
    bool st = 0;
    if (sleep_flag) {
        ESP_LOGI(TAG, "click, cur st: sleep mode, restart!");
        fflush(stdout);
        esp_restart();
        return;
    }
    if (indicator_display_st_get()) {
        ESP_LOGI(TAG, "click, off");
        indicator_display_off();
        st = 0;
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SCREEN_CTRL, &st,
                          sizeof(st), portMAX_DELAY);
    } else {
        ESP_LOGI(TAG, "click, on");
        st = 1;
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SCREEN_CTRL, &st,
                          sizeof(st), portMAX_DELAY);
        indicator_display_on();
    }
}

static void _btn_double_click_callback(void *arg) {
    ESP_LOGI(TAG, "double click");
}

static void _btn_press_start_callback(void *arg) {
    ESP_LOGI(TAG, "press start");
    hold_cnt = 1500;
    sleep_start_flag = false;
}

static void _btn_long_press_hold_callback(void *arg) {
    static bool factory_reset_flag = false;
    hold_cnt += CONFIG_BUTTON_PERIOD_TIME_MS;

    if (hold_cnt >= 3000 && hold_cnt < 10000) {
        if (sleep_flag) {
            ESP_LOGI(TAG, "wake, restart");
            fflush(stdout);
            esp_restart();
        } else if (!sleep_start_flag) {
            sleep_start_flag = true;
            ESP_LOGI(TAG, "long_press: entry sleep mode");
            indicator_display_off();
            bool st = 0;
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SCREEN_CTRL, &st,
                              sizeof(st), portMAX_DELAY);
        }
    } else if (hold_cnt >= 10000 && !factory_reset_flag) {
        ESP_LOGI(TAG, "factory reset");
        factory_reset_flag = true;
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_FACTORY_RESET, NULL, 0,
                          portMAX_DELAY);
        bool st = 1;
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SCREEN_CTRL, &st,
                          sizeof(st), portMAX_DELAY);
        indicator_display_on();

        const esp_timer_create_args_t timer_args = {
            .callback = &_factory_reset_callback,
            .arg = (void *)factory_reset_timer_handle,
            .name = "factory_reset"
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &factory_reset_timer_handle));
        ESP_ERROR_CHECK(esp_timer_start_once(factory_reset_timer_handle, 1000000 * 3));
    }
}

static void _btn_press_up_callback(void *arg) {
    if (hold_cnt >= 3000 && hold_cnt < 15000) {
        ESP_LOGI(TAG, "entry sleep mode");
        sleep_flag = true;
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SHUTDOWN, NULL, 0,
                          portMAX_DELAY);
    }
}

static void _btn_shutdown_event_handler(void* handler_args, esp_event_base_t base, int32_t id, void* event_data) {
    if (id == VIEW_EVENT_SHUTDOWN) {
        ESP_LOGI(TAG, "Shutdown event received, entering sleep mode");
        sleep_flag = true;

        /* Turn off screen backlight and update display controller state */
        indicator_display_off();
        bool st = 0;
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SCREEN_CTRL, &st,
                          sizeof(st), portMAX_DELAY);
    }
}

int indicator_btn_init(void) {
    bsp_btn_register_callback(BOARD_BTN_ID_USER, BUTTON_SINGLE_CLICK, _btn_click_callback, NULL);
    bsp_btn_register_callback(BOARD_BTN_ID_USER, BUTTON_DOUBLE_CLICK, _btn_double_click_callback, NULL);
    bsp_btn_register_callback(BOARD_BTN_ID_USER, BUTTON_LONG_PRESS_START, _btn_press_start_callback, NULL);
    bsp_btn_register_callback(BOARD_BTN_ID_USER, BUTTON_LONG_PRESS_HOLD, _btn_long_press_hold_callback, NULL);
    bsp_btn_register_callback(BOARD_BTN_ID_USER, BUTTON_PRESS_UP, _btn_press_up_callback, NULL);

    /* Register Event Handler for System Shutdown */
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
        view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SHUTDOWN,
        _btn_shutdown_event_handler, NULL, NULL));

    return ESP_OK;
}
