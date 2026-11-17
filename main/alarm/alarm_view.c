/**
 * @file alarm_view.c
 * @brief Modal Fullscreen Alarm View UI implementation using lv_layer_top.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "alarm_view.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "lv_port.h"
#include "alarm/alarm_model.h"
#include "gui/i18n/gui_i18n.h"

LV_FONT_DECLARE(ui_font_font0);

static const char *TAG = "alarm-view";

/* Modal top layer widgets */
static lv_obj_t *s_alarm_modal = NULL;
static lv_obj_t *s_list_alarms = NULL;

/* Header controls */
static lv_obj_t *s_btn_mute = NULL;
static lv_obj_t *s_lbl_mute = NULL;

/* Dedicated static toolbar controls (Order: Priorität, Zeit, Reset, Zustand) */
static lv_obj_t *s_btn_filter_prio = NULL;
static lv_obj_t *s_lbl_filter_prio = NULL;
static lv_obj_t *s_btn_sort_time = NULL;
static lv_obj_t *s_lbl_sort_time = NULL;
static lv_obj_t *s_btn_reset = NULL;
static lv_obj_t *s_lbl_reset = NULL;
static lv_obj_t *s_btn_filter_state = NULL;
static lv_obj_t *s_lbl_filter_state = NULL;

/* Sorting and filtering state */
static alarm_sort_mode_t s_current_sort_mode = ALARM_SORT_BY_TIME_DESC;
static alarm_filter_t    s_current_filter = ALARM_FILTER_ALL;
static int               s_current_state_filter = 0; /* 0=All, 1=Active, 2=Ack, 3=Going */

void alarm_view_hide(void);

/* ── Color & Severity Helpers ───────────────────────────────────────────── */

static lv_color_t _get_severity_color(alarm_severity_t severity) {
    const alarm_ui_config_t *cfg = alarm_get_ui_config();
    switch (severity) {
        case ALARM_SEVERITY_DANGER:  return lv_color_hex(cfg ? cfg->colors.color_danger : 0xE74C3C);
        case ALARM_SEVERITY_WARNING: return lv_color_hex(cfg ? cfg->colors.color_warning : 0xF39C12);
        case ALARM_SEVERITY_INFO:    return lv_color_hex(cfg ? cfg->colors.color_info : 0x9E9E9E);
        default:                     return lv_color_white();
    }
}

static lv_color_t _get_state_color(alarm_state_t state, alarm_severity_t severity) {
    const alarm_ui_config_t *cfg = alarm_get_ui_config();
    if (!cfg) return _get_severity_color(severity);

    switch (state) {
        case ALARM_STATE_INCOMING:     return lv_color_hex(cfg->colors.color_incoming);
        case ALARM_STATE_GOING:        return lv_color_hex(cfg->colors.color_going);
        case ALARM_STATE_MUTED:        return lv_color_hex(cfg->colors.color_muted);
        case ALARM_STATE_RETRIGGERED:  return lv_color_hex(cfg->colors.color_retriggered);
        case ALARM_STATE_ACKNOWLEDGED: return lv_color_hex(cfg->colors.color_acknowledged);
        default:                       return _get_severity_color(severity);
    }
}

static const char* _get_state_abbr(alarm_state_t state) {
    switch (state) {
        case ALARM_STATE_INCOMING:     return gui_i18n_get("state_abbr.incoming");
        case ALARM_STATE_GOING:        return gui_i18n_get("state_abbr.going");
        case ALARM_STATE_MUTED:        return gui_i18n_get("state_abbr.muted");
        case ALARM_STATE_RETRIGGERED:  return gui_i18n_get("state_abbr.retriggered");
        case ALARM_STATE_ACKNOWLEDGED: return gui_i18n_get("state_abbr.acknowledged");
        default:                       return "---";
    }
}

