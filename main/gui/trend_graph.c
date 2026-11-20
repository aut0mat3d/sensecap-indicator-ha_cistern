/**
 * @file trend_graph.c
 * @brief Generic long-term trend widget (see trend_graph.h).
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#include "trend_graph.h"
#include "lv_port.h"
#include "gui/i18n/gui_i18n.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(ui_font_font0);

static const char *TAG = "trend_graph";

#define TG_BG_COLOR        lv_color_hex(0x282828)
#define TG_PLOT_BG_COLOR   lv_color_hex(0x1A2126)
#define TG_BORDER_COLOR    lv_color_hex(0x000000)
#define TG_TEXT_COLOR      lv_color_hex(0x9E9E9E)
#define TG_HEAD_COLOR      lv_color_hex(0xFFFFFF)
#define TG_LINE_NEUTRAL    0x7F8C8D
#define TG_ZERO_LINE_COLOR lv_color_hex(0x55626D)
#define TG_EXT_LINE_COLOR  lv_color_hex(0xB0BEC5)
#define TG_CURSOR_COLOR    lv_color_hex(0xFFFFFF)
#define TG_DEFAULT_COLOR   lv_color_hex(0x3498DB)
#define TG_BORDER_W        2
#define TG_PAD             6
#define TG_CONTENT_OFS     (TG_BORDER_W + TG_PAD) /* container edge -> content origin */
#define TG_Y_LABEL_W       40      /* left column for the Y labels */
#define TG_HEAD_H          26
#define TG_X_LABEL_H       18
#define TG_PLOT_PAD        6       /* keeps points at the border fully visible */
#define TG_SMALL_LABEL_H   16      /* montserrat_14 line height */
#define TG_EXT_LABEL_W     110
#define TG_POLL_PERIOD_MS  500
#define TG_FRAME_TICK_MS   60000   /* moves the "now" edge of the frame */
#define TG_SCALE_TICKS     4       /* the automatic scale aims at ~4 steps */
#define TG_MAX_DECIMALS    3

/* Touch zones of a press, from the press position */
enum { TG_ZONE_NONE = 0, TG_ZONE_HEAD, TG_ZONE_PLOT };

static bool color_is_empty(lv_color_t c) {
    return c.red == 0 && c.green == 0 && c.blue == 0;
}

static uint32_t now_utc(void) {
    time_t now = time(NULL);
    return (now >= 1700000000) ? (uint32_t)now : 0;
}

/* Request interval of a window: the window is split into TREND_GRAPH_LTTB_POINTS steps */
static uint32_t lttb_interval(uint32_t window) {
    uint32_t iv = window / TREND_GRAPH_LTTB_POINTS;
    return iv ? iv : 1;
}

/* Bucket width the RP2040 reports for a window (same formula as data_storage_query_lttb) */
static uint32_t lttb_expected_step(uint32_t window) {
    uint32_t m = CHART_LTTB_BUCKETS(TREND_GRAPH_LTTB_POINTS);
    return (window + m - 1) / m;
}

static bool point_visible(const trend_graph_t *g, const chart_data_point_t *p) {
    return !(p->flags & DATA_FLAG_NO_DATA) && p->timestamp >= g->frame_start && p->timestamp <= g->frame_end;
}

/* ── Frame & scale ─────────────────────────────────────────────────────── */

static void update_frame(trend_graph_t *g) {
    if (g->custom_range || g->preset_count == 0) return;
    const trend_range_preset_t *p = &g->presets[g->preset_idx];
    uint32_t window = TREND_GRAPH_LTTB_POINTS * lttb_interval(p->step_sec * p->point_count);
    uint32_t now = now_utc();
    g->frame_end = now;
    g->frame_start = (now > window) ? now - window : 0;
}

/* 1, 2 or 5 times a power of ten, >= raw */
static float nice_step(float raw) {
    if (!(raw > 0.0f) || isinf(raw)) return 1.0f;
    float pow10 = powf(10.0f, floorf(log10f(raw)));
    float f = raw / pow10;
    float nf = (f <= 1.0f) ? 1.0f : (f <= 2.0f) ? 2.0f : (f <= 5.0f) ? 5.0f : 10.0f;
    return nf * pow10;
}

/* Decimals needed to show multiples of step */
static uint8_t decimals_for_step(float step) {
    if (step >= 1.0f) return 0;
    int d = (int)ceilf(-log10f(step) - 1e-4f);
    if (d < 0) d = 0;
    if (d > TG_MAX_DECIMALS) d = TG_MAX_DECIMALS;
    return (uint8_t)d;
}

