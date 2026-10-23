/**
 * @file lora_diag_view.c
 * @brief Live RF Diagnostic & Dynamic Telemetry View for SenseCAP Indicator.
 */

#include "lora_diag_view.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lv_port.h"
#include "sdkconfig.h"
#include "view_data.h"
#include "rp2040.h"

LV_FONT_DECLARE(ui_font_font0);
LV_FONT_DECLARE(ui_font_font2);

static const char *TAG = "lora-diag-view";

#define LORA_DEV_ADDR_LOCAL 0UL
#define MAX_DYN_METRICS     16
#define TIMEFRAME_BUF_SIZE  8

typedef struct {
    char            key[24];
    char            unit[12];
    lora_val_type_t val_type;
    union {
        float       f_val;
        int32_t     i_val;
        bool        b_val;
        char        str_val[32];
    } val;
    lv_obj_t       *lbl_item;
    bool            active;
} dyn_metric_t;

/* ── UI Widgets ─────────────────────────────────────────────────────────── */

static lv_obj_t *s_diag_modal = NULL;
static lv_obj_t *s_setup_modal = NULL;

/* Main Page Elements */
static lv_obj_t *s_lbl_target_info = NULL;
static lv_obj_t *s_lbl_rf_params = NULL;

static lv_obj_t *s_lbl_rf_metrics = NULL;
static lv_obj_t *s_lbl_snr_header = NULL;

static lv_obj_t *s_card_sensor = NULL;
static lv_obj_t *s_cont_telemetry = NULL;
static lv_obj_t *s_lbl_node_header = NULL;

static dyn_metric_t s_dyn_metrics[MAX_DYN_METRICS];
static uint8_t  s_dyn_metric_count = 0;

static lv_obj_t *s_lbl_timeframes = NULL;
static lv_obj_t *s_lbl_footer = NULL;

/* Setup Sub-Page Elements */
static lv_obj_t *s_dd_target_dev = NULL;
static lv_obj_t *s_dd_frequency = NULL;
static lv_obj_t *s_dd_sf_bw = NULL;

/* ── Tracking State ─────────────────────────────────────────────────────── */

static uint32_t s_timeframe_deltas[TIMEFRAME_BUF_SIZE] = {0};
static uint8_t  s_timeframe_count = 0;
static uint32_t s_last_packet_uptime_sec = 0;
static uint32_t s_packet_rx_counter = 0;

static lv_timer_t *s_flash_timer = NULL;

static const char *s_eu868_freq_options = 
    "867.100 MHz (Ch 3)\n"
    "867.300 MHz (Ch 4)\n"
    "867.500 MHz (Ch 5)\n"
    "867.700 MHz (Ch 6)\n"
    "867.900 MHz (Ch 7)\n"
    "868.100 MHz (Ch 0)\n"
    "868.300 MHz (Ch 1)\n"
    "868.500 MHz (Ch 2)\n"
    "869.525 MHz (Ch 8 Downlink)";

/* ── Callbacks ──────────────────────────────────────────────────────────── */

static void lora_diag_on_back_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lora_diag_view_hide();
    }
}

static void lora_diag_on_setup_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && s_setup_modal) {
        lv_obj_remove_flag(s_setup_modal, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_setup_modal);
    }
}

