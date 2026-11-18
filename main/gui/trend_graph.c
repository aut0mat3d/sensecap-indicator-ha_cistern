/**
 * @file trend_graph.c
 * @brief Generic long-term trend widget (see trend_graph.h).
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#include "trend_graph.h"
#include "lv_port.h"
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
#define TG_DEFAULT_COLOR   lv_color_hex(0x3498DB)
#define TG_Y_LABEL_W       40      /* left column for the Y labels */
#define TG_HEAD_H          26
#define TG_X_LABEL_H       18
#define TG_PLOT_PAD        6       /* keeps points at the border fully visible */
#define TG_POLL_PERIOD_MS  500
#define TG_FRAME_TICK_MS   60000   /* moves the "now" edge of the frame */

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

/* ── Frame & scale ─────────────────────────────────────────────────────── */

static void update_frame(trend_graph_t *g) {
    if (g->custom_range || g->preset_count == 0) return;
    const trend_range_preset_t *p = &g->presets[g->preset_idx];
    uint32_t window = TREND_GRAPH_LTTB_POINTS * lttb_interval(p->step_sec * p->point_count);
    uint32_t now = now_utc();
    g->frame_end = now;
    g->frame_start = (now > window) ? now - window : 0;
}

static void compute_scale(trend_graph_t *g) {
    if (g->max_val > g->min_val) {
        g->scale_min = g->min_val;
        g->scale_max = g->max_val;
        return;
    }
    /* Automatic: range of the valid points plus 10 % margin */
    bool any = false;
    float lo = 0.0f, hi = 0.0f;
    for (uint16_t i = 0; i < g->point_count; i++) {
        const chart_data_point_t *p = &g->points[i];
        if (p->flags & DATA_FLAG_NO_DATA) continue;
        if (!any || p->value < lo) lo = p->value;
        if (!any || p->value > hi) hi = p->value;
        any = true;
    }
    if (!any) {
        lo = 0.0f;
        hi = 100.0f;
    }
    float margin = (hi - lo) * 0.1f;
    if (margin < 1.0f) margin = 1.0f;
    g->scale_min = lo - margin;
    g->scale_max = hi + margin;
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

/* ── Rendering (LVGL lock held) ────────────────────────────────────────── */

static void render_head(trend_graph_t *g) {
    char buf[64];
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

static void render(trend_graph_t *g) {
    if (!g->plot) return;
    update_frame(g);
    compute_scale(g);

    char buf[24];
    float range = g->scale_max - g->scale_min;
    const char *fmt = (range < 10.0f) ? "%.1f" : "%.0f";
    snprintf(buf, sizeof(buf), fmt, (double)g->scale_max);
    lv_label_set_text(g->lbl_y_max, buf);
    snprintf(buf, sizeof(buf), fmt, (double)g->scale_min);
    lv_label_set_text(g->lbl_y_min, buf);

    uint32_t window = g->frame_end - g->frame_start;
    format_time(buf, sizeof(buf), g->frame_start, window);
    lv_label_set_text(g->lbl_x_start, buf);
    format_time(buf, sizeof(buf), g->frame_end, window);
    lv_label_set_text(g->lbl_x_end, buf);

    render_head(g);
    lv_obj_invalidate(g->plot);
}

/* Custom drawing of the plot area: level lines, line segments, points */
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

    gui_theme_colors_t theme;
    gui_theme_get_colors(&theme);

    /* 1. Level lines (thresholds) */
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.width = 1;
    line.opa = LV_OPA_80;
    for (uint8_t i = 0; i < g->line_count; i++) {
        float v = g->lines[i].value;
        if (v < g->scale_min || v > g->scale_max) continue;
        int32_t y = y0 + h - (int32_t)lroundf((v - g->scale_min) / range * (float)h);
        line.color = lv_color_hex(gui_theme_alarm_level_color(g->lines[i].level, TG_LINE_NEUTRAL));
        line.p1.x = a.x1;
        line.p1.y = y;
        line.p2.x = a.x2;
        line.p2.y = y;
        lv_draw_line(layer, &line);
    }

    /* 2. Line segments between neighbouring points (a gap marker breaks the line) */
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
        if ((p->flags & DATA_FLAG_NO_DATA) || p->timestamp < g->frame_start || p->timestamp > g->frame_end) {
            have_prev = false;
            continue;
        }
        float v = p->value < g->scale_min ? g->scale_min : (p->value > g->scale_max ? g->scale_max : p->value);
        int32_t x = x0 + (int32_t)lround((double)(p->timestamp - g->frame_start) / span * (double)w);
        int32_t y = y0 + h - (int32_t)lroundf((v - g->scale_min) / range * (float)h);
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

    /* 3. Points (smaller when dense) */
    int32_t r = (g->point_count > 40) ? 2 : 3;
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    dot.border_width = 0;
    for (uint16_t i = 0; i < g->point_count; i++) {
        const chart_data_point_t *p = &g->points[i];
        if ((p->flags & DATA_FLAG_NO_DATA) || p->timestamp < g->frame_start || p->timestamp > g->frame_end) continue;
        float v = p->value < g->scale_min ? g->scale_min : (p->value > g->scale_max ? g->scale_max : p->value);
        int32_t x = x0 + (int32_t)lround((double)(p->timestamp - g->frame_start) / span * (double)w);
        int32_t y = y0 + h - (int32_t)lroundf((v - g->scale_min) / range * (float)h);
        if (p->flags & DATA_FLAG_ALARM_DANGER) dot.bg_color = lv_color_hex(theme.alarm_danger);
        else if (p->flags & DATA_FLAG_ALARM_WARN) dot.bg_color = lv_color_hex(theme.alarm_warning);
        else dot.bg_color = g->point_color;
        dot.bg_opa = (p->flags & DATA_FLAG_ESTIMATED) ? LV_OPA_50 : LV_OPA_COVER;
        lv_area_t pa = { x - r, y - r, x + r, y + r };
        lv_draw_rect(layer, &dot, &pa);
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
                render(g);
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
}

static void click_cb(lv_event_t *e) {
    trend_graph_t *g = (trend_graph_t *)lv_event_get_user_data(e);
    if (g) trend_graph_next_preset(g);
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
    lv_obj_set_style_radius(g->container, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g->container, TG_BG_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(g->container, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(g->container, TG_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_outline_width(g->container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g->container, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(g->container, click_cb, LV_EVENT_CLICKED, g);

    /* Layout derived from the size: head line, Y label column, plot, X label row */
    int32_t inner_w = g->width - 2 * 2 - 2 * 6;
    int32_t inner_h = g->height - 2 * 2 - 2 * 6;
    int32_t plot_x = TG_Y_LABEL_W;
    int32_t plot_y = TG_HEAD_H;
    int32_t plot_w = inner_w - plot_x;
    int32_t plot_h = inner_h - TG_HEAD_H - TG_X_LABEL_H;

    g->lbl_head = lv_label_create(g->container);
    lv_obj_set_style_text_font(g->lbl_head, &ui_font_font0, LV_PART_MAIN);
    lv_obj_set_style_text_color(g->lbl_head, TG_HEAD_COLOR, LV_PART_MAIN);
    lv_obj_set_pos(g->lbl_head, 2, 0);

    g->lbl_y_max = lv_label_create(g->container);
    g->lbl_y_min = lv_label_create(g->container);
    g->lbl_x_start = lv_label_create(g->container);
    g->lbl_x_end = lv_label_create(g->container);
    lv_obj_t *small[] = { g->lbl_y_max, g->lbl_y_min, g->lbl_x_start, g->lbl_x_end };
    for (size_t i = 0; i < sizeof(small) / sizeof(small[0]); i++) {
        lv_obj_set_style_text_font(small[i], &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(small[i], TG_TEXT_COLOR, LV_PART_MAIN);
    }
    lv_obj_set_width(g->lbl_y_max, TG_Y_LABEL_W - 4);
    lv_obj_set_width(g->lbl_y_min, TG_Y_LABEL_W - 4);
    lv_obj_set_style_text_align(g->lbl_y_max, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_align(g->lbl_y_min, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_pos(g->lbl_y_max, 0, plot_y + TG_PLOT_PAD - 8);
    lv_obj_set_pos(g->lbl_y_min, 0, plot_y + plot_h - TG_PLOT_PAD - 8);
    lv_obj_set_pos(g->lbl_x_start, plot_x, plot_y + plot_h + 2);
    lv_obj_align(g->lbl_x_end, LV_ALIGN_TOP_RIGHT, 0, plot_y + plot_h + 2);

    g->plot = lv_obj_create(g->container);
    lv_obj_remove_style_all(g->plot);
    lv_obj_set_size(g->plot, plot_w, plot_h);
    lv_obj_set_pos(g->plot, plot_x, plot_y);
    lv_obj_set_style_bg_color(g->plot, TG_PLOT_BG_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g->plot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g->plot, 4, LV_PART_MAIN);
    lv_obj_remove_flag(g->plot, LV_OBJ_FLAG_CLICKABLE); /* clicks reach the container */
    lv_obj_add_event_cb(g->plot, plot_draw_cb, LV_EVENT_DRAW_POST, g);

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
