/**
 * @file cistern_view.c
 * @brief Modular Cistern View screen implementation (10x8 grid).
 *        Combines vert_graph (4x5), text_widget (6x5 with 5 rows), and multi-zone horiz_bargraph (10x3)
 *        with autonomous polling, instant cache population, and clean footer separation.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "cistern_view.h"
#include "vert_graph.h"
#include "text_widget.h"
#include "horiz_bargraph.h"
#include "gui_header.h"
#include "gui_footer.h"
#include "i18n/gui_i18n.h"
#include "view_data.h"
#include "rp2040.h"
#include "lv_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "chart_query_scheduler.h"

static const char *TAG = "cistern-view";

/* Primary Cistern Sensor Identification (matches Wasser_Hochbehaelter.json) */
#define CISTERN_DEV_ADDR       0x05975DC9
#define CISTERN_KEY_DEPTH      "depth"
#define CISTERN_MAX_DEPTH_CM   140.0f
#define CISTERN_ALARM_MIN_CM   21.0f  /* 15% dry run protection */

/* Static widget instances */
static vert_graph_t      s_vert_graph;
static text_widget_t     s_text_widget;
static horiz_bargraph_t  s_horiz_graph;

static lv_obj_t         *s_view_parent = NULL;
static lv_timer_t       *s_clock_timer = NULL;
static esp_event_handler_instance_t s_telemetry_handler = NULL;

/* Subscription handles for chart query scheduler (Zone 0: 7d, Zone 1: 24h, Zone 2: 55m) */
static int8_t s_zone_subs[3] = {-1, -1, -1};

/* State flags */
static bool   s_initial_chart_loaded = false;
static time_t s_last_rx_time = 0;

void horiz_bargraph_clear_all(horiz_bargraph_t *graph) {
    if (!graph || !graph->chart) return;
    for (uint8_t z = 0; z < graph->zone_count; z++) {
        horiz_bargraph_update_zone_data(graph, z, NULL, 0);
    }
}

static void cistern_update_last_rx_ui(time_t rx_ts) {
    if (rx_ts >= 1700000000UL) {
        struct tm rx_tm;
        localtime_r(&rx_ts, &rx_tm);
        char rx_buf[32];
        snprintf(rx_buf, sizeof(rx_buf), "%02d.%02d. %02d:%02d:%02d",
                 rx_tm.tm_mday, rx_tm.tm_mon + 1, rx_tm.tm_hour, rx_tm.tm_min, rx_tm.tm_sec);
        text_widget_update_value(&s_text_widget, 4, rx_buf);
    }
}

/* ── Autonomous 1-Second Timer Callback (Clock & Handshake Check) ──────── */

