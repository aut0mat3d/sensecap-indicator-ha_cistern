/**
 * @file horiz_bargraph.h
 * @brief Horizontal multi-zone bar/line graph module for historical time-series data.
 *        Supports dynamic grid scaling, multi-zone segmentation (1-4 zones),
 *        and LVGL 9 draw-task interception for data quality rendering.
 *
 * ============================================================================
 * 10x8 SEAMLESS TILE GRID SPECIFICATION
 * ============================================================================
 * Screen Dimensions:      480 x 480 pixels
 * Top Status Header:      Y = 0 .. 64    (Height: 64 px)
 * Bottom Status Footer:   Y = 448 .. 480 (Height: 32 px)
 * Usable Content Area:    X = 0 .. 480, Y = 64 .. 448 (Width: 480 px, Height: 384 px)
 *
 * Grid Structure:         10 Columns x 8 Rows
 * Cell Dimensions:        48 x 48 pixels per cell
 *
 * Coordinate Mapping Formulas:
 *   X_pos  = col * 48
 *   Y_pos  = 64 + (row * 48)
 *   Width  = col_span * 48
 *   Height = row_span * 48
 *
 * Widget Grid Constraints for Horizontal Bar Graph:
 *   Min Dimensions:       5 cols x 2 rows  (240 x 96 px)
 *   Max Dimensions:       10 cols x 8 rows (480 x 384 px - Full Content Area)
 *   Standard Bottom Tile: 10 cols x 3 rows (480 x 144 px - Standard Trend Card)
 *   Compact Split Tile:   5 cols x 3 rows  (240 x 144 px)
 *
 * Boundary Design Rule:
 *   Widgets snap directly to grid cell boundaries. External outlines are strictly
 *   prohibited to eliminate framebuffer underruns and horizontal shifting on RGB panels.
 *   Visual separation and alarm highlights are implemented exclusively via internal borders.
 * ============================================================================
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef HORIZ_BARGRAPH_H
#define HORIZ_BARGRAPH_H

#include "lvgl.h"
#include "widget_base.h"
#include "rp2040.h"
#include "gui_theme.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Grid Geometry Definitions ─────────────────────────────────────────── */
#define GRID_CELL_SIZE                      48
#define GRID_CONTENT_Y_OFFSET               64
#define GRID_CONTENT_MAX_W                  480
#define GRID_CONTENT_MAX_H                  384

#define GRID_COORD_X(col)                   ((int16_t)((col) * GRID_CELL_SIZE))
#define GRID_COORD_Y(row)                   ((int16_t)(GRID_CONTENT_Y_OFFSET + ((row) * GRID_CELL_SIZE)))
#define GRID_SPAN_W(col_span)               ((uint16_t)((col_span) * GRID_CELL_SIZE))
#define GRID_SPAN_H(row_span)               ((uint16_t)((row_span) * GRID_CELL_SIZE))

/* Raster Constraint Constants */
#define HORIZ_BARGRAPH_MIN_COLS             5
#define HORIZ_BARGRAPH_MIN_ROWS             2
#define HORIZ_BARGRAPH_MAX_COLS             10
#define HORIZ_BARGRAPH_MAX_ROWS             8

/* Default Grid Fallback: 10 cols x 3 rows (480 x 144 px) */
#define HORIZ_BARGRAPH_DEFAULT_W            GRID_SPAN_W(10)
#define HORIZ_BARGRAPH_DEFAULT_H            GRID_SPAN_H(3)

/**
 * @brief Maximum number of historical data points across all zones
 */
#define HORIZ_BARGRAPH_MAX_POINTS           40

/**
 * @brief Maximum number of configurable time zones (1 to 4)
 */
#define HORIZ_BARGRAPH_MAX_ZONES            4

/**
 * @brief Maximum number of division labels for the time axis (matches MAX_ZONES)
 */
#define HORIZ_BARGRAPH_MAX_LABELS           4

/**
 * @brief Chart rendering type for the horizontal graph
 */
typedef enum {
    HORIZ_BARGRAPH_TYPE_BAR = 0,    ///< Standard vertical bar chart
    HORIZ_BARGRAPH_TYPE_LINE,       ///< Continuous line chart
    HORIZ_BARGRAPH_TYPE_POINT       ///< Scatter/point chart without connecting lines
} horiz_bargraph_type_t;

