/**
 * @file battery_icon.c
 * @brief Standing battery indicator bound to one data point (see battery_icon.h).
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#include "battery_icon.h"
#include "lv_port.h"
#include "view_data.h"
#include "rp2040.h"
#include "lora_model.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>
#include <time.h>

static const char *TAG = "battery_icon";

#define BATTERY_TIMER_PERIOD_MS       10000  /* stale check + archive request retry */
#define BATTERY_MAX_ARCHIVE_REQUESTS  60     /* retry for ~10 min until a valid archive answer */
#define BATTERY_OUTLINE_COLOR         lv_color_hex(0xD0D3D4)
#define BATTERY_BORDER_W              2
#define BATTERY_INNER_PAD             2

static bool time_is_valid(time_t now) {
    return now >= 1700000000;
}

/* ── Level resolution ──────────────────────────────────────────────────── */

/**
 * @brief Alarm level of a value: thresholds of the bound device/key (any direction, severity of
 *        the crossed threshold) or, if the key has no thresholds, the fallback bands.
 */
static gui_alarm_level_t resolve_level(const battery_icon_t *icon, float value) {
    bool has_thresholds = false;
    int severity = lora_model_crossed_severity(icon->dev_addr, icon->key, value, &has_thresholds);
    if (has_thresholds) {
        return (severity < 0) ? GUI_ALARM_NONE : gui_theme_level_from_severity((uint8_t)severity);
    }

    /* No thresholds for this key: fallback bands */
    float danger = (icon->danger_pct > 0.0f) ? icon->danger_pct : BATTERY_ICON_DEFAULT_DANGER_PCT;
    float warn = (icon->warn_pct > 0.0f) ? icon->warn_pct : BATTERY_ICON_DEFAULT_WARN_PCT;
    if (value <= danger) return GUI_ALARM_DANGER;
    if (value <= warn) return GUI_ALARM_WARNING;
    return GUI_ALARM_NONE;
}

/* ── Rendering (call with the LVGL lock held) ─────────────────────────── */