static void _format_timestamp(uint32_t timestamp, char *buf, size_t buf_size) {
    struct tm timeinfo;
    time_t rawtime = (time_t)timestamp;
    localtime_r(&rawtime, &timeinfo);
    snprintf(buf, buf_size, "%02d.%02d. %02d:%02d",
             timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_hour, timeinfo.tm_min);
}

/* ── UI Building & List Refresh ────────────────────────────────────────── */

static void _update_toolbar_texts(void) {
    if (!s_lbl_filter_prio || !s_lbl_sort_time || !s_lbl_reset || !s_lbl_filter_state) return;
    const alarm_ui_config_t *cfg = alarm_get_ui_config();

    /* 1. Priority filter (Button 1, links) */
    char prio_buf[32];
    const char *prio_str = gui_i18n_get("alarm.filter_all");
    const char *prio_sym = LV_SYMBOL_WARNING;
    lv_color_t prio_color = lv_color_white();

    if (s_current_filter == ALARM_FILTER_DANGER) {
        prio_str = cfg ? cfg->texts.danger : "Alarm";
        prio_color = _get_severity_color(ALARM_SEVERITY_DANGER);
        prio_sym = LV_SYMBOL_WARNING;
    } else if (s_current_filter == ALARM_FILTER_WARNING) {
        prio_str = cfg ? cfg->texts.warning : "Warn";
        prio_color = _get_severity_color(ALARM_SEVERITY_WARNING);
        prio_sym = LV_SYMBOL_WARNING;
    } else if (s_current_filter == ALARM_FILTER_INFO) {
        prio_str = cfg ? cfg->texts.info : "Info";
        prio_color = _get_severity_color(ALARM_SEVERITY_INFO);
        prio_sym = LV_SYMBOL_BULLET;
    }
    snprintf(prio_buf, sizeof(prio_buf), "%s %s", prio_sym, prio_str);
    lv_label_set_text(s_lbl_filter_prio, prio_buf);
    lv_obj_set_style_text_color(s_lbl_filter_prio, prio_color, LV_PART_MAIN);

    /* 2. Sort direction (Button 2, mitte-links) */
    char time_text[32];
    snprintf(time_text, sizeof(time_text), "%s %s", gui_i18n_get("alarm.hdr_time"),
             (s_current_sort_mode == ALARM_SORT_BY_TIME_ASC) ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
    lv_label_set_text(s_lbl_sort_time, time_text);

    /* 3. Reset (Button 3, mitte-rechts) */
    char reset_str[32];
    snprintf(reset_str, sizeof(reset_str), "%s %s", LV_SYMBOL_REFRESH, gui_i18n_get("common.reset"));
    lv_label_set_text(s_lbl_reset, reset_str);

    /* 4. State filter (Button 4, rechts) */
    char st_buf[32];
    const char *st_sym = LV_SYMBOL_BELL;
    const char *st_str = gui_i18n_get("alarm.filter_all");
    lv_color_t st_color = lv_color_white();

    if (s_current_state_filter == 1) {
        st_sym = LV_SYMBOL_BELL;
        st_str = gui_i18n_get("state_abbr.incoming");
        st_color = _get_state_color(ALARM_STATE_INCOMING, ALARM_SEVERITY_DANGER);
    } else if (s_current_state_filter == 2) {
        st_sym = LV_SYMBOL_OK;
        st_str = gui_i18n_get("state_abbr.acknowledged");
        st_color = _get_state_color(ALARM_STATE_ACKNOWLEDGED, ALARM_SEVERITY_INFO);
    } else if (s_current_state_filter == 3) {
        st_sym = LV_SYMBOL_REFRESH;
        st_str = gui_i18n_get("state_abbr.going");
        st_color = _get_state_color(ALARM_STATE_GOING, ALARM_SEVERITY_INFO);
    }
    snprintf(st_buf, sizeof(st_buf), "%s %s", st_sym, st_str);
    lv_label_set_text(s_lbl_filter_state, st_buf);
    lv_obj_set_style_text_color(s_lbl_filter_state, st_color, LV_PART_MAIN);

    /* 5. Header Mute button */
    if (s_lbl_mute) {
        if (alarm_is_muted()) {
            lv_label_set_text(s_lbl_mute, gui_i18n_get("status.muted"));
        } else {
            char buf[32];
            uint32_t dur = cfg ? cfg->mute_duration_sec : 7200;
            if (dur == 0 || (cfg && strcmp(cfg->hw_button_behavior, "MUTE_PERM") == 0)) {
                snprintf(buf, sizeof(buf), "%s", gui_i18n_get("common.mute"));
            } else {
                const char *unit_h = gui_i18n_get("common.unit_h");
                const char *unit_m = gui_i18n_get("common.unit_m");
                if (strcmp(unit_h, "common.unit_h") == 0) unit_h = "h";
                if (strcmp(unit_m, "common.unit_m") == 0) unit_m = "min";

                if (dur >= 3600) {
                    snprintf(buf, sizeof(buf), "%s (%lu%s)", gui_i18n_get("common.mute"),
                             (unsigned long)(dur / 3600), unit_h);
                } else {
                    snprintf(buf, sizeof(buf), "%s (%lu%s)", gui_i18n_get("common.mute"),
                             (unsigned long)(dur / 60), unit_m);
                }
            }
            lv_label_set_text(s_lbl_mute, buf);
        }
    }
}

static lv_obj_t* _create_alarm_list_item(const alarm_entry_t *alarm) {
    if (!alarm) return NULL;

    lv_obj_t *item = lv_obj_create(s_list_alarms);
    lv_obj_set_width(item, LV_PCT(100));
    lv_obj_set_height(item, LV_SIZE_CONTENT);
    lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(item, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(item, lv_color_hex(0x1F252B), LV_PART_MAIN);
    lv_obj_set_style_border_color(item, lv_color_hex(0x2C3E50), LV_PART_MAIN);
    lv_obj_set_style_border_width(item, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(item, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(item, 4, LV_PART_MAIN);

    const char *state_text = _get_state_abbr(alarm->state);
    size_t src_len = strlen(alarm->source);
    size_t badge_len = strlen(state_text);
    bool fits_line_1 = (src_len * 11 + badge_len * 11 + 160 <= 440);

    /* ── Line 1: Header Row (Icon + Timestamp + Badge) ── */
    lv_obj_t *row_top = lv_obj_create(item);
    lv_obj_set_width(row_top, LV_PCT(100));
    lv_obj_set_height(row_top, LV_SIZE_CONTENT);
    lv_obj_remove_flag(row_top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row_top, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row_top, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row_top, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(row_top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_top, 8, LV_PART_MAIN);

    /* Severity Icon (links) */
    lv_obj_t *lbl_icon = lv_label_create(row_top);
    lv_label_set_text(lbl_icon, (alarm->severity == ALARM_SEVERITY_INFO) ? LV_SYMBOL_BULLET : LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(lbl_icon, _get_severity_color(alarm->severity), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_icon, &ui_font_font0, LV_PART_MAIN);

    /* Timestamp (mitte-links) */
    char time_buf[24];
    _format_timestamp(alarm->timestamp, time_buf, sizeof(time_buf));
    lv_obj_t *lbl_time = lv_label_create(row_top);
    lv_label_set_text(lbl_time, time_buf);
    lv_obj_set_style_text_font(lbl_time, &ui_font_font0, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(0xBDC3C7), LV_PART_MAIN);

    /* Badge Helper Macro (rechts) */
    #define CREATE_BADGE(parent_obj) do { \
        lv_obj_t *badge = lv_obj_create(parent_obj); \
        lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT); \
        lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE); \
        lv_obj_set_style_radius(badge, 4, LV_PART_MAIN); \
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x101418), LV_PART_MAIN); \
        lv_obj_set_style_border_width(badge, 1, LV_PART_MAIN); \
        lv_obj_set_style_border_color(badge, _get_state_color(alarm->state, alarm->severity), LV_PART_MAIN); \
        lv_obj_set_style_pad_hor(badge, 6, LV_PART_MAIN); \
        lv_obj_set_style_pad_ver(badge, 2, LV_PART_MAIN); \
        lv_obj_t *lbl_st = lv_label_create(badge); \
        lv_label_set_text(lbl_st, state_text); \
        lv_obj_set_style_text_font(lbl_st, &ui_font_font0, LV_PART_MAIN); \
        lv_obj_set_style_text_color(lbl_st, _get_state_color(alarm->state, alarm->severity), LV_PART_MAIN); \
        lv_obj_center(lbl_st); \
    } while(0)

    if (fits_line_1) {
        lv_obj_t *lbl_src = lv_label_create(row_top);
        lv_label_set_text(lbl_src, alarm->source);
        lv_obj_set_style_text_font(lbl_src, &ui_font_font0, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl_src, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_flex_grow(lbl_src, 1);

        CREATE_BADGE(row_top);
    } else {
        lv_obj_t *spacer = lv_obj_create(row_top);
        lv_obj_set_height(spacer, 1);
        lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(spacer, 0, LV_PART_MAIN);
        lv_obj_set_flex_grow(spacer, 1);

        CREATE_BADGE(row_top);

        /* Dedicated Source Row */
        lv_obj_t *lbl_src = lv_label_create(item);
        lv_label_set_text(lbl_src, alarm->source);
        lv_obj_set_style_text_font(lbl_src, &ui_font_font0, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl_src, lv_color_white(), LV_PART_MAIN);
        lv_label_set_long_mode(lbl_src, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl_src, LV_PCT(100));
    }

    #undef CREATE_BADGE

    /* ── Bottom Line: Full Message Text ── */
    lv_obj_t *lbl_msg = lv_label_create(item);
    lv_label_set_text(lbl_msg, alarm->message);
    lv_obj_set_style_text_font(lbl_msg, &ui_font_font0, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_msg, lv_color_hex(0x95A5A6), LV_PART_MAIN);
    lv_label_set_long_mode(lbl_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_msg, LV_PCT(100));

    return item;
}

void alarm_view_update_list(void) {
    if (!s_list_alarms) return;

    _update_toolbar_texts();

    /* Clear dynamic list entries */
    lv_obj_clean(s_list_alarms);

    size_t count = 0;
    alarm_entry_t *alarms = alarm_get_list(s_current_sort_mode, s_current_filter, &count);

    if (!alarms || count == 0) {
        lv_obj_t *no_alarms = lv_label_create(s_list_alarms);
        lv_label_set_text(no_alarms, gui_i18n_get("alarm.no_alarms"));
        lv_obj_set_style_text_font(no_alarms, &ui_font_font0, LV_PART_MAIN);
        lv_obj_set_style_text_color(no_alarms, lv_color_hex(0x7F8C8D), LV_PART_MAIN);
        lv_obj_align(no_alarms, LV_ALIGN_CENTER, 0, 40);
        if (alarms) free(alarms);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        /* Apply State Filter */
        if (s_current_state_filter == 1 && alarms[i].state != ALARM_STATE_INCOMING && alarms[i].state != ALARM_STATE_RETRIGGERED) continue;
        if (s_current_state_filter == 2 && alarms[i].state != ALARM_STATE_ACKNOWLEDGED) continue;
        if (s_current_state_filter == 3 && alarms[i].state != ALARM_STATE_GOING) continue;

        _create_alarm_list_item(&alarms[i]);
    }
    free(alarms);
}

/* ── Callbacks ──────────────────────────────────────────────────────────── */

static void _on_back_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        alarm_view_hide();
    }
}

