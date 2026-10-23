/**
 * @file gui_header.c
 * @brief Resilient Header Implementation with Native Asset Icons
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "gui_header.h"
#include "lv_port.h"
#include "esp_event.h"
#include "view_data.h"
#include "esp_wifi.h"
#include "string.h"

/* Declare fonts */
LV_FONT_DECLARE(ui_font_font0);          /* For Page Title */
LV_FONT_DECLARE(lv_font_montserrat_20);   /* For smaller button symbols/text */
LV_FONT_DECLARE(lv_font_montserrat_24);   /* For prominent header icons (Gear, Warning) */

/* Declare image assets from main/assets/ */
LV_IMAGE_DECLARE(ui_img_wifi_3_png);
LV_IMAGE_DECLARE(ui_img_wifi_2_png);
LV_IMAGE_DECLARE(ui_img_wifi_1_png);
LV_IMAGE_DECLARE(ui_img_wifi_disconet_png);
LV_IMAGE_DECLARE(ui_img_ic_lora_png);

#define HEADER_HEIGHT   64
//#define SLOT_WIDTH      95
#define SLOT_HEIGHT     40
#define SLOT_Y_OFFSET   12
/* Geometry: Edge slots are 46px, inner slots have wider touch area (56px) */
#define SLOT_W_EDGE     46
#define SLOT_W_INNER    56

typedef struct {
    lv_obj_t *btn;
    lv_obj_t *lbl;
    lv_obj_t *img;
} header_slot_ui_t;

static lv_obj_t *s_header_cont = NULL;
static lv_obj_t *s_lbl_title = NULL;
static header_slot_ui_t s_slots[4];

/* Cached network state */
static bool s_wifi_connected = false;
static int8_t s_wifi_rssi = -100;
static lora_header_status_t s_lora_status = LORA_STATUS_UNINIT;

/* External callback to open settings modal */
extern void settings_view_show(void);

/* Default Click Callbacks */
static void default_wifi_click_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        uint8_t screen = 1; // SCREEN_WIFI_CONFIG
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SCREEN_START, &screen, sizeof(screen), portMAX_DELAY);
    }
}

static void default_settings_click_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        settings_view_show();
    }
}

static void default_lora_click_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
#if CONFIG_LORA_ENABLE
        extern void lora_diag_view_show(void);
        lora_diag_view_show();
#endif
    }
}

static bool is_wifi_configured(void) {
    wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
        return (strlen((const char *)wifi_cfg.sta.ssid) > 0);
    }
    return false;
}

static void apply_wifi_status_ui(void) {
    const lv_image_dsc_t *src;
    lv_color_t tint_color;

    bool wifiparametrized = is_wifi_configured();

    if (wifiparametrized) {
        if (!s_wifi_connected) {
            src = &ui_img_wifi_disconet_png;
            tint_color = lv_color_hex(0xe74c3c); // Red
        } else if (s_wifi_rssi < -80) {
            src = &ui_img_wifi_1_png;
            tint_color = lv_color_hex(0xf39c12); // Orange (1 bar)
        } else if (s_wifi_rssi < -65) {
            src = &ui_img_wifi_2_png;
            tint_color = lv_color_hex(0xf1c40f); // Yellow (2 bars)
        } else {
            src = &ui_img_wifi_3_png;
            tint_color = lv_color_hex(0x2ecc71); // Green (3 bars)
        }
    }
    else {
        src = &ui_img_wifi_disconet_png;
        tint_color = lv_color_hex(0x7f8c8d); // Grey
    }

    gui_header_set_slot_image(HEADER_SLOT_RIGHT_1, src, default_wifi_click_cb, NULL);
    
    /* Apply color tinting to the WiFi icon */
    lv_port_sem_take();
    lv_obj_set_style_image_recolor_opa(s_slots[HEADER_SLOT_RIGHT_1].img, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor(s_slots[HEADER_SLOT_RIGHT_1].img, tint_color, 0);
    lv_port_sem_give();
}

static void apply_lora_status_ui(void) {
    lv_color_t color;
    switch (s_lora_status) {
        case LORA_STATUS_ACTIVE:  color = lv_color_hex(0x2ecc71); break; // Green
        case LORA_STATUS_WAITING: color = lv_color_hex(0xf39c12); break; // Orange
        case LORA_STATUS_TIMEOUT: color = lv_color_hex(0xe74c3c); break; // Red
        default:                  color = lv_color_hex(0x7f8c8d); break; // Gray (Uninit)
    }

    gui_header_set_slot_image(HEADER_SLOT_RIGHT_2, &ui_img_ic_lora_png, default_lora_click_cb, NULL);

    lv_port_sem_take();
    lv_obj_set_style_image_recolor_opa(s_slots[HEADER_SLOT_RIGHT_2].img, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor(s_slots[HEADER_SLOT_RIGHT_2].img, color, 0);
    lv_port_sem_give();
}

/* ── Global Alarm Visualisation ────────────────────────────────────────── */

static bool s_alarm_active = false;
static lv_timer_t *s_alarm_timer = NULL;
static bool s_alarm_blink_state = false;

static void alarm_click_interceptor_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        /* Open the Alarm View */
        extern void alarm_view_show(void);
        alarm_view_show();
    }
}

