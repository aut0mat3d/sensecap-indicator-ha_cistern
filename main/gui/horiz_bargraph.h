/**
 * @file horiz_bargraph.h
 * @brief Horizontal bar/line graph module for displaying historical time-series data.
 *        Configurable: position, size, colors, data points, chart type, ranges,
 *        and multi-zone time segmentation (1-4 zones).
 *        Supports LVGL 9 draw-task interception for data quality (estimated/alarm) rendering.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef HORIZ_BARGRAPH_H
#define HORIZ_BARGRAPH_H

#include "lvgl.h"
#include "widget_base.h"
#include "rp2040.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum number of historical data points across all zones
 */
#define HORIZ_BARGRAPH_MAX_POINTS 40

/**
 * @brief Maximum number of configurable time zones (1 to 4)
 */
#define HORIZ_BARGRAPH_MAX_ZONES  4

/**
 * @brief Maximum number of division labels for the time axis (matches MAX_ZONES)
 */
#define HORIZ_BARGRAPH_MAX_LABELS 4

/**
 * @brief Chart type for the horizontal graph
 */
typedef enum {
    HORIZ_BARGRAPH_TYPE_BAR = 0,    ///< Bar chart
    HORIZ_BARGRAPH_TYPE_LINE,       ///< Line chart
    HORIZ_BARGRAPH_TYPE_POINT       ///< Point scatter chart
} horiz_bargraph_type_t;

/**
 * @brief Configuration for a single time zone within the multi-zone chart
 */
typedef struct {
    uint8_t count;          ///< Number of bars in this zone
    char    title[16];      ///< Zone header text (e.g. "-7d", "-16h", "Jetzt")
} horiz_bargraph_zone_t;

/**
 * @brief Configuration structure for horizontal bar graph module
 */
typedef struct {
    lv_obj_t* parent;           ///< Parent LVGL object (tile or view container)
    int16_t x;                  ///< X position in pixels
    int16_t y;                  ///< Y position in pixels
    uint16_t width;             ///< Width in pixels
    uint16_t height;            ///< Height in pixels
    
    const char* title;          ///< Title text (optional, displayed top-left in single mode)
    char unit[16];              ///< Engineering unit (e.g. "m³", "cm")
    horiz_bargraph_type_t chart_type; ///< Type of chart to display
    
    /* Data storage: raw float arrays for backward compatibility and fast access */
    float data[HORIZ_BARGRAPH_MAX_POINTS];           ///< Array of data values
    bool  is_estimated[HORIZ_BARGRAPH_MAX_POINTS];   ///< Per-point quality flag: true if estimated
    bool  is_alarm[HORIZ_BARGRAPH_MAX_POINTS];       ///< Per-point alarm flag: true if threshold violation
    bool  has_data[HORIZ_BARGRAPH_MAX_POINTS];       ///< Per-point presence flag: false if empty slot
    uint8_t data_count;                              ///< Current number of valid data points
    float min_value;                                 ///< Minimum value for Y-axis
    float max_value;                                 ///< Maximum value for Y-axis
    
    /* Legacy / Single-zone axis labels */
    const char* axis_labels[HORIZ_BARGRAPH_MAX_LABELS]; ///< Labels for X-axis divisions
    uint8_t axis_label_count;                           ///< Number of axis labels
    
    /* Multi-Zone Configuration */
    horiz_bargraph_zone_t zones[HORIZ_BARGRAPH_MAX_ZONES]; ///< Multi-zone configuration
    uint8_t zone_count;                                    ///< Number of active zones
    
    const lv_font_t* font;      ///< Font for axis text and title
    lv_color_t bg_color;        ///< Background card color
    lv_color_t bar_color;       ///< Default bar/line color (media color)
    lv_color_t text_color;      ///< Axis label text color
    lv_color_t alarm_color;     ///< Alarm bar highlight color
    
    lv_event_cb_t click_cb;     ///< Click callback for configuration subpage
    void* user_data;            ///< User data passed to click callback
    
    /* Internal LVGL state */
    lv_obj_t* container;        ///< Container card object
    lv_obj_t* lbl_title;        ///< Title label object (optional)
    lv_obj_t* chart;            ///< LVGL chart widget
    lv_chart_series_t* series;  ///< Primary data series
    lv_obj_t* axis_labels_obj[HORIZ_BARGRAPH_MAX_LABELS]; ///< Label objects for zones or axis divisions
    lv_obj_t* zone_dividers[HORIZ_BARGRAPH_MAX_ZONES - 1]; ///< Vertical divider lines between zones
} horiz_bargraph_t;