static void _on_mute_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        const alarm_ui_config_t *cfg = alarm_get_ui_config();
        uint32_t dur = cfg ? cfg->mute_duration_sec : 7200;
        alarm_mute(dur);
        alarm_buzzer_stop();
        alarm_view_update_list();
    }
}

static void _on_acknowledge_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        alarm_view_acknowledge_all();
        alarm_view_update_list();
    }
}

static void _on_prio_filter_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        s_current_filter = (alarm_filter_t)((s_current_filter + 1) % (ALARM_FILTER_INFO + 1));
        alarm_view_update_list();
    }
}

static void _on_time_sort_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        s_current_sort_mode = (s_current_sort_mode == ALARM_SORT_BY_TIME_DESC) ?
                               ALARM_SORT_BY_TIME_ASC : ALARM_SORT_BY_TIME_DESC;
        alarm_view_update_list();
    }
}

static void _on_state_filter_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        s_current_state_filter = (s_current_state_filter + 1) % 4;
        alarm_view_update_list();
    }
}

static void _alarm_view_reset_filters(void) {
    s_current_sort_mode = ALARM_SORT_BY_TIME_DESC;
    s_current_filter = ALARM_FILTER_ALL;
    s_current_state_filter = 0;
    alarm_view_update_list();
}

static void _on_reset_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        _alarm_view_reset_filters();
    }
}

