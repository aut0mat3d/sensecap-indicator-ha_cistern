# Trend Graph Module

`trend_graph` is a generic long-term trend widget: a point-supported line chart on a real time axis
for one data point (`dev_addr` + `key`). The RP2040 reduces the archived values of the shown window
to at most 100 representative points (Largest-Triangle-Three-Buckets, LTTB), so spikes and dips stay
visible even on a 31-day range. Used by the detail page (`main/display/key_detail_view.c`).

## Overview

- **Time axis, not an index axis:** every point is placed at its timestamp. Oldest left, newest
  right ("now" on the right edge). Periods without data stay empty, data gaps break the line.
- **Presets:** up to 4 time ranges (`label`, `step_sec`, `point_count`; window = step × points).
  A tap on the widget cycles to the next preset, the head line shows e.g. `31 Tage 3/3`.
  With a single preset a tap does nothing and the counter is omitted.
- **One scheduler slot:** the widget owns exactly one `chart_query_scheduler` subscription in
  `CHART_QUERY_MODE_LTTB`; switching the preset re-targets it and triggers one immediate query.
- **Calendar frame follows the clock:** the frame `[now - window, now]` moves every 60 s; the data
  refresh comes from the scheduler (bucket boundaries).
- **Point colors:** normal points and lines in `point_color` (media color), `DATA_FLAG_ESTIMATED`
  points at 50 % opacity, `DATA_FLAG_ALARM_WARN` / `DATA_FLAG_ALARM_DANGER` points in the theme
  warning / danger color (`/gui/theme.json`).
- **Level lines:** up to 4 horizontal lines (e.g. thresholds) in warning / danger / neutral color.
- **Size:** any width / height; use the 48 px grid (`GRID_COORD_X/Y`, `GRID_SPAN_W/H` from
  `collapsible_list.h`). Minimum 4 × 3 cells (`TREND_GRAPH_MIN_W` × `TREND_GRAPH_MIN_H` = 192 × 144 px);
  smaller values are raised to the minimum.

## Quick Start

```c
#include "trend_graph.h"
#include "collapsible_list.h"   /* GRID_* macros */

static trend_graph_t s_trend;   /* must outlive the widget (timer and draw callbacks use it) */

memset(&s_trend, 0, sizeof(s_trend));
s_trend.parent   = screen;
s_trend.x        = GRID_COORD_X(0);
s_trend.y        = GRID_COORD_Y(0);
s_trend.width    = GRID_SPAN_W(10);
s_trend.height   = GRID_SPAN_H(5);
s_trend.dev_addr = 0x05975DC9;
strlcpy(s_trend.key, "battery_soc", sizeof(s_trend.key));

/* Presets: 24 h of hours, 7 days, 31 days */
s_trend.preset_count = 3;
strlcpy(s_trend.presets[0].label, gui_i18n_get("trend.24h"), sizeof(s_trend.presets[0].label));
s_trend.presets[0].step_sec = 3600;   s_trend.presets[0].point_count = 24;
strlcpy(s_trend.presets[1].label, gui_i18n_get("trend.7d"), sizeof(s_trend.presets[1].label));
s_trend.presets[1].step_sec = 86400;  s_trend.presets[1].point_count = 7;
strlcpy(s_trend.presets[2].label, gui_i18n_get("trend.31d"), sizeof(s_trend.presets[2].label));
s_trend.presets[2].step_sec = 86400;  s_trend.presets[2].point_count = 31;
s_trend.preset_idx = 2;                /* start with 31 days */

/* Y scale 0..100 (min == max: automatic from the data, +10 % margin) */
s_trend.min_val = 0.0f;
s_trend.max_val = 100.0f;

/* Falling warning threshold at 25 %: points below are drawn in the warning color */
s_trend.alarm_min   = 25.0f;
s_trend.alarm_flags = CHART_ALARM_MIN_IS_WARNING;
s_trend.lines[0]    = (trend_level_line_t){ .value = 25.0f, .level = GUI_ALARM_WARNING };
s_trend.line_count  = 1;

s_trend.point_color = lv_color_hex(media_get_color("water"));

if (trend_graph_init(&s_trend) != 0) {
    /* invalid configuration or no free scheduler slot */
}

/* ... */
trend_graph_destroy(&s_trend);   /* releases the scheduler slot, timer and LVGL objects */
```

