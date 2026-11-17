/**
 * @file collapsible_list.h
 * @brief Modular accordion/tree-view list widget supporting expandable groups
 *        and editable property rows for configuration and telemetry views.
 *
 * ============================================================================
 * 10x8 SEAMLESS TILE GRID SPECIFICATION
 * ============================================================================
 * Screen Dimensions:      480 x 480 pixels
 * Top Status Header:      Y = 0 .. 64   (Height: 64 px)
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
 * Widget Grid Constraints for Collapsible List:
 *   Min Dimensions:       4 cols x 3 rows  (192 x 144 px)
 *   Max Dimensions:       10 cols x 8 rows (480 x 384 px - Full Content Area)
 *   Standard Side Tile:   6 cols x 5 rows  (288 x 240 px - Complements 4x5 Vert Graph)
 *   Modal Dialog View:    10 cols x 7 rows (480 x 336 px) or 10 cols x 8 rows (480 x 384 px)
 *
 * Boundary Design Rule:
 *   Widgets snap directly to grid cell boundaries. External outlines are strictly
 *   prohibited to prevent framebuffer shifting and clipping artifacts on RGB panels.
 *   All visual separation and alarm highlights use internal borders only.
 * ============================================================================
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef COLLAPSIBLE_LIST_H
#define COLLAPSIBLE_LIST_H

#include "lvgl.h"
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
#define COLLAPSIBLE_LIST_MIN_COLS           4
#define COLLAPSIBLE_LIST_MIN_ROWS           3
#define COLLAPSIBLE_LIST_MAX_COLS           10
#define COLLAPSIBLE_LIST_MAX_ROWS           8

/* Default Grid Fallback: 6 cols x 5 rows (288 x 240 px) */
#define COLLAPSIBLE_LIST_DEFAULT_W          GRID_SPAN_W(6)
#define COLLAPSIBLE_LIST_DEFAULT_H          GRID_SPAN_H(5)

/* Maximum capacities */
#define COLLAPSIBLE_LIST_MAX_GROUPS         8
#define COLLAPSIBLE_LIST_MAX_ITEMS_PER_GROUP 8

typedef struct collapsible_list_s collapsible_list_t;

/**
 * @brief Callback fired when a child property row is clicked.
 *
 * @param list Pointer to the collapsible list widget instance.
 * @param group_idx Index of the parent group (0-based).
 * @param item_idx Index of the item within the group (0-based).
 * @param user_data User-defined pointer attached to the specific item.
 */
typedef void (*collapsible_item_click_cb_t)(collapsible_list_t *list, uint8_t group_idx, uint8_t item_idx, void *user_data);

/**
 * @brief Callback fired when a group header is expanded or collapsed.
 *
 * @param list Pointer to the collapsible list widget instance.
 * @param group_idx Index of the group (0-based).
 * @param expanded True if now expanded, false if collapsed.
 */
typedef void (*collapsible_group_toggle_cb_t)(collapsible_list_t *list, uint8_t group_idx, bool expanded);

/**
 * @brief Descriptor for an individual property row inside a group.
 */
typedef struct {
    char        item_id[32];       ///< Property identifier (e.g. "value", "hysteresis", "damping")
    const char *label;             ///< Display label (e.g. "Threshold:", "Hysteresis:")
    char        value_text[32];    ///< Current value formatted string (e.g. "15.00 %", "0 cycles")
    lv_color_t  value_color;       ///< Accent color for value text
    void       *user_data;         ///< Context pointer passed to click callback

    /* LVGL internal widget references */
    lv_obj_t   *row_btn;           ///< Clickable row container
    lv_obj_t   *lbl_label;         ///< Left label object
    lv_obj_t   *lbl_value;         ///< Value text object
    lv_obj_t   *lbl_icon;          ///< Edit symbol object
} collapsible_list_item_t;

/**
 * @brief Descriptor for a collapsible group section.
 */
typedef struct {
    char                    group_id[32];   ///< Group identifier (e.g. "alarm_low")
    char                    title[64];      ///< Header text (e.g. "-- alarm_low - depth --")
    bool                    is_expanded;    ///< True if unfolded, false if folded
    uint8_t                 item_count;     ///< Number of registered property rows
    collapsible_list_item_t items[COLLAPSIBLE_LIST_MAX_ITEMS_PER_GROUP];

    /* LVGL internal widget references */
    lv_obj_t               *header_btn;     ///< Clickable header button
    lv_obj_t               *lbl_arrow;      ///< Fold indicator (LV_SYMBOL_DOWN / LV_SYMBOL_RIGHT)
    lv_obj_t               *lbl_title;      ///< Header title label
    lv_obj_t               *items_cont;     ///< Indented container holding the items
} collapsible_list_group_t;