static void alarm_blink_timer_cb(lv_timer_t *timer) {
    if (!s_header_cont || !s_lbl_title) return;

    lv_port_sem_take();
    s_alarm_blink_state = !s_alarm_blink_state;

    if (s_alarm_blink_state) {
        /* Warn-Zustand: Roter Hintergrund, schwarzer Text */
        lv_obj_set_style_bg_color(s_header_cont, lv_color_hex(0xE74C3C), LV_PART_MAIN);
        lv_obj_set_style_text_color(s_lbl_title, lv_color_black(), LV_PART_MAIN);
    } else {
        /* Normal-Zustand: Dunkler Hintergrund, weißer Text */
        lv_obj_set_style_bg_color(s_header_cont, lv_color_hex(0x101418), LV_PART_MAIN);
        lv_obj_set_style_text_color(s_lbl_title, lv_color_white(), LV_PART_MAIN);
    }
    lv_port_sem_give();
}

void gui_header_set_alarm_state(bool has_active_alarms) {
    if (s_alarm_active == has_active_alarms) return; /* No change */
    
    s_alarm_active = has_active_alarms;

    lv_port_sem_take();

    if (has_active_alarms) {
        /* 1. Timer starten (Blinken alle 600ms) */
        if (!s_alarm_timer) {
            s_alarm_timer = lv_timer_create(alarm_blink_timer_cb, 600, NULL);
        } else {
            lv_timer_resume(s_alarm_timer);
        }

        /* 2. Slot 1 (WiFi) kapern: Image ausblenden, Symbol anzeigen, Callback biegen */
        lv_obj_add_flag(s_slots[HEADER_SLOT_RIGHT_1].img, LV_OBJ_FLAG_HIDDEN);
        
        lv_label_set_text(s_slots[HEADER_SLOT_RIGHT_1].lbl, LV_SYMBOL_WARNING);
        lv_obj_set_style_text_color(s_slots[HEADER_SLOT_RIGHT_1].lbl, lv_color_hex(0xF1C40F), LV_PART_MAIN); /* Yellow */
        lv_obj_remove_flag(s_slots[HEADER_SLOT_RIGHT_1].lbl, LV_OBJ_FLAG_HIDDEN);
        
        /* Replace click callback */
        uint32_t event_cnt = lv_obj_get_event_count(s_slots[HEADER_SLOT_RIGHT_1].btn);
        for (uint32_t e = 0; e < event_cnt; e++) {
            lv_event_dsc_t *dsc = lv_obj_get_event_dsc(s_slots[HEADER_SLOT_RIGHT_1].btn, e);
            if (lv_event_dsc_get_cb(dsc) != NULL) {
                lv_obj_remove_event_cb(s_slots[HEADER_SLOT_RIGHT_1].btn, lv_event_dsc_get_cb(dsc));
            }
        }
        lv_obj_add_event_cb(s_slots[HEADER_SLOT_RIGHT_1].btn, alarm_click_interceptor_cb, LV_EVENT_CLICKED, NULL);

    } else {
        /* 1. Timer stoppen & Farben zurücksetzen */
        if (s_alarm_timer) {
            lv_timer_pause(s_alarm_timer);
        }
        s_alarm_blink_state = false;
        if (s_header_cont && s_lbl_title) {
            lv_obj_set_style_bg_color(s_header_cont, lv_color_hex(0x101418), LV_PART_MAIN);
            lv_obj_set_style_text_color(s_lbl_title, lv_color_white(), LV_PART_MAIN);
        }

        /* 2. Slot 1 freigeben und Standard-WiFi wiederherstellen */
        lv_obj_add_flag(s_slots[HEADER_SLOT_RIGHT_1].lbl, LV_OBJ_FLAG_HIDDEN);
        
        /* Remove interceptor callback */
        uint32_t event_cnt = lv_obj_get_event_count(s_slots[HEADER_SLOT_RIGHT_1].btn);
        for (uint32_t e = 0; e < event_cnt; e++) {
            lv_event_dsc_t *dsc = lv_obj_get_event_dsc(s_slots[HEADER_SLOT_RIGHT_1].btn, e);
            if (lv_event_dsc_get_cb(dsc) != NULL) {
                lv_obj_remove_event_cb(s_slots[HEADER_SLOT_RIGHT_1].btn, lv_event_dsc_get_cb(dsc));
            }
        }
        
        /* Call our internal apply function which sets the PNG and default callback again */
        lv_port_sem_give(); /* apply_wifi_status_ui internally takes the semaphore */
        apply_wifi_status_ui();
        return; /* Skip give below */
    }

    lv_port_sem_give();
}

