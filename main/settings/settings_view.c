#include "settings_view.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lv_port.h"
#include "nav.h"
#include "sdkconfig.h"
#include "view_data.h"

#if CONFIG_LORA_ENABLE
#include "lora_diag_view.h"
#endif

#if CONFIG_GUI_CONFIG_EXTERN
#include "sd_view.h"
#endif

LV_FONT_DECLARE(ui_font_font0);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(lv_font_montserrat_28);

LV_IMAGE_DECLARE(ui_img_wifi_3_png);
LV_IMAGE_DECLARE(ui_img_wifi_2_png);
LV_IMAGE_DECLARE(ui_img_wifi_1_png);
LV_IMAGE_DECLARE(ui_img_wifi_disconet_png);
LV_IMAGE_DECLARE(ui_img_ic_lora_png);

static const char *TAG = "settings-view";

/* Header Data Structures per Tile */
typedef struct {
    lv_obj_t *header_container;
    lv_obj_t *title_label;
    
    /* Slot Objects */
    lv_obj_t *slot_obj[4];   /* Touch container / button */
    lv_obj_t *slot_icon[4];  /* Label symbol or Image widget inside slot */
    bool      is_image[4];   /* true if slot holds an image, false for label */
} tile_header_t;

static tile_header_t s_headers[NAV_TILE_COUNT];

static lv_obj_t *s_settings_modal = NULL;
static lv_obj_t *s_settings_ip_label = NULL;
static lv_obj_t *s_settings_tileview = NULL;
static lv_obj_t *s_settings_tile1 = NULL;
static lv_obj_t *s_settings_tile2 = NULL;

__attribute__((weak)) void lora_diag_view_show(void) {
    ESP_LOGI(TAG, "lora_diag_view_show called (default weak implementation)");
}

#if CONFIG_GUI_CONFIG_EXTERN
static void settings_format_sd_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        sd_view_show();
    }
}
#endif

/* ── Central Header Status Update Implementation ────────────────────────── */

void header_update_wifi_st(bool is_connected, int8_t rssi) {
    const lv_image_dsc_t *src;
    if (!is_connected) {
        src = &ui_img_wifi_disconet_png;
    } else if (rssi < -80) {
        src = &ui_img_wifi_1_png;
    } else if (rssi < -65) {
        src = &ui_img_wifi_2_png;
    } else {
        src = &ui_img_wifi_3_png;
    }

    lv_port_sem_take();
    for (int i = 0; i < NAV_TILE_COUNT; i++) {
        if (s_headers[i].slot_icon[3] && s_headers[i].is_image[3]) {
            lv_image_set_src(s_headers[i].slot_icon[3], src);
        }
    }
    lv_port_sem_give();
}

void header_update_lora_st(lora_ui_state_t state) {
    lv_color_t color;
    switch (state) {
        case LORA_UI_UNINIT:  color = lv_color_hex(0x7F8C8D); break; /* Gray */
        case LORA_UI_WAITING: color = lv_color_hex(0xF39C12); break; /* Orange */
        case LORA_UI_ACTIVE:  color = lv_color_hex(0x2ECC71); break; /* Green */
        case LORA_UI_TIMEOUT: color = lv_color_hex(0xE74C3C); break; /* Red */
        default: return;
    }

    lv_port_sem_take();
    for (int i = 0; i < NAV_TILE_COUNT; i++) {
        if (s_headers[i].slot_icon[2] && s_headers[i].is_image[2]) {
            lv_obj_set_style_image_recolor_opa(s_headers[i].slot_icon[2], LV_OPA_COVER, 0);
            lv_obj_set_style_image_recolor(s_headers[i].slot_icon[2], color, 0);
        }
    }
    lv_port_sem_give();
}

/* ── Default Callbacks ──────────────────────────────────────────────────── */

static void default_on_settings_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        settings_view_show();
    }
}

static void default_on_lora_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
#if CONFIG_LORA_ENABLE
        lora_diag_view_show();
