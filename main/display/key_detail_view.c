/**
 * @file key_detail_view.c
 * @brief Full-screen detail page for one data point (see key_detail_view.h).
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#include "key_detail_view.h"
#include "trend_graph.h"
#include "collapsible_list.h" /* GRID_* layout macros */
#include "gui_theme.h"
#include "gui/i18n/gui_i18n.h"
#include "lv_port.h"
#include "lora_model.h"
#include "media_standards.h"
#include "rp2040.h"
#include "view_data.h"
#include "esp_attr.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(ui_font_font0);

static const char *TAG = "key_detail";

#define DETAIL_BG_COLOR      lv_color_hex(0x101418)
#define DETAIL_BAR_BG_COLOR  lv_color_hex(0x161B20)
#define DETAIL_LABEL_COLOR   lv_color_hex(0x9E9E9E)
#define DETAIL_VALUE_COLOR   lv_color_hex(0xFFFFFF)
#define DETAIL_ROW_COUNT     5
#define DETAIL_TIMER_MS      10000

enum { ROW_VALUE = 0, ROW_UPDATED, ROW_STATUS, ROW_INTERVAL, ROW_DEVICE };

typedef struct {
    bool          open;
    uint32_t      dev_addr;
    char          key[24];               ///< Key of the info rows (single key page)
    char          unit[12];
    bool          has_value;
    float         value;
    uint32_t      value_ts;
    uint32_t      expected_interval_sec;
    bool          has_limits;
    lv_obj_t     *root;
    lv_obj_t     *lbl_values[DETAIL_ROW_COUNT];
    lv_timer_t   *timer;
    uint8_t       trend_count;
    trend_graph_t trends[KEY_DETAIL_MAX_TRENDS];
    char          trend_titles[KEY_DETAIL_MAX_TRENDS][32];
    esp_event_handler_instance_t telem_handler;
    esp_event_handler_instance_t last_value_handler;
} detail_view_t;

/* PSRAM: holds the point buffers of both trend graphs (~2 KB) */
EXT_RAM_BSS_ATTR static detail_view_t s_view;

/* i18n text with the trailing ':' of label keys removed */
static void label_without_colon(const char *i18n_key, const char *fallback, char *out, size_t len) {
    const char *txt = gui_i18n_get(i18n_key);
    if (!txt || strcmp(txt, i18n_key) == 0) txt = fallback;
    strlcpy(out, txt, len);
    size_t n = strlen(out);
    if (n > 0 && out[n - 1] == ':') out[n - 1] = '\0';
}

static void format_interval(uint32_t sec, char *out, size_t len) {
    if (sec == 0) snprintf(out, len, "--");
    else if (sec % 86400 == 0) snprintf(out, len, "%lu d", (unsigned long)(sec / 86400));
    else if (sec % 3600 == 0) snprintf(out, len, "%lu h", (unsigned long)(sec / 3600));
    else snprintf(out, len, "%lu min", (unsigned long)(sec / 60));
}

/* ── Info rows (LVGL lock held) ────────────────────────────────────────── */

