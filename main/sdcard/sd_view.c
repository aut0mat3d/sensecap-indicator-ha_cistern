#include "sd_view.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "lv_port.h"
#include "sdkconfig.h"
#include "view_data.h"
#include "rp2040.h"

LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(lv_font_montserrat_28);

static const char *TAG = "sd-view";
static void sd_show_confirm_dialog(uint8_t format_type);

static lv_obj_t *s_sd_modal = NULL;
static lv_obj_t *s_mismatch_popup = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_info_label = NULL;
static lv_obj_t *s_confirm_box = NULL;
static lv_obj_t *s_spinner = NULL;

static uint8_t s_pending_format_type = 0; /* 0 = LittleFS only, 1 = Full SD */
static bool s_usb_mounted = false;
static lv_obj_t *s_btn_lfs = NULL;
static lv_obj_t *s_btn_full = NULL;
static lv_obj_t *s_btn_usb = NULL;

static lv_obj_t *s_error_popup = NULL;

#if CONFIG_GUI_CONFIG_EXTERN
static lv_obj_t *s_gui_ext_error_page = NULL;

static void sd_on_open_sd_mgmt_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        sd_view_show();
    }
}

static void sd_update_gui_ext_guard(const struct sd_card_info *info) {
    /* Consider SD valid if explicit state is valid OR if labels match despite RP2040 desync */
    bool label_matches = (strlen(info->volume_label) > 0 && strncmp(info->volume_label, info->expected_label, 11) == 0);
    bool sd_ok = (info->is_present && (info->state >= SD_STATE_VALID_FAT32 || label_matches));

    if (!sd_ok) {
        if (!s_gui_ext_error_page) {
            /* Create full screen blocking overlay on lv_layer_top */
            s_gui_ext_error_page = lv_obj_create(lv_layer_top());
            lv_obj_set_size(s_gui_ext_error_page, CONFIG_LCD_EVB_SCREEN_WIDTH, CONFIG_LCD_EVB_SCREEN_HEIGHT);
            lv_obj_set_pos(s_gui_ext_error_page, 0, 0);
            lv_obj_set_align(s_gui_ext_error_page, LV_ALIGN_TOP_LEFT);
            lv_obj_remove_flag(s_gui_ext_error_page, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(s_gui_ext_error_page, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_set_style_bg_color(s_gui_ext_error_page, lv_color_hex(0x101418), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(s_gui_ext_error_page, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(s_gui_ext_error_page, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

            /* Error Card */
            lv_obj_t *card = lv_obj_create(s_gui_ext_error_page);
            lv_obj_set_size(card, 420, 260);
            lv_obj_set_align(card, LV_ALIGN_CENTER);
            lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_color(card, lv_color_hex(0x1a2126), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(card, lv_color_hex(0xe74c3c), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(card, 3, LV_PART_MAIN | LV_STATE_DEFAULT);

            lv_obj_t *title = lv_label_create(card);
            lv_label_set_text(title, "GUI Load Error!");
            lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(title, lv_color_hex(0xe74c3c), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_align(title, LV_ALIGN_TOP_MID);
            lv_obj_set_y(title, 10);

            lv_obj_t *msg = lv_label_create(card);
            lv_label_set_text(msg, "External GUI config required!\nSD Card missing or unformatted.");
            lv_obj_set_style_text_font(msg, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(msg, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_align(msg, LV_ALIGN_CENTER);
            lv_obj_set_y(msg, -10);

            lv_obj_t *btn_mgmt = lv_button_create(card);
            lv_obj_set_size(btn_mgmt, 220, 45);
            lv_obj_set_align(btn_mgmt, LV_ALIGN_BOTTOM_MID);
            lv_obj_set_y(btn_mgmt, -10);
            lv_obj_set_style_bg_color(btn_mgmt, lv_color_hex(0x27ae60), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_add_event_cb(btn_mgmt, sd_on_open_sd_mgmt_clicked, LV_EVENT_CLICKED, NULL);

            lv_obj_t *lbl_mgmt = lv_label_create(btn_mgmt);
            lv_label_set_text(lbl_mgmt, "Open SD Management");
            lv_obj_center(lbl_mgmt);
        }
    } else {
        /* Auto-Recovery: Clear overlay when valid SD state arrives */
        if (s_gui_ext_error_page) {
            lv_obj_delete(s_gui_ext_error_page);
            s_gui_ext_error_page = NULL;
            ESP_LOGI(TAG, "Auto-Recovery: Main GUI unlocked");
        }
    }
}
#endif

static void sd_close_error_popup(void) {
    if (s_error_popup) {
        lv_obj_delete(s_error_popup);
        s_error_popup = NULL;
    }
}

static void sd_on_error_ok_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        sd_close_error_popup();
    }
}

static void sd_hide_modal(void)
{
	if(s_sd_modal)
	{
		lv_obj_add_flag(s_sd_modal, LV_OBJ_FLAG_HIDDEN);
	}
}

static void sd_on_back(lv_event_t *e)
{
	if(lv_event_get_code(e) == LV_EVENT_CLICKED)
	{
		sd_hide_modal();
	}
}

static void sd_close_confirm_box(void)
{
	if(s_confirm_box)
	{
		lv_obj_delete(s_confirm_box);
		s_confirm_box = NULL;
	}
}

static void sd_on_confirm_cancel(lv_event_t *e)
{
	if(lv_event_get_code(e) == LV_EVENT_CLICKED)
	{
		sd_close_confirm_box();
	}
}

static void sd_show_error_popup(const char *reason) {
    if (s_error_popup) return;

    s_error_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_error_popup, CONFIG_LCD_EVB_SCREEN_WIDTH, CONFIG_LCD_EVB_SCREEN_HEIGHT);
    lv_obj_set_align(s_error_popup, LV_ALIGN_CENTER);
    lv_obj_remove_flag(s_error_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_error_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_error_popup, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_error_popup, LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_error_popup, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *card = lv_obj_create(s_error_popup);
    lv_obj_set_size(card, 380, 220);
    lv_obj_set_align(card, LV_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1a2126), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(card, lv_color_hex(0xe74c3c), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(card, 3, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Format Failed!");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(0xe74c3c), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_align(title, LV_ALIGN_TOP_MID);
    lv_obj_set_y(title, 10);

    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg, reason ? reason : "Could not write partition table.\nCheck SD card hardware.");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(msg, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_align(msg, LV_ALIGN_CENTER);
    lv_obj_set_y(msg, -10);

    lv_obj_t *btn_ok = lv_button_create(card);
    lv_obj_set_size(btn_ok, 120, 45);
    lv_obj_set_align(btn_ok, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_y(btn_ok, -10);
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(0x34495e), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn_ok, sd_on_error_ok_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_ok = lv_label_create(btn_ok);
    lv_label_set_text(lbl_ok, "OK");
    lv_obj_center(lbl_ok);
}

static void sd_on_confirm_proceed(lv_event_t *e)
{
	if(lv_event_get_code(e) != LV_EVENT_CLICKED)
	{
		return;
	}

	sd_close_confirm_box();

	/* Show spinner during format operation */
	if(s_spinner)
	{
		lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
	}

	/* Dispatch format request to storage backend */
	esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
					  VIEW_EVENT_SD_FORMAT_REQ, &s_pending_format_type,
					  sizeof(s_pending_format_type), portMAX_DELAY);

	ESP_LOGI(TAG, "Format request (type=%d) sent to backend", s_pending_format_type);
}

static void sd_on_mismatch_shutdown(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Shutdown initiated from SD mismatch popup");

        /* Send global shutdown event */
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                          VIEW_EVENT_SHUTDOWN, NULL, 0, portMAX_DELAY);
    }
}

static void sd_on_mismatch_format(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        uint8_t format_type = 1; /* Full SD Format */

        /* Disable format button to prevent double-clicks */
        lv_obj_t *btn = lv_event_get_target_obj(e);
        lv_obj_add_state(btn, LV_STATE_DISABLED);

        /* Send format request directly to storage model */
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                          VIEW_EVENT_SD_FORMAT_REQ, &format_type,
                          sizeof(format_type), portMAX_DELAY);

        ESP_LOGI(TAG, "Direct format request sent from mismatch popup");
    }
}

static void sd_show_mismatch_popup(const char *found_label, const char *expected_label) {
    if (s_mismatch_popup) return; /* Already showing */

    /* 1. Full-screen backdrop to block ALL background clicks (e.g. WiFi button) */
    s_mismatch_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_mismatch_popup, CONFIG_LCD_EVB_SCREEN_WIDTH, CONFIG_LCD_EVB_SCREEN_HEIGHT);
    lv_obj_set_align(s_mismatch_popup, LV_ALIGN_CENTER);
    lv_obj_remove_flag(s_mismatch_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_mismatch_popup, LV_OBJ_FLAG_CLICKABLE); /* Intercepts touches */
    lv_obj_set_style_bg_color(s_mismatch_popup, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_mismatch_popup, LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_mismatch_popup, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_mismatch_popup, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* 2. Dialog Card centered on backdrop */
    lv_obj_t *card = lv_obj_create(s_mismatch_popup);
    lv_obj_set_size(card, 420, 280);
    lv_obj_set_align(card, LV_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1a2126), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(card, lv_color_hex(0xe74c3c), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(card, 3, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "SD Card Mismatch!");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(0xe74c3c), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_align(title, LV_ALIGN_TOP_MID);
    lv_obj_set_y(title, 5);

    char msg_buf[128];
    snprintf(msg_buf, sizeof(msg_buf), "Card belongs to another device!\nFound: %s\nExpected: %s",
             found_label[0] ? found_label : "UNLABELED", expected_label);

    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text(msg, msg_buf);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(msg, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_align(msg, LV_ALIGN_CENTER);
    lv_obj_set_y(msg, -15);

    /* Shutdown Button */
    lv_obj_t *btn_down = lv_button_create(card);
    lv_obj_set_size(btn_down, 150, 45);
    lv_obj_set_pos(btn_down, 10, 180);
    lv_obj_set_style_bg_color(btn_down, lv_color_hex(0xc0392b), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn_down, sd_on_mismatch_shutdown, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_down = lv_label_create(btn_down);
    lv_label_set_text(lbl_down, "Shutdown");
    lv_obj_center(lbl_down);

    /* Format Button */
    lv_obj_t *btn_fmt = lv_button_create(card);
    lv_obj_set_size(btn_fmt, 210, 45);
    lv_obj_set_pos(btn_fmt, 175, 180);
    lv_obj_set_style_bg_color(btn_fmt, lv_color_hex(0x27ae60), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn_fmt, sd_on_mismatch_format, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_fmt = lv_label_create(btn_fmt);
    lv_label_set_text(lbl_fmt, "Format for This Device");
    lv_obj_center(lbl_fmt);
}

static void sd_close_mismatch_popup(void) {
    if (s_mismatch_popup) {
        lv_obj_delete(s_mismatch_popup);
        s_mismatch_popup = NULL;
    }
}

static void sd_show_confirm_dialog(uint8_t format_type)
{
	s_pending_format_type = format_type;

	if(s_confirm_box)
	{
		sd_close_confirm_box();
	}

	s_confirm_box = lv_obj_create(s_sd_modal);
	lv_obj_set_size(s_confirm_box, 380, 240);
	lv_obj_set_align(s_confirm_box, LV_ALIGN_CENTER);
	lv_obj_remove_flag(s_confirm_box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(s_confirm_box, lv_color_hex(0x1a2126),
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(s_confirm_box, lv_color_hex(0xe74c3c),
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(s_confirm_box, 2,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *title = lv_label_create(s_confirm_box);
	lv_label_set_text(title, format_type == 0 ? "Format Logs?" : "Wipe SD Card?");
	lv_obj_set_style_text_font(title, &lv_font_montserrat_24,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_color(title, lv_color_hex(0xe74c3c),
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_align(title, LV_ALIGN_TOP_MID);
	lv_obj_set_y(title, 10);

	lv_obj_t *msg = lv_label_create(s_confirm_box);
	if(format_type == 0)
	{
		lv_label_set_text(msg, "This will delete all stored\nsensor logs & history.\nGUI config will be kept.");
	}
	else
	{
		lv_label_set_text(msg, "WARNING: All partitions, configs\nand logs will be erased!");
	}
	lv_obj_set_style_text_font(msg, &lv_font_montserrat_20,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_color(msg, lv_color_white(),
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_align(msg, LV_ALIGN_CENTER);
	lv_obj_set_y(msg, -10);

	/* Cancel Button */
	lv_obj_t *btn_cancel = lv_button_create(s_confirm_box);
	lv_obj_set_size(btn_cancel, 120, 45);
	lv_obj_set_pos(btn_cancel, 30, 160);
	lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x34495e),
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_add_event_cb(btn_cancel, sd_on_confirm_cancel, LV_EVENT_CLICKED, NULL);

	lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
	lv_label_set_text(lbl_cancel, "Cancel");
	lv_obj_center(lbl_cancel);

	/* Confirm Button */
	lv_obj_t *btn_confirm = lv_button_create(s_confirm_box);
	lv_obj_set_size(btn_confirm, 120, 45);
	lv_obj_set_pos(btn_confirm, 190, 160);
	lv_obj_set_style_bg_color(btn_confirm, lv_color_hex(0xc0392b),
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_add_event_cb(btn_confirm, sd_on_confirm_proceed, LV_EVENT_CLICKED, NULL);

	lv_obj_t *lbl_confirm = lv_label_create(btn_confirm);
	lv_label_set_text(lbl_confirm, "Format");
	lv_obj_center(lbl_confirm);
}

static void sd_on_format_logs_clicked(lv_event_t *e)
{
	if(lv_event_get_code(e) == LV_EVENT_CLICKED)
	{
		sd_show_confirm_dialog(0);
	}
}

static void sd_on_format_full_clicked(lv_event_t *e)
{
	if(lv_event_get_code(e) == LV_EVENT_CLICKED)
	{
		sd_show_confirm_dialog(1);
	}
}

static void sd_on_usb_toggle_clicked(lv_event_t *e)
{
	if(lv_event_get_code(e) == LV_EVENT_CLICKED)
	{
		s_usb_mounted = !s_usb_mounted;
		esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
						  VIEW_EVENT_SD_USB_MOUNT_REQ, &s_usb_mounted,
						  sizeof(s_usb_mounted), portMAX_DELAY);

		lv_obj_t *btn = lv_event_get_target_obj(e);
		lv_obj_t *lbl = lv_obj_get_child(btn, 0);
		if(s_usb_mounted)
		{
			lv_label_set_text(lbl, LV_SYMBOL_USB "  Unmount USB Mode");
			lv_obj_set_style_bg_color(btn, lv_color_hex(0xd35400), LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		else
		{
			lv_label_set_text(lbl, LV_SYMBOL_USB "  Mount SD via USB (PC)");
			lv_obj_set_style_bg_color(btn, lv_color_hex(0x16a085), LV_PART_MAIN | LV_STATE_DEFAULT);
		}
	}
}

static void sd_create_modal(void)
{
	if(s_sd_modal) return;

	s_sd_modal = lv_obj_create(lv_layer_top());
	lv_obj_set_size(s_sd_modal, CONFIG_LCD_EVB_SCREEN_WIDTH, CONFIG_LCD_EVB_SCREEN_HEIGHT);
	lv_obj_set_pos(s_sd_modal, 0, 0);
	lv_obj_set_align(s_sd_modal, LV_ALIGN_TOP_LEFT);
	lv_obj_add_flag(s_sd_modal, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(s_sd_modal, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

	/* Strict zero padding & zero border to ensure exact pixel coordinates */
	lv_obj_set_style_radius(s_sd_modal, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_all(s_sd_modal, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(s_sd_modal, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_set_style_bg_color(s_sd_modal, lv_color_hex(0x101418), LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(s_sd_modal, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_add_flag(s_sd_modal, LV_OBJ_FLAG_HIDDEN);

	/* Header bar */
	lv_obj_t *header = lv_obj_create(s_sd_modal);
	lv_obj_set_size(header, CONFIG_LCD_EVB_SCREEN_WIDTH, 85);
	lv_obj_set_align(header, LV_ALIGN_TOP_MID);
	lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_opa(header, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *back = lv_button_create(header);
	lv_obj_set_size(back, 100, 50);
	lv_obj_set_pos(back, 10, 17);
	lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(back, lv_color_hex(0x2a3036), LV_PART_MAIN | LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(back, LV_OPA_40, LV_PART_MAIN | LV_STATE_PRESSED);
	lv_obj_set_style_border_width(back, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(back, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_add_event_cb(back, sd_on_back, LV_EVENT_CLICKED, NULL);

	lv_obj_t *back_label = lv_label_create(back);
	lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
	lv_obj_set_style_text_color(back_label, lv_color_hex(0xe7ecef), LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(back_label);

	/* Title positioned safely to the right of the back button */
	lv_obj_t *title = lv_label_create(header);
	lv_label_set_text(title, "SD Card Management");
	lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_align(title, LV_ALIGN_LEFT_MID);
	lv_obj_set_pos(title, 120, 0);

	/* Information status area */
	s_status_label = lv_label_create(s_sd_modal);
	lv_label_set_text(s_status_label, "Status: Checking SD Card...");
	lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x27AE60), LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_align(s_status_label, LV_ALIGN_TOP_MID);
	lv_obj_set_y(s_status_label, 95);

	s_info_label = lv_label_create(s_sd_modal);
	lv_label_set_text(s_info_label, "Min. size requirement: 4 GB");
	lv_obj_set_style_text_font(s_info_label, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_color(s_info_label, lv_color_hex(0xbdc3c7), LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_align(s_info_label, LV_ALIGN_TOP_MID);
	lv_obj_set_y(s_info_label, 130);

	/* Action Button 1: Format LittleFS Partition */
	s_btn_lfs = lv_button_create(s_sd_modal);
	lv_obj_set_size(s_btn_lfs, 340, 55);
	lv_obj_set_pos(s_btn_lfs, 70, 180);
	lv_obj_set_style_bg_color(s_btn_lfs, lv_color_hex(0x2980b9), LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(s_btn_lfs, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_add_event_cb(s_btn_lfs, sd_on_format_logs_clicked, LV_EVENT_CLICKED, NULL);

	/* Disabled by default until valid LittleFS partition is confirmed */
	lv_obj_add_state(s_btn_lfs, LV_STATE_DISABLED);

	lv_obj_t *lbl_logs = lv_label_create(s_btn_lfs);
	lv_label_set_text(lbl_logs, LV_SYMBOL_TRASH "  Format LittleFS Part.");
	lv_obj_set_style_text_font(lbl_logs, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(lbl_logs);

	/* Action Button 2: Format Complete SD Card */
	s_btn_full = lv_button_create(s_sd_modal);
	lv_obj_set_size(s_btn_full, 340, 55);
	lv_obj_set_pos(s_btn_full, 70, 255);
	lv_obj_set_style_bg_color(s_btn_full, lv_color_hex(0xc0392b), LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(s_btn_full, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_add_event_cb(s_btn_full, sd_on_format_full_clicked, LV_EVENT_CLICKED, NULL);

	/* Disable button by default until valid SD card is detected */
	lv_obj_add_state(s_btn_full, LV_STATE_DISABLED);

	lv_obj_t *lbl_full = lv_label_create(s_btn_full);
	lv_label_set_text(lbl_full, LV_SYMBOL_REFRESH "  Format Full SD (FAT32+LFS)");
	lv_obj_set_style_text_font(lbl_full, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(lbl_full);

	/* Action Button 3: Toggle USB Mass Storage */
	s_btn_usb = lv_button_create(s_sd_modal);
	lv_obj_set_size(s_btn_usb, 340, 55);
	lv_obj_set_pos(s_btn_usb, 70, 330);
	lv_obj_set_style_bg_color(s_btn_usb, lv_color_hex(0x16a085), LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(s_btn_usb, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_add_event_cb(s_btn_usb, sd_on_usb_toggle_clicked, LV_EVENT_CLICKED, NULL);

	/* Disable button by default until a valid SD card is confirmed */
	lv_obj_add_state(s_btn_usb, LV_STATE_DISABLED);

	lv_obj_t *lbl_usb = lv_label_create(s_btn_usb);
	lv_label_set_text(lbl_usb, LV_SYMBOL_USB "  Mount SD via USB (PC)");
	lv_obj_set_style_text_font(lbl_usb, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(lbl_usb);

	/* Busy spinner overlay */
	s_spinner = lv_spinner_create(s_sd_modal);
	lv_spinner_set_anim_params(s_spinner, 1000, 90);
	lv_obj_set_size(s_spinner, 60, 60);
	lv_obj_set_align(s_spinner, LV_ALIGN_CENTER);
	lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
}

void sd_view_show(void)
{
	if(!s_sd_modal)
	{
		sd_create_modal();
	}

	if(s_sd_modal)
	{
		lv_obj_remove_flag(s_sd_modal, LV_OBJ_FLAG_HIDDEN);
		lv_obj_move_foreground(s_sd_modal);

		/* Request fresh card info from backend */
		esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
						  VIEW_EVENT_SD_INFO_REQ, NULL, 0, portMAX_DELAY);
	}
}

static void sd_view_event_handler(void *handler_args, esp_event_base_t base,
                                   int32_t id, void *event_data)
{
	if(id == VIEW_EVENT_SD_FORMAT_RET)
	{
		bool success = *(bool *)event_data;
		lv_port_sem_take();

		/* Hide loading spinner */
		if(s_spinner)
		{
			lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
		}

		if (success) {
			sd_close_mismatch_popup();
			sd_close_error_popup();
			if(s_status_label)
			{
				lv_label_set_text(s_status_label, "Status: Format Successful!");
				lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x27AE60), LV_PART_MAIN | LV_STATE_DEFAULT);
			}
		}
		else
		{
			/* Show explicit error popup on format failure */
			sd_show_error_popup("MBR Write Failed!\nSD Card write-protected or corrupt.");
		}

		/* Re-enable action buttons */
		if(s_btn_lfs) lv_obj_remove_state(s_btn_lfs, LV_STATE_DISABLED);
		if(s_btn_full) lv_obj_remove_state(s_btn_full, LV_STATE_DISABLED);
		if(s_btn_usb) lv_obj_remove_state(s_btn_usb, LV_STATE_DISABLED);

		lv_port_sem_give();
	}

	else if(id == VIEW_EVENT_SD_INFO_RET)
	{
		struct sd_card_info *info = (struct sd_card_info *)event_data;
		lv_port_sem_take();

		/* Auto-heal RP2040 expected label after RP2040 re-flash or reset */
		if (info->is_present && info->state == SD_STATE_MISMATCH &&
		    strlen(info->volume_label) > 0 &&
		    strncmp(info->volume_label, info->expected_label, 11) == 0)
		{
			ESP_LOGW(TAG, "RP2040 expected label out of sync, re-sending ESP32 MAC");
			rp2040_send_esp_mac();
		}

#if CONFIG_GUI_CONFIG_EXTERN
		/* Evaluate External GUI Error Page Guard */
		sd_update_gui_ext_guard(info);
#endif

		if(info->is_present)
		{
			/* 1. Global Mismatch Check: Only show popup if NOT in USB mode and labels TRULY mismatch */
			if(!info->is_usb_mounted && info->state == SD_STATE_MISMATCH && strncmp(info->volume_label, info->expected_label, 11) != 0)
			{
				sd_show_mismatch_popup(info->volume_label, info->expected_label);
			}
			else
			{
				sd_close_mismatch_popup();
			}

			/* 2. Update Modal Status Labels ONLY if created */
			if(s_status_label && s_info_label)
			{
				char buf[64];
				if (info->state == SD_STATE_MISMATCH) {
					snprintf(buf, sizeof(buf), "Status: Unformatted / Mismatch (%llu MB)", (unsigned long long)info->total_mb);
					lv_label_set_text(s_status_label, buf);
					lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xe67e22), LV_PART_MAIN | LV_STATE_DEFAULT); /* Orange */
				} else if (info->state == SD_STATE_VALID_FAT32) {
					snprintf(buf, sizeof(buf), "Status: LittleFS unformatted (%llu MB)", (unsigned long long)info->total_mb);
					lv_label_set_text(s_status_label, buf);
					lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xe67e22), LV_PART_MAIN | LV_STATE_DEFAULT); /* Orange */
				} else {
					snprintf(buf, sizeof(buf), "Status: SD Card Ready (%llu MB)", (unsigned long long)info->total_mb);
					lv_label_set_text(s_status_label, buf);
					lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x27ae60), LV_PART_MAIN | LV_STATE_DEFAULT); /* Green */
				}

				snprintf(buf, sizeof(buf), "Label: %s", 
         				info->volume_label[0] ? info->volume_label : "NONE");
				lv_label_set_text(s_info_label, buf);
				lv_obj_set_style_text_color(s_info_label, lv_color_hex(0xbdc3c7), LV_PART_MAIN | LV_STATE_DEFAULT);

				/* Format LittleFS Part. -> Active if SD belongs to device and P2 exists in MBR */
				if(s_btn_lfs)
				{
					if(info->state >= SD_STATE_VALID_FAT32) {
						lv_obj_remove_state(s_btn_lfs, LV_STATE_DISABLED);
					} else {
						lv_obj_add_state(s_btn_lfs, LV_STATE_DISABLED);
					}
				}

				/* Full SD Format -> Active whenever an SD card is physically present */
				if(s_btn_full)
				{
					lv_obj_remove_state(s_btn_full, LV_STATE_DISABLED);
				}

				/* Mount SD via USB -> Requires valid FAT32 partition */
				if(s_btn_usb)
				{
					if(info->state >= SD_STATE_VALID_FAT32) {
						lv_obj_remove_state(s_btn_usb, LV_STATE_DISABLED);
					} else {
						lv_obj_add_state(s_btn_usb, LV_STATE_DISABLED);
					}

					/* Sync local s_usb_mounted with real hardware state from RP2040 */
					s_usb_mounted = info->is_usb_mounted;
					lv_obj_t *lbl = lv_obj_get_child(s_btn_usb, 0);
					if(lbl) {
						if(s_usb_mounted) {
							lv_label_set_text(lbl, LV_SYMBOL_USB "  Unmount USB Mode");
							lv_obj_set_style_bg_color(s_btn_usb, lv_color_hex(0xd35400), LV_PART_MAIN | LV_STATE_DEFAULT);
						} else {
							lv_label_set_text(lbl, LV_SYMBOL_USB "  Mount SD via USB (PC)");
							lv_obj_set_style_bg_color(s_btn_usb, lv_color_hex(0x16a085), LV_PART_MAIN | LV_STATE_DEFAULT);
						}
					}
				}
			}
		}
		else
		{
			/* Card removed at runtime */
			sd_close_mismatch_popup();

			if(s_status_label && s_info_label)
			{
				lv_label_set_text(s_status_label, "Status: No SD Card Found!");
				lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xe74c3c), LV_PART_MAIN | LV_STATE_DEFAULT);
				lv_label_set_text(s_info_label, "Please insert a 4 GB - 64 GB MicroSD card");

				if(s_btn_lfs) lv_obj_add_state(s_btn_lfs, LV_STATE_DISABLED);
				if(s_btn_full) lv_obj_add_state(s_btn_full, LV_STATE_DISABLED); /* Disable Full Format */
				if(s_btn_usb) lv_obj_add_state(s_btn_usb, LV_STATE_DISABLED);
			}
		}

		lv_port_sem_give();
	}
}

int sd_view_init(void)
{
	lv_port_sem_take();
	sd_create_modal();
	lv_port_sem_give();

	ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
		view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SD_FORMAT_RET,
		sd_view_event_handler, NULL, NULL));

	ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
		view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SD_INFO_RET,
		sd_view_event_handler, NULL, NULL));

	return 0;
}