static void gui_header_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data) {
    if (id == VIEW_EVENT_WIFI_ST) {
        struct view_data_wifi_st *st = (struct view_data_wifi_st *)event_data;
        if (st) {
            gui_header_update_wifi(st->is_connected, st->rssi);
        }
    }
}

void gui_header_init(lv_obj_t *parent) {
    if (s_header_cont) return;

    lv_port_sem_take();

    s_header_cont = lv_obj_create(parent);
    lv_obj_set_size(s_header_cont, 480, HEADER_HEIGHT);
    lv_obj_set_pos(s_header_cont, 0, 0);
    lv_obj_remove_flag(s_header_cont, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_set_style_radius(s_header_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_header_cont, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_border_side(s_header_cont, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_header_cont, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_header_cont, lv_color_hex(0x2c3e50), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_header_cont, 0, LV_PART_MAIN);

    /* Main Title with correct UI font */
    s_lbl_title = lv_label_create(s_header_cont);
    lv_label_set_text(s_lbl_title, "");
    lv_obj_set_style_text_color(s_lbl_title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_title, &ui_font_font0, LV_PART_MAIN);
    lv_obj_align(s_lbl_title, LV_ALIGN_CENTER, 0, 0);

    /* Initialize Slots with transparent backgrounds (no grey pills) */
    for (int i = 0; i < 4; i++) {
        s_slots[i].btn = lv_button_create(s_header_cont);
        lv_obj_set_style_radius(s_slots[i].btn, 6, LV_PART_MAIN);
        
        /* Transparent default, subtle highlight when pressed */
        lv_obj_set_style_bg_opa(s_slots[i].btn, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_slots[i].btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(s_slots[i].btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(s_slots[i].btn, lv_color_hex(0x3a4048), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(s_slots[i].btn, LV_OPA_50, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_pad_all(s_slots[i].btn, 0, LV_PART_MAIN);

        /* Label */
        s_slots[i].lbl = lv_label_create(s_slots[i].btn);
        lv_label_set_text(s_slots[i].lbl, "");
        /* Montserrat 24 contains all LVGL FontAwesome symbols natively */
        lv_obj_set_style_text_font(s_slots[i].lbl, &lv_font_montserrat_24, LV_PART_MAIN);
        lv_obj_center(s_slots[i].lbl);
        lv_obj_add_flag(s_slots[i].lbl, LV_OBJ_FLAG_HIDDEN);

        /* Image */
        s_slots[i].img = lv_image_create(s_slots[i].btn);
        lv_obj_center(s_slots[i].img);
        lv_obj_add_flag(s_slots[i].img, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(s_slots[i].btn, LV_OBJ_FLAG_HIDDEN);
    }

    /* Position & Size Slots */
    /* Left (Settings / Back) */
    lv_obj_set_size(s_slots[HEADER_SLOT_LEFT].btn, SLOT_W_EDGE, SLOT_HEIGHT);
    lv_obj_set_pos(s_slots[HEADER_SLOT_LEFT].btn, 10, SLOT_Y_OFFSET);

    /* Right 1 (Outer: WiFi) */
    lv_obj_set_size(s_slots[HEADER_SLOT_RIGHT_1].btn, SLOT_W_EDGE, SLOT_HEIGHT);
    lv_obj_set_pos(s_slots[HEADER_SLOT_RIGHT_1].btn, 480 - 10 - SLOT_W_EDGE, SLOT_Y_OFFSET);

    /* Right 2 (Inner: LoRa) - larger touch area */
    lv_obj_set_size(s_slots[HEADER_SLOT_RIGHT_2].btn, SLOT_W_INNER, SLOT_HEIGHT);
    lv_obj_set_pos(s_slots[HEADER_SLOT_RIGHT_2].btn, 480 - 10 - SLOT_W_EDGE - 6 - SLOT_W_INNER, SLOT_Y_OFFSET);

    /* Right 3 (Action button / Mute) */
    lv_obj_set_size(s_slots[HEADER_SLOT_RIGHT_3].btn, 80, SLOT_HEIGHT);
    lv_obj_set_pos(s_slots[HEADER_SLOT_RIGHT_3].btn, 480 - 10 - SLOT_W_EDGE - 6 - SLOT_W_INNER - 6 - 80, SLOT_Y_OFFSET);

    lv_port_sem_give();

    /* Register on system event bus for live WiFi status updates */
    esp_event_handler_instance_register_with(
        view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_ST,
        gui_header_event_handler, NULL, NULL);

    /* Apply initial default icons */
    gui_header_clear();
}

void gui_header_clear(void) {
    if (!s_header_cont) return;

    lv_port_sem_take();
    lv_label_set_text(s_lbl_title, "");

    /* Wipe custom buttons & callbacks on all slots */
    for (int i = 0; i < 4; i++) {
        lv_obj_add_flag(s_slots[i].btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_slots[i].lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_slots[i].img, LV_OBJ_FLAG_HIDDEN);

        uint32_t event_cnt = lv_obj_get_event_count(s_slots[i].btn);
        for (uint32_t e = 0; e < event_cnt; e++) {
            lv_event_dsc_t *dsc = lv_obj_get_event_dsc(s_slots[i].btn, e);
            if (lv_event_dsc_get_cb(dsc) != NULL) {
                lv_obj_remove_event_cb(s_slots[i].btn, lv_event_dsc_get_cb(dsc));
            }
        }
    }
    lv_port_sem_give();

    /* 1. Left Slot: Default Settings Gear */
    gui_header_set_slot(HEADER_SLOT_LEFT, LV_SYMBOL_SETTINGS, default_settings_click_cb, NULL);

    /* 2. Right Slots: Default WiFi & LoRa Status */
    apply_wifi_status_ui();
    apply_lora_status_ui();
}

void gui_header_set_title(const char *title) {
    if (!s_lbl_title) return;
    lv_port_sem_take();
    lv_label_set_text(s_lbl_title, title ? title : "");
    lv_port_sem_give();
}

void gui_header_set_slot(header_slot_t slot, const char *text, lv_event_cb_t cb, void *user_data) {
    if (slot > 3 || !s_slots[slot].btn) return;

    lv_port_sem_take();
    lv_obj_add_flag(s_slots[slot].img, LV_OBJ_FLAG_HIDDEN);
    
    lv_label_set_text(s_slots[slot].lbl, text ? text : "");
    lv_obj_remove_flag(s_slots[slot].lbl, LV_OBJ_FLAG_HIDDEN);

    if (cb != NULL) {
        lv_obj_add_event_cb(s_slots[slot].btn, cb, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_remove_flag(s_slots[slot].btn, LV_OBJ_FLAG_HIDDEN);
    lv_port_sem_give();
}

void gui_header_set_slot_image(header_slot_t slot, const lv_image_dsc_t *img_src, lv_event_cb_t cb, void *user_data) {
    if (slot > 3 || !s_slots[slot].btn || !img_src) return;

    lv_port_sem_take();
    lv_obj_add_flag(s_slots[slot].lbl, LV_OBJ_FLAG_HIDDEN);

    lv_image_set_src(s_slots[slot].img, img_src);
    lv_obj_remove_flag(s_slots[slot].img, LV_OBJ_FLAG_HIDDEN);

    if (cb != NULL) {
        lv_obj_add_event_cb(s_slots[slot].btn, cb, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_remove_flag(s_slots[slot].btn, LV_OBJ_FLAG_HIDDEN);
    lv_port_sem_give();
}

void gui_header_set_visible(bool visible) {
    if (!s_header_cont) return;
    lv_port_sem_take();
    if (visible) {
        lv_obj_remove_flag(s_header_cont, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_header_cont, LV_OBJ_FLAG_HIDDEN);
    }
    lv_port_sem_give();
}

void gui_header_update_wifi(bool is_connected, int8_t rssi) {
    s_wifi_connected = is_connected;
    s_wifi_rssi = rssi;
    apply_wifi_status_ui();
}

void gui_header_update_lora(lora_header_status_t status) {
    s_lora_status = status;
    apply_lora_status_ui();
}