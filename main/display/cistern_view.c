/**
 * @file cistern_view.c
 * @brief Modular Cistern View screen implementation (10x8 seamless grid).
 *        Combines vert_graph (4x5), keyvalue_card (6x5 with 5 rows), and multi-zone horiz_bargraph (10x3)
 *        with autonomous polling, instant cache population, and clean footer separation.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "cistern_view.h"
#include "vert_graph.h"
#include "keyvalue_card.h"
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
#include "../util/device_config_controller.h"
#include "lora_model.h"
#include "media_standards.h"
#include <math.h>

static const char *TAG = "cistern-view";

/* Primary Cistern Sensor Identification (matches Wasser_Hochbehaelter.json) */
#define CISTERN_DEV_ADDR               0x05975DC9
#define CISTERN_KEY_DEPTH              "depth"
#define CISTERN_FALLBACK_MIN_DEPTH_CM  0.0f
#define CISTERN_FALLBACK_MAX_DEPTH_CM  140.0f

/* Static widget instances */
static vert_graph_t      s_vert_graph;
static keyvalue_card_t   s_keyvalue_card;
static horiz_bargraph_t  s_horiz_graph;

static lv_obj_t         *s_view_parent = NULL;
static lv_timer_t       *s_clock_timer = NULL;
static esp_event_handler_instance_t s_telemetry_handler = NULL;

/* Subscription handles for chart query scheduler (Zone 0: 7d, Zone 1: 24h, Zone 2: 55m) */
static int8_t s_zone_subs[3] = {-1, -1, -1};

/* State flags */
static bool   s_initial_chart_loaded = false;
static bool   s_dev_cfg_applied = false;
static uint32_t s_media_rev_applied = 0; /* Last applied media_standards revision (0 = none) */
static time_t s_last_rx_time = 0;

/**
 * @brief Resolve display scale (from device limits) and depth alarm levels (from device thresholds).
 * @return true if device configuration was found in RAM, false if fallback defaults were used.
 */
static bool cistern_resolve_depth_config(float *out_min, float *out_max,
                                         float *out_alarm_min, float *out_alarm_max,
                                         float *out_hyst_min, float *out_hyst_max) {
    float min_val   = CISTERN_FALLBACK_MIN_DEPTH_CM;
    float max_val   = CISTERN_FALLBACK_MAX_DEPTH_CM;
    float alarm_min = 0.0f;
    float alarm_max = 0.0f;
    float hyst_min  = 0.0f;
    float hyst_max  = 0.0f;

    lora_device_config_t *dev = lora_model_get_device_by_addr(CISTERN_DEV_ADDR);
    if (!dev || !dev->enabled) {
        if (out_min) *out_min = min_val;
        if (out_max) *out_max = max_val;
        if (out_alarm_min) *out_alarm_min = alarm_min;
        if (out_alarm_max) *out_alarm_max = alarm_max;
        if (out_hyst_min) *out_hyst_min = hyst_min;
        if (out_hyst_max) *out_hyst_max = hyst_max;
        return false;
    }

    /* 1. Resolve display scale from device limits["depth"] (fallback: geometry.max_depth_cm) */
    bool has_limit = false;
    for (uint8_t l = 0; l < dev->limit_count; l++) {
        if (dev->limits[l].valid && strcmp(dev->limits[l].key, CISTERN_KEY_DEPTH) == 0) {
            min_val = dev->limits[l].val_min;
            max_val = dev->limits[l].val_max;
            has_limit = true;
            break;
        }
    }
    if (!has_limit && dev->geometry.max_depth_cm > 0.0f) {
        max_val = dev->geometry.max_depth_cm;
    }

    /* 2. Resolve depth thresholds (low / high) relative to resolved limit range */
    float span = max_val - min_val;
    for (uint8_t t = 0; t < dev->threshold_count; t++) {
        const lora_threshold_config_t *th = &dev->thresholds[t];
        if (!th->valid || strcmp(th->key, CISTERN_KEY_DEPTH) != 0) continue;

        float eval_val  = th->threshold_val;
        float eval_hyst = th->hysteresis_val;
        if (th->is_percent && span > 0.0f) {
            eval_val  = min_val + (th->threshold_val / 100.0f) * span;
            eval_hyst = (th->hysteresis_val / 100.0f) * span;
        }

        if (th->op == 0) { /* THRESHOLD_OP_LESS_THAN (falling / low level) */
            alarm_min = eval_val;
            hyst_min  = eval_hyst;
        } else {           /* THRESHOLD_OP_GREATER_THAN (rising / high level) */
            alarm_max = eval_val;
            hyst_max  = eval_hyst;
        }
    }

    if (out_min) *out_min = min_val;
    if (out_max) *out_max = max_val;
    if (out_alarm_min) *out_alarm_min = alarm_min;
    if (out_alarm_max) *out_alarm_max = alarm_max;
    if (out_hyst_min) *out_hyst_min = hyst_min;
    if (out_hyst_max) *out_hyst_max = hyst_max;
    return true;
}

