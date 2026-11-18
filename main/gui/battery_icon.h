/**
 * @file battery_icon.h
 * @brief Standing battery indicator bound to one data point (dev_addr + key), drawn from LVGL
 *        primitives (no bitmaps): continuous fill level, theme colors, unknown and stale states.
 *
 * States:
 *  - unknown: no value received or archived yet -> grey empty battery with "?"
 *  - value:   fill height = value in % (0..100), color by level:
 *             thresholds of the bound device/key (direction + severity, same resolution as the
 *             alarm evaluator) or, without thresholds, the fallback bands warn_pct / danger_pct;
 *             no alarm level -> theme "ok" color
 *  - stale:   no new value for 2 x the expected interval of the key -> drawn at 50 % opacity
 *
 * The widget listens to live telemetry (VIEW_EVENT_LORA_TELEMETRY) and to the newest archived
 * value (VIEW_EVENT_ARCHIVE_LAST_VALUE, requested from the RP2040 at start and after rebinding).
 * The whole widget area is a touch target (click_cb), e.g. to open a detail page.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#ifndef BATTERY_ICON_H
#define BATTERY_ICON_H

#include "lvgl.h"
#include "gui_theme.h"
#include "esp_event.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BATTERY_ICON_DEFAULT_WARN_PCT    25.0f
#define BATTERY_ICON_DEFAULT_DANGER_PCT  10.0f

typedef struct {
    /* ── Configuration (set before battery_icon_init) ── */
    lv_obj_t   *parent;          ///< Parent object (e.g. gui_header_get_slot_container())
    int16_t     x;               ///< Position inside the parent
    int16_t     y;
    uint16_t    width;           ///< Touch area size (0 = size of the parent)
    uint16_t    height;
    uint32_t    dev_addr;        ///< Bound device
    char        key[24];         ///< Bound key, e.g. "battery_soc" (value in %)
    float       warn_pct;        ///< Fallback band without thresholds (0 = default 25 %)
    float       danger_pct;      ///< Fallback band without thresholds (0 = default 10 %)
    lv_event_cb_t click_cb;      ///< Optional click callback (whole widget area)
    void       *user_data;       ///< Passed to click_cb as event user data

    /* ── State (read only) ── */
    bool        has_value;
    float       value;
    uint32_t    value_ts;              ///< UTC of the shown value
    uint32_t    expected_interval_sec; ///< From the archive config (0 = unknown, no stale check)
    bool        is_stale;
    gui_alarm_level_t level;
    bool        archive_answered;      ///< A valid archive answer arrived (stops re-requesting)
    uint8_t     archive_requests;

    /* ── Internal ── */
    lv_obj_t   *container;
    lv_obj_t   *body;
    lv_obj_t   *cap;
    lv_obj_t   *fill;
    lv_obj_t   *lbl_unknown;
    lv_timer_t *timer;
    esp_event_handler_instance_t telem_handler;
    esp_event_handler_instance_t last_value_handler;
} battery_icon_t;

/**
 * @brief Create the widget, register its event handlers and request the newest archived value.
 * @return 0 on success, -1 on invalid configuration.
 */
int battery_icon_init(battery_icon_t *icon);

/**
 * @brief Bind to another data point; resets to the unknown state and requests the archive value.
 */
void battery_icon_set_binding(battery_icon_t *icon, uint32_t dev_addr, const char *key);

/**
 * @brief Show or hide the widget (e.g. when a page has no battery for its device).
 */
void battery_icon_set_visible(battery_icon_t *icon, bool visible);

/**
 * @brief Feed a value manually (value in %, timestamp UTC; 0 = now).
 */
void battery_icon_set_value(battery_icon_t *icon, float value_pct, uint32_t timestamp);

/**
 * @brief Unregister handlers, stop the timer and delete the LVGL objects.
 */
void battery_icon_destroy(battery_icon_t *icon);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_ICON_H */
