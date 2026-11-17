/**
 * @file vert_graph.h
 * @brief Vertical graph/bar module for displaying real-time sensor levels,
 *        fill percentages, and dynamic alarm threshold boundaries.
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
 * Widget Grid Constraints for Vertical Level Graph:
 *   Min Dimensions:       3 cols x 4 rows  (144 x 192 px)
 *   Max Dimensions:       10 cols x 8 rows (480 x 384 px - Full Content Area)
 *   Standard Level Tile:  4 cols x 5 rows  (192 x 240 px - Standard Cistern Gauge)
 *   Tall Column Tile:     4 cols x 8 rows  (192 x 384 px - Full-Height Gauge)
 *   Compact Mini Tile:    3 cols x 5 rows  (144 x 240 px)
 *
 * Boundary Design Rule:
 *   Widgets snap directly to grid cell boundaries. External outlines are strictly
 *   prohibited to prevent framebuffer underruns and horizontal display shifting on RGB panels.
 *   Visual separation and alarm highlights are implemented exclusively via internal borders.
 * ============================================================================
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
#define VERT_GRAPH_MIN_COLS                 3
#define VERT_GRAPH_MIN_ROWS                 4
#define VERT_GRAPH_MAX_COLS                 10
#define VERT_GRAPH_MAX_ROWS                 8

/* Default Grid Fallback: 4 cols x 5 rows (192 x 240 px) */
#define VERT_GRAPH_DEFAULT_W                GRID_SPAN_W(4)
#define VERT_GRAPH_DEFAULT_H                GRID_SPAN_H(5)

/**
 * @brief Configuration and state descriptor for vertical graph module
 */
typedef struct {
    lv_obj_t* parent;           ///< Parent LVGL screen or container
    int16_t x;                  ///< X position in pixels (snapped to GRID_COORD_X)
    int16_t y;                  ///< Y position in pixels (snapped to GRID_COORD_Y)
    uint16_t width;             ///< Width in pixels (multiple of GRID_CELL_SIZE)
    uint16_t height;            ///< Height in pixels (multiple of GRID_CELL_SIZE)

    const char* title;          ///< Title or unit fallback text (optional)
    char unit[16];              ///< Engineering unit string (e.g. "m³", "cm")
    float min_value;            ///< Minimum value for scale
    float max_value;            ///< Maximum value for scale
    float current_value;        ///< Current numeric value displayed

    const lv_font_t* font;      ///< Font for text labels
    lv_color_t bg_color;        ///< Background card color
    lv_color_t bar_color;       ///< Bar indicator color (media color)
    lv_color_t text_color;      ///< Value text color
    lv_color_t alarm_color;     ///< Alarm level marker and danger color

    float alarm_level_min;      ///< Minimum alarm level (0.0f = disabled)
    float alarm_level_max;      ///< Maximum alarm level (0.0f = disabled)
    float hysteresis_min;       ///< Hysteresis for min alarm
    float hysteresis_max;       ///< Hysteresis for max alarm

    bool is_estimated;          ///< Quality flag: true if replacement/fallback value
    bool is_alarm;              ///< Quality flag: true if active threshold violation

    lv_event_cb_t click_cb;     ///< Click event callback for configuration subpage
    void* user_data;            ///< User data passed to click callback

    /* Internal LVGL widget references */
    lv_obj_t* container;        ///< Root card container
    lv_obj_t* bar;              ///< Vertical LVGL bar widget
    lv_obj_t* lbl_percent;      ///< Percentage and current value label (centered)
    lv_obj_t* lbl_max;          ///< Scale maximum label (top)
    lv_obj_t* lbl_min;          ///< Scale minimum label (bottom)
    lv_obj_t* alarm_marker_min; ///< Visual min threshold marker line
    lv_obj_t* alarm_marker_max; ///< Visual max threshold marker line
    uint16_t bar_height;        ///< Cached dynamic bar height
} vert_graph_t;

/**
 * @brief Factory function for dynamic instantiation via Page Manager.
 *
 * @param parent Parent LVGL screen or container.
 * @param id Unique widget ID from JSON.
 * @param binding_key Data binding key (e.g. "depth", "volume").
 * @param x Pixel X coordinate.
 * @param y Pixel Y coordinate.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @return Generic widget_t pointer, or NULL on allocation failure.
 */
widget_t* vert_graph_create(lv_obj_t *parent, const char *id, const char *binding_key,
                            int16_t x, int16_t y, uint16_t w, uint16_t h);

/**
 * @brief Initialize and render a vertical graph widget.
 *
 * @param graph Pointer to pre-allocated vert_graph_t structure.
 * @return 0 on success, -1 on failure.
 */
int vert_graph_init(vert_graph_t* graph);

/**
 * @brief Standard update function (scalar float value).
 *
 * @param graph Pointer to widget instance.
 * @param value New value to display.
 */
void vert_graph_update(vert_graph_t* graph, float value);

/**
 * @brief Extended update function with unit and data quality attributes.
 *
 * @param graph Pointer to widget instance.
 * @param value New numeric value.
 * @param unit Unit string (optional, NULL to keep existing).
 * @param is_estimated True if value is an interpolated replacement value.
 * @param is_alarm True if value violates an active alarm threshold.
 */
void vert_graph_update_value(vert_graph_t *graph, float value, const char *unit, bool is_estimated, bool is_alarm);

/**
 * @brief Update visual quality status without changing current numeric value.
 *
 * @param graph Pointer to widget instance.
 * @param is_estimated Replacement value flag.
 * @param is_alarm Active threshold violation flag.
 */
void vert_graph_set_status(vert_graph_t *graph, bool is_estimated, bool is_alarm);

/**
 * @brief Dynamically set scale range and refresh labels.
 *
 * @param graph Pointer to widget instance.
 * @param min_val New minimum value.
 * @param max_val New maximum value.
 */
void vert_graph_set_range(vert_graph_t *graph, float min_val, float max_val);

/**
 * @brief Set media accent color (e.g. from media_standards.json).
 *
 * @param graph Pointer to widget instance.
 * @param color New media color.
 */
void vert_graph_set_media_color(vert_graph_t *graph, lv_color_t color);

/**
 * @brief Set alarm threshold levels and refresh marker visibility.
 *
 * @param graph Pointer to widget instance.
 * @param min_alarm Minimum alarm level (<= min_value to disable).
 * @param max_alarm Maximum alarm level (<= min_value to disable).
 * @param hysteresis_min Hysteresis for min alarm.
 * @param hysteresis_max Hysteresis for max alarm.
 */
void vert_graph_set_alarm_levels(vert_graph_t* graph, float min_alarm, float max_alarm, float hysteresis_min, float hysteresis_max);

/**
 * @brief Destroy widget and free all associated LVGL components.
 *
 * @param graph Pointer to widget instance.
 */
void vert_graph_destroy(vert_graph_t* graph);

#ifdef __cplusplus
}
#endif

#endif /* VERT_GRAPH_H */