static void cistern_update_last_rx_ui(time_t rx_ts) {
    if (rx_ts >= 1700000000UL) {
        struct tm rx_tm;
        localtime_r(&rx_ts, &rx_tm);
        char rx_buf[32];
        snprintf(rx_buf, sizeof(rx_buf), "%02d.%02d. %02d:%02d:%02d",
                 rx_tm.tm_mday, rx_tm.tm_mon + 1, rx_tm.tm_hour, rx_tm.tm_min, rx_tm.tm_sec);
        keyvalue_card_update_value(&s_keyvalue_card, 4, rx_buf);
    }
}

/* ── Autonomous 1-Second Timer Callback (Clock & Handshake Check) ──────── */

static void cistern_clock_timer_cb(lv_timer_t *timer) {
    time_t now = time(NULL);

    /* 0. Late-binding sync if device config arrived via UART after view init */
    if (!s_dev_cfg_applied) {
        float min_d, max_d, alm_min, alm_max, hyst_min, hyst_max;
        if (cistern_resolve_depth_config(&min_d, &max_d, &alm_min, &alm_max, &hyst_min, &hyst_max)) {
            s_dev_cfg_applied = true;
            vert_graph_set_range(&s_vert_graph, min_d, max_d);
            vert_graph_set_alarm_levels(&s_vert_graph, alm_min, alm_max, hyst_min, hyst_max);
            s_horiz_graph.min_value = min_d;
            s_horiz_graph.max_value = max_d;
            ESP_LOGI(TAG, "Applied device depth config: range=[%.1f..%.1f] cm, alarm_min=%.1f cm, alarm_max=%.1f cm",
                     min_d, max_d, alm_min, alm_max);
        }
    }

    /* 0b. Late-binding main color from the device medium (/gui/media_standards.json).
     *     Re-applied whenever the media table is reloaded from SD. */
    uint32_t media_rev = media_standards_get_revision();
    if (media_rev != 0 && media_rev != s_media_rev_applied) {
        lora_device_config_t *dev = lora_model_get_device_by_addr(CISTERN_DEV_ADDR);
        if (dev && dev->medium[0]) {
            lv_color_t color = lv_color_hex(media_get_color(dev->medium));
            vert_graph_set_media_color(&s_vert_graph, color);
            horiz_bargraph_set_media_color(&s_horiz_graph, color);
            s_media_rev_applied = media_rev;
            ESP_LOGI(TAG, "Applied media color for '%s': 0x%06lX",
                     dev->medium, (unsigned long)media_get_color(dev->medium));
        }
    }

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
    const char *zone_names[] = { "Zone 0 (7 Days)", "Zone 1 (24 Hours)", "Zone 2 (55 Min)" };
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
                

                /* DEBUG LOGGING */
                if (z == 2) {
                        ESP_LOGI(TAG, "  Slot %2u: %02d:%02d val=%.2f %s",
                                 i, tm_buf.tm_hour, tm_buf.tm_min, p->value, flag_str);
                    }
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
                        keyvalue_card_update_binding(&s_keyvalue_card, "depth", last_pt->value, "cm", is_est, is_alarm);

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

    /* 3. Autonomous Live Stale Timeout Check (Interval: 300s + 15s grace) */
    if (s_last_rx_time > 0 && (now - s_last_rx_time) > 315) {
        /* Dim vertical bar to 50% opacity and prepend ~ */
        vert_graph_set_status(&s_vert_graph, true, s_vert_graph.is_alarm);

        /* Dim live bar (Zone 3) to 50% opacity */
        chart_data_point_t stale_live_pt = {
            .timestamp = (uint32_t)now,
            .value = s_vert_graph.current_value,
            .flags = DATA_FLAG_ESTIMATED
        };
        if (s_vert_graph.is_alarm) {
            stale_live_pt.flags |= DATA_FLAG_ALARM_DANGER;
        }
        horiz_bargraph_update_zone_data(&s_horiz_graph, 3, &stale_live_pt, 1);
    }
}