static void compute_scale(trend_graph_t *g) {
    if (g->max_val > g->min_val) {
        g->scale_min = g->min_val;
        g->scale_max = g->max_val;
        g->scale_decimals = (g->max_val - g->min_val < 10.0f) ? 1 : 0;
        return;
    }

    /* Automatic: points plus the window extremes (peaks the point selection skipped) */
    bool any = false;
    float lo = 0.0f, hi = 0.0f;
    for (uint16_t i = 0; i < g->point_count; i++) {
        const chart_data_point_t *p = &g->points[i];
        if (p->flags & DATA_FLAG_NO_DATA) continue;
        if (!any || p->value < lo) lo = p->value;
        if (!any || p->value > hi) hi = p->value;
        any = true;
    }
    if (g->have_extremes) {
        if (!any || g->win_min < lo) lo = g->win_min;
        if (!any || g->win_max > hi) hi = g->win_max;
        any = true;
    }
    if (!any && !g->auto_include_zero) {
        lo = 0.0f;
        hi = 100.0f;
    }
    if (g->auto_include_zero) {
        if (lo > 0.0f) lo = 0.0f;
        if (hi < 0.0f) hi = 0.0f;
    }

    /* The span comes from the data only: 10 % margin around it. Flat data (a single value in the
     * whole window) has no span; it gets +-10 % of its value, or +-1 around 0 (the line then lies on
     * the zero line anyway), so the scale never divides by 0. */
    if (hi > lo) {
        float margin = (hi - lo) * 0.1f;
        lo -= margin;
        hi += margin;
    } else {
        float half = (fabsf(lo) > 0.0f) ? fabsf(lo) * 0.1f : 1.0f;
        lo -= half;
        hi += half;
    }

    /* Round outwards to a "nice" step so the labels are short */
    float step = nice_step((hi - lo) / (float)TG_SCALE_TICKS);
    g->scale_min = floorf(lo / step) * step;
    g->scale_max = ceilf(hi / step) * step;
    if (g->scale_max <= g->scale_min) g->scale_max = g->scale_min + step;
    g->scale_decimals = decimals_for_step(step);
}

/* Y position (container content coordinates) of a value on the current scale */
static int32_t value_to_content_y(const trend_graph_t *g, float v) {
    int32_t h = g->plot_h - 2 * TG_PLOT_PAD;
    float range = g->scale_max - g->scale_min;
    if (h <= 0 || range <= 0.0f) return g->plot_y;
    return g->plot_y + TG_PLOT_PAD + h - (int32_t)lroundf((v - g->scale_min) / range * (float)h);
}

static void format_value(char *buf, size_t len, float v, uint8_t decimals) {
    /* Avoid "-0.00" */
    float eps = 0.5f * powf(10.0f, -(float)decimals);
    if (fabsf(v) < eps) v = 0.0f;
    snprintf(buf, len, "%.*f", (int)decimals, (double)v);
}

/* Decimals of cursor / extreme values: configured, else one more than the scale labels */
static uint8_t value_decimals(const trend_graph_t *g) {
    if (g->decimals > 0) return g->decimals;
    uint8_t d = (uint8_t)(g->scale_decimals + 1);
    return (d > TG_MAX_DECIMALS) ? TG_MAX_DECIMALS : d;
}

static void format_time(char *buf, size_t len, uint32_t ts, uint32_t window) {
    if (ts < 1700000000U) {
        snprintf(buf, len, "--");
        return;
    }
    time_t t = (time_t)ts;
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(buf, len, (window >= 2U * 86400U) ? "%d.%m." : "%d.%m. %H:%M", &lt);
}

/* ── Cursor ────────────────────────────────────────────────────────────── */

/* Visible point nearest to ts; false if the frame shows no point */
static bool find_nearest(const trend_graph_t *g, uint32_t ts, uint16_t *out_idx) {
    bool found = false;
    uint32_t best = 0;
    for (uint16_t i = 0; i < g->point_count; i++) {
        const chart_data_point_t *p = &g->points[i];
        if (!point_visible(g, p)) continue;
        uint32_t d = (p->timestamp > ts) ? p->timestamp - ts : ts - p->timestamp;
        if (!found || d < best) {
            best = d;
            *out_idx = i;
            found = true;
        }
    }
    return found;
}

/* ── Rendering (LVGL lock held) ────────────────────────────────────────── */