/**
 * @brief Main widget control structure.
 */
struct collapsible_list_s {
    lv_obj_t                    *parent;           ///< Parent LVGL screen or modal container
    int16_t                      x;                ///< X position in pixels (snapped to GRID_COORD_X)
    int16_t                      y;                ///< Y position in pixels (snapped to GRID_COORD_Y)
    uint16_t                     width;            ///< Width in pixels (multiple of GRID_CELL_SIZE)
    uint16_t                     height;           ///< Height in pixels (multiple of GRID_CELL_SIZE)

    collapsible_list_group_t     groups[COLLAPSIBLE_LIST_MAX_GROUPS];
    uint8_t                      group_count;

    const lv_font_t             *font;             ///< Default font (defaults to &ui_font_font0)
    lv_color_t                   bg_color;         ///< Background container color
    lv_color_t                   header_bg_color;  ///< Group header button background
    lv_color_t                   item_bg_color;    ///< Child item row background
    lv_color_t                   text_color;       ///< Primary label text color

    collapsible_item_click_cb_t  item_click_cb;    ///< Handler for item clicks
    collapsible_group_toggle_cb_t group_toggle_cb; ///< Optional handler for group fold events

    /* LVGL internal widget references */
    lv_obj_t                    *container;        ///< Scrollable root container
};

/**
 * @brief Initialize and render a collapsible list widget.
 *
 * @param widget Pointer to pre-allocated collapsible_list_t structure.
 * @return 0 on success, -1 on failure.
 */
int collapsible_list_init(collapsible_list_t *widget);

/**
 * @brief Clean up and delete all LVGL objects created by the widget.
 *
 * @param widget Pointer to widget instance.
 */
void collapsible_list_destroy(collapsible_list_t *widget);

/**
 * @brief Remove all groups and child items, retaining root container geometry.
 *
 * @param widget Pointer to widget instance.
 */
void collapsible_list_clear(collapsible_list_t *widget);

/**
 * @brief Add a new group header to the list.
 *
 * @param widget Pointer to widget instance.
 * @param group_id Unique identifier string for the group.
 * @param title Header title text.
 * @param start_expanded True to unfold on creation, false to fold.
 * @return Group index (0-based) on success, -1 if list is full.
 */
int collapsible_list_add_group(collapsible_list_t *widget, const char *group_id,
                               const char *title, bool start_expanded);

/**
 * @brief Add an editable property item row to an existing group.
 *
 * @param widget Pointer to widget instance.
 * @param group_idx Target group index.
 * @param item_id Unique identifier string for this property.
 * @param label Text shown on the left.
 * @param initial_value Text shown on the right.
 * @param value_color Accent color for the value text.
 * @param user_data Custom context pointer passed to item_click_cb.
 * @return Item index (0-based) on success, -1 if group is full or invalid.
 */
int collapsible_list_add_item(collapsible_list_t *widget, uint8_t group_idx,
                              const char *item_id, const char *label,
                              const char *initial_value, lv_color_t value_color,
                              void *user_data);

/**
 * @brief Update the displayed value string of an item.
 *
 * @param widget Pointer to widget instance.
 * @param group_idx Group index.
 * @param item_idx Item index within group.
 * @param value New value text.
 */
void collapsible_list_set_item_value(collapsible_list_t *widget, uint8_t group_idx,
                                     uint8_t item_idx, const char *value);

/**
 * @brief Update the accent color of an item's value text.
 *
 * @param widget Pointer to widget instance.
 * @param group_idx Group index.
 * @param item_idx Item index within group.
 * @param color New text color.
 */
void collapsible_list_set_item_color(collapsible_list_t *widget, uint8_t group_idx,
                                     uint8_t item_idx, lv_color_t color);

/**
 * @brief Programmatically fold or unfold a group.
 *
 * @param widget Pointer to widget instance.
 * @param group_idx Target group index.
 * @param expanded True to unfold, false to fold.
 */
void collapsible_list_set_group_expanded(collapsible_list_t *widget, uint8_t group_idx, bool expanded);

/**
 * @brief Check current fold state of a group.
 *
 * @param widget Pointer to widget instance.
 * @param group_idx Group index.
 * @return True if expanded, false if collapsed or invalid.
 */
bool collapsible_list_is_group_expanded(const collapsible_list_t *widget, uint8_t group_idx);

#ifdef __cplusplus
}
#endif

#endif /* COLLAPSIBLE_LIST_H */