static void render_info(void) {
    if (!s_view.root || s_view.trend_count != 1) return;
    char buf[48];
    time_t now = time(NULL);

    if (s_view.has_value) {
        snprintf(buf, sizeof(buf), "%.1f %s", (double)s_view.value, s_view.unit);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    lv_label_set_text(s_view.lbl_values[ROW_VALUE], buf);

    if (s_view.has_value && s_view.value_ts >= 1700000000U) {
        time_t t = (time_t)s_view.value_ts;
        struct tm lt;
        localtime_r(&t, &lt);
        strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M", &lt);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    lv_label_set_text(s_view.lbl_values[ROW_UPDATED], buf);

    /* Status: no data / stale / alarm level of the value */
    gui_theme_colors_t theme;
    gui_theme_get_colors(&theme);
    const char *status;
    uint32_t color;
    bool stale = s_view.has_value && s_view.expected_interval_sec > 0 && s_view.value_ts > 0 && now >= 1700000000 &&
                 ((uint32_t)now - s_view.value_ts) > 2U * s_view.expected_interval_sec;
    if (!s_view.has_value) {
        status = gui_i18n_get("status.no_data");
        color = theme.alarm_info;
    } else if (stale) {
        status = gui_i18n_get("status.stale");
        color = theme.alarm_info;
    } else {
        int sev = lora_model_crossed_severity(s_view.dev_addr, s_view.key, s_view.value, NULL);
        if (sev == 0) {
            status = gui_i18n_get("status.danger");
            color = theme.alarm_danger;
        } else if (sev > 0) {
            status = gui_i18n_get("status.warning");
            color = theme.alarm_warning;
        } else {
            status = gui_i18n_get("status.ok");
            color = theme.alarm_ok;
        }
    }
    lv_label_set_text(s_view.lbl_values[ROW_STATUS], status);
    lv_obj_set_style_text_color(s_view.lbl_values[ROW_STATUS], lv_color_hex(color), LV_PART_MAIN);

    format_interval(s_view.expected_interval_sec, buf, sizeof(buf));
    lv_label_set_text(s_view.lbl_values[ROW_INTERVAL], buf);
}

/* ── Event handlers (view event loop task) ────────────────────────────── */

static void telemetry_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id != VIEW_EVENT_LORA_TELEMETRY || !data) return;
    const struct view_data_lora_telemetry *t = (const struct view_data_lora_telemetry *)data;
    if (!s_view.open || t->dev_addr != s_view.dev_addr || t->context == 2) return;

    /* Unit of the trend graphs (cursor / extreme values) */
    if (t->unit[0]) {
        for (uint8_t i = 0; i < s_view.trend_count; i++) {
            if (strcmp(t->key, s_view.trends[i].key) == 0) trend_graph_set_unit(&s_view.trends[i], t->unit);
        }
    }
    if (s_view.trend_count != 1 || strcmp(t->key, s_view.key) != 0) return;

    float v;
    if (t->val_type == LORA_VAL_TYPE_FLOAT) v = t->val.f_val;
    else if (t->val_type == LORA_VAL_TYPE_INT) v = (float)t->val.i_val;
    else return;

    time_t now = time(NULL);
    lv_port_sem_take();
    if (s_view.open) {
        s_view.has_value = true;
        s_view.value = v;
        s_view.value_ts = (now >= 1700000000) ? (uint32_t)now : 0;
        if (t->unit[0]) strlcpy(s_view.unit, t->unit, sizeof(s_view.unit));
        render_info();
    }
    lv_port_sem_give();
}

static void last_value_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id != VIEW_EVENT_ARCHIVE_LAST_VALUE || !data) return;
    const struct view_data_last_value *lv = (const struct view_data_last_value *)data;
    if (!s_view.open || lv->dev_addr != s_view.dev_addr || !lv->valid) return;

    if (lv->unit[0]) {
        for (uint8_t i = 0; i < s_view.trend_count; i++) {
            if (strcmp(lv->key, s_view.trends[i].key) == 0) trend_graph_set_unit(&s_view.trends[i], lv->unit);
        }
    }
    if (s_view.trend_count != 1 || strcmp(lv->key, s_view.key) != 0) return;

    lv_port_sem_take();
    if (s_view.open) {
        s_view.expected_interval_sec = lv->expected_interval_sec;
        if (lv->unit[0]) strlcpy(s_view.unit, lv->unit, sizeof(s_view.unit));
        if (!s_view.has_value || lv->timestamp >= s_view.value_ts) {
            s_view.has_value = true;
            s_view.value = lv->value;
            s_view.value_ts = lv->timestamp;
        }
        /* Percent values without configured limits: fixed 0..100 % scale */
        if (!s_view.has_limits && strcmp(s_view.unit, "%") == 0) {
            trend_graph_set_scale(&s_view.trends[0], 0.0f, 100.0f);
        }
        render_info();
    }
    lv_port_sem_give();
}

/* Periodic refresh of the status (stale detection) */
static void timer_cb(lv_timer_t *timer) {
    (void)timer;
    render_info();
}