static void render(battery_icon_t *icon) {
    if (!icon->container) return;

    gui_theme_colors_t theme;
    gui_theme_get_colors(&theme);

    if (!icon->has_value) {
        lv_color_t grey = lv_color_hex(theme.alarm_info);
        lv_obj_set_style_border_color(icon->body, grey, LV_PART_MAIN);
        lv_obj_set_style_bg_color(icon->cap, grey, LV_PART_MAIN);
        lv_obj_add_flag(icon->fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(icon->lbl_unknown, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(icon->container, LV_OPA_COVER, LV_PART_MAIN);
        return;
    }

    float pct = icon->value;
    if (isnan(pct) || pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;

    lv_obj_set_style_border_color(icon->body, BATTERY_OUTLINE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon->cap, BATTERY_OUTLINE_COLOR, LV_PART_MAIN);
    lv_obj_add_flag(icon->lbl_unknown, LV_OBJ_FLAG_HIDDEN);

    /* Continuous fill from the bottom of the body's content area */
    int32_t inner_h = lv_obj_get_content_height(icon->body);
    int32_t fill_h = (int32_t)lroundf((float)inner_h * pct / 100.0f);
    if (pct > 0.0f && fill_h < 1) fill_h = 1;
    lv_obj_set_height(icon->fill, fill_h);
    lv_obj_set_style_bg_color(icon->fill, lv_color_hex(gui_theme_alarm_level_color(icon->level, theme.alarm_ok)),
                              LV_PART_MAIN);
    if (fill_h > 0) {
        lv_obj_remove_flag(icon->fill, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(icon->fill, LV_OBJ_FLAG_HIDDEN);
    }

    /* Stale value: shown, but dimmed like estimated values elsewhere */
    lv_obj_set_style_opa(icon->container, icon->is_stale ? LV_OPA_50 : LV_OPA_COVER, LV_PART_MAIN);
}

static void update_stale(battery_icon_t *icon) {
    time_t now = time(NULL);
    icon->is_stale = icon->has_value && icon->expected_interval_sec > 0 && icon->value_ts > 0 &&
                     time_is_valid(now) && ((uint32_t)now - icon->value_ts) > 2U * icon->expected_interval_sec;
}

/* Apply a new value (LVGL lock held) */
static void apply_value(battery_icon_t *icon, float value, uint32_t ts) {
    icon->has_value = true;
    icon->value = value;
    icon->value_ts = ts;
    icon->level = resolve_level(icon, value);
    update_stale(icon);
    render(icon);
}

/* ── Event handlers (view event loop task) ────────────────────────────── */

static void telemetry_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    battery_icon_t *icon = (battery_icon_t *)arg;
    if (id != VIEW_EVENT_LORA_TELEMETRY || !data || !icon) return;
    const struct view_data_lora_telemetry *t = (const struct view_data_lora_telemetry *)data;
    if (t->dev_addr != icon->dev_addr || strcmp(t->key, icon->key) != 0) return;
    if (t->context == 2) return; /* sensor-side history replay, not a current value */

    float v;
    if (t->val_type == LORA_VAL_TYPE_FLOAT) v = t->val.f_val;
    else if (t->val_type == LORA_VAL_TYPE_INT) v = (float)t->val.i_val;
    else return;

    time_t now = time(NULL);
    lv_port_sem_take();
    apply_value(icon, v, time_is_valid(now) ? (uint32_t)now : 0);
    lv_port_sem_give();
}

static void last_value_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    battery_icon_t *icon = (battery_icon_t *)arg;
    if (id != VIEW_EVENT_ARCHIVE_LAST_VALUE || !data || !icon) return;
    const struct view_data_last_value *lv = (const struct view_data_last_value *)data;
    if (lv->dev_addr != icon->dev_addr || strcmp(lv->key, icon->key) != 0 || !lv->valid) return;

    lv_port_sem_take();
    icon->archive_answered = true;
    icon->expected_interval_sec = lv->expected_interval_sec;
    /* A live value received meanwhile is newer than the archive: keep it */
    if (!icon->has_value || lv->timestamp >= icon->value_ts) {
        apply_value(icon, lv->value, lv->timestamp);
    } else {
        update_stale(icon);
        render(icon);
    }
    lv_port_sem_give();
    ESP_LOGI(TAG, "0x%08lX.%s: archived value %.1f (ts %lu, interval %lu s)", (unsigned long)icon->dev_addr,
             icon->key, (double)lv->value, (unsigned long)lv->timestamp, (unsigned long)lv->expected_interval_sec);
}

/* ── Timer (LVGL task, lock held): archive request retry + stale check ── */

static void timer_cb(lv_timer_t *timer) {
    battery_icon_t *icon = (battery_icon_t *)lv_timer_get_user_data(timer);
    if (!icon) return;

    /* The RP2040 may not have loaded the archive yet: ask again until a valid answer arrives */
    if (!icon->archive_answered && icon->archive_requests < BATTERY_MAX_ARCHIVE_REQUESTS) {
        icon->archive_requests++;
        rp2040_request_last_value(icon->dev_addr, icon->key);
    }

    bool was_stale = icon->is_stale;
    update_stale(icon);
    /* Thresholds may change in the config dialog: re-evaluate the level as well */
    gui_alarm_level_t level = icon->has_value ? resolve_level(icon, icon->value) : GUI_ALARM_NONE;
    if (was_stale != icon->is_stale || level != icon->level) {
        icon->level = level;
        render(icon);
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

int battery_icon_init(battery_icon_t *icon) {
    if (!icon || !icon->parent || icon->key[0] == '\0') return -1;

    lv_port_sem_take();

    uint16_t w = icon->width ? icon->width : (uint16_t)lv_obj_get_width(icon->parent);
    uint16_t h = icon->height ? icon->height : (uint16_t)lv_obj_get_height(icon->parent);
    if (w < 12) w = 12;
    if (h < 20) h = 20;

    /* Touch area: the whole widget */
    icon->container = lv_obj_create(icon->parent);
    lv_obj_remove_style_all(icon->container);
    lv_obj_set_size(icon->container, w, h);
    lv_obj_set_pos(icon->container, icon->x, icon->y);
    lv_obj_remove_flag(icon->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(icon->container, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon->container, lv_color_hex(0x3A4048), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(icon->container, LV_OPA_50, LV_PART_MAIN | LV_STATE_PRESSED);
    if (icon->click_cb) {
        lv_obj_add_flag(icon->container, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(icon->container, icon->click_cb, LV_EVENT_CLICKED, icon->user_data);
    } else {
        lv_obj_remove_flag(icon->container, LV_OBJ_FLAG_CLICKABLE);
    }

    /* Standing battery: body ~70 % of the height, width half the body height, cap on top */
    int32_t body_h = (h * 7) / 10;
    int32_t body_w = body_h / 2;
    if (body_w < 10) body_w = 10;
    int32_t cap_h = (body_h >= 24) ? 3 : 2;
    int32_t cap_w = body_w / 2;
    int32_t top = (h - (body_h + cap_h)) / 2;

    icon->cap = lv_obj_create(icon->container);
    lv_obj_remove_style_all(icon->cap);
    lv_obj_set_size(icon->cap, cap_w, cap_h);
    lv_obj_set_pos(icon->cap, (w - cap_w) / 2, top);
    lv_obj_set_style_bg_opa(icon->cap, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(icon->cap, 1, LV_PART_MAIN);
    lv_obj_remove_flag(icon->cap, LV_OBJ_FLAG_CLICKABLE);

    icon->body = lv_obj_create(icon->container);
    lv_obj_remove_style_all(icon->body);
    lv_obj_set_size(icon->body, body_w, body_h);
    lv_obj_set_pos(icon->body, (w - body_w) / 2, top + cap_h);
    lv_obj_set_style_border_width(icon->body, BATTERY_BORDER_W, LV_PART_MAIN);
    lv_obj_set_style_border_opa(icon->body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(icon->body, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(icon->body, BATTERY_INNER_PAD, LV_PART_MAIN);
    lv_obj_remove_flag(icon->body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(icon->body, LV_OBJ_FLAG_SCROLLABLE);

    icon->fill = lv_obj_create(icon->body);
    lv_obj_remove_style_all(icon->fill);
    lv_obj_set_width(icon->fill, lv_pct(100));
    lv_obj_set_height(icon->fill, 0);
    lv_obj_align(icon->fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(icon->fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(icon->fill, 1, LV_PART_MAIN);
    lv_obj_remove_flag(icon->fill, LV_OBJ_FLAG_CLICKABLE);

    icon->lbl_unknown = lv_label_create(icon->body);
    lv_label_set_text(icon->lbl_unknown, "?");
    lv_obj_set_style_text_font(icon->lbl_unknown, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon->lbl_unknown, lv_color_hex(0x9E9E9E), LV_PART_MAIN);
    lv_obj_center(icon->lbl_unknown);
    lv_obj_remove_flag(icon->lbl_unknown, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_update_layout(icon->container);
    icon->has_value = false;
    icon->is_stale = false;
    icon->level = GUI_ALARM_NONE;
    icon->archive_answered = false;
    icon->archive_requests = 0;
    render(icon);

    icon->timer = lv_timer_create(timer_cb, BATTERY_TIMER_PERIOD_MS, icon);
    lv_port_sem_give();

    esp_event_handler_instance_register_with(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_LORA_TELEMETRY,
                                             telemetry_handler, icon, &icon->telem_handler);
    esp_event_handler_instance_register_with(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_ARCHIVE_LAST_VALUE,
                                             last_value_handler, icon, &icon->last_value_handler);

    icon->archive_requests++;
    rp2040_request_last_value(icon->dev_addr, icon->key);
    return 0;
}

void battery_icon_set_binding(battery_icon_t *icon, uint32_t dev_addr, const char *key) {
    if (!icon || !key) return;
    lv_port_sem_take();
    icon->dev_addr = dev_addr;
    strlcpy(icon->key, key, sizeof(icon->key));
    icon->has_value = false;
    icon->value_ts = 0;
    icon->expected_interval_sec = 0;
    icon->is_stale = false;
    icon->level = GUI_ALARM_NONE;
    icon->archive_answered = false;
    icon->archive_requests = 1;
    render(icon);
    lv_port_sem_give();
    rp2040_request_last_value(dev_addr, key);
}

void battery_icon_set_visible(battery_icon_t *icon, bool visible) {
    if (!icon || !icon->container) return;
    lv_port_sem_take();
    if (visible) lv_obj_remove_flag(icon->container, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(icon->container, LV_OBJ_FLAG_HIDDEN);
    lv_port_sem_give();
}

void battery_icon_set_value(battery_icon_t *icon, float value_pct, uint32_t timestamp) {
    if (!icon || !icon->container) return;
    time_t now = time(NULL);
    if (timestamp == 0 && time_is_valid(now)) timestamp = (uint32_t)now;
    lv_port_sem_take();
    apply_value(icon, value_pct, timestamp);
    lv_port_sem_give();
}

void battery_icon_destroy(battery_icon_t *icon) {
    if (!icon) return;
    if (icon->telem_handler) {
        esp_event_handler_instance_unregister_with(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_LORA_TELEMETRY,
                                                   icon->telem_handler);
        icon->telem_handler = NULL;
    }
    if (icon->last_value_handler) {
        esp_event_handler_instance_unregister_with(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_ARCHIVE_LAST_VALUE,
                                                   icon->last_value_handler);
        icon->last_value_handler = NULL;
    }
    lv_port_sem_take();
    if (icon->timer) {
        lv_timer_delete(icon->timer);
        icon->timer = NULL;
    }
    if (icon->container) {
        lv_obj_delete(icon->container);
        icon->container = NULL;
    }
    lv_port_sem_give();
}