static void cistern_clock_timer_cb(lv_timer_t *timer) {
    time_t now = time(NULL);

    /* 1. Update Footer Clock (Left Side: Date & Time) */
    if (now >= 1700000000UL) {
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        char date_buf[32];
        snprintf(date_buf, sizeof(date_buf), "%02d.%02d.%04d  %02d:%02d:%02d",
                 tm_info.tm_mday, tm_info.tm_mon + 1, tm_info.tm_year + 1900,
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        gui_footer_set_left_text(date_buf);
    } else {
        gui_footer_set_left_text("Warte auf Zeitsync...");
    }

    /* 2. Check for completed chart archive responses via non-blocking handshake */
    const char *zone_names[] = { "Zone 0 (7 Tage)", "Zone 1 (24 Std)", "Zone 2 (55 Min)" };
    for (uint8_t z = 0; z < 3; z++) {
        if (s_zone_subs[z] >= 0 && chart_query_scheduler_is_data_ready(s_zone_subs[z])) {
            chart_data_point_t pts[32];
            uint16_t count = 0;
            if (chart_query_scheduler_consume_data(s_zone_subs[z], pts, 32, &count)) {
                ESP_LOGI(TAG, "── UART RX: %s (count=%u) ──", zone_names[z], count);

                for (uint16_t i = 0; i < count; i++) {
                    const chart_data_point_t *p = &pts[i];
                    time_t pt_ts = (time_t)p->timestamp;
                    struct tm tm_buf;
                    localtime_r(&pt_ts, &tm_buf);

                    char flag_str[32] = "";
                    if (p->flags & DATA_FLAG_NO_DATA) {
                        strcat(flag_str, "[NO_DATA/GAP] ");
                    } else {
                        if (p->flags & DATA_FLAG_ESTIMATED) strcat(flag_str, "[ESTIMATED] ");
                        if (p->flags & DATA_FLAG_AVERAGED)  strcat(flag_str, "[AVERAGED] ");
                        if (p->flags & DATA_FLAG_ALARM_DANGER) strcat(flag_str, "[ALARM] ");
                        if (p->flags == 0) strcat(flag_str, "[REAL] ");
                    }

                    ESP_LOGI(TAG, "  Slot [%02u] %02d.%02d. %02d:%02d:%02d UTC=%lu -> Val: %6.2f | Flags: 0x%02X %s",
                             i, tm_buf.tm_mday, tm_buf.tm_mon + 1, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                             (unsigned long)p->timestamp, p->value, p->flags, flag_str);
                }

                horiz_bargraph_update_zone_data(&s_horiz_graph, z, pts, count);
                s_initial_chart_loaded = true;

                /* Cold-start restore: restore depth and timestamp from newest non-gap point of Zone 2 */
                if (z == 2 && s_last_rx_time == 0) {
                    int valid_idx = -1;
                    for (int k = (int)count - 1; k >= 0; k--) {
                        if (!(pts[k].flags & DATA_FLAG_NO_DATA)) {
                            valid_idx = k;
                            break;
                        }
                    }

                    if (valid_idx >= 0) {
                        const chart_data_point_t *last_pt = &pts[valid_idx];
                        bool is_est = (last_pt->flags & DATA_FLAG_ESTIMATED) != 0;
                        bool is_alarm = (last_pt->flags & (DATA_FLAG_ALARM_DANGER | DATA_FLAG_ALARM_WARN)) != 0;

                        vert_graph_update_value(&s_vert_graph, last_pt->value, "cm", is_est, is_alarm);
                        text_widget_update_binding(&s_text_widget, "depth", last_pt->value, "cm", is_est, is_alarm);

                        horiz_bargraph_update_zone_data(&s_horiz_graph, 3, last_pt, 1);

                        s_last_rx_time = (time_t)last_pt->timestamp;
                        cistern_update_last_rx_ui(s_last_rx_time);
                        ESP_LOGI(TAG, "Restored latest depth from archive: %.2f cm (ts: %lu, slot: %d)", 
                                 last_pt->value, (unsigned long)last_pt->timestamp, valid_idx);
                    }
                }
            }
        }
    }
}

/* ── User Interaction Handlers ─────────────────────────────────────────── */

static void on_vert_graph_clicked(lv_event_t *e) {
    ESP_LOGI(TAG, "Level graph touched! Context: [DevAddr: 0x%08X, Key: %s, Value: %.2f cm]",
             (unsigned int)CISTERN_DEV_ADDR, CISTERN_KEY_DEPTH, s_vert_graph.current_value);
}

/* ── Event Listener: Live Telemetry Dispatcher ─────────────────────────── */

static void cistern_telemetry_event_handler(void *handler_args, esp_event_base_t base, 
                                            int32_t id, void *event_data) {
    if (id != VIEW_EVENT_LORA_TELEMETRY || !event_data) return;

    struct view_data_lora_telemetry *telem = (struct view_data_lora_telemetry *)event_data;

    float val = 0.0f;
    if (telem->val_type == LORA_VAL_TYPE_FLOAT) {
        val = telem->val.f_val;
    } else if (telem->val_type == LORA_VAL_TYPE_INT) {
        val = (float)telem->val.i_val;
    } else {
        return;
    }

    ESP_LOGI(TAG, "Telemetry received: key='%s', val=%.2f %s", telem->key, val, telem->unit);

    bool is_estimated = (telem->context == 2);
    bool is_alarm = false;

    /* 1. Evaluate alarm threshold for primary depth/level sensor */
    bool is_depth = (strcmp(telem->key, "depth") == 0 || strcmp(telem->key, "level") == 0);
    if (is_depth) {
        if (s_vert_graph.alarm_level_min > 0.0f && val < s_vert_graph.alarm_level_min) {
            is_alarm = true;
        }
        if (s_vert_graph.alarm_level_max > 0.0f && val > s_vert_graph.alarm_level_max) {
            is_alarm = true;
        }
    }

    /* 2. Update Text Card (6x5) */
    text_widget_update_binding(&s_text_widget, telem->key, val, telem->unit, is_estimated, is_alarm);

    /* 3. Primary depth sensor updates: Vertical bar and Live-Slot (Zone 3) only */
    if (is_depth) {
        vert_graph_update_value(&s_vert_graph, val, telem->unit, is_estimated, is_alarm);

        /* Update Live-Bargraph in Zone 3 (most-right bar) ONLY for depth */
        chart_data_point_t live_pt = {
            .timestamp = (uint32_t)time(NULL),
            .value = val,
            .flags = 0
        };
        if (is_alarm) live_pt.flags |= DATA_FLAG_ALARM_DANGER;
        if (is_estimated) live_pt.flags |= DATA_FLAG_ESTIMATED;

        horiz_bargraph_update_zone_data(&s_horiz_graph, 3, &live_pt, 1);
    }

    /* 4. Record last telemetry reception timestamp and update 5th row */
    s_last_rx_time = time(NULL);
    cistern_update_last_rx_ui(s_last_rx_time);
}

/* ── View Lifecycle ────────────────────────────────────────────────────── */

int cistern_view_init(void) {
    lv_obj_t *parent = lv_screen_active();
    if (!parent) return -1;
    s_view_parent = parent;

    /* 1. Configure Header */
    gui_header_clear();
    gui_header_set_title("Hochbehälter");

    /* 2. Configure Clean Footer */
    s_initial_chart_loaded = false;
    gui_footer_set_left_text("Warte auf Zeitsync...");
    gui_footer_set_right_text(""); /* Clean: Right side unused */

    /* ========================================================================
     * 10x8 GRID GEOMETRY CALCULATIONS (Cell = 48px, Bezel = 4px, Y_Offset = 69)
     * ======================================================================== */

    /* Widget 1: Vertical Level Bar (4x5 Grid) -> 184 x 232 px */
    memset(&s_vert_graph, 0, sizeof(s_vert_graph));
    s_vert_graph.parent = parent;
    s_vert_graph.x = 4;
    s_vert_graph.y = 73;
    s_vert_graph.width = 184;
    s_vert_graph.height = 232;
    s_vert_graph.min_value = 0.0f;
    s_vert_graph.max_value = CISTERN_MAX_DEPTH_CM;
    s_vert_graph.current_value = 0.0f;
    
    s_vert_graph.alarm_level_min = CISTERN_ALARM_MIN_CM;
    s_vert_graph.alarm_level_max = 0.0f;
    s_vert_graph.alarm_color = lv_color_hex(0xE74C3C);
    
    strncpy(s_vert_graph.unit, "cm", sizeof(s_vert_graph.unit) - 1);
    s_vert_graph.bar_color = lv_color_hex(0x3498DB);
    s_vert_graph.click_cb = on_vert_graph_clicked;
    vert_graph_init(&s_vert_graph);

    /* Widget 2: Text Card (6x5 Grid) -> 280 x 232 px */
    memset(&s_text_widget, 0, sizeof(s_text_widget));
    s_text_widget.parent = parent;
    s_text_widget.x = 196;
    s_text_widget.y = 73;
    s_text_widget.width = 280;
    s_text_widget.height = 232;
    s_text_widget.title = "Telemetrie";
    text_widget_init(&s_text_widget);

    /* 5 rows: depth, volume, rate, flow, last_rx */
    text_widget_add_pair(&s_text_widget, "depth",      gui_i18n_get("telemetry.depth"),  "-- cm",   "cm",   lv_color_hex(0x52AAE5));
    text_widget_add_pair(&s_text_widget, "volume",     gui_i18n_get("telemetry.volume"), "-- m³",  "m³",   lv_color_hex(0x2ECC71));
    text_widget_add_pair(&s_text_widget, "level_gain", gui_i18n_get("telemetry.rate"),   "-- cm/h", "cm/h", lv_color_hex(0xF39C12));
    text_widget_add_pair(&s_text_widget, "flowrate",   gui_i18n_get("telemetry.flow"),   "-- m³/h", "m³/h", lv_color_hex(0x9B59B6));
    text_widget_add_pair(&s_text_widget, "last_rx",    "Letzter Empfang:",               "--",      "",     lv_color_hex(0xBDC3C7));

    /* Widget 3: 3-Zone Multi-Resolution Trend Bar (10x3 Grid) -> 472 x 136 px */
    memset(&s_horiz_graph, 0, sizeof(s_horiz_graph));
    s_horiz_graph.parent = parent;
    s_horiz_graph.x = 4;
    s_horiz_graph.y = 313;
    s_horiz_graph.width = 472;
    s_horiz_graph.height = 136;
    s_horiz_graph.title = "Verlauf Pegel";
    s_horiz_graph.min_value = 0.0f;
    s_horiz_graph.max_value = CISTERN_MAX_DEPTH_CM;
    s_horiz_graph.bar_color = lv_color_hex(0x3498DB);
    s_horiz_graph.chart_type = HORIZ_BARGRAPH_TYPE_BAR;
    horiz_bargraph_init(&s_horiz_graph);

    /* Configure 4 zones: 7 (days) + 12 (2h) + 11 (5min) + 1 (live) = 31 bars */
    const horiz_bargraph_zone_t zones[4] = {
        { .count = 7,  .title = "7 Tage <" },
        { .count = 12, .title = "24 Std <" },
        { .count = 11, .title = "1 Std <" },
        { .count = 1,  .title = ""       } /* Live zone: no text, separated by vertical line */
    };
    horiz_bargraph_set_zones(&s_horiz_graph, zones, 4);
    horiz_bargraph_clear_all(&s_horiz_graph);
    s_initial_chart_loaded = false;

    /* Register subscriptions in chart_query_scheduler */
    s_zone_subs[0] = chart_query_scheduler_subscribe(CISTERN_DEV_ADDR, CISTERN_KEY_DEPTH, 0, 86400, 7, 0.0f, CISTERN_MAX_DEPTH_CM);
    s_zone_subs[1] = chart_query_scheduler_subscribe(CISTERN_DEV_ADDR, CISTERN_KEY_DEPTH, 1, 7200, 12, 0.0f, CISTERN_MAX_DEPTH_CM);
    s_zone_subs[2] = chart_query_scheduler_subscribe(CISTERN_DEV_ADDR, CISTERN_KEY_DEPTH, 2, 300, 11, 0.0f, CISTERN_MAX_DEPTH_CM);

    /* Activate subscriptions for this view */
    for (int i = 0; i < 3; i++) {
        if (s_zone_subs[i] >= 0) {
            chart_query_scheduler_set_active(s_zone_subs[i], true);
        }
    }

    /* 3. Register Event Listener for LoRa Telemetry */
    esp_event_handler_instance_register_with(
        view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_LORA_TELEMETRY,
        cistern_telemetry_event_handler, NULL, &s_telemetry_handler);

    /* 4. Start 1-Second Autonomous Timer (Clock + Handshake polling) */
    s_clock_timer = lv_timer_create(cistern_clock_timer_cb, 1000, NULL);

    ESP_LOGI(TAG, "Cistern View initialized.");
    return 0;
}

void cistern_view_destroy(void) {
    if (s_clock_timer) {
        lv_timer_delete(s_clock_timer);
        s_clock_timer = NULL;
    }

    /* Deactivate chart subscriptions */
    for (int i = 0; i < 3; i++) {
        if (s_zone_subs[i] >= 0) {
            chart_query_scheduler_set_active(s_zone_subs[i], false);
        }
    }

    if (s_telemetry_handler) {
        esp_event_handler_instance_unregister_with(view_event_handle, VIEW_EVENT_BASE, 
                                                   VIEW_EVENT_LORA_TELEMETRY, s_telemetry_handler);
        s_telemetry_handler = NULL;
    }

    vert_graph_destroy(&s_vert_graph);
    text_widget_destroy(&s_text_widget);
    horiz_bargraph_destroy(&s_horiz_graph);

    s_view_parent = NULL;
    ESP_LOGI(TAG, "Cistern View destroyed.");
}