static void render_head(trend_graph_t *g) {
    char buf[80];
    uint16_t idx;
    if (g->cursor_active && find_nearest(g, g->cursor_ts, &idx)) {
        /* Cursor readout: time and value of the selected point */
        const chart_data_point_t *p = &g->points[idx];
        char t[24], v[24];
        time_t ts = (time_t)p->timestamp;
        struct tm lt;
        localtime_r(&ts, &lt);
        strftime(t, sizeof(t), "%d.%m. %H:%M", &lt);
        format_value(v, sizeof(v), p->value, value_decimals(g));
        snprintf(buf, sizeof(buf), "%s   %s %s", t, v, g->unit);
        lv_label_set_text(g->lbl_head, buf);
        return;
    }
    const char *label = (g->preset_count > 0 && !g->custom_range) ? g->presets[g->preset_idx].label : "";
    const char *title = (g->title && g->title[0]) ? g->title : "";
    if (g->custom_range || g->preset_count <= 1) {
        snprintf(buf, sizeof(buf), "%s%s%s", title, (title[0] && label[0]) ? "  " : "", label);
    } else {
        snprintf(buf, sizeof(buf), "%s%s%s %u/%u", title, title[0] ? "  " : "", label,
                 (unsigned)(g->preset_idx + 1), (unsigned)g->preset_count);
    }
    lv_label_set_text(g->lbl_head, buf);
}