#endif
    }
}

static void default_on_wifi_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SCREEN_START,
                          &(uint8_t){SCREEN_WIFI_CONFIG}, sizeof(uint8_t), portMAX_DELAY);
    }
}

static void settings_view_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data) {
    if (id == VIEW_EVENT_WIFI_ST) {
        struct view_data_wifi_st *st = (struct view_data_wifi_st *)event_data;
        if (st) {
            header_update_wifi_st(st->is_connected, st->rssi);
        }
    }
}

/* ── Settings View Modal Implementation ─────────────────────────────────── */

static void settings_refresh_ip_label(void) {
    if (!s_settings_ip_label) return;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {0};

    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        char buf[40];
        snprintf(buf, sizeof(buf), "IP: " IPSTR, IP2STR(&ip_info.ip));
        lv_label_set_text(s_settings_ip_label, buf);
    } else {
        lv_label_set_text(s_settings_ip_label, "IP: not connected");
    }
}

static void settings_hide_modal(void) {
    if (s_settings_modal) {
        lv_obj_add_flag(s_settings_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void settings_post_screen(enum start_screen screen) {
    esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SCREEN_START, &screen, sizeof(screen), portMAX_DELAY);
}

static void settings_open_wifi(lv_event_t *e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) settings_post_screen(SCREEN_WIFI_CONFIG); }
static void settings_open_display(lv_event_t *e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) settings_post_screen(SCREEN_DISPLAY_MODAL); }
static void settings_open_broker(lv_event_t *e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) settings_post_screen(SCREEN_BROKER_MODAL); }
static void settings_open_lora_diag(lv_event_t *e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) lora_diag_view_show(); }
static void settings_on_back(lv_event_t *e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) settings_hide_modal(); }

void settings_view_show(void) {
    if (!s_settings_modal) return;
    settings_refresh_ip_label();
    if (s_settings_tileview) {
        lv_tileview_set_tile_by_index(s_settings_tileview, 0, 0, LV_ANIM_OFF);
    }
    lv_obj_remove_flag(s_settings_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_settings_modal);
}