`trend_graph_init()` takes the (recursive) LVGL lock itself, so it can be called from LVGL context
(e.g. a click handler) as well as from another task.

## Configuration Structure

| Field | Meaning |
|---|---|
| `parent`, `x`, `y`, `width`, `height` | Placement (pixels, use the grid macros) |
| `title` | Optional head line prefix (e.g. key name) |
| `dev_addr`, `key` | Data point; the archive channel is `dev_addr` + `key` |
| `zone_id` | Scheduler zone id; 0 = `TREND_GRAPH_DEFAULT_ZONE` (100, distinct from page zones 0..3). Use different ids if a page shows several trend graphs of the same key |
| `presets[]`, `preset_count`, `preset_idx` | Time ranges (1..4) and the initial one |
| `min_val`, `max_val` | Y scale; `min == max` = automatic |
| `alarm_min`, `alarm_max`, `alarm_flags` | Levels for the RP2040 point coloring (0 = off); `CHART_ALARM_MIN/MAX_IS_WARNING` selects warning instead of danger |
| `lines[]`, `line_count` | Horizontal level lines (`GUI_ALARM_NONE` = neutral grey) |
| `point_color` | Normal point / line color; black (all zero) = default blue |

Read-only state: `sub_id`, `custom_range`, `frame_start`, `frame_end`, `expected_step`,
`point_count`, `points[]`.

## API

| Function | Description |
|---|---|
| `trend_graph_init(g)` | Create the widget, subscribe and request the initial preset. Returns 0 or -1 |
| `trend_graph_set_preset(g, idx)` | Select a preset and request its data |
| `trend_graph_next_preset(g)` | Next preset (wraps; no-op with one preset) — also bound to a tap |
| `trend_graph_set_custom_range(g, start, end)` | Show any window `[start, end[` (UTC), LTTB-reduced, no periodic refresh until a preset is selected again |
| `trend_graph_set_scale(g, min, max)` | Change the Y scale and redraw |
| `trend_graph_set_lines(g, lines, count)` | Replace the level lines |
| `trend_graph_destroy(g)` | Unsubscribe, stop the timer, delete the objects |

## Data path

```
trend_graph ─► chart_query_scheduler (slot, CHART_QUERY_MODE_LTTB, 100 points, interval = window / 100)
            ─► 0x3B GET_CHART_DATA (byte 45 = 1) ─► RP2040 data_storage_query_lttb()
            ◄─ 0x3C RES_CHART_DATA (step = bucket width, ≤ 100 chart_data_point_t)
```

- The RP2040 splits the window into `CHART_LTTB_BUCKETS(100)` = 66 time buckets, so the answer has
  room for gap markers. It joins the daily, hourly and raw archive levels chronologically and keeps
  one point per non-empty bucket at its real timestamp; the newest value is always kept.
- Records further apart than the expected spacing of their level (raw: update interval + tolerance,
  hour 1.5 h, day 1.5 d, never less than one bucket) are separated by a `DATA_FLAG_NO_DATA` marker,
  which breaks the line. Hourly data in a 24 h window therefore stays connected.
- Alarm flags come from the bucket minimum / maximum (episodes inside a bucket stay visible).
- The widget polls the scheduler every 500 ms. An answer whose bucket width differs from
  `expected_step` belongs to the previously selected preset and is dropped.
- Protocol details: [OVERVIEW_DATAFLOW.md §5](../../../docs_extensions_n_changes/overview/OVERVIEW_DATAFLOW.md).

## Layout

```
┌──────────────────────────────────────────────┐
│ head line: [title]  label  n/N               │  26 px
│ y_max ┌──────────────────────────────────┐   │
│       │ level lines, line segments,      │   │  plot (custom drawn, LV_EVENT_DRAW_POST)
│ y_min └──────────────────────────────────┘   │
│        start date            end date (now)  │  18 px
└──────────────────────────────────────────────┘
  40 px Y label column
```

X labels show `dd.mm. HH:MM` for windows below 2 days, `dd.mm.` otherwise (local time).
The Y labels use one decimal for ranges below 10.

## Notes

- Memory: the `trend_graph_t` holds a copy of up to 100 points (~900 B); the scheduler slot buffer
  lives in PSRAM.
- Without valid time (no SNTP / P2P time yet) the frame is empty and the X labels show `--`.
- Daily values appear only after the first daily aggregation (local midnight); until then the
  7 / 31 day presets are filled from the hourly and raw levels.