/**
 * @brief Configuration for a single time zone within the multi-zone chart
 */
typedef struct {
    uint8_t count;          ///< Number of bars allocated to this zone
    char    title[16];      ///< Zone header text (e.g. "7 Tage <", "24 Std <", "1 Std <")
} horiz_bargraph_zone_t;

/**
 * @brief Main configuration and runtime state structure for horizontal bar graph module
 */
typedef struct {
    lv_obj_t* parent;           ///< Parent LVGL screen or container
    int16_t x;                  ///< X position in pixels (snapped to GRID_COORD_X)
    int16_t y;                  ///< Y position in pixels (snapped to GRID_COORD_Y)
    uint16_t width;             ///< Width in pixels (multiple of GRID_CELL_SIZE)
    uint16_t height;            ///< Height in pixels (multiple of GRID_CELL_SIZE)

    const char* title;          ///< Title text (optional, displayed top-left in single mode)
    char unit[16];              ///< Engineering unit string (e.g. "m³", "cm")
    horiz_bargraph_type_t chart_type; ///< Type of chart rendering

    /* Data storage: cached scalar arrays */
    float data[HORIZ_BARGRAPH_MAX_POINTS];           ///< Array of numeric values
    bool  is_estimated[HORIZ_BARGRAPH_MAX_POINTS];   ///< Quality flag: true if interpolated/fallback
    gui_alarm_level_t alarm_level[HORIZ_BARGRAPH_MAX_POINTS]; ///< Alarm level of the point (bar color)
    bool  has_data[HORIZ_BARGRAPH_MAX_POINTS];       ///< Presence flag: false if slot is an empty gap
    uint8_t data_count;                              ///< Current number of valid data slots
    float min_value;                                 ///< Minimum Y-axis scale value
    float max_value;                                 ///< Maximum Y-axis scale value

    /* Legacy / Single-zone axis labels */
    const char* axis_labels[HORIZ_BARGRAPH_MAX_LABELS]; ///< X-axis division label strings
    uint8_t axis_label_count;                           ///< Number of active axis labels

    /* Multi-Zone Configuration */
    horiz_bargraph_zone_t zones[HORIZ_BARGRAPH_MAX_ZONES]; ///< Configured time zones
    uint8_t zone_count;                                    ///< Number of active zones (1 to 4)

    const lv_font_t* font;      ///< Font for axis labels and title
    lv_color_t bg_color;        ///< Background card color
    lv_color_t bar_color;       ///< Default bar/line color (media color)
    lv_color_t text_color;      ///< Axis label text color
    lv_color_t alarm_color;     ///< Optional override of the danger color (empty = theme, gui_theme.h)

    lv_event_cb_t click_cb;     ///< Click event callback for subpage navigation
    void* user_data;            ///< Custom user context passed to click callback

    /* Internal LVGL widget references */
    lv_obj_t* container;        ///< Root card container
    lv_obj_t* lbl_title;        ///< Title label object
    lv_obj_t* chart;            ///< LVGL chart widget
    lv_chart_series_t* series;  ///< Primary data series handle
    lv_obj_t* axis_labels_obj[HORIZ_BARGRAPH_MAX_LABELS]; ///< Header labels for zones or axis divisions
    lv_obj_t* zone_dividers[HORIZ_BARGRAPH_MAX_ZONES - 1]; ///< Vertical divider lines between zones
} horiz_bargraph_t;

/**
 * @brief Factory function for dynamic instantiation via Page Manager.
 *
 * @param parent Parent LVGL screen or container.
 * @param id Unique widget identifier from JSON.
 * @param binding_key Channel key from JSON (e.g. "05975dc9.EM500-SWL.depth").
 * @param x Pixel X coordinate.
 * @param y Pixel Y coordinate.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @return Generic widget_t pointer, or NULL on allocation failure.
 */
widget_t* horiz_bargraph_create(lv_obj_t *parent, const char *id, const char *binding_key,
                                int16_t x, int16_t y, uint16_t w, uint16_t h);