/* Y labels: max / min at the plot edges, 0 at the zero line if it has room */
static void render_y_labels(trend_graph_t *g) {
    char buf[24];
    format_value(buf, sizeof(buf), g->scale_max, g->scale_decimals);
    lv_label_set_text(g->lbl_y_max, buf);
    format_value(buf, sizeof(buf), g->scale_min, g->scale_decimals);
    lv_label_set_text(g->lbl_y_min, buf);

    bool zero_inside = g->scale_min < 0.0f && g->scale_max > 0.0f;
    int32_t y0 = value_to_content_y(g, 0.0f);
    int32_t y_top = value_to_content_y(g, g->scale_max);
    int32_t y_bottom = value_to_content_y(g, g->scale_min);
    if (zero_inside && y0 - y_top >= TG_SMALL_LABEL_H && y_bottom - y0 >= TG_SMALL_LABEL_H) {
        lv_label_set_text(g->lbl_y_zero, "0");
        lv_obj_set_pos(g->lbl_y_zero, 0, y0 - TG_SMALL_LABEL_H / 2);
        lv_obj_remove_flag(g->lbl_y_zero, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g->lbl_y_zero, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Labels of the window extremes at the right end of their dashed lines */
static void render_extremes(trend_graph_t *g) {
    bool show = g->show_extremes && g->have_extremes && g->win_min >= g->scale_min && g->win_max <= g->scale_max;
    if (!show) {
        lv_obj_add_flag(g->lbl_ext_max, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g->lbl_ext_min, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char v[24], buf[48];
    uint8_t dec = value_decimals(g);
    int32_t x = g->plot_x + g->plot_w - TG_EXT_LABEL_W - 4;
    int32_t top = g->plot_y;
    int32_t bottom = g->plot_y + g->plot_h - TG_SMALL_LABEL_H;

    format_value(v, sizeof(v), g->win_max, dec);
    snprintf(buf, sizeof(buf), "%s %s", gui_i18n_get("trend.max"), v);
    lv_label_set_text(g->lbl_ext_max, buf);
    int32_t y = value_to_content_y(g, g->win_max) - TG_SMALL_LABEL_H - 1; /* above its line */
    if (y < top) y = top;
    lv_obj_set_pos(g->lbl_ext_max, x, y);
    lv_obj_remove_flag(g->lbl_ext_max, LV_OBJ_FLAG_HIDDEN);

    format_value(v, sizeof(v), g->win_min, dec);
    snprintf(buf, sizeof(buf), "%s %s", gui_i18n_get("trend.min"), v);
    lv_label_set_text(g->lbl_ext_min, buf);
    y = value_to_content_y(g, g->win_min) + 2; /* below its line */
    if (y > bottom) y = bottom;
    lv_obj_set_pos(g->lbl_ext_min, x, y);
    lv_obj_remove_flag(g->lbl_ext_min, LV_OBJ_FLAG_HIDDEN);
}

static void render(trend_graph_t *g) {
    if (!g->plot) return;
    update_frame(g);
    compute_scale(g);
    render_y_labels(g);
    render_extremes(g);

    char buf[24];
    uint32_t window = g->frame_end - g->frame_start;
    format_time(buf, sizeof(buf), g->frame_start, window);
    lv_label_set_text(g->lbl_x_start, buf);
    format_time(buf, sizeof(buf), g->frame_end, window);
    lv_label_set_text(g->lbl_x_end, buf);

    render_head(g);
    lv_obj_invalidate(g->plot);
}

static void draw_hline(lv_layer_t *layer, lv_draw_line_dsc_t *line, const lv_area_t *a, int32_t y) {
    line->p1.x = a->x1;
    line->p1.y = y;
    line->p2.x = a->x2;
    line->p2.y = y;
    lv_draw_line(layer, line);
}

/* Custom drawing of the plot area: zero / extreme / level lines, line segments, points, cursor */
static void plot_draw_cb(lv_event_t *e) {
    trend_graph_t *g = (trend_graph_t *)lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!g || !layer || g->frame_end <= g->frame_start) return;

    lv_area_t a;
    lv_obj_get_coords(g->plot, &a);
    int32_t x0 = a.x1 + TG_PLOT_PAD;
    int32_t y0 = a.y1 + TG_PLOT_PAD;
    int32_t w = lv_area_get_width(&a) - 2 * TG_PLOT_PAD;
    int32_t h = lv_area_get_height(&a) - 2 * TG_PLOT_PAD;
    float range = g->scale_max - g->scale_min;
    if (w <= 0 || h <= 0 || range <= 0.0f) return;
    double span = (double)(g->frame_end - g->frame_start);
#define TG_VAL_Y(v) (y0 + h - (int32_t)lroundf(((v) - g->scale_min) / range * (float)h))
#define TG_TS_X(ts) (x0 + (int32_t)lround((double)((ts) - g->frame_start) / span * (double)w))

    gui_theme_colors_t theme;
    gui_theme_get_colors(&theme);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.width = 1;

    /* 1. Zero line (signed values) */
    if (g->scale_min < 0.0f && g->scale_max > 0.0f) {
        line.color = TG_ZERO_LINE_COLOR;
        line.opa = LV_OPA_COVER;
        draw_hline(layer, &line, &a, TG_VAL_Y(0.0f));
    }

    /* 2. Window extremes (dashed) */
    if (g->show_extremes && g->have_extremes) {
        line.color = TG_EXT_LINE_COLOR;
        line.opa = LV_OPA_60;
        line.dash_width = 6;
        line.dash_gap = 4;
        const float ext[2] = { g->win_max, g->win_min };
        for (int i = 0; i < 2; i++) {
            if (ext[i] < g->scale_min || ext[i] > g->scale_max) continue;
            draw_hline(layer, &line, &a, TG_VAL_Y(ext[i]));
        }
        line.dash_width = 0;
        line.dash_gap = 0;
    }

    /* 3. Level lines (thresholds) */
    line.opa = LV_OPA_80;
    for (uint8_t i = 0; i < g->line_count; i++) {
        float v = g->lines[i].value;
        if (v < g->scale_min || v > g->scale_max) continue;
        line.color = lv_color_hex(gui_theme_alarm_level_color(g->lines[i].level, TG_LINE_NEUTRAL));
        draw_hline(layer, &line, &a, TG_VAL_Y(v));
    }

    /* 4. Line segments between neighbouring points (a gap marker breaks the line) */
    line.width = 2;
    line.opa = LV_OPA_COVER;
    line.color = g->point_color;
    line.round_start = 1;
    line.round_end = 1;
    bool have_prev = false;
    int32_t px = 0, py = 0;
    bool prev_est = false;
    for (uint16_t i = 0; i < g->point_count; i++) {
        const chart_data_point_t *p = &g->points[i];
        if (!point_visible(g, p)) {
            have_prev = false;
            continue;
        }
        float v = p->value < g->scale_min ? g->scale_min : (p->value > g->scale_max ? g->scale_max : p->value);
        int32_t x = TG_TS_X(p->timestamp);
        int32_t y = TG_VAL_Y(v);
        bool est = (p->flags & DATA_FLAG_ESTIMATED) != 0;
        if (have_prev) {
            line.opa = (est || prev_est) ? LV_OPA_50 : LV_OPA_COVER;
            line.p1.x = px;
            line.p1.y = py;
            line.p2.x = x;
            line.p2.y = y;
            lv_draw_line(layer, &line);
        }
        have_prev = true;
        px = x;
        py = y;
        prev_est = est;
    }

    /* 5. Points (smaller when dense) */
    int32_t r = (g->point_count > 40) ? 2 : 3;
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    dot.border_width = 0;
    for (uint16_t i = 0; i < g->point_count; i++) {
        const chart_data_point_t *p = &g->points[i];
        if (!point_visible(g, p)) continue;
        float v = p->value < g->scale_min ? g->scale_min : (p->value > g->scale_max ? g->scale_max : p->value);
        int32_t x = TG_TS_X(p->timestamp);
        int32_t y = TG_VAL_Y(v);
        if (p->flags & DATA_FLAG_ALARM_DANGER) dot.bg_color = lv_color_hex(theme.alarm_danger);
        else if (p->flags & DATA_FLAG_ALARM_WARN) dot.bg_color = lv_color_hex(theme.alarm_warning);
        else dot.bg_color = g->point_color;
        dot.bg_opa = (p->flags & DATA_FLAG_ESTIMATED) ? LV_OPA_50 : LV_OPA_COVER;
        lv_area_t pa = { x - r, y - r, x + r, y + r };
        lv_draw_rect(layer, &dot, &pa);
    }

    /* 6. Cursor: vertical line and ring around the selected point */
    uint16_t idx;
    if (g->cursor_active && find_nearest(g, g->cursor_ts, &idx)) {
        const chart_data_point_t *p = &g->points[idx];
        float v = p->value < g->scale_min ? g->scale_min : (p->value > g->scale_max ? g->scale_max : p->value);
        int32_t x = TG_TS_X(p->timestamp);
        int32_t y = TG_VAL_Y(v);
        line.width = 1;
        line.opa = LV_OPA_70;
        line.color = TG_CURSOR_COLOR;
        line.round_start = 0;
        line.round_end = 0;
        line.p1.x = x;
        line.p1.y = a.y1;
        line.p2.x = x;
        line.p2.y = a.y2;
        lv_draw_line(layer, &line);

        lv_draw_rect_dsc_t ring;
        lv_draw_rect_dsc_init(&ring);
        ring.radius = LV_RADIUS_CIRCLE;
        ring.bg_opa = LV_OPA_TRANSP;
        ring.border_width = 2;
        ring.border_color = TG_CURSOR_COLOR;
        ring.border_opa = LV_OPA_COVER;
        lv_area_t ra = { x - 6, y - 6, x + 6, y + 6 };
        lv_draw_rect(layer, &ring, &ra);
    }
#undef TG_VAL_Y
#undef TG_TS_X
}

/* Place the cursor at the point nearest to ts (LVGL lock held); true if it moved to another point */
static bool cursor_set(trend_graph_t *g, uint32_t ts) {
    uint16_t idx;
    if (!find_nearest(g, ts, &idx)) return false;
    uint32_t pt_ts = g->points[idx].timestamp;
    bool changed = !g->cursor_active || pt_ts != g->cursor_ts;
    g->cursor_active = true;
    g->cursor_ts = pt_ts;
    g->cursor_tick = lv_tick_get();
    if (changed) {
        render_head(g);
        lv_obj_invalidate(g->plot);
    }
    return changed;
}

static void cursor_clear(trend_graph_t *g) {
    if (!g->cursor_active) return;
    g->cursor_active = false;
    if (g->plot) {
        render_head(g);
        lv_obj_invalidate(g->plot);
    }
}

/* ── Data handshake (LVGL timer) ───────────────────────────────────────── */

static void poll_timer_cb(lv_timer_t *timer) {
    trend_graph_t *g = (trend_graph_t *)lv_timer_get_user_data(timer);
    if (!g || g->sub_id < 0) return;

    uint32_t tick = lv_tick_get();

    if (chart_query_scheduler_is_data_ready(g->sub_id)) {
        chart_data_point_t pts[TREND_GRAPH_MAX_POINTS];
        uint16_t count = 0;
        if (chart_query_scheduler_consume_data(g->sub_id, pts, TREND_GRAPH_MAX_POINTS, &count)) {
            /* A late answer for the previous window carries another bucket width: drop it */
            uint32_t step = chart_query_scheduler_get_data_step(g->sub_id);
            if (step == g->expected_step) {
                memcpy(g->points, pts, count * sizeof(chart_data_point_t));
                g->point_count = count;
                float mn = 0.0f, mx = 0.0f;
                g->have_extremes = chart_query_scheduler_get_window_extremes(g->sub_id, &mn, &mx);
                g->win_min = mn;
                g->win_max = mx;
                render(g);
                /* Keep the cursor on its time; it disappears if no point is left */
                if (g->cursor_active) {
                    uint16_t idx;
                    if (find_nearest(g, g->cursor_ts, &idx)) {
                        g->cursor_ts = g->points[idx].timestamp;
                        render_head(g);
                    } else {
                        cursor_clear(g);
                    }
                }
            } else {
                ESP_LOGD(TAG, "Dropped data with step %lu (expected %lu)", (unsigned long)step,
                         (unsigned long)g->expected_step);
            }
        }
    } else if (tick - g->last_frame_tick >= TG_FRAME_TICK_MS) {
        /* Let the frame follow the clock (the data refresh comes from the scheduler) */
        g->last_frame_tick = tick;
        if (!g->custom_range) render(g);
    }

    if (g->cursor_active && !lv_obj_has_state(g->container, LV_STATE_PRESSED) &&
        lv_tick_elaps(g->cursor_tick) >= TREND_GRAPH_CURSOR_HIDE_MS) {
        cursor_clear(g);
    }
}

/* Touch zone of a screen point: head line, plot, or the dead zone below the plot */
static uint8_t touch_zone(const trend_graph_t *g, const lv_point_t *pt) {
    lv_area_t a;
    lv_obj_get_coords(g->container, &a);
    int32_t rel = pt->y - a.y1 - TG_CONTENT_OFS;
    if (rel < g->plot_y) return TG_ZONE_HEAD;
    if (rel < g->plot_y + g->plot_h) return TG_ZONE_PLOT;
    return TG_ZONE_NONE;
}

/* Cursor from a screen x position inside the plot */
static void cursor_from_x(trend_graph_t *g, int32_t screen_x) {
    if (g->frame_end <= g->frame_start) return;
    lv_area_t a;
    lv_obj_get_coords(g->plot, &a);
    int32_t x0 = a.x1 + TG_PLOT_PAD;
    int32_t w = lv_area_get_width(&a) - 2 * TG_PLOT_PAD;
    if (w <= 0) return;
    int32_t dx = screen_x - x0;
    if (dx < 0) dx = 0;
    if (dx > w) dx = w;
    uint32_t ts = g->frame_start + (uint32_t)((double)dx / (double)w * (double)(g->frame_end - g->frame_start));
    if (cursor_set(g, ts) && g->cursor_cb) g->cursor_cb(g, g->cursor_ts, g->user_data);
}

static void touch_cb(lv_event_t *e) {
    trend_graph_t *g = (trend_graph_t *)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (!g || !indev || !g->plot) return;
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    if (code == LV_EVENT_PRESSED) {
        g->press_zone = touch_zone(g, &pt);
        if (g->press_zone == TG_ZONE_PLOT) cursor_from_x(g, pt.x);
    } else if (code == LV_EVENT_PRESSING) {
        if (g->press_zone == TG_ZONE_PLOT) cursor_from_x(g, pt.x);
    } else if (code == LV_EVENT_CLICKED) {
        if (g->press_zone == TG_ZONE_HEAD && g->preset_count > 1) {
            trend_graph_next_preset(g);
            if (g->preset_changed_cb) g->preset_changed_cb(g, g->preset_idx, g->user_data);
        }
    }
}

/* Point the scheduler slot at the current preset window (LVGL lock not required) */
static void request_preset(trend_graph_t *g) {
    const trend_range_preset_t *p = &g->presets[g->preset_idx];
    uint32_t interval = lttb_interval(p->step_sec * p->point_count);
    g->expected_step = lttb_expected_step(interval * TREND_GRAPH_LTTB_POINTS);
    chart_query_scheduler_subscribe(g->dev_addr, g->key, g->zone_id, interval, TREND_GRAPH_LTTB_POINTS,
                                    g->alarm_min, g->alarm_max, g->alarm_flags);
    chart_query_scheduler_set_query_mode(g->sub_id, CHART_QUERY_MODE_LTTB);
    /* Re-activation queries immediately and resumes the periodic refresh */
    chart_query_scheduler_set_active(g->sub_id, false);
    chart_query_scheduler_set_active(g->sub_id, true);
}

static lv_obj_t *create_small_label(lv_obj_t *parent) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, TG_TEXT_COLOR, LV_PART_MAIN);
    return lbl;
}

/* ── Public API ───────────────────────────────────────────────────────── */

int trend_graph_init(trend_graph_t *g) {
    if (!g || !g->parent || g->key[0] == '\0' || g->preset_count == 0 ||
        g->preset_count > TREND_GRAPH_MAX_PRESETS) {
        return -1;
    }
    for (uint8_t i = 0; i < g->preset_count; i++) {
        if (g->presets[i].step_sec == 0 || g->presets[i].point_count == 0) return -1;
    }
    if (g->preset_idx >= g->preset_count) g->preset_idx = 0;
    if (g->zone_id == 0) g->zone_id = TREND_GRAPH_DEFAULT_ZONE;
    if (color_is_empty(g->point_color)) g->point_color = TG_DEFAULT_COLOR;
    if (g->line_count > TREND_GRAPH_MAX_LINES) g->line_count = TREND_GRAPH_MAX_LINES;
    if (g->width < TREND_GRAPH_MIN_W) g->width = TREND_GRAPH_MIN_W;
    if (g->height < TREND_GRAPH_MIN_H) g->height = TREND_GRAPH_MIN_H;
    g->custom_range = false;
    g->point_count = 0;
    g->have_extremes = false;
    g->cursor_active = false;
    g->press_zone = TG_ZONE_NONE;

    const trend_range_preset_t *p = &g->presets[g->preset_idx];
    uint32_t interval = lttb_interval(p->step_sec * p->point_count);
    g->sub_id = chart_query_scheduler_subscribe(g->dev_addr, g->key, g->zone_id, interval, TREND_GRAPH_LTTB_POINTS,
                                                g->alarm_min, g->alarm_max, g->alarm_flags);
    if (g->sub_id < 0) return -1;

    lv_port_sem_take();

    g->container = lv_obj_create(g->parent);
    lv_obj_set_size(g->container, g->width, g->height);
    lv_obj_set_pos(g->container, g->x, g->y);
    lv_obj_remove_flag(g->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g->container, LV_OBJ_FLAG_SCROLL_CHAIN); /* dragging the cursor never scrolls a parent */
    lv_obj_set_style_radius(g->container, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g->container, TG_BG_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(g->container, TG_BORDER_W, LV_PART_MAIN);
    lv_obj_set_style_border_color(g->container, TG_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_outline_width(g->container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g->container, TG_PAD, LV_PART_MAIN);
    lv_obj_add_event_cb(g->container, touch_cb, LV_EVENT_PRESSED, g);
    lv_obj_add_event_cb(g->container, touch_cb, LV_EVENT_PRESSING, g);
    lv_obj_add_event_cb(g->container, touch_cb, LV_EVENT_CLICKED, g);

    /* Layout derived from the size: head line, Y label column, plot, X label row */
    int32_t inner_w = g->width - 2 * TG_CONTENT_OFS;
    int32_t inner_h = g->height - 2 * TG_CONTENT_OFS;
    g->plot_x = TG_Y_LABEL_W;
    g->plot_y = TG_HEAD_H;
    g->plot_w = (int16_t)(inner_w - g->plot_x);
    g->plot_h = (int16_t)(inner_h - TG_HEAD_H - TG_X_LABEL_H);

    g->lbl_head = lv_label_create(g->container);
    lv_obj_set_style_text_font(g->lbl_head, &ui_font_font0, LV_PART_MAIN);
    lv_obj_set_style_text_color(g->lbl_head, TG_HEAD_COLOR, LV_PART_MAIN);
    lv_label_set_long_mode(g->lbl_head, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(g->lbl_head, inner_w - 4);
    lv_obj_set_pos(g->lbl_head, 2, 0);

    g->lbl_y_max = create_small_label(g->container);
    g->lbl_y_min = create_small_label(g->container);
    g->lbl_y_zero = create_small_label(g->container);
    g->lbl_x_start = create_small_label(g->container);
    g->lbl_x_end = create_small_label(g->container);
    lv_obj_t *y_labels[] = { g->lbl_y_max, g->lbl_y_min, g->lbl_y_zero };
    for (size_t i = 0; i < sizeof(y_labels) / sizeof(y_labels[0]); i++) {
        lv_obj_set_width(y_labels[i], TG_Y_LABEL_W - 4);
        lv_obj_set_style_text_align(y_labels[i], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    }
    lv_obj_add_flag(g->lbl_y_zero, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(g->lbl_y_max, 0, g->plot_y + TG_PLOT_PAD - 8);
    lv_obj_set_pos(g->lbl_y_min, 0, g->plot_y + g->plot_h - TG_PLOT_PAD - 8);
    lv_obj_set_pos(g->lbl_x_start, g->plot_x, g->plot_y + g->plot_h + 2);
    lv_obj_align(g->lbl_x_end, LV_ALIGN_TOP_RIGHT, 0, g->plot_y + g->plot_h + 2);

    g->plot = lv_obj_create(g->container);
    lv_obj_remove_style_all(g->plot);
    lv_obj_set_size(g->plot, g->plot_w, g->plot_h);
    lv_obj_set_pos(g->plot, g->plot_x, g->plot_y);
    lv_obj_set_style_bg_color(g->plot, TG_PLOT_BG_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g->plot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g->plot, 4, LV_PART_MAIN);
    lv_obj_remove_flag(g->plot, LV_OBJ_FLAG_CLICKABLE); /* touches reach the container */
    lv_obj_add_event_cb(g->plot, plot_draw_cb, LV_EVENT_DRAW_POST, g);

    /* Extreme labels after the plot, so they are drawn above its lines */
    g->lbl_ext_max = create_small_label(g->container);
    g->lbl_ext_min = create_small_label(g->container);
    lv_obj_t *ext_labels[] = { g->lbl_ext_max, g->lbl_ext_min };
    for (size_t i = 0; i < 2; i++) {
        lv_obj_set_width(ext_labels[i], TG_EXT_LABEL_W);
        lv_obj_set_style_text_align(ext_labels[i], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        lv_obj_set_style_text_color(ext_labels[i], TG_EXT_LINE_COLOR, LV_PART_MAIN);
        lv_obj_add_flag(ext_labels[i], LV_OBJ_FLAG_HIDDEN);
    }

    render(g);
    g->timer = lv_timer_create(poll_timer_cb, TG_POLL_PERIOD_MS, g);
    lv_port_sem_give();

    request_preset(g);
    return 0;
}

void trend_graph_set_preset(trend_graph_t *g, uint8_t idx) {
    if (!g || g->sub_id < 0 || idx >= g->preset_count) return;
    lv_port_sem_take();
    g->preset_idx = idx;
    g->custom_range = false;
    g->point_count = 0;
    g->have_extremes = false;
    g->cursor_active = false;
    render(g);
    lv_port_sem_give();
    request_preset(g);
}

void trend_graph_next_preset(trend_graph_t *g) {
    if (!g || g->preset_count <= 1) return;
    trend_graph_set_preset(g, (uint8_t)((g->preset_idx + 1) % g->preset_count));
}

bool trend_graph_set_custom_range(trend_graph_t *g, uint32_t start_ts, uint32_t end_ts) {
    if (!g || g->sub_id < 0 || end_ts <= start_ts) return false;
    uint32_t interval = lttb_interval(end_ts - start_ts);
    uint32_t window = interval * TREND_GRAPH_LTTB_POINTS;

    lv_port_sem_take();
    g->custom_range = true;
    g->frame_end = end_ts;
    g->frame_start = end_ts - window;
    g->expected_step = lttb_expected_step(window);
    g->point_count = 0;
    g->have_extremes = false;
    g->cursor_active = false;
    render(g);
    lv_port_sem_give();

    /* Fixed window: no periodic refresh replacing it */
    chart_query_scheduler_set_active(g->sub_id, false);
    chart_query_scheduler_subscribe(g->dev_addr, g->key, g->zone_id, interval, TREND_GRAPH_LTTB_POINTS,
                                    g->alarm_min, g->alarm_max, g->alarm_flags);
    chart_query_scheduler_set_query_mode(g->sub_id, CHART_QUERY_MODE_LTTB);
    return chart_query_scheduler_poll_range(g->sub_id, g->frame_start, end_ts);
}

void trend_graph_set_scale(trend_graph_t *g, float min_val, float max_val) {
    if (!g) return;
    lv_port_sem_take();
    g->min_val = min_val;
    g->max_val = max_val;
    render(g);
    lv_port_sem_give();
}

void trend_graph_set_lines(trend_graph_t *g, const trend_level_line_t *lines, uint8_t count) {
    if (!g) return;
    if (count > TREND_GRAPH_MAX_LINES) count = TREND_GRAPH_MAX_LINES;
    lv_port_sem_take();
    for (uint8_t i = 0; i < count; i++) g->lines[i] = lines[i];
    g->line_count = count;
    if (g->plot) lv_obj_invalidate(g->plot);
    lv_port_sem_give();
}

void trend_graph_set_unit(trend_graph_t *g, const char *unit) {
    if (!g || !unit || strcmp(g->unit, unit) == 0) return;
    lv_port_sem_take();
    strlcpy(g->unit, unit, sizeof(g->unit));
    if (g->plot) render_head(g);
    lv_port_sem_give();
}

void trend_graph_show_cursor_at(trend_graph_t *g, uint32_t timestamp) {
    if (!g) return;
    lv_port_sem_take();
    if (g->plot && !cursor_set(g, timestamp)) {
        g->cursor_tick = lv_tick_get(); /* same point: only keep it visible */
    }
    lv_port_sem_give();
}

void trend_graph_hide_cursor(trend_graph_t *g) {
    if (!g) return;
    lv_port_sem_take();
    cursor_clear(g);
    lv_port_sem_give();
}

void trend_graph_destroy(trend_graph_t *g) {
    if (!g) return;
    if (g->sub_id >= 0) {
        chart_query_scheduler_unsubscribe(g->sub_id);
        g->sub_id = -1;
    }
    lv_port_sem_take();
    if (g->timer) {
        lv_timer_delete(g->timer);
        g->timer = NULL;
    }
    if (g->container) {
        lv_obj_delete(g->container);
        g->container = NULL;
        g->plot = NULL;
    }
    lv_port_sem_give();
}