/* ── User Interaction Handlers ─────────────────────────────────────────── */

static void cistern_view_on_threshold_updated(uint32_t dev_addr, const char *key, uint8_t op, float value, float hyst) {
    if (dev_addr == CISTERN_DEV_ADDR && (strcmp(key, "depth") == 0 || strcmp(key, "level") == 0)) {
        if (op == 0) {
            /* THRESHOLD_OP_LESS_THAN: Update dry-run minimum level marker */
            vert_graph_set_alarm_levels(&s_vert_graph, value, s_vert_graph.alarm_level_max, hyst, s_vert_graph.hysteresis_max);
        } else {
            /* THRESHOLD_OP_GREATER_THAN: Update overflow maximum level marker */
            vert_graph_set_alarm_levels(&s_vert_graph, s_vert_graph.alarm_level_min, value, s_vert_graph.hysteresis_min, hyst);
        }
    }
}

static void on_vert_graph_clicked(lv_event_t *e) {
    ESP_LOGI(TAG, "Level graph clicked! Opening device configuration controller...");
    device_config_controller_open_thresholds(CISTERN_DEV_ADDR);
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

    /* Context: 0 = measurement, 1 = diagnostic, 2 = history (sensor-side log replay, ignored for now).
     * A live packet is always a real value; estimated values only exist in the RP2040 archive. */
    if (telem->context == 2) return;
    /* This static view shows exactly one device; equal keys of other devices must not leak in */
    if (telem->dev_addr != CISTERN_DEV_ADDR) return;
    bool is_estimated = false;
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

    /* 2. Update Text Card (6x5 Grid) */
    keyvalue_card_update_binding(&s_keyvalue_card, telem->key, val, telem->unit, is_estimated, is_alarm);

    /* 3. Primary depth sensor updates: Vertical bar and Live Slot (Zone 3) */
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
    s_vert_graph.current_value = NAN;

    /* 1. Configure Header */
    gui_header_clear();
    gui_header_set_title("Hochbehälter");

    /* 2. Configure Clean Footer */
    s_initial_chart_loaded = false;
    gui_footer_set_left_text("Warte auf Zeitsync...");
    gui_footer_set_right_text("");

    /* Resolve dynamic scale and alarm boundaries from device configuration */
    float min_depth_cm = CISTERN_FALLBACK_MIN_DEPTH_CM;
    float max_depth_cm = CISTERN_FALLBACK_MAX_DEPTH_CM;
    float alarm_min_cm = 0.0f;
    float alarm_max_cm = 0.0f;
    float hyst_min_cm  = 0.0f;
    float hyst_max_cm  = 0.0f;
    s_dev_cfg_applied = cistern_resolve_depth_config(&min_depth_cm, &max_depth_cm,
                                                     &alarm_min_cm, &alarm_max_cm,
                                                     &hyst_min_cm, &hyst_max_cm);

    /* ========================================================================
     * 10x8 SEAMLESS GRID GEOMETRY CALCULATIONS
     * Cell = 48x48 px, Y_Offset = 64 px (Content Area: 480x384 px)
     * ======================================================================== */

    /* Widget 1: Vertical Level Bar (4 cols x 5 rows) -> (0, 64), 192 x 240 px */
    memset(&s_vert_graph, 0, sizeof(s_vert_graph));
    s_vert_graph.parent = parent;
    s_vert_graph.x = GRID_COORD_X(0);
    s_vert_graph.y = GRID_COORD_Y(0);
    s_vert_graph.width = GRID_SPAN_W(4);
    s_vert_graph.height = GRID_SPAN_H(5);
    s_vert_graph.min_value = min_depth_cm;
    s_vert_graph.max_value = max_depth_cm;
    s_vert_graph.current_value = 0.0f;

    s_vert_graph.alarm_level_min = alarm_min_cm;
    s_vert_graph.alarm_level_max = alarm_max_cm;
    s_vert_graph.hysteresis_min = hyst_min_cm;
    s_vert_graph.hysteresis_max = hyst_max_cm;
    s_vert_graph.alarm_color = lv_color_hex(0xE74C3C);

    strncpy(s_vert_graph.unit, "cm", sizeof(s_vert_graph.unit) - 1);
    s_vert_graph.bar_color = lv_color_hex(0x3498DB);
    s_vert_graph.click_cb = on_vert_graph_clicked;
    vert_graph_init(&s_vert_graph);

    /* Widget 2: Key-Value Telemetry Card (6 cols x 5 rows) -> (192, 64), 288 x 240 px */
    memset(&s_keyvalue_card, 0, sizeof(s_keyvalue_card));
    s_keyvalue_card.parent = parent;
    s_keyvalue_card.x = GRID_COORD_X(4);
    s_keyvalue_card.y = GRID_COORD_Y(0);
    s_keyvalue_card.width = GRID_SPAN_W(6);
    s_keyvalue_card.height = GRID_SPAN_H(5);
    s_keyvalue_card.title = "Telemetrie";
    keyvalue_card_init(&s_keyvalue_card);

    /* 5 rows: depth, volume, rate, flow, last_rx */
    keyvalue_card_add_pair(&s_keyvalue_card, "depth",      gui_i18n_get("telemetry.depth"),  "-- cm",   "cm",   lv_color_hex(0x52AAE5));
    keyvalue_card_add_pair(&s_keyvalue_card, "volume",     gui_i18n_get("telemetry.volume"), "-- m³",  "m³",   lv_color_hex(0x2ECC71));
    keyvalue_card_add_pair(&s_keyvalue_card, "level_gain", gui_i18n_get("telemetry.rate"),   "-- cm/h", "cm/h", lv_color_hex(0xF39C12));
    keyvalue_card_add_pair(&s_keyvalue_card, "flowrate",   gui_i18n_get("telemetry.flow"),   "-- m³/h", "m³/h", lv_color_hex(0x9B59B6));
    keyvalue_card_add_pair(&s_keyvalue_card, "last_rx",    "Letzter Empfang:",               "--",      "",     lv_color_hex(0xBDC3C7));

    /* Widget 3: 4-Zone Trend Bar Graph (10 cols x 3 rows) -> (0, 304), 480 x 144 px */
    memset(&s_horiz_graph, 0, sizeof(s_horiz_graph));
    s_horiz_graph.parent = parent;
    s_horiz_graph.x = GRID_COORD_X(0);
    s_horiz_graph.y = GRID_COORD_Y(5);
    s_horiz_graph.width = GRID_SPAN_W(10);
    s_horiz_graph.height = GRID_SPAN_H(3);
    s_horiz_graph.title = "Verlauf Pegel";
    s_horiz_graph.min_value = min_depth_cm;
    s_horiz_graph.max_value = max_depth_cm;
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
    s_zone_subs[0] = chart_query_scheduler_subscribe(CISTERN_DEV_ADDR, CISTERN_KEY_DEPTH, 0, 86400, 7, alarm_min_cm, alarm_max_cm);
    s_zone_subs[1] = chart_query_scheduler_subscribe(CISTERN_DEV_ADDR, CISTERN_KEY_DEPTH, 1, 7200, 12, alarm_min_cm, alarm_max_cm);
    s_zone_subs[2] = chart_query_scheduler_subscribe(CISTERN_DEV_ADDR, CISTERN_KEY_DEPTH, 2, 300, 11, alarm_min_cm, alarm_max_cm);

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

    /* Register runtime threshold listener to update level markers */
    device_config_controller_register_threshold_cb(cistern_view_on_threshold_updated);

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
    keyvalue_card_destroy(&s_keyvalue_card);
    horiz_bargraph_destroy(&s_horiz_graph);
    device_config_controller_register_threshold_cb(NULL);

    s_view_parent = NULL;
    ESP_LOGI(TAG, "Cistern View destroyed.");
}

void cistern_view_set_visible(bool visible) {
    lv_port_sem_take();
    if (visible) {
        if (s_vert_graph.container)   lv_obj_remove_flag(s_vert_graph.container, LV_OBJ_FLAG_HIDDEN);
        if (s_keyvalue_card.container) lv_obj_remove_flag(s_keyvalue_card.container, LV_OBJ_FLAG_HIDDEN);
        if (s_horiz_graph.container)  lv_obj_remove_flag(s_horiz_graph.container, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (s_vert_graph.container)   lv_obj_add_flag(s_vert_graph.container, LV_OBJ_FLAG_HIDDEN);
        if (s_keyvalue_card.container) lv_obj_add_flag(s_keyvalue_card.container, LV_OBJ_FLAG_HIDDEN);
        if (s_horiz_graph.container)  lv_obj_add_flag(s_horiz_graph.container, LV_OBJ_FLAG_HIDDEN);
    }
    lv_port_sem_give();
}