static void back_clicked_cb(lv_event_t *e) {
    (void)e;
    key_detail_view_hide();
}

/* ── Coupling of the trend graphs (LVGL context) ──────────────────────── */

/* One graph switched the range by touch: the others follow */
static void trend_preset_changed(trend_graph_t *graph, uint8_t preset_idx, void *user_data) {
    (void)user_data;
    for (uint8_t i = 0; i < s_view.trend_count; i++) {
        if (&s_view.trends[i] != graph) trend_graph_set_preset(&s_view.trends[i], preset_idx);
    }
}

/* Cursor moved by touch: the others show their value at the same time */
static void trend_cursor_moved(trend_graph_t *graph, uint32_t timestamp, void *user_data) {
    (void)user_data;
    for (uint8_t i = 0; i < s_view.trend_count; i++) {
        if (&s_view.trends[i] != graph) trend_graph_show_cursor_at(&s_view.trends[i], timestamp);
    }
}

/* ── Trend configuration from the device config ───────────────────────── */

static void configure_trend(trend_graph_t *tg, lora_device_config_t *dev, uint8_t preset_idx, bool use_limits) {
    tg->preset_count = 3;
    strlcpy(tg->presets[0].label, gui_i18n_get("trend.24h"), sizeof(tg->presets[0].label));
    tg->presets[0].step_sec = 3600;
    tg->presets[0].point_count = 24;
    strlcpy(tg->presets[1].label, gui_i18n_get("trend.7d"), sizeof(tg->presets[1].label));
    tg->presets[1].step_sec = 86400;
    tg->presets[1].point_count = 7;
    /* 31 days: "1 month" on the coupled page, "31 days" on the single key page */
    strlcpy(tg->presets[2].label, gui_i18n_get(use_limits ? "trend.31d" : "trend.1m"), sizeof(tg->presets[2].label));
    tg->presets[2].step_sec = 86400;
    tg->presets[2].point_count = 31;
    tg->preset_idx = (preset_idx < tg->preset_count) ? preset_idx : KEY_DETAIL_PRESET_1M;

    if (!dev) return;

    /* Y scale from limits[key] (otherwise automatic, or 0..100 % once the unit is known) */
    for (uint8_t l = 0; use_limits && l < dev->limit_count; l++) {
        if (dev->limits[l].valid && strcmp(dev->limits[l].key, tg->key) == 0) {
            tg->min_val = dev->limits[l].val_min;
            tg->max_val = dev->limits[l].val_max;
            s_view.has_limits = true;
            break;
        }
    }

    /* Thresholds: one line each; the RP2040 bar coloring takes the first-reached level per
     * direction (highest falling, lowest rising threshold) */
    bool has_min = false, has_max = false;
    for (uint8_t t = 0; t < dev->threshold_count && tg->line_count < TREND_GRAPH_MAX_LINES; t++) {
        const lora_threshold_config_t *th = &dev->thresholds[t];
        if (!th->valid || strcmp(th->key, tg->key) != 0) continue;
        float limit, hyst;
        lora_model_resolve_threshold(dev, th, &limit, &hyst);
        gui_alarm_level_t level = gui_theme_level_from_severity(th->severity);
        tg->lines[tg->line_count].value = limit;
        tg->lines[tg->line_count].level = level;
        tg->line_count++;

        if (th->op == 0 && (!has_min || limit > tg->alarm_min)) {
            has_min = true;
            tg->alarm_min = limit;
            if (level == GUI_ALARM_WARNING) tg->alarm_flags |= CHART_ALARM_MIN_IS_WARNING;
            else tg->alarm_flags &= (uint8_t)~CHART_ALARM_MIN_IS_WARNING;
        } else if (th->op != 0 && (!has_max || limit < tg->alarm_max)) {
            has_max = true;
            tg->alarm_max = limit;
            if (level == GUI_ALARM_WARNING) tg->alarm_flags |= CHART_ALARM_MAX_IS_WARNING;
            else tg->alarm_flags &= (uint8_t)~CHART_ALARM_MAX_IS_WARNING;
        }
    }

    if (dev->medium[0]) tg->point_color = lv_color_hex(media_get_color(dev->medium));
}

