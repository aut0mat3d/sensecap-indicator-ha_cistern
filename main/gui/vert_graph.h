/**
 * @file vert_graph.h
 * @brief Vertical graph/bar module for displaying sensor data.
 *        Configurable: position, size, range, colors, fonts, alarm levels.
 *        Supports dynamic grid scaling, quality status rendering, and widget_base integration.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef VERT_GRAPH_H
#define VERT_GRAPH_H

#include "lvgl.h"
#include "widget_base.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration structure for vertical graph module
 */
typedef struct {
    lv_obj_t* parent;           ///< Parent LVGL object (tile or view container)
    int16_t x;                  ///< X position in pixels
    int16_t y;                  ///< Y position in pixels
    uint16_t width;            ///< Width in pixels
    uint16_t height;           ///< Height in pixels
    
    const char* title;         ///< Title or unit fallback text (optional)
    char unit[16];             ///< Unit string (e.g. "m³", "cm")
    float min_value;           ///< Minimum value for scale
    float max_value;           ///< Maximum value for scale
    float current_value;       ///< Current value displayed
    
    const lv_font_t* font;     ///< Font for text labels
    lv_color_t bg_color;       ///< Background card color
    lv_color_t bar_color;      ///< Bar indicator color (media color)
    lv_color_t text_color;     ///< Value text color
    lv_color_t alarm_color;    ///< Alarm level marker / danger color
    
    float alarm_level_min;     ///< Minimum alarm level (0.0f = disabled)
    float alarm_level_max;     ///< Maximum alarm level (0.0f = disabled)
    float hysteresis_min;      ///< Hysteresis for min alarm
    float hysteresis_max;      ///< Hysteresis for max alarm

    bool is_estimated;         ///< Quality flag: true if replacement/fallback value
    bool is_alarm;             ///< Quality flag: true if active threshold violation
    
    lv_event_cb_t click_cb;    ///< Click callback for configuration subpage
    void* user_data;           ///< User data passed to click callback
    
    /* Internal LVGL elements */
    lv_obj_t* container;       ///< Root card container
    lv_obj_t* bar;             ///< Vertical LVGL bar widget
    lv_obj_t* lbl_percent;     ///< Percentage label (centered)
    lv_obj_t* lbl_max;         ///< Scale maximum label (top)
    lv_obj_t* lbl_min;         ///< Scale minimum label (bottom)
    lv_obj_t* alarm_marker_min;   ///< Visual min threshold marker
    lv_obj_t* alarm_marker_max;   ///< Visual max threshold marker
    uint16_t bar_height;       ///< Cached dynamic bar height
} vert_graph_t;

/**
 * @brief Factory function for dynamic instantiation via Page Manager.
 * @param parent Parent LVGL object.
 * @param id Unique widget ID from JSON.
 * @param binding_key Data binding key (e.g. "depth", "volume").
 * @param x Pixel X coordinate.
 * @param y Pixel Y coordinate.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @return Generic widget_t pointer, or NULL on failure.
 */
widget_t* vert_graph_create(lv_obj_t *parent, const char *id, const char *binding_key,
                            int16_t x, int16_t y, uint16_t w, uint16_t h);

/**
 * @brief Initialize a pre-allocated vertical graph instance (backward compatibility).
 * @param graph Pointer to graph configuration structure.
 * @return 0 on success, -1 on error.
 */
int vert_graph_init(vert_graph_t* graph);

/**
 * @brief Standard update function (backward compatibility).
 * @param graph Pointer to graph configuration structure.
 * @param value New value to display.
 */
void vert_graph_update(vert_graph_t* graph, float value);

/**
 * @brief Extended update function with unit and data quality attributes.
 * @param graph Pointer to graph configuration structure.
 * @param value New numeric value.
 * @param unit Unit string (optional, NULL to keep existing).
 * @param is_estimated True if value is an interpolated replacement value.
 * @param is_alarm True if value violates an active alarm threshold.
 */
void vert_graph_update_value(vert_graph_t *graph, float value, const char *unit, bool is_estimated, bool is_alarm);

/**
 * @brief Update visual quality status without changing current value.
 * @param graph Pointer to graph configuration structure.
 * @param is_estimated Replacement value flag.
 * @param is_alarm Active threshold violation flag.
 */
void vert_graph_set_status(vert_graph_t *graph, bool is_estimated, bool is_alarm);

/**
 * @brief Dynamically set scale range and refresh labels.
 * @param graph Pointer to graph configuration structure.
 * @param min_val New minimum value.
 * @param max_val New maximum value.
 */
void vert_graph_set_range(vert_graph_t *graph, float min_val, float max_val);

/**
 * @brief Set media accent color (e.g. from media_standards.json).
 * @param graph Pointer to graph configuration structure.
 * @param color New media color.
 */
void vert_graph_set_media_color(vert_graph_t *graph, lv_color_t color);

/**
 * @brief Set alarm threshold levels and refresh marker visibility.
 * @param graph Pointer to graph configuration structure.
 * @param min_alarm Minimum alarm level (<= min_value to disable).
 * @param max_alarm Maximum alarm level (<= min_value to disable).
 * @param hysteresis_min Hysteresis for min alarm.
 * @param hysteresis_max Hysteresis for max alarm.
 */
void vert_graph_set_alarm_levels(vert_graph_t* graph, float min_alarm, float max_alarm, float hysteresis_min, float hysteresis_max);

/**
 * @brief Clean up all LVGL objects associated with the vertical graph.
 * @param graph Pointer to graph configuration structure.
 */
void vert_graph_destroy(vert_graph_t* graph);

#ifdef __cplusplus
}
#endif

#endif /* VERT_GRAPH_H */