/**
 * @brief Initialize and render a horizontal bar graph widget.
 *
 * @param graph Pointer to pre-allocated horiz_bargraph_t structure.
 * @return 0 on success, -1 on failure.
 */
int horiz_bargraph_init(horiz_bargraph_t* graph);

/**
 * @brief Clear all data points across all configured zones.
 *
 * @param graph Pointer to widget instance.
 */
void horiz_bargraph_clear_all(horiz_bargraph_t *graph);

/**
 * @brief Update historical data points from a simple scalar array.
 *
 * @param graph Pointer to widget instance.
 * @param new_data Array of numeric float values.
 * @param count Number of elements in new_data.
 */
void horiz_bargraph_update_data(horiz_bargraph_t *graph, const float *new_data, uint8_t count);

/**
 * @brief Update historical data series with quality and alarm attributes.
 *
 * @param graph Pointer to widget instance.
 * @param values Array of numeric float values.
 * @param is_estimated Array of boolean flags (true if point is an estimated fallback).
 * @param alarm_level Array of alarm levels (NULL = no alarm).
 * @param count Number of points in the update.
 */
void horiz_bargraph_update_series(horiz_bargraph_t *graph, const float *values,
                                  const bool *is_estimated, const gui_alarm_level_t *alarm_level, uint8_t count);

/**
 * @brief Update a single data point by index.
 *
 * @param graph Pointer to widget instance.
 * @param index Target zero-based point index.
 * @param value New numeric value.
 * @param is_estimated True if point is an estimated fallback.
 * @param alarm_level Alarm level of the point.
 */
void horiz_bargraph_update_point(horiz_bargraph_t *graph, uint8_t index, float value, bool is_estimated,
                                 gui_alarm_level_t alarm_level);

/**
 * @brief Configure time/division axis labels for single-zone / legacy mode.
 *
 * @param graph Pointer to widget instance.
 * @param labels Array of label strings (e.g. {"-8t", "-32h", "-1h", "Jetzt"}).
 * @param count Number of label strings (max 4).
 */
void horiz_bargraph_set_axis_labels(horiz_bargraph_t *graph, const char **labels, uint8_t count);

/**
 * @brief Configure 1 to 4 distinct time zones across the horizontal chart width.
 *
 * @param graph Pointer to widget instance.
 * @param zones Array of zone configurations (sum of counts <= 40).
 * @param zone_count Number of active zones (1 to 4).
 */
void horiz_bargraph_set_zones(horiz_bargraph_t *graph, const horiz_bargraph_zone_t *zones, uint8_t zone_count);

/**
 * @brief Populate data points belonging to a specific zone.
 *
 * @param graph Pointer to widget instance.
 * @param zone_idx Zero-based zone index (0 to zone_count - 1).
 * @param points Array of chart_data_point_t received from RP2040 storage engine.
 * @param count Number of points in this update batch.
 */
void horiz_bargraph_update_zone_data(horiz_bargraph_t *graph, uint8_t zone_idx,
                                     const chart_data_point_t *points, uint16_t count);

/**
 * @brief Set Y-axis scale range dynamically.
 *
 * @param graph Pointer to widget instance.
 * @param min_val Minimum scale value.
 * @param max_val Maximum scale value.
 */
void horiz_bargraph_set_range(horiz_bargraph_t *graph, float min_val, float max_val);

/**
 * @brief Set chart rendering type (bar, line, point).
 *
 * @param graph Pointer to widget instance.
 * @param chart_type Chart rendering mode.
 */
void horiz_bargraph_set_type(horiz_bargraph_t *graph, horiz_bargraph_type_t chart_type);

/**
 * @brief Set media accent color (e.g. based on media_standards.json).
 *
 * @param graph Pointer to widget instance.
 * @param color New bar/line color.
 */
void horiz_bargraph_set_media_color(horiz_bargraph_t *graph, lv_color_t color);

/**
 * @brief Destroy widget and free all associated LVGL components.
 *
 * @param graph Pointer to widget instance.
 */
void horiz_bargraph_destroy(horiz_bargraph_t* graph);

#ifdef __cplusplus
}
#endif

#endif /* HORIZ_BARGRAPH_H */