/* ── Public API ───────────────────────────────────────────────────────── */

void key_detail_view_show_ex(const key_detail_opts_t *opts) {
    if (!opts || opts->trend_count == 0 || opts->trend_count > KEY_DETAIL_MAX_TRENDS) return;
    for (uint8_t i = 0; i < opts->trend_count; i++) {
        if (!opts->trends[i].key || !opts->trends[i].key[0]) return;
    }
    if (s_view.open) key_detail_view_hide();

    uint32_t dev_addr = opts->dev_addr;
    bool single = (opts->trend_count == 1);

    lv_port_sem_take();
    memset(&s_view, 0, sizeof(s_view));
    s_view.dev_addr = dev_addr;
    s_view.trend_count = opts->trend_count;
    strlcpy(s_view.key, opts->trends[0].key, sizeof(s_view.key));
    lora_device_config_t *dev = lora_model_get_device_by_addr(dev_addr);

    /* Opaque full screen modal (radius 0: lv_port skips rendering the page underneath) */
    s_view.root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_view.root, 480, 480);
    lv_obj_set_pos(s_view.root, 0, 0);
    lv_obj_remove_flag(s_view.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_view.root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_view.root, DETAIL_BG_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_view.root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_view.root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_view.root, 0, LV_PART_MAIN);

    /* Top bar: back button + "<device>: <key>[ / <key>]" */
    /* Same height as the global header, so the content uses the common 48 px grid below it */
    lv_obj_t *bar = lv_obj_create(s_view.root);
    lv_obj_set_size(bar, 480, GRID_CONTENT_Y_OFFSET);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, DETAIL_BAR_BG_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x2C3E50), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bar, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 10, LV_PART_MAIN);

    lv_obj_t *btn_back = lv_button_create(bar);
    lv_obj_set_size(btn_back, 100, 36);
    lv_obj_add_event_cb(btn_back, back_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    char back_txt[32];
    snprintf(back_txt, sizeof(back_txt), LV_SYMBOL_LEFT " %s", gui_i18n_get("common.back"));
    lv_label_set_text(lbl_back, back_txt);
    lv_obj_set_style_text_font(lbl_back, &ui_font_font0, LV_PART_MAIN);
    lv_obj_center(lbl_back);

    char i18n_key[48];
    for (uint8_t i = 0; i < s_view.trend_count; i++) {
        snprintf(i18n_key, sizeof(i18n_key), "telemetry.%s", opts->trends[i].key);
        label_without_colon(i18n_key, opts->trends[i].key, s_view.trend_titles[i], sizeof(s_view.trend_titles[i]));
    }
    char title[96];
    snprintf(title, sizeof(title), "%s: %s%s%s", (dev && dev->device_name[0]) ? dev->device_name : "?",
             s_view.trend_titles[0], single ? "" : " / ", single ? "" : s_view.trend_titles[1]);
    lv_obj_t *lbl_title = lv_label_create(bar);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_font(lbl_title, &ui_font_font0, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_title, DETAIL_VALUE_COLOR, LV_PART_MAIN);

    /* Trend graphs: single key 10 x 5 cells, two keys 10 x 4 cells each (content area 10 x 8) */
    uint8_t rows = single ? 5 : 4;
    for (uint8_t i = 0; i < s_view.trend_count; i++) {
        trend_graph_t *tg = &s_view.trends[i];
        tg->parent = s_view.root;
        tg->x = GRID_COORD_X(0);
        tg->y = GRID_COORD_Y(i * rows);
        tg->width = GRID_SPAN_W(10);
        tg->height = GRID_SPAN_H(rows);
        tg->dev_addr = dev_addr;
        strlcpy(tg->key, opts->trends[i].key, sizeof(tg->key));
        tg->auto_include_zero = opts->trends[i].include_zero;
        configure_trend(tg, dev, opts->preset_idx, single);
        if (!single) {
            /* Each graph names its key; the head line also shows the cursor readout */
            tg->title = s_view.trend_titles[i];
            tg->show_extremes = true;
            tg->preset_changed_cb = trend_preset_changed;
            tg->cursor_cb = trend_cursor_moved;
        }
        if (trend_graph_init(tg) != 0) {
            ESP_LOGE(TAG, "Trend graph for 0x%08lX.%s could not be created", (unsigned long)dev_addr, tg->key);
        }
    }

    /* Info rows (single key page) */
    if (single) {
        static const char *const row_keys[DETAIL_ROW_COUNT] = {
            "detail.value", "detail.updated", "detail.status", "detail.interval", "detail.device"
        };
        for (int r = 0; r < DETAIL_ROW_COUNT; r++) {
            int32_t y = GRID_COORD_Y(5) + 8 + r * 30;
            lv_obj_t *lbl = lv_label_create(s_view.root);
            lv_label_set_text(lbl, gui_i18n_get(row_keys[r]));
            lv_obj_set_style_text_font(lbl, &ui_font_font0, LV_PART_MAIN);
            lv_obj_set_style_text_color(lbl, DETAIL_LABEL_COLOR, LV_PART_MAIN);
            lv_obj_set_pos(lbl, 16, y);

            s_view.lbl_values[r] = lv_label_create(s_view.root);
            lv_obj_set_style_text_font(s_view.lbl_values[r], &ui_font_font0, LV_PART_MAIN);
            lv_obj_set_style_text_color(s_view.lbl_values[r], DETAIL_VALUE_COLOR, LV_PART_MAIN);
            lv_obj_set_pos(s_view.lbl_values[r], 230, y);
        }
        char dev_txt[48];
        snprintf(dev_txt, sizeof(dev_txt), "0x%08lX", (unsigned long)dev_addr);
        lv_label_set_text(s_view.lbl_values[ROW_DEVICE], dev_txt);
    }

    s_view.open = true;
    render_info();
    if (single) s_view.timer = lv_timer_create(timer_cb, DETAIL_TIMER_MS, NULL);
    lv_port_sem_give();

    esp_event_handler_instance_register_with(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_LORA_TELEMETRY,
                                             telemetry_handler, NULL, &s_view.telem_handler);
    esp_event_handler_instance_register_with(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_ARCHIVE_LAST_VALUE,
                                             last_value_handler, NULL, &s_view.last_value_handler);
    for (uint8_t i = 0; i < s_view.trend_count; i++) {
        rp2040_request_last_value(dev_addr, opts->trends[i].key);
    }
    ESP_LOGI(TAG, "Detail page opened for 0x%08lX.%s (%u graph(s))", (unsigned long)dev_addr, s_view.key,
             (unsigned)s_view.trend_count);
}