/**
 * @brief Factory function for dynamic instantiation via Page Manager.
 * @param parent Parent LVGL object.
 * @param id Unique widget ID from JSON.
 * @param binding_key Channel key from JSON (e.g. "05975dc9.EM500-SWL.depth").
 * @param x Pixel X coordinate.
 * @param y Pixel Y coordinate.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @return Generic widget_t pointer, or NULL on failure.
 */
widget_t* horiz_bargraph_create(lv_obj_t *parent, const char *id, const char *binding_key,
                                int16_t x, int16_t y, uint16_t w, uint16_t h);

/**
 * @brief Initialize a pre-allocated horizontal bar graph module.
 * @param graph Pointer to graph configuration structure.
 * @return 0 on success, -1 on error.
 */
int horiz_bargraph_init(horiz_bargraph_t* graph);

void horiz_bargraph_clear_all(horiz_bargraph_t *graph);

/**
 * @brief Update historical data points (simple scalar array).
 * @param graph Pointer to graph configuration structure.
 * @param new_data Array of numeric float values.
 * @param count Number of new data points.
 */
void horiz_bargraph_update_data(horiz_bargraph_t *graph, const float *new_data, uint8_t count);

/**
 * @brief Update historical data series with quality flags (from LittleFS query).
 * @param graph Pointer to graph configuration structure.
 * @param values Array of float values.
 * @param is_estimated Array of boolean flags (true if point was interpolated/estimated).
 * @param is_alarm Array of boolean flags (true if point exceeded alarm threshold).
 * @param count Number of points.
 */
void horiz_bargraph_update_series(horiz_bargraph_t *graph, const float *values, 
                                  const bool *is_estimated, const bool *is_alarm, uint8_t count);

/**
 * @brief Update a single data point in the horizontal graph.
 * @param graph Pointer to graph configuration structure.
 * @param index Index of the data point to update.
 * @param value New value.
 * @param is_estimated True if this point is an estimated replacement value.
 * @param is_alarm True if this point violates an active threshold.
 */
void horiz_bargraph_update_point(horiz_bargraph_t *graph, uint8_t index, float value, bool is_estimated, bool is_alarm);

/**
 * @brief Configure time/division axis labels (single-zone/legacy mode).
 * @param graph Pointer to graph configuration structure.
 * @param labels Array of label strings (e.g. {"-8t", "-32h", "-1h", "Jetzt"}).
 * @param count Number of labels (max 4).
 */
void horiz_bargraph_set_axis_labels(horiz_bargraph_t *graph, const char **labels, uint8_t count);

/**
 * @brief Configure 1 to 4 distinct time zones across the horizontal chart width.
 * @param graph Pointer to graph configuration structure.
 * @param zones Array of zone configurations (sum of counts <= 40).
 * @param zone_count Number of zones (1 to 4).
 */
void horiz_bargraph_set_zones(horiz_bargraph_t *graph, const horiz_bargraph_zone_t *zones, uint8_t zone_count);

/**
 * @brief Update the data points belonging to a specific zone.
 * @param graph Pointer to graph configuration structure.
 * @param zone_idx Zero-based zone index (0 to zone_count - 1).
 * @param points Array of chart_data_point_t from RP2040.
 * @param count Number of points in this update.
 */
void horiz_bargraph_update_zone_data(horiz_bargraph_t *graph, uint8_t zone_idx, 
                                     const chart_data_point_t *points, uint16_t count);

/**
 * @brief Set Y-axis range scale dynamically.
 * @param graph Pointer to graph configuration structure.
 * @param min_val Minimum scale value.
 * @param max_val Maximum scale value.
 */
void horiz_bargraph_set_range(horiz_bargraph_t *graph, float min_val, float max_val);

/**
 * @brief Set chart type (bar, line, point).
 * @param graph Pointer to graph configuration structure.
 * @param chart_type Chart rendering type.
 */
void horiz_bargraph_set_type(horiz_bargraph_t *graph, horiz_bargraph_type_t chart_type);

/**
 * @brief Set media accent color (e.g. from media_standards.json).
 * @param graph Pointer to graph configuration structure.
 * @param color New bar/line color.
 */
void horiz_bargraph_set_media_color(horiz_bargraph_t *graph, lv_color_t color);

/**
 * @brief Destroy/cleanup horizontal bar graph and free LVGL objects.
 * @param graph Pointer to graph configuration structure.
 */
void horiz_bargraph_destroy(horiz_bargraph_t* graph);

#ifdef __cplusplus
}
#endif

#endif /* HORIZ_BARGRAPH_H */