/* ── Modal Creation ─────────────────────────────────────────────────────── */

static void _create_alarm_modal(void) {
    if (s_alarm_modal) return;

    /* 1. Fullscreen overlay on top layer */
    s_alarm_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_alarm_modal, 480, 480);
    lv_obj_set_pos(s_alarm_modal, 0, 0);
    lv_obj_remove_flag(s_alarm_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_alarm_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_alarm_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_style_bg_color(s_alarm_modal, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_alarm_modal, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_alarm_modal, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_alarm_modal, 0, LV_PART_MAIN);

    /* 2. Top Bar (Header: y=0..52) */
    lv_obj_t *header = lv_obj_create(s_alarm_modal);
    lv_obj_set_size(header, 480, 52);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x161B20), LV_PART_MAIN);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(header, lv_color_hex(0x2C3E50), LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN);

    /* Back button */
    lv_obj_t *btn_back = lv_button_create(header);
    lv_obj_set_size(btn_back, 100, 38);
    lv_obj_set_pos(btn_back, 8, 7);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2A3036), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_back, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_back, _on_back_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_back = lv_label_create(btn_back);
    char back_str[32];
    snprintf(back_str, sizeof(back_str), "%s %s", LV_SYMBOL_LEFT, gui_i18n_get("common.back"));
    lv_label_set_text(lbl_back, back_str);
    lv_obj_set_style_text_font(lbl_back, &ui_font_font0, LV_PART_MAIN);
    lv_obj_center(lbl_back);

    /* Title */
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, gui_i18n_get("alarm.title"));
    lv_obj_set_style_text_font(title, &ui_font_font0, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, -60, 0);

    /* Mute button */
    s_btn_mute = lv_button_create(header);
    lv_obj_set_size(s_btn_mute, 110, 38);
    lv_obj_set_pos(s_btn_mute, 232, 7);
    lv_obj_set_style_bg_color(s_btn_mute, lv_color_hex(0x8E44AD), LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_mute, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_btn_mute, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_mute, _on_mute_clicked, LV_EVENT_CLICKED, NULL);

    s_lbl_mute = lv_label_create(s_btn_mute);
    lv_label_set_text(s_lbl_mute, gui_i18n_get("common.mute"));
    lv_obj_set_style_text_font(s_lbl_mute, &ui_font_font0, LV_PART_MAIN);
    lv_obj_center(s_lbl_mute);

    /* Acknowledge button */
    lv_obj_t *btn_ack = lv_button_create(header);
    lv_obj_set_size(btn_ack, 124, 38);
    lv_obj_set_pos(btn_ack, 348, 7);
    lv_obj_set_style_bg_color(btn_ack, lv_color_hex(0x27AE60), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_ack, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn_ack, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_ack, _on_acknowledge_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_ack = lv_label_create(btn_ack);
    char ack_str[32];
    snprintf(ack_str, sizeof(ack_str), "%s %s", LV_SYMBOL_OK, gui_i18n_get("common.quit"));
    lv_label_set_text(lbl_ack, ack_str);
    lv_obj_set_style_text_font(lbl_ack, &ui_font_font0, LV_PART_MAIN);
    lv_obj_center(lbl_ack);

    /* 3. Dedicated Static Toolbar (y=52..92, 40px height) */
    lv_obj_t *toolbar = lv_obj_create(s_alarm_modal);
    lv_obj_set_size(toolbar, 480, 40);
    lv_obj_set_pos(toolbar, 0, 52);
    lv_obj_remove_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(toolbar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(toolbar, lv_color_hex(0x161B20), LV_PART_MAIN);
    lv_obj_set_style_border_side(toolbar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_width(toolbar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(toolbar, lv_color_hex(0x2C3E50), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(toolbar, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(toolbar, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Toolbar Button 1: Priorität (links) */
    s_btn_filter_prio = lv_button_create(toolbar);
    lv_obj_set_size(s_btn_filter_prio, 108, 30);
    lv_obj_set_style_bg_color(s_btn_filter_prio, lv_color_hex(0x2A3036), LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_filter_prio, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_btn_filter_prio, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_filter_prio, _on_prio_filter_clicked, LV_EVENT_CLICKED, NULL);

    s_lbl_filter_prio = lv_label_create(s_btn_filter_prio);
    lv_label_set_text(s_lbl_filter_prio, gui_i18n_get("alarm.filter_all"));
    lv_obj_set_style_text_font(s_lbl_filter_prio, &ui_font_font0, LV_PART_MAIN);
    lv_obj_center(s_lbl_filter_prio);

    /* Toolbar Button 2: Zeit-Sortierung (mitte-links) */
    s_btn_sort_time = lv_button_create(toolbar);
    lv_obj_set_size(s_btn_sort_time, 108, 30);
    lv_obj_set_style_bg_color(s_btn_sort_time, lv_color_hex(0x2A3036), LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_sort_time, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_btn_sort_time, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_sort_time, _on_time_sort_clicked, LV_EVENT_CLICKED, NULL);

    s_lbl_sort_time = lv_label_create(s_btn_sort_time);
    lv_label_set_text(s_lbl_sort_time, gui_i18n_get("alarm.hdr_time"));
    lv_obj_set_style_text_font(s_lbl_sort_time, &ui_font_font0, LV_PART_MAIN);
    lv_obj_center(s_lbl_sort_time);

    /* Toolbar Button 3: Reset Filter (mitte-rechts) */
    s_btn_reset = lv_button_create(toolbar);
    lv_obj_set_size(s_btn_reset, 108, 30);
    lv_obj_set_style_bg_color(s_btn_reset, lv_color_hex(0x34495E), LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_reset, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_btn_reset, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_reset, _on_reset_clicked, LV_EVENT_CLICKED, NULL);

    s_lbl_reset = lv_label_create(s_btn_reset);
    char reset_str[32];
    snprintf(reset_str, sizeof(reset_str), "%s %s", LV_SYMBOL_REFRESH, gui_i18n_get("common.reset"));
    lv_label_set_text(s_lbl_reset, reset_str);
    lv_obj_set_style_text_font(s_lbl_reset, &ui_font_font0, LV_PART_MAIN);
    lv_obj_center(s_lbl_reset);

    /* Toolbar Button 4: Zustand (rechts) */
    s_btn_filter_state = lv_button_create(toolbar);
    lv_obj_set_size(s_btn_filter_state, 108, 30);
    lv_obj_set_style_bg_color(s_btn_filter_state, lv_color_hex(0x2A3036), LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_filter_state, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_btn_filter_state, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_filter_state, _on_state_filter_clicked, LV_EVENT_CLICKED, NULL);

    s_lbl_filter_state = lv_label_create(s_btn_filter_state);
    lv_label_set_text(s_lbl_filter_state, gui_i18n_get("alarm.filter_all"));
    lv_obj_set_style_text_font(s_lbl_filter_state, &ui_font_font0, LV_PART_MAIN);
    lv_obj_center(s_lbl_filter_state);

    /* 4. Scrollable List Container (y=92..480, 388px height) */
    s_list_alarms = lv_obj_create(s_alarm_modal);
    lv_obj_set_size(s_list_alarms, 480, 388);
    lv_obj_set_pos(s_list_alarms, 0, 92);
    lv_obj_set_style_radius(s_list_alarms, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_list_alarms, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list_alarms, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list_alarms, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list_alarms, 6, LV_PART_MAIN);
    lv_obj_add_flag(s_list_alarms, LV_OBJ_FLAG_SCROLLABLE);

    /* Enforce vertical column layout so items do not stack at (0, 0) */
    lv_obj_set_flex_flow(s_list_alarms, LV_FLEX_FLOW_COLUMN);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void alarm_view_show(void) {
    if (!s_alarm_modal) {
        _create_alarm_modal();
    }
    if (s_alarm_modal) {
        s_current_sort_mode = ALARM_SORT_BY_TIME_DESC;
        s_current_filter = ALARM_FILTER_ALL;
        s_current_state_filter = 0;

        lv_obj_remove_flag(s_alarm_modal, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_alarm_modal);
        alarm_view_update_list();
    }
}

void alarm_view_hide(void) {
    if (s_alarm_modal) {
        lv_obj_add_flag(s_alarm_modal, LV_OBJ_FLAG_HIDDEN);
        
        /* Free all list widgets back to the heap when screen is closed */
        if (s_list_alarms) {
            lv_obj_clean(s_list_alarms);
        }
    }
}

void alarm_view_acknowledge_all(void) {
    size_t count = 0;
    alarm_entry_t *alarms = alarm_get_ringbuffer(&count);
    if (alarms) {
        for (size_t i = 0; i < count; i++) {
            if (alarms[i].state == ALARM_STATE_INCOMING || alarms[i].state == ALARM_STATE_RETRIGGERED) {
                alarm_acknowledge(&alarms[i]);
            }
        }
        free(alarms);
    }
    alarm_buzzer_stop();
    ESP_LOGI(TAG, "All alarms acknowledged");
}

void alarm_view_mute_time(void) {
    const alarm_ui_config_t *cfg = alarm_get_ui_config();
    uint32_t dur = (cfg && cfg->mute_duration_sec > 0) ? cfg->mute_duration_sec : 7200;
    alarm_mute(dur);
    alarm_buzzer_stop();
}

void alarm_view_handle_hw_button(void) {
    const alarm_ui_config_t *cfg = alarm_get_ui_config();
    if (cfg && strcmp(cfg->hw_button_behavior, "ACK_ALL") == 0) {
        alarm_view_acknowledge_all();
    } else {
        alarm_view_mute_time();
    }

    lv_port_sem_take();
    if (s_alarm_modal && !lv_obj_has_flag(s_alarm_modal, LV_OBJ_FLAG_HIDDEN)) {
        alarm_view_update_list();
    }
    lv_port_sem_give();
}

int alarm_view_init(void) {
    lv_port_sem_take();
    _create_alarm_modal();
    lv_port_sem_give();
    ESP_LOGI(TAG, "Modal Alarm View initialized on lv_layer_top");
    return 0;
}

void alarm_view_notify_change(void) {
    lv_port_sem_take();
    if (s_alarm_modal && !lv_obj_has_flag(s_alarm_modal, LV_OBJ_FLAG_HIDDEN)) {
        alarm_view_update_list();
    }
    lv_port_sem_give();
}