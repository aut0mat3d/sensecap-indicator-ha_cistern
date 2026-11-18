/**
 * @file keyvalue_card.h
 * @brief Flexible multi-row text and telemetry card module supporting dynamic grid scaling,
 *        word-wrapping, per-channel data bindings, and generic widget_base integration.
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
 * Widget Grid Constraints for Key-Value Card:
 *   Min Dimensions:       3 cols x 1 row   (144 x 48 px  - Compact Single-Row Tile)
 *   Max Dimensions:       10 cols x 8 rows (480 x 384 px - Full Content Area)
 *   Standard Telemetry:   6 cols x 5 rows  (288 x 240 px - Complements 4x5 Vert Graph)
 *   Wide Full Banner:     10 cols x 2 rows (480 x 96 px)
 *
 * Boundary Design Rule:
 *   Widgets snap directly to grid cell boundaries. External outlines are strictly
 *   prohibited to prevent framebuffer underruns and horizontal display shifting on RGB panels.
 *   Visual separation and alarm highlights are implemented exclusively via internal borders.
 * ============================================================================
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef KEYVALUE_CARD_H
#define KEYVALUE_CARD_H

#include "gui_theme.h"
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
#define KEYVALUE_CARD_MIN_COLS              3
#define KEYVALUE_CARD_MIN_ROWS              1
#define KEYVALUE_CARD_MAX_COLS              10
#define KEYVALUE_CARD_MAX_ROWS              8

/* Default Grid Fallback: 6 cols x 5 rows (288 x 240 px) */
#define KEYVALUE_CARD_DEFAULT_W             GRID_SPAN_W(6)
#define KEYVALUE_CARD_DEFAULT_H             GRID_SPAN_H(5)

/**
 * @brief Maximum number of label-value pairs in a single card
 */
#define keyvalue_card_MAX_PAIRS             8
#define KEYVALUE_CARD_MAX_PAIRS             keyvalue_card_MAX_PAIRS

/**
 * @brief Configuration and state descriptor for a single text value pair / item
 */
typedef struct {
    char        binding_key[32];   ///< Channel/binding key (e.g. "depth", "volume", "flowrate")
    const char* label;             ///< Label text (e.g. "Füllstand:", "Flow Rate:")
    const char* value;             ///< Initial or static value text (e.g. "3.42 m³")
    const char* unit;              ///< Optional engineering unit string (e.g. "m³", "cm")
    lv_color_t  label_color;       ///< Color for label text
    lv_color_t  value_color;       ///< Color for value text
    const lv_font_t* font;         ///< Font override for value (optional)
    gui_alarm_level_t alarm_level; ///< Alarm level of the current value (colors value text and bezel)
    bool        is_estimated;      ///< Quality flag: true if current value is interpolated/fallback
} keyvalue_card_pair_t;

/**
 * @brief Main configuration and runtime state structure for key-value card module
 */
typedef struct {
    lv_obj_t* parent;           ///< Parent LVGL screen or container
    int16_t x;                  ///< X position in pixels (snapped to GRID_COORD_X)
    int16_t y;                  ///< Y position in pixels (snapped to GRID_COORD_Y)
    uint16_t width;             ///< Width in pixels (multiple of GRID_CELL_SIZE)
    uint16_t height;            ///< Height in pixels (multiple of GRID_CELL_SIZE)

    const char* title;          ///< Title or card header text (optional)
    keyvalue_card_pair_t pairs[keyvalue_card_MAX_PAIRS];  ///< Registered label-value pairs
    uint8_t pair_count;         ///< Number of pairs configured

    const lv_font_t* font;      ///< Default font for labels and values
    lv_color_t bg_color;        ///< Background card color
    lv_color_t text_color;      ///< Default text color
    lv_color_t title_color;     ///< Title color

    lv_event_cb_t click_cb;     ///< Click event callback for subpage navigation
    void* user_data;            ///< Custom user context passed to click callback

    /* Internal LVGL widget references */
    lv_obj_t* container;        ///< Root scrollable card container
    lv_obj_t* lbl_title;        ///< Title label object
    lv_obj_t* pair_conts[keyvalue_card_MAX_PAIRS]; ///< Row containers for each pair
    lv_obj_t* labels[keyvalue_card_MAX_PAIRS];     ///< Label text objects
    lv_obj_t* values[keyvalue_card_MAX_PAIRS];     ///< Value text objects
} keyvalue_card_t;