static void settings_style_card(lv_obj_t *card, lv_color_t color) {
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(card, LV_OPA_80, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(card, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(card, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void settings_create_card(lv_obj_t *parent, int32_t x, int32_t y, lv_color_t color, const char *label, const char *icon, lv_event_cb_t cb) {
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_size(card, 150, 150);
    lv_obj_set_pos(card, x, y);
    settings_style_card(card, color);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon_label = lv_label_create(card);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_28, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(icon_label, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_align(icon_label, LV_ALIGN_CENTER);
    lv_obj_set_y(icon_label, -26);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, label);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_align(title, LV_ALIGN_CENTER);
    lv_obj_set_y(title, 35);
}

static void settings_create_modal(void) {
    if (s_settings_modal) return;

    s_settings_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_settings_modal, CONFIG_LCD_EVB_SCREEN_WIDTH, CONFIG_LCD_EVB_SCREEN_HEIGHT);
    lv_obj_set_pos(s_settings_modal, 0, 0);
    lv_obj_set_align(s_settings_modal, LV_ALIGN_TOP_LEFT);
    lv_obj_add_flag(s_settings_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_settings_modal, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_scrollbar_mode(s_settings_modal, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_settings_modal, lv_color_hex(0x101418), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_settings_modal, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_settings_modal, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_settings_modal, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_settings_modal, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(s_settings_modal, LV_OBJ_FLAG_HIDDEN);

    s_settings_tileview = lv_tileview_create(s_settings_modal);
    lv_obj_set_size(s_settings_tileview, CONFIG_LCD_EVB_SCREEN_WIDTH, CONFIG_LCD_EVB_SCREEN_HEIGHT);
    lv_obj_set_pos(s_settings_tileview, 0, 0);
    lv_obj_set_align(s_settings_tileview, LV_ALIGN_TOP_LEFT);
    lv_obj_set_scrollbar_mode(s_settings_tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_settings_tileview, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_style_bg_opa(s_settings_tileview, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_settings_tileview, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_settings_tileview, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_settings_tileview, 0, LV_PART_MAIN);

    s_settings_tile1 = lv_tileview_add_tile(s_settings_tileview, 0, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_set_size(s_settings_tile1, CONFIG_LCD_EVB_SCREEN_WIDTH, CONFIG_LCD_EVB_SCREEN_HEIGHT);
    lv_obj_remove_flag(s_settings_tile1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_settings_tile1, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_settings_tile1, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_settings_tile1, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_settings_tile1, 0, LV_PART_MAIN);

    s_settings_tile2 = lv_tileview_add_tile(s_settings_tileview, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_set_size(s_settings_tile2, CONFIG_LCD_EVB_SCREEN_WIDTH, CONFIG_LCD_EVB_SCREEN_HEIGHT);
    lv_obj_remove_flag(s_settings_tile2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_settings_tile2, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_settings_tile2, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_settings_tile2, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_settings_tile2, 0, LV_PART_MAIN);

    settings_create_card(s_settings_tile1, 80, 150, lv_color_hex(0x4EACE4), "WiFi", LV_SYMBOL_WIFI, settings_open_wifi);
    settings_create_card(s_settings_tile1, 250, 150, lv_color_hex(0xEEBF41), "Display", LV_SYMBOL_IMAGE, settings_open_display);

#if CONFIG_GUI_CONFIG_EXTERN
    settings_create_card(s_settings_tile1, 80, 310, lv_color_hex(0x27AE60), "SD Card", LV_SYMBOL_SAVE, settings_format_sd_cb);
    settings_create_card(s_settings_tile1, 250, 310, lv_color_hex(0xE66D39), "Broker", LV_SYMBOL_SETTINGS, settings_open_broker);
#else
    lv_obj_t *broker = lv_button_create(s_settings_tile1);
    lv_obj_set_size(broker, 300, 60);
    lv_obj_set_pos(broker, 90, 330);
    settings_style_card(broker, lv_color_hex(0xE66D39));
    lv_obj_add_event_cb(broker, settings_open_broker, LV_EVENT_CLICKED, NULL);

    lv_obj_t *broker_label = lv_label_create(broker);
    lv_label_set_text(broker_label, "Change MQTT Broker Address");
    lv_obj_set_style_text_color(broker_label, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(broker_label);
#endif

    settings_create_card(s_settings_tile2, 80, 150, lv_color_hex(0x8E44AD), "LoRa Diag", LV_SYMBOL_SETTINGS, settings_open_lora_diag);

    lv_obj_t *header = lv_obj_create(s_settings_modal);
    lv_obj_set_size(header, CONFIG_LCD_EVB_SCREEN_WIDTH, 85);
    lv_obj_set_align(header, LV_ALIGN_TOP_MID);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(header, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_set_size(back, 100, 50);
    lv_obj_set_pos(back, 10, 17);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x2a3036), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_40, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(back, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(back, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(back, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(back, settings_on_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(back_label, lv_color_hex(0xe7ecef), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(back_label);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_align(title, LV_ALIGN_BOTTOM_MID);

    s_settings_ip_label = lv_label_create(s_settings_modal);
    lv_label_set_text(s_settings_ip_label, "IP: not connected");
    lv_obj_set_style_text_font(s_settings_ip_label, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_settings_ip_label, lv_color_hex(0xe7ecef), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_align(s_settings_ip_label, LV_ALIGN_TOP_MID);
    lv_obj_set_y(s_settings_ip_label, 105);
}

int settings_view_init(void) {
    lv_port_sem_take();

    /* NOTE: Legacy per-tile header generation removed. 
     * The unified persistent header is now handled via gui_header_init() */

    settings_create_modal();
    lv_port_sem_give();

    /* Single system-wide WiFi status event registration */
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
        view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_ST,
        settings_view_event_handler, NULL, NULL));

    return 0;
}