void key_detail_view_show(uint32_t dev_addr, const char *key) {
    if (!key) return;
    key_detail_opts_t opts = {
        .dev_addr = dev_addr,
        .trends = { { .key = key } },
        .trend_count = 1,
        .preset_idx = KEY_DETAIL_PRESET_1M,
    };
    key_detail_view_show_ex(&opts);
}

void key_detail_view_hide(void) {
    if (!s_view.open) return;

    if (s_view.telem_handler) {
        esp_event_handler_instance_unregister_with(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_LORA_TELEMETRY,
                                                   s_view.telem_handler);
        s_view.telem_handler = NULL;
    }
    if (s_view.last_value_handler) {
        esp_event_handler_instance_unregister_with(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_ARCHIVE_LAST_VALUE,
                                                   s_view.last_value_handler);
        s_view.last_value_handler = NULL;
    }

    for (uint8_t i = 0; i < s_view.trend_count; i++) {
        trend_graph_destroy(&s_view.trends[i]);
    }

    lv_port_sem_take();
    s_view.open = false;
    if (s_view.timer) {
        lv_timer_delete(s_view.timer);
        s_view.timer = NULL;
    }
    if (s_view.root) {
        /* Async: this may run inside the back button's own click event */
        lv_obj_delete_async(s_view.root);
        s_view.root = NULL;
    }
    lv_port_sem_give();
}

bool key_detail_view_is_open(void) {
    return s_view.open;
}