/**
 * @brief Factory function for dynamic instantiation via Page Manager.
 *
 * @param parent Parent LVGL screen or container.
 * @param id Unique widget ID from JSON.
 * @param binding_key Optional default binding key.
 * @param x Pixel X coordinate.
 * @param y Pixel Y coordinate.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @return Generic widget_t pointer, or NULL on allocation failure.
 */
widget_t* keyvalue_card_create(lv_obj_t *parent, const char *id, const char *binding_key,
                               int16_t x, int16_t y, uint16_t w, uint16_t h);

/**
 * @brief Initialize and render a key-value card widget.
 *
 * @param widget Pointer to pre-allocated keyvalue_card_t structure.
 * @return 0 on success, -1 on failure.
 */
int keyvalue_card_init(keyvalue_card_t* widget);

/**
 * @brief Add a label-value pair dynamically to the widget.
 *
 * @param widget Pointer to widget instance.
 * @param binding_key Key matching live telemetry (e.g. "volume", "flowrate").
 * @param label Human-readable label (e.g. "Füllstand:").
 * @param initial_value Starting value string (e.g. "--").
 * @param unit Unit string (optional).
 * @param value_color Accent color for the value text.
 * @return 0 on success, -1 if maximum capacity reached.
 */
int keyvalue_card_add_pair(keyvalue_card_t *widget, const char *binding_key, const char *label,
                           const char *initial_value, const char *unit, lv_color_t value_color);

/**
 * @brief Update a value by zero-based pair index.
 *
 * @param widget Pointer to widget instance.
 * @param pair_index Zero-based index of the pair to update.
 * @param value New value string.
 */
void keyvalue_card_update_value(keyvalue_card_t* widget, uint8_t pair_index, const char* value);

/**
 * @brief Update telemetry value across all pairs matching a specific binding key.
 *
 * @param widget Pointer to widget instance.
 * @param key Target telemetry key (e.g. "depth", "volume").
 * @param value New numeric float value.
 * @param unit Unit string (optional, NULL to use pair default).
 * @param is_estimated True if value is an interpolated fallback.
 * @param alarm_level Alarm level of the value (GUI_ALARM_NONE / WARNING / DANGER).
 */
void keyvalue_card_update_binding(keyvalue_card_t *widget, const char *key, float value,
                                  const char *unit, bool is_estimated, gui_alarm_level_t alarm_level);

/**
 * @brief Update label text by zero-based pair index.
 *
 * @param widget Pointer to widget instance.
 * @param pair_index Zero-based index of the pair to update.
 * @param label New label text.
 */
void keyvalue_card_update_label(keyvalue_card_t* widget, uint8_t pair_index, const char* label);

/**
 * @brief Set or update title for the card.
 *
 * @param widget Pointer to widget instance.
 * @param title New title text (NULL to hide).
 */
void keyvalue_card_set_title(keyvalue_card_t* widget, const char* title);

/**
 * @brief Set value color for a specific pair.
 *
 * @param widget Pointer to widget instance.
 * @param pair_index Zero-based index of the pair to update.
 * @param color New color for value text.
 */
void keyvalue_card_set_value_color(keyvalue_card_t* widget, uint8_t pair_index, lv_color_t color);

/**
 * @brief Set label color for a specific pair.
 *
 * @param widget Pointer to widget instance.
 * @param pair_index Zero-based index of the pair to update.
 * @param color New color for label text.
 */
void keyvalue_card_set_label_color(keyvalue_card_t* widget, uint8_t pair_index, lv_color_t color);

/**
 * @brief Destroy widget and free all associated LVGL components.
 *
 * @param widget Pointer to widget instance.
 */
void keyvalue_card_destroy(keyvalue_card_t* widget);

#ifdef __cplusplus
}
#endif

#endif /* KEYVALUE_CARD_H */