static void lora_diag_on_setup_save_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (s_dd_frequency && s_lbl_rf_params) {
            char freq_buf[32], sf_buf[32], info_buf[64];
            lv_dropdown_get_selected_str(s_dd_frequency, freq_buf, sizeof(freq_buf));
            lv_dropdown_get_selected_str(s_dd_sf_bw, sf_buf, sizeof(sf_buf));

            snprintf(info_buf, sizeof(info_buf), "%s | %s", freq_buf, sf_buf);
            lv_label_set_text(s_lbl_rf_params, info_buf);
        }

        if (s_dd_target_dev && s_lbl_target_info) {
            char dev_buf[32], target_buf[48];
            lv_dropdown_get_selected_str(s_dd_target_dev, dev_buf, sizeof(dev_buf));

            snprintf(target_buf, sizeof(target_buf), "Focus: %s", dev_buf);
            lv_label_set_text(s_lbl_target_info, target_buf);
        }

        if (s_setup_modal) {
            lv_obj_add_flag(s_setup_modal, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void lora_diag_on_setup_dismiss_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && s_setup_modal) {
        lv_obj_add_flag(s_setup_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void lora_diag_flash_timer_cb(lv_timer_t *timer) {
    if (s_lbl_timeframes) {
        lv_obj_set_style_text_color(s_lbl_timeframes, lv_color_hex(0xe67e22), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (s_flash_timer) {
        lv_timer_delete(s_flash_timer);
        s_flash_timer = NULL;
    }
}

static void lora_diag_clear_telemetry_card(void) {
    for (uint8_t i = 0; i < s_dyn_metric_count; i++) {
        if (s_dyn_metrics[i].active && s_dyn_metrics[i].lbl_item) {
            lv_obj_delete(s_dyn_metrics[i].lbl_item);
            s_dyn_metrics[i].lbl_item = NULL;
            s_dyn_metrics[i].active = false;
        }
    }
    s_dyn_metric_count = 0;
}

/* ── UI Building ────────────────────────────────────────────────────────── */

static lv_obj_t* lora_diag_create_card(lv_obj_t *parent, int32_t y, int32_t height, const char *title_text) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 440, height);
    lv_obj_set_pos(card, 20, y);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1a2126), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2c3e50), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(card, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, title_text);
    lv_obj_set_style_text_font(title, &ui_font_font0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(0x3498db), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(title, 0, 0);

    return card;
}

static void lora_diag_create_setup_subpage(void) {
    if (s_setup_modal) return;

    s_setup_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_setup_modal, CONFIG_LCD_EVB_SCREEN_WIDTH, CONFIG_LCD_EVB_SCREEN_HEIGHT);
    lv_obj_set_pos(s_setup_modal, 0, 0);
    lv_obj_remove_flag(s_setup_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_setup_modal, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_bg_color(s_setup_modal, lv_color_hex(0x101418), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_setup_modal, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_setup_modal, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_setup_modal, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(s_setup_modal, LV_OBJ_FLAG_HIDDEN);

    /* Header */
    lv_obj_t *title = lv_label_create(s_setup_modal);
    lv_label_set_text(title, "RF & Filter Setup");
    lv_obj_set_style_text_font(title, &ui_font_font2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(title, 20, 15);

    /* Field 1: Target Device Dropdown */
    lv_obj_t *lbl_target = lv_label_create(s_setup_modal);
    lv_label_set_text(lbl_target, "Target Device Filter:");
    lv_obj_set_style_text_font(lbl_target, &ui_font_font0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_target, lv_color_hex(0xbdc3c7), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(lbl_target, 20, 60);

    s_dd_target_dev = lv_dropdown_create(s_setup_modal);
    lv_dropdown_set_options(s_dd_target_dev, "ALL DEVICES\nHochbehälter");
    lv_obj_set_size(s_dd_target_dev, 440, 42);
    lv_obj_set_pos(s_dd_target_dev, 20, 88);

    /* Field 2: Frequency Dropdown */
    lv_obj_t *lbl_freq = lv_label_create(s_setup_modal);
    lv_label_set_text(lbl_freq, "Frequency / Channel:");
    lv_obj_set_style_text_font(lbl_freq, &ui_font_font0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_freq, lv_color_hex(0xbdc3c7), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(lbl_freq, 20, 142);

    s_dd_frequency = lv_dropdown_create(s_setup_modal);
    lv_dropdown_set_options(s_dd_frequency, s_eu868_freq_options);
    lv_dropdown_set_selected(s_dd_frequency, 6); /* Default 868.300 MHz (Ch 1) */
    lv_obj_set_size(s_dd_frequency, 440, 42);
    lv_obj_set_pos(s_dd_frequency, 20, 170);

    /* Field 3: SF & BW Dropdown */
    lv_obj_t *lbl_sf = lv_label_create(s_setup_modal);
    lv_label_set_text(lbl_sf, "Spreading Factor / Bandwidth:");
    lv_obj_set_style_text_font(lbl_sf, &ui_font_font0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lbl_sf, lv_color_hex(0xbdc3c7), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(lbl_sf, 20, 224);

    s_dd_sf_bw = lv_dropdown_create(s_setup_modal);
    lv_dropdown_set_options(s_dd_sf_bw, "SF10 / BW 125 kHz\nSF7 / BW 125 kHz\nSF8 / BW 125 kHz\nSF9 / BW 125 kHz\nSF11 / BW 125 kHz\nSF12 / BW 125 kHz");
    lv_obj_set_size(s_dd_sf_bw, 440, 42);
    lv_obj_set_pos(s_dd_sf_bw, 20, 252);

    /* Save (Green) & Dismiss (Red) Buttons */
    lv_obj_t *btn_save = lv_button_create(s_setup_modal);
    lv_obj_set_size(btn_save, 210, 48);
    lv_obj_set_pos(btn_save, 20, 390);
    lv_obj_set_style_bg_color(btn_save, lv_color_hex(0x27ae60), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn_save, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn_save, lora_diag_on_setup_save_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_save = lv_label_create(btn_save);
    lv_label_set_text(lbl_save, LV_SYMBOL_OK " Save & Apply");
    lv_obj_set_style_text_font(lbl_save, &ui_font_font0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl_save);

    lv_obj_t *btn_dismiss = lv_button_create(s_setup_modal);
    lv_obj_set_size(btn_dismiss, 210, 48);
    lv_obj_set_pos(btn_dismiss, 250, 390);
    lv_obj_set_style_bg_color(btn_dismiss, lv_color_hex(0xc0392b), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn_dismiss, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn_dismiss, lora_diag_on_setup_dismiss_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_dismiss = lv_label_create(btn_dismiss);
    lv_label_set_text(lbl_dismiss, LV_SYMBOL_CLOSE " Dismiss");
    lv_obj_set_style_text_font(lbl_dismiss, &ui_font_font0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl_dismiss);
}

static void lora_diag_create_modal(void) {
    if (s_diag_modal) return;

    s_diag_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_diag_modal, CONFIG_LCD_EVB_SCREEN_WIDTH, CONFIG_LCD_EVB_SCREEN_HEIGHT);
    lv_obj_set_pos(s_diag_modal, 0, 0);
    lv_obj_remove_flag(s_diag_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_diag_modal, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_bg_color(s_diag_modal, lv_color_hex(0x101418), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_diag_modal, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_diag_modal, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_diag_modal, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(s_diag_modal, LV_OBJ_FLAG_HIDDEN);

    /* Header Bar (Unpadded to allow full button display) */
    lv_obj_t *header = lv_obj_create(s_diag_modal);
    lv_obj_set_size(header, CONFIG_LCD_EVB_SCREEN_WIDTH, 48);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *btn_back = lv_button_create(header);
    lv_obj_set_size(btn_back, 85, 38);
    lv_obj_set_pos(btn_back, 10, 5);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2a3036), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn_back, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn_back, lora_diag_on_back_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lbl_back);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "LoRa Diagnostic");
    lv_obj_set_style_text_font(title, &ui_font_font2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_align(title, LV_ALIGN_CENTER);

    lv_obj_t *btn_setup = lv_button_create(header);
    lv_obj_set_size(btn_setup, 95, 38);
    lv_obj_set_pos(btn_setup, 375, 5);
    lv_obj_set_style_bg_color(btn_setup, lv_color_hex(0x8e44ad), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn_setup, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn_setup, lora_diag_on_setup_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_setup = lv_label_create(btn_setup);
    lv_label_set_text(lbl_setup, LV_SYMBOL_SETTINGS " Setup");
    lv_obj_center(lbl_setup);

    /* Sub-Headers (Shifted down to Y=56 & Y=78 so they clear the top buttons) */
    s_lbl_target_info = lv_label_create(s_diag_modal);
    lv_label_set_text(s_lbl_target_info, "Focus: ALL DEVICES");
    lv_obj_set_style_text_font(s_lbl_target_info, &ui_font_font0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_lbl_target_info, lv_color_hex(0xf39c12), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_lbl_target_info, 20, 56);

    s_lbl_rf_params = lv_label_create(s_diag_modal);
    lv_label_set_text(s_lbl_rf_params, "868.300 MHz | SF10 / BW 125 kHz");
    lv_obj_set_style_text_font(s_lbl_rf_params, &ui_font_font0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_lbl_rf_params, lv_color_hex(0xbdc3c7), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_lbl_rf_params, 20, 78);

    /* ── Card 1: RF Signal Quality ────────────────────────────────────────── */
    lv_obj_t *card_rf = lora_diag_create_card(s_diag_modal, 100, 58, "RF Signal Quality");

    s_lbl_snr_header = lv_label_create(card_rf);
    lv_label_set_text(s_lbl_snr_header, "SNR: -- dB");
    lv_obj_set_style_text_font(s_lbl_snr_header, &ui_font_font0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_lbl_snr_header, lv_color_hex(0x2ecc71), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_lbl_snr_header, 310, 0);

    s_lbl_rf_metrics = lv_label_create(card_rf);
    lv_label_set_text(s_lbl_rf_metrics, "Floor: -100 dBm  |  RSSI: -- dBm");
    lv_obj_set_style_text_font(s_lbl_rf_metrics, &ui_font_font0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_lbl_rf_metrics, 0, 26);

    /* ── Card 2: Decoded Telemetry (EXPANDED TO H=204 px) ─────────────────── */
    s_card_sensor = lora_diag_create_card(s_diag_modal, 166, 204, "Decoded Telemetry");

    s_lbl_node_header = lv_label_create(s_card_sensor);
    lv_label_set_text(s_lbl_node_header, "Waiting...");
    lv_obj_set_style_text_font(s_lbl_node_header, &ui_font_font0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_lbl_node_header, lv_color_hex(0x2ecc71), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_lbl_node_header, 210, 0);

    /* Ultra-compact flex container without row gaps (pad_row = 0) */
    s_cont_telemetry = lv_obj_create(s_card_sensor);
    lv_obj_set_size(s_cont_telemetry, 424, 166);
    lv_obj_set_pos(s_cont_telemetry, 0, 26);
    lv_obj_set_flex_flow(s_cont_telemetry, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_cont_telemetry, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(s_cont_telemetry, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_pad_all(s_cont_telemetry, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(s_cont_telemetry, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(s_cont_telemetry, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_cont_telemetry, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_cont_telemetry, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ── Card 3: Timeframe Intervals (s) (Shifted to Y=378) ────────────────── */
    lv_obj_t *card_tf = lora_diag_create_card(s_diag_modal, 378, 58, "Timeframe Intervals (s)");

    s_lbl_timeframes = lv_label_create(card_tf);
    lv_label_set_text(s_lbl_timeframes, "Waiting for packets...");
    lv_obj_set_style_text_font(s_lbl_timeframes, &ui_font_font0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_lbl_timeframes, lv_color_hex(0xe67e22), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_lbl_timeframes, 0, 26);

    /* ── Footer Status Bar (Shifted to Y=446) ─────────────────────────────── */
    s_lbl_footer = lv_label_create(s_diag_modal);
    lv_label_set_text(s_lbl_footer, "Rx: 0  |  Last: --:--:--  |  Dev: --");
    lv_obj_set_style_text_font(s_lbl_footer, &ui_font_font0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_lbl_footer, lv_color_hex(0x7f8c8d), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_lbl_footer, 20, 446);

    lora_diag_create_setup_subpage();
}

/* ── Public API Implementation ─────────────────────────────────────────── */

void lora_diag_view_show(void) {
    if (!s_diag_modal) {
        lora_diag_create_modal();
    }
    if (s_diag_modal) {
        lv_obj_remove_flag(s_diag_modal, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_diag_modal);
    }
}

void lora_diag_view_hide(void) {
    if (s_diag_modal) {
        lv_obj_add_flag(s_diag_modal, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_setup_modal) {
        lv_obj_add_flag(s_setup_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void lora_diag_view_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data) {
    if (id == VIEW_EVENT_LORA_DIAG_DATA) {
        struct view_data_lora_diag *diag = (struct view_data_lora_diag *)event_data;
        if (!diag) return;

        lv_port_sem_take();

        if (diag->dev_addr != LORA_DEV_ADDR_LOCAL) {
            s_packet_rx_counter++;

            /* Clear dynamic metric widgets for fresh incoming packet */
            lora_diag_clear_telemetry_card();

            uint32_t now_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);

            if (s_last_packet_uptime_sec > 0 && now_sec > s_last_packet_uptime_sec) {
                uint32_t delta = now_sec - s_last_packet_uptime_sec;

                for (int i = TIMEFRAME_BUF_SIZE - 1; i > 0; i--) {
                    s_timeframe_deltas[i] = s_timeframe_deltas[i - 1];
                }
                s_timeframe_deltas[0] = delta;

                if (s_timeframe_count < TIMEFRAME_BUF_SIZE) s_timeframe_count++;

                char tf_buf[128] = "";
                int offset = 0;
                for (uint8_t i = 0; i < s_timeframe_count; i++) {
                    offset += snprintf(tf_buf + offset, sizeof(tf_buf) - offset, "%lu%s",
                                       (unsigned long)s_timeframe_deltas[i],
                                       (i == s_timeframe_count - 1) ? "" : " | ");
                }

                if (s_lbl_timeframes) {
                    lv_label_set_text(s_lbl_timeframes, tf_buf);
                    lv_obj_set_style_text_color(s_lbl_timeframes, lv_color_hex(0xf39c12), LV_PART_MAIN | LV_STATE_DEFAULT);
                    s_flash_timer = lv_timer_create(lora_diag_flash_timer_cb, 600, NULL);
                }
            }
            s_last_packet_uptime_sec = now_sec;

            if (s_lbl_rf_metrics) {
                char buf[64];
                snprintf(buf, sizeof(buf), "Floor: %d dBm  |  RSSI: %d dBm", diag->noise_floor, diag->rssi);
                lv_label_set_text(s_lbl_rf_metrics, buf);
            }

            if (s_lbl_snr_header) {
                char buf[32];
                snprintf(buf, sizeof(buf), "SNR: %+d dB", diag->snr);
                lv_label_set_text(s_lbl_snr_header, buf);
            }

            if (s_lbl_node_header) {
                char buf[48];
                snprintf(buf, sizeof(buf), "%s", diag->dev_name);
                lv_label_set_text(s_lbl_node_header, buf);
            }

            if (s_lbl_footer) {
                time_t now = time(NULL);
                struct tm timeinfo;
                
                /* Force evaluation of TZ environment variable */
                tzset();
                localtime_r(&now, &timeinfo);

                char buf[80];
                if (now > 1700000000) {
                    snprintf(buf, sizeof(buf), "Rx: %lu  |  Last: %02d:%02d:%02d  |  Dev: 0x%08lX",
                             (unsigned long)s_packet_rx_counter,
                             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                             (unsigned long)diag->dev_addr);
                } else {
                    snprintf(buf, sizeof(buf), "Rx: %lu  |  Last: No Sync  |  Dev: 0x%08lX",
                             (unsigned long)s_packet_rx_counter,
                             (unsigned long)diag->dev_addr);
                }
                lv_label_set_text(s_lbl_footer, buf);
            }
        } else if (diag->noise_floor != 0 && s_lbl_rf_metrics) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Floor: %d dBm  |  RSSI: -- dBm", diag->noise_floor);
            lv_label_set_text(s_lbl_rf_metrics, buf);
        }

        lv_port_sem_give();
    }
    else if (id == VIEW_EVENT_LORA_TELEMETRY) {
        struct view_data_lora_telemetry *data = (struct view_data_lora_telemetry *)event_data;
        /* Ignore local I2C and MQTT values on RF diagnostic screen */
        if (!data || data->origin != DATA_ORIGIN_LORA) return;

        lv_port_sem_take();

        /* Search for existing dynamic metric slot by key name */
        int slot = -1;
        for (uint8_t i = 0; i < s_dyn_metric_count; i++) {
            if (s_dyn_metrics[i].active && strcmp(s_dyn_metrics[i].key, data->key) == 0) {
                slot = i;
                break;
            }
        }

        /* Dynamically create new metric slot in Flex Container */
        if (slot < 0 && s_dyn_metric_count < MAX_DYN_METRICS) {
            slot = s_dyn_metric_count++;
            dyn_metric_t *m = &s_dyn_metrics[slot];
            memset(m, 0, sizeof(dyn_metric_t));
            strlcpy(m->key, data->key, sizeof(m->key));
            strlcpy(m->unit, data->unit, sizeof(m->unit));
            m->active = true;

            m->lbl_item = lv_label_create(s_cont_telemetry);
            lv_obj_set_style_text_font(m->lbl_item, &ui_font_font0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_all(m->lbl_item, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        }

        /* Update metric value & formatted recolor text */
        if (slot >= 0) {
            dyn_metric_t *m = &s_dyn_metrics[slot];
            m->val_type = data->val_type;
            strlcpy(m->unit, data->unit, sizeof(m->unit));

            char buf[96];
            switch (m->val_type) {
                case LORA_VAL_TYPE_FLOAT:
                    m->val.f_val = data->val.f_val;
                    snprintf(buf, sizeof(buf), "#bdc3c7 %s:# #2ecc71 %.1f %s#", m->key, m->val.f_val, m->unit);
                    break;
                case LORA_VAL_TYPE_INT:
                    m->val.i_val = data->val.i_val;
                    snprintf(buf, sizeof(buf), "#bdc3c7 %s:# #2ecc71 %ld %s#", m->key, (long)m->val.i_val, m->unit);
                    break;
                case LORA_VAL_TYPE_BOOL:
                    m->val.b_val = data->val.b_val;
                    snprintf(buf, sizeof(buf), "#bdc3c7 %s:# #2ecc71 %s#", m->key, m->val.b_val ? "TRUE" : "FALSE");
                    break;
                case LORA_VAL_TYPE_STRING:
                    strlcpy(m->val.str_val, data->val.str_val, sizeof(m->val.str_val));
                    snprintf(buf, sizeof(buf), "#bdc3c7 %s:# #2ecc71 %s %s#", m->key, m->val.str_val, m->unit);
                    break;
            }

            if (m->lbl_item) {
                lv_label_set_recolor(m->lbl_item, true);
                lv_label_set_text(m->lbl_item, buf);
            }
        }

        lv_port_sem_give();
    }
}

int lora_diag_view_init(void) {
    lv_port_sem_take();
    lora_diag_create_modal();
    lv_port_sem_give();

    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
        view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_LORA_TELEMETRY,
        lora_diag_view_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
        view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_LORA_DIAG_DATA,
        lora_diag_view_event_handler, NULL, NULL));

    ESP_LOGI(TAG, "Dynamic Compact Flex LoRa Diagnostic View initialized successfully.");
    return 0;
}