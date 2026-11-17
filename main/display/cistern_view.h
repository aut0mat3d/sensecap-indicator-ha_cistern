/**
 * @file cistern_view.h
 * @brief Modular Cistern View screen using the 10x8 seamless grid widget architecture.
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
 * Cistern View Composition:
 *   1. Vertical Level Gauge:
 *      - Position:        Col 0, Row 0 -> (0, 64)
 *      - Span:            4 cols x 5 rows (192 x 240 px)
 *      - Constraints:     Min 3x4 (144x192 px), Max 10x8 (480x384 px)
 *   2. Key-Value Telemetry Card:
 *      - Position:        Col 4, Row 0 -> (192, 64)
 *      - Span:            6 cols x 5 rows (288 x 240 px)
 *      - Constraints:     Min 3x1 (144x48 px), Max 10x8 (480x384 px)
 *   3. Horizontal Trend Bar Graph:
 *      - Position:        Col 0, Row 5 -> (0, 304)
 *      - Span:            10 cols x 3 rows (480 x 144 px)
 *      - Constraints:     Min 5x2 (240x96 px), Max 10x8 (480x384 px)
 *      - Bottom Edge:     Y = 304 + 144 = 448 px (seamlessly meets Footer)
 *
 * Boundary Design Rule:
 *   All widgets align seamlessly to grid lines. External margins and outlines are
 *   strictly avoided. Separation and alarm indications are handled internally.
 * ============================================================================
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef CISTERN_VIEW_H
#define CISTERN_VIEW_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Grid Geometry Definitions ─────────────────────────────────────────── */
#ifndef GRID_CELL_SIZE
#define GRID_CELL_SIZE                      48
#define GRID_CONTENT_Y_OFFSET               64
#define GRID_CONTENT_MAX_W                  480
#define GRID_CONTENT_MAX_H                  384

#define GRID_COORD_X(col)                   ((int16_t)((col) * GRID_CELL_SIZE))
#define GRID_COORD_Y(row)                   ((int16_t)(GRID_CONTENT_Y_OFFSET + ((row) * GRID_CELL_SIZE)))
#define GRID_SPAN_W(col_span)               ((uint16_t)((col_span) * GRID_CELL_SIZE))
#define GRID_SPAN_H(row_span)               ((uint16_t)((row_span) * GRID_CELL_SIZE))
#endif

/**
 * @brief Initialize the cistern view widgets and schedule periodic tasks.
 *
 * @return 0 on success, -1 on failure.
 */
int cistern_view_init(void);

/**
 * @brief Destroy all view widgets, cancel timers, and unregister event subscriptions.
 */
void cistern_view_destroy(void);

/**
 * @brief Show or hide all container objects belonging to the cistern view.
 *
 * @param visible True to show widgets, false to hide.
 */
void cistern_view_set_visible(bool visible);

#ifdef __cplusplus
}
#endif

#endif /* CISTERN_VIEW_H */