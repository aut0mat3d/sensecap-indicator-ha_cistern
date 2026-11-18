/**
 * @file trend_graph.h
 * @brief Generic long-term trend widget: point-based line chart on a time axis with a fixed
 *        calendar frame, tap-to-cycle presets and LTTB-reduced data.
 *
 *  - Up to 4 time range presets (label, step_sec, point_count; window = step x points). A click
 *    switches to the next preset and updates the head line (e.g. "31 Tage 2/4"); with a single
 *    preset a click does nothing and the counter is omitted.
 *  - Data: the RP2040 reduces the archived values of the window to at most
 *    TREND_GRAPH_LTTB_POINTS representative points (Largest-Triangle-Three-Buckets, raw / hourly /
 *    daily levels joined), so spikes and dips stay visible on long ranges.
 *  - Uses exactly one chart_query_scheduler subscription (LTTB mode); switching the preset
 *    updates it and triggers one immediate poll.
 *  - trend_graph_set_custom_range() shows any window [start_ts, end_ts[ (connector API); the
 *    periodic refresh is paused until a preset is selected again.
 *  - Fixed calendar frame: X = time of the window (newest right, oldest left), every point at its
 *    timestamp, periods without data stay empty and gaps break the line. Y = min..max (or
 *    automatic), optional horizontal level lines (e.g. thresholds), X labels = start / end.
 *  - Points: media color; DATA_FLAG_ESTIMATED at 50 % opacity; DATA_FLAG_ALARM_WARN / DANGER in
 *    the theme warning / danger color.
 *  - Size: any width / height (use the 48 px grid, GRID_COORD_* / GRID_SPAN_*); minimum
 *    TREND_GRAPH_MIN_W x TREND_GRAPH_MIN_H (4 x 3 cells).
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#ifndef TREND_GRAPH_H
#define TREND_GRAPH_H

#include "lvgl.h"
#include "gui_theme.h"
#include "rp2040.h"
#include "chart_query_scheduler.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TREND_GRAPH_MAX_PRESETS  4
#define TREND_GRAPH_MAX_LINES    4
#define TREND_GRAPH_LTTB_POINTS  100   /* points requested per window (incl. gap markers) */
#define TREND_GRAPH_MAX_POINTS   CHART_SUB_MAX_POINTS
#define TREND_GRAPH_DEFAULT_ZONE 100   /* scheduler zone id, distinct from page zones 0..3 */
#define TREND_GRAPH_MIN_W        192   /* 4 grid cells */
#define TREND_GRAPH_MIN_H        144   /* 3 grid cells */

/** Time range preset: window = step_sec x point_count */
typedef struct {
    char     label[24];      ///< Shown in the head line, e.g. "31 Tage"
    uint32_t step_sec;       ///< e.g. 3600 or 86400
    uint16_t point_count;    ///< e.g. 24 (hours) or 31 (days)
} trend_range_preset_t;

/** Horizontal level line (e.g. a threshold) */
typedef struct {
    float             value;
    gui_alarm_level_t level;  ///< Line color: WARNING / DANGER (NONE = neutral grey)
} trend_level_line_t;

typedef struct {
    /* ── Configuration (set before trend_graph_init) ── */
    lv_obj_t   *parent;
    int16_t     x;
    int16_t     y;
    uint16_t    width;
    uint16_t    height;
    const char *title;                 ///< Optional head line prefix
    uint32_t    dev_addr;              ///< Data point
    char        key[24];
    uint8_t     zone_id;               ///< Scheduler zone id (0 = TREND_GRAPH_DEFAULT_ZONE)
    trend_range_preset_t presets[TREND_GRAPH_MAX_PRESETS];
    uint8_t     preset_count;
    uint8_t     preset_idx;            ///< Initial / current preset
    float       min_val;               ///< Y scale (min == max: automatic from the data)
    float       max_val;
    float       alarm_min;             ///< Level for the RP2040 point coloring, falling (0 = off)
    float       alarm_max;             ///< Level for the RP2040 point coloring, rising (0 = off)
    uint8_t     alarm_flags;           ///< CHART_ALARM_*_IS_WARNING for alarm_min / alarm_max
    trend_level_line_t lines[TREND_GRAPH_MAX_LINES];
    uint8_t     line_count;
    lv_color_t  point_color;           ///< Normal point / line color (media color)

    /* ── State (read only) ── */
    int8_t      sub_id;
    bool        custom_range;
    uint32_t    frame_start;           ///< Shown window [frame_start, frame_end[ (UTC)
    uint32_t    frame_end;
    uint32_t    expected_step;         ///< LTTB bucket width of the requested window
    uint16_t    point_count;
    chart_data_point_t points[TREND_GRAPH_MAX_POINTS];

    /* ── Internal ── */
    lv_obj_t   *container;
    lv_obj_t   *lbl_head;
    lv_obj_t   *lbl_y_max;
    lv_obj_t   *lbl_y_min;
    lv_obj_t   *lbl_x_start;
    lv_obj_t   *lbl_x_end;
    lv_obj_t   *plot;
    lv_timer_t *timer;
    float       scale_min;             ///< Y scale in use (configured or automatic)
    float       scale_max;
    uint32_t    last_frame_tick;       ///< lv_tick of the last frame move
} trend_graph_t;

/**
 * @brief Create the widget, subscribe to the scheduler and request the initial preset.
 * @return 0 on success, -1 on invalid configuration or no free scheduler slot.
 */
int trend_graph_init(trend_graph_t *graph);

/** Select a preset (0 .. preset_count-1) and request its data */
void trend_graph_set_preset(trend_graph_t *graph, uint8_t idx);

/** Switch to the next preset (wraps around); no-op with a single preset */
void trend_graph_next_preset(trend_graph_t *graph);

/**
 * @brief Show a free window [start_ts, end_ts[ (LTTB-reduced, no periodic refresh).
 * @return false if the range is invalid or the request could not be queued.
 */
bool trend_graph_set_custom_range(trend_graph_t *graph, uint32_t start_ts, uint32_t end_ts);

/** Change the Y scale (min == max: automatic) and redraw */
void trend_graph_set_scale(trend_graph_t *graph, float min_val, float max_val);

/** Replace the horizontal level lines */
void trend_graph_set_lines(trend_graph_t *graph, const trend_level_line_t *lines, uint8_t count);

/** Release the scheduler slot, stop the timer and delete the LVGL objects */
void trend_graph_destroy(trend_graph_t *graph);

#ifdef __cplusplus
}
#endif

#endif /* TREND_GRAPH_H */
