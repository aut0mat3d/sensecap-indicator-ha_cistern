/**
 * @file alarm_model.c
 * @brief Alarm system implementation: event journal, active state tracking, and SD persistence.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "alarm_model.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "gui_header.h"
#include "gui/i18n/gui_i18n.h"
#include "alarm_view.h"
#include "rp2040.h"
#include "view_data.h"
#include "alarm_evaluator.h"

static const char* TAG = "alarm_model";

#define ALARM_PERSIST_SETTLE_MS    5000      /* 5 seconds debounce window for alarm bursts & state settling */
#define ALARMS_PER_UART_CHUNK      1         /* 1 entry (184 B) + 4 B header = 188 B per UART chunk */
#define MAX_ACTIVE_CONDITIONS      16        /* Maximum distinct active alarm conditions tracked */

/* Forward declarations */
static void _alarm_persist_timer_cb(void *arg);
static void _alarm_shutdown_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data);
static void _alarm_mark_dirty(void);

extern int rp2040_alarm_set(uint16_t t_on_ms, uint16_t t_off_ms);
extern int rp2040_alarm_mute(uint32_t mute_sec);
extern int rp2040_alarm_rearm(void);
extern int indicator_display_on(void);
extern bool indicator_display_st_get(void);

// ============================================================================
// Internal State
// ============================================================================

/* Active condition tracking structure (independent of historical log) */
typedef struct {
    char             source[ALARM_SOURCE_MAX_LEN];
    char             rule_id[ALARM_SOURCE_MAX_LEN];
    alarm_severity_t severity;
    bool             is_active;
    bool             is_acknowledged;
    uint8_t          buzzer_mode;
} active_condition_t;

static active_condition_t s_active_conditions[MAX_ACTIVE_CONDITIONS];
static size_t             s_active_conditions_count = 0;

static bool s_sd_card_ready = false;
static bool s_history_loaded = false;

/* Append-only event journal ringbuffer */
static alarm_entry_t      s_ringbuffer[ALARM_RINGBUFFER_SIZE];
static size_t             s_ringbuffer_count = 0;
static size_t             s_ringbuffer_head = 0;
static size_t             s_ringbuffer_tail = 0;

static bool               s_history_dirty = false;
static int64_t            s_dirty_time_ms = 0;     /* Monotonic uptime timestamp in milliseconds */
static esp_timer_handle_t s_persist_timer = NULL;

static alarm_ui_config_t  s_ui_config;
static uint32_t           s_mute_until = 0;
static bool               s_has_active_alarms = false;
static uint8_t            s_current_buzzer_pattern = 0xFF;

// ============================================================================
// UI & Configuration Helpers
// ============================================================================

static uint32_t _parse_hex_color(const char *hex_str, uint32_t fallback) {
    if (!hex_str || hex_str[0] == '\0') return fallback;
    if (strncmp(hex_str, "0x", 2) == 0 || strncmp(hex_str, "0X", 2) == 0) hex_str += 2;
    char *endptr = NULL;
    uint32_t val = (uint32_t)strtoul(hex_str, &endptr, 16);
    return (endptr != hex_str) ? val : fallback;
}

static alarm_ui_config_t _get_default_ui_config(void) {
    alarm_ui_config_t cfg;

    cfg.colors.color_danger       = _parse_hex_color(gui_i18n_get("colors.color_danger"), 0xE74C3C);
    cfg.colors.color_warning      = _parse_hex_color(gui_i18n_get("colors.color_warning"), 0xF39C12);
    cfg.colors.color_info         = _parse_hex_color(gui_i18n_get("colors.color_info"), 0x9E9E9E);
    cfg.colors.color_incoming     = _parse_hex_color(gui_i18n_get("colors.color_incoming"), 0xE74C3C);
    cfg.colors.color_going        = _parse_hex_color(gui_i18n_get("colors.color_going"), 0x2ECC71);
    cfg.colors.color_muted        = _parse_hex_color(gui_i18n_get("colors.color_muted"), 0x95A5A6);
    cfg.colors.color_retriggered  = _parse_hex_color(gui_i18n_get("colors.color_retriggered"), 0x3498DB);
    cfg.colors.color_acknowledged = _parse_hex_color(gui_i18n_get("colors.color_acknowledged"), 0x7F8C8D);

    strlcpy(cfg.texts.danger,       gui_i18n_get("texts.danger"), sizeof(cfg.texts.danger));
    strlcpy(cfg.texts.warning,      gui_i18n_get("texts.warning"), sizeof(cfg.texts.warning));
    strlcpy(cfg.texts.info,         gui_i18n_get("texts.info"), sizeof(cfg.texts.info));
    strlcpy(cfg.texts.incoming,     gui_i18n_get("texts.incoming"), sizeof(cfg.texts.incoming));
    strlcpy(cfg.texts.going,        gui_i18n_get("texts.going"), sizeof(cfg.texts.going));
    strlcpy(cfg.texts.muted,        gui_i18n_get("texts.muted"), sizeof(cfg.texts.muted));
    strlcpy(cfg.texts.retriggered,  gui_i18n_get("texts.retriggered"), sizeof(cfg.texts.retriggered));
    strlcpy(cfg.texts.acknowledged, gui_i18n_get("texts.acknowledged"), sizeof(cfg.texts.acknowledged));

    strlcpy(cfg.hw_button_behavior, gui_i18n_get("button.hw_button_behavior"), sizeof(cfg.hw_button_behavior));
    if (strlen(cfg.hw_button_behavior) == 0 || strcmp(cfg.hw_button_behavior, "button.hw_button_behavior") == 0) {
        strlcpy(cfg.hw_button_behavior, "MUTE_TIME", sizeof(cfg.hw_button_behavior));
    }

    const char *dur_str = gui_i18n_get("button.mute_duration_sec");
    if (dur_str && strcmp(dur_str, "button.mute_duration_sec") != 0 && atoi(dur_str) > 0) {
        cfg.mute_duration_sec = (uint32_t)atoi(dur_str);
    } else {
        cfg.mute_duration_sec = 7200;
    }

    return cfg;
}

bool alarm_time_is_valid(void) {
    time_t now = time(NULL);
    return (now >= 1700000000UL);
}

// ============================================================================
// Journal Ringbuffer Management (Append-Only)
// ============================================================================

static void _ringbuffer_append(const alarm_entry_t* entry) {
    if (s_ringbuffer_count >= ALARM_RINGBUFFER_SIZE) {
        s_ringbuffer_head = (s_ringbuffer_head + 1) % ALARM_RINGBUFFER_SIZE;
        s_ringbuffer_count--;
    }
    memcpy(&s_ringbuffer[s_ringbuffer_tail], entry, sizeof(alarm_entry_t));
    s_ringbuffer_tail = (s_ringbuffer_tail + 1) % ALARM_RINGBUFFER_SIZE;
    s_ringbuffer_count++;
}

static alarm_entry_t* _ringbuffer_get(size_t index) {
    if (index >= s_ringbuffer_count) return NULL;
    size_t actual_index = (s_ringbuffer_tail - 1 - index + ALARM_RINGBUFFER_SIZE) % ALARM_RINGBUFFER_SIZE;
    return &s_ringbuffer[actual_index];
}

/* Helper to strip measurement value from message string e.g. "alarm_low (10.0)" -> "alarm_low" */
static void _extract_base_rule_id(const char *msg, char *out_buf, size_t out_size) {
    if (!msg || !out_buf || out_size == 0) return;
    strlcpy(out_buf, msg, out_size);
    char *paren = strstr(out_buf, " (");
    if (paren) {
        *paren = '\0';
    }
}

static active_condition_t* _find_active_condition(const char *source, const char *base_rule) {
    for (size_t i = 0; i < s_active_conditions_count; i++) {
        if (strcmp(s_active_conditions[i].source, source) == 0 &&
            strcmp(s_active_conditions[i].rule_id, base_rule) == 0) {
            return &s_active_conditions[i];
        }
    }
    return NULL;
}

static active_condition_t* _get_or_create_condition(const char *source, const char *base_rule, alarm_severity_t severity) {
    active_condition_t *cond = _find_active_condition(source, base_rule);
    if (cond) return cond;

    if (s_active_conditions_count < MAX_ACTIVE_CONDITIONS) {
        cond = &s_active_conditions[s_active_conditions_count++];
        memset(cond, 0, sizeof(active_condition_t));
        strlcpy(cond->source, source, sizeof(cond->source));
        strlcpy(cond->rule_id, base_rule, sizeof(cond->rule_id));
        cond->severity = severity;
        return cond;
    }
    return NULL;
}

// ============================================================================
// Persistence Management
// ============================================================================

bool alarm_history_load(void) {
    int ret = rp2040_alarm_request_load();
    return (ret > 0);
}

bool alarm_history_save(void) {
    if (!s_history_dirty) {
        return true;
    }

    if (s_ringbuffer_count == 0) {
        rp2040_alarm_send_save_chunk(0, 1, 0, NULL);
    } else {
        uint8_t total_chunks = (uint8_t)((s_ringbuffer_count + ALARMS_PER_UART_CHUNK - 1) / ALARMS_PER_UART_CHUNK);
        alarm_entry_t chunk_buf[ALARMS_PER_UART_CHUNK];

        for (uint8_t chunk_idx = 0; chunk_idx < total_chunks; chunk_idx++) {
            size_t start_idx = chunk_idx * ALARMS_PER_UART_CHUNK;
            size_t remaining = s_ringbuffer_count - start_idx;
            uint8_t count_in_chunk = (remaining < ALARMS_PER_UART_CHUNK) ? (uint8_t)remaining : ALARMS_PER_UART_CHUNK;

            for (uint8_t i = 0; i < count_in_chunk; i++) {
                alarm_entry_t *entry = _ringbuffer_get(s_ringbuffer_count - 1 - (start_idx + i));
                if (entry) {
                    memcpy(&chunk_buf[i], entry, sizeof(alarm_entry_t));
                }
            }

            rp2040_alarm_send_save_chunk(chunk_idx, total_chunks, count_in_chunk, chunk_buf);
        }
    }

    s_history_dirty = false;
    ESP_LOGI(TAG, "Dispatched %u alarm journal entries to RP2040 SD card", (unsigned)s_ringbuffer_count);
    return true;
}

static void _alarm_mark_dirty(void) {
    s_dirty_time_ms = esp_timer_get_time() / 1000ULL;
    s_history_dirty = true;
}

void alarm_model_on_history_chunk_received(const uint8_t *data, size_t len) {
    if (!data || len < 3) return;

    uint8_t chunk_idx    = data[0];
    uint8_t total_chunks = data[1];
    uint8_t count        = data[2];

    size_t expected_bytes = 3 + (count * sizeof(alarm_entry_t));
    if (len < expected_bytes) {
        ESP_LOGE(TAG, "Malformed alarm history chunk received (len=%u, expected=%u)",
                 (unsigned)len, (unsigned)expected_bytes);
        return;
    }

    if (chunk_idx == 0) {
        s_ringbuffer_count = 0;
        s_ringbuffer_head = 0;
        s_ringbuffer_tail = 0;
        s_active_conditions_count = 0;
    }

    const alarm_entry_t *entries = (const alarm_entry_t *)&data[3];
    for (uint8_t i = 0; i < count; i++) {
        _ringbuffer_append(&entries[i]);

        /* Reconstruct active condition table from latest event states */
        char base_rule[ALARM_SOURCE_MAX_LEN];
        _extract_base_rule_id(entries[i].message, base_rule, sizeof(base_rule));
        active_condition_t *cond = _get_or_create_condition(entries[i].source, base_rule, entries[i].severity);
     if (cond) {
         cond->buzzer_mode = alarm_evaluator_get_rule_buzzer_mode(entries[i].source, base_rule);
         if (entries[i].state == ALARM_STATE_GOING) {
             cond->is_active = false;
         } else if (entries[i].state == ALARM_STATE_ACKNOWLEDGED) {
             cond->is_acknowledged = true;
         } else if (entries[i].state == ALARM_STATE_MUTED) {
             cond->is_active = true;
             if (s_ui_config.mute_duration_sec == 0) {
                 s_mute_until = 0xFFFFFFFF;
             } else {
                 uint32_t mute_exp = entries[i].timestamp + s_ui_config.mute_duration_sec;
                 if (mute_exp > s_mute_until) {
                     s_mute_until = mute_exp;
                 }
             }
         } else {
             cond->is_active = true;
             cond->is_acknowledged = entries[i].is_acknowledged;
         }
     }
    }

    if (chunk_idx + 1 >= total_chunks) {
        s_history_dirty = false;
        alarm_buzzer_update();
        alarm_view_notify_change(); /* Trigger redraw if screen is already open during boot */
        ESP_LOGI(TAG, "Alarm history restored from SD card. Journal size: %u entries", (unsigned)s_ringbuffer_count);
    }
}

// ============================================================================
// Public API: Alarm Lifecycle Management (Append-Only Event Journal)
// ============================================================================

static void _alarm_auto_wake_display(void) {
    if (!indicator_display_st_get()) {
        ESP_LOGI(TAG, "Auto-waking display due to incoming alarm condition");
        bool st = 1;
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SCREEN_CTRL, &st,
                          sizeof(st), portMAX_DELAY);
        indicator_display_on();
    }
}

alarm_entry_t* alarm_add_ex(const char* source, const char* message, alarm_severity_t severity, uint8_t buzzer_mode) {
    if (!source || !message) return NULL;

    if (!alarm_time_is_valid()) {
        ESP_LOGW(TAG, "Suppressed incoming alarm [%s] %s: Time not synchronized yet", source, message);
        return NULL;
    }

    uint32_t now = (uint32_t)time(NULL);
    char base_rule[ALARM_SOURCE_MAX_LEN];
    _extract_base_rule_id(message, base_rule, sizeof(base_rule));

    active_condition_t *cond = _get_or_create_condition(source, base_rule, severity);
    alarm_state_t new_state = ALARM_STATE_INCOMING;

    if (cond) {
        if (cond->is_active && !cond->is_acknowledged) {
            /* Update buzzer mode in case configuration changed at runtime */
            cond->buzzer_mode = buzzer_mode;
            /* Condition already actively signaled, do not duplicate incoming event entry */
            return _ringbuffer_get(0);
        } else if (cond->is_active && cond->is_acknowledged) {
            new_state = ALARM_STATE_RETRIGGERED;
        }
        cond->is_active = true;
        cond->is_acknowledged = false;
        cond->severity = severity;
        cond->buzzer_mode = buzzer_mode;
    }

    /* Append distinct event row into journal */
    alarm_entry_t event_entry = {
        .timestamp = now,
        .state = new_state,
        .severity = severity,
        .is_acknowledged = false,
        .acknowledged_at = 0,
        .triggered_count = 1
    };
    strlcpy(event_entry.source, source, sizeof(event_entry.source));
    strlcpy(event_entry.message, message, sizeof(event_entry.message));

    _ringbuffer_append(&event_entry);

    ESP_LOGW(TAG, "EVENT JOURNAL: [%s] %s -> State: %s (buzzer_mode=%u)",
             source, message, (new_state == ALARM_STATE_RETRIGGERED) ? "RETRIGGERED" : "INCOMING", buzzer_mode);

    /* Only re-arm buzzer acoustics if active mute has expired and buzzer is enabled for this condition */
    if (!alarm_is_muted() && buzzer_mode != 2) {
        alarm_unmute();
    }

    alarm_buzzer_update();
    _alarm_auto_wake_display();
    _alarm_mark_dirty();
    alarm_view_notify_change();

    return _ringbuffer_get(0);
}

alarm_entry_t* alarm_add(const char* source, const char* message, alarm_severity_t severity) {
    return alarm_add_ex(source, message, severity, 0);
}

void alarm_resolve(const char* source, const char* message) {
    if (!source || !message) return;

    if (!alarm_time_is_valid()) {
        return;
    }

    char base_rule[ALARM_SOURCE_MAX_LEN];
    _extract_base_rule_id(message, base_rule, sizeof(base_rule));

    active_condition_t *cond = _find_active_condition(source, base_rule);
    if (!cond || !cond->is_active) {
        return; /* Condition not active, ignore redundant recovery call */
    }

    cond->is_active = false;
    cond->is_acknowledged = false;

    /* Append distinct GOING event row into journal */
    alarm_entry_t going_entry = {
        .timestamp = (uint32_t)time(NULL),
        .state = ALARM_STATE_GOING,
        .severity = cond->severity,
        .is_acknowledged = true,
        .acknowledged_at = (uint32_t)time(NULL),
        .triggered_count = 1
    };
    strlcpy(going_entry.source, source, sizeof(going_entry.source));
    strlcpy(going_entry.message, message, sizeof(going_entry.message));

    _ringbuffer_append(&going_entry);

    ESP_LOGI(TAG, "EVENT JOURNAL: [%s] %s -> State: GOING", source, message);

    _alarm_mark_dirty();
    alarm_buzzer_update();
    alarm_view_notify_change();
}

void alarm_acknowledge(alarm_entry_t* entry) {
    if (!entry) return;

    if (!alarm_time_is_valid()) {
        ESP_LOGW(TAG, "Suppressed acknowledge: Time not synchronized yet");
        return;
    }

    char base_rule[ALARM_SOURCE_MAX_LEN];
    _extract_base_rule_id(entry->message, base_rule, sizeof(base_rule));
    active_condition_t *cond = _find_active_condition(entry->source, base_rule);

    /* Ignore archive-only entries that have no active unacknowledged condition */
    if (!cond || !cond->is_active || cond->is_acknowledged) {
        return;
    }

    cond->is_acknowledged = true;

    /* Append distinct ACKNOWLEDGED event row into journal */
    alarm_entry_t ack_entry = {
        .timestamp = (uint32_t)time(NULL),
        .state = ALARM_STATE_ACKNOWLEDGED,
        .severity = entry->severity,
        .is_acknowledged = true,
        .acknowledged_at = (uint32_t)time(NULL),
        .triggered_count = 1
    };
    strlcpy(ack_entry.source, entry->source, sizeof(ack_entry.source));
    strlcpy(ack_entry.message, entry->message, sizeof(ack_entry.message));

    _ringbuffer_append(&ack_entry);

    ESP_LOGI(TAG, "EVENT JOURNAL: [%s] %s -> State: ACKNOWLEDGED", entry->source, entry->message);

    _alarm_mark_dirty();
    alarm_buzzer_update();
    alarm_view_notify_change();
}

void alarm_mute(uint32_t seconds) {
    if (seconds == 0) {
        s_mute_until = 0xFFFFFFFF;
    } else {
        s_mute_until = (uint32_t)time(NULL) + seconds;
    }

    if (alarm_time_is_valid()) {
        uint32_t now = (uint32_t)time(NULL);
        for (size_t i = 0; i < s_active_conditions_count; i++) {
            if (s_active_conditions[i].is_active && !s_active_conditions[i].is_acknowledged) {
                alarm_entry_t mute_entry = {
                    .timestamp = now,
                    .state = ALARM_STATE_MUTED,
                    .severity = s_active_conditions[i].severity,
                    .is_acknowledged = false,
                    .acknowledged_at = 0,
                    .triggered_count = 1
                };
                strlcpy(mute_entry.source, s_active_conditions[i].source, sizeof(mute_entry.source));
                strlcpy(mute_entry.message, s_active_conditions[i].rule_id, sizeof(mute_entry.message));
                _ringbuffer_append(&mute_entry);
                _alarm_mark_dirty();
            }
        }
    }

    rp2040_alarm_mute(seconds);
    alarm_buzzer_stop();
    alarm_view_notify_change();
    ESP_LOGI(TAG, "Alarms muted for %lu s", (unsigned long)seconds);
}

void alarm_unmute(void) {
    s_mute_until = 0;
    rp2040_alarm_rearm();
    alarm_buzzer_update();
    ESP_LOGI(TAG, "Alarms unmuted (acoustics re-armed)");
}

bool alarm_is_muted(void) {
    if (s_mute_until == 0) return false;
    if (s_mute_until == 0xFFFFFFFF) return true;
    return ((uint32_t)time(NULL) < s_mute_until);
}

uint32_t alarm_get_mute_remaining(void) {
    if (s_mute_until == 0) return 0;
    if (s_mute_until == 0xFFFFFFFF) return 0xFFFFFFFF;
    uint32_t now = (uint32_t)time(NULL);
    return (s_mute_until > now) ? (s_mute_until - now) : 0;
}

bool alarm_has_active_unacknowledged(void) {
    if (alarm_is_muted()) return false;

    for (size_t i = 0; i < s_active_conditions_count; i++) {
        if (s_active_conditions[i].is_active && !s_active_conditions[i].is_acknowledged) {
            return true;
        }
    }
    return false;
}

void alarm_set_state(alarm_entry_t* entry, alarm_state_t state) {
    if (!entry) return;
    entry->state = state;
    _alarm_mark_dirty();
    alarm_buzzer_update();
}

// ============================================================================
// Public API: List Access & Sorting
// ============================================================================

static int _compare_by_time_desc(const void* a, const void* b) {
    uint32_t ta = ((const alarm_entry_t*)a)->timestamp;
    uint32_t tb = ((const alarm_entry_t*)b)->timestamp;
    if (tb > ta) return 1;
    if (tb < ta) return -1;
    return 0;
}
static int _compare_by_time_asc(const void* a, const void* b) {
    uint32_t ta = ((const alarm_entry_t*)a)->timestamp;
    uint32_t tb = ((const alarm_entry_t*)b)->timestamp;
    if (ta > tb) return 1;
    if (ta < tb) return -1;
    return 0;
}
static int _compare_by_severity(const void* a, const void* b) {
    return (((const alarm_entry_t*)a)->severity - ((const alarm_entry_t*)b)->severity);
}
static int _compare_by_source(const void* a, const void* b) {
    return strcmp(((const alarm_entry_t*)a)->source, ((const alarm_entry_t*)b)->source);
}

static bool _filter_alarm(const alarm_entry_t* entry, alarm_filter_t filter) {
    if (filter == ALARM_FILTER_ALL) return true;
    if (filter == ALARM_FILTER_DANGER)  return entry->severity == ALARM_SEVERITY_DANGER;
    if (filter == ALARM_FILTER_WARNING) return entry->severity == ALARM_SEVERITY_WARNING;
    if (filter == ALARM_FILTER_INFO)    return entry->severity == ALARM_SEVERITY_INFO;
    return true;
}

alarm_entry_t* alarm_get_list(alarm_sort_mode_t sort_mode, alarm_filter_t filter, size_t* count) {
    if (!count) return NULL;
    *count = 0;

    if (s_ringbuffer_count == 0) {
        return NULL;
    }

    /* Allocate in PSRAM, Fallback to Standard-Heap */
    alarm_entry_t* filtered = (alarm_entry_t*)heap_caps_malloc(
        s_ringbuffer_count * sizeof(alarm_entry_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (!filtered) {
        filtered = (alarm_entry_t*)malloc(s_ringbuffer_count * sizeof(alarm_entry_t));
    }
    if (!filtered) {
        return NULL;
    }

    size_t filtered_count = 0;
    for (size_t i = 0; i < s_ringbuffer_count; i++) {
        alarm_entry_t* entry = _ringbuffer_get(i);
        if (entry && _filter_alarm(entry, filter)) {
            filtered[filtered_count++] = *entry;
        }
    }

    if (filtered_count == 0) {
        free(filtered);
        return NULL;
    }

    /* Sort in-place */
    switch (sort_mode) {
        case ALARM_SORT_BY_TIME_ASC:
            qsort(filtered, filtered_count, sizeof(alarm_entry_t), _compare_by_time_asc);
            break;
        case ALARM_SORT_BY_TIME_DESC:
            qsort(filtered, filtered_count, sizeof(alarm_entry_t), _compare_by_time_desc);
            break;
        case ALARM_SORT_BY_SEVERITY:
            qsort(filtered, filtered_count, sizeof(alarm_entry_t), _compare_by_severity);
            break;
        case ALARM_SORT_BY_SOURCE:
            qsort(filtered, filtered_count, sizeof(alarm_entry_t), _compare_by_source);
            break;
    }

    *count = filtered_count;
    return filtered;
}

alarm_entry_t* alarm_get_ringbuffer(size_t* count) {
    if (!count) return NULL;
    *count = 0;

    /* No Alarms in Buffer, nothing to do */
    if (s_ringbuffer_count == 0) {
        return NULL;
    }

    /* Prioritize Allocation in PSRAM */
    alarm_entry_t* result = (alarm_entry_t*)heap_caps_malloc(
        s_ringbuffer_count * sizeof(alarm_entry_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    /* Fallback to Standard-Heap if PSRAM full or not ready */
    if (!result) {
        result = (alarm_entry_t*)malloc(s_ringbuffer_count * sizeof(alarm_entry_t));
    }

    /* Error Handing if Both Allocations fail */
    if (!result) {
        *count = 0;
        return NULL;
    }

    /* Copy data */
    for (size_t i = 0; i < s_ringbuffer_count; i++) {
        alarm_entry_t* entry = _ringbuffer_get(i);
        if (entry) {
            result[i] = *entry;
        }
    }

    *count = s_ringbuffer_count;
    return result;
}

// ============================================================================
// Buzzer & Header Arbitration
// ============================================================================

void alarm_buzzer_start(uint8_t pattern) {
    if (s_current_buzzer_pattern == pattern) return;

    uint16_t t_on = 0, t_off = 0;
    if (pattern == 3) {
        t_on = 1000; t_off = 0;    /* Continuous steady tone */
    } else if (pattern == 0) {
        t_on = 250; t_off = 250;   /* Fast intermittent (Danger) */
    } else if (pattern == 1) {
        t_on = 500; t_off = 1500;  /* Medium intermittent (Warning) */
    } else {
        t_on = 80; t_off = 4000;   /* Slow intermittent (Info) */
    }

    rp2040_alarm_set(t_on, t_off);
    s_current_buzzer_pattern = pattern;
    ESP_LOGI(TAG, "Hardware Buzzer started (Pattern %u)", pattern);
}

void alarm_buzzer_stop(void) {
    if (s_current_buzzer_pattern == 0xFF) return;
    rp2040_alarm_set(0, 0);
    s_current_buzzer_pattern = 0xFF;
    ESP_LOGI(TAG, "Hardware Buzzer stopped");
}

void alarm_buzzer_update(void) {
    bool has_unack_continuous = false;
    bool has_unack_danger = false;
    bool has_unack_warning = false;
    bool has_unack_info = false;
    s_has_active_alarms = false;

    /* Evaluate live status strictly from active condition table */
    for (size_t i = 0; i < s_active_conditions_count; i++) {
        if (!s_active_conditions[i].is_active) continue;

        s_has_active_alarms = true;

        if (!s_active_conditions[i].is_acknowledged) {
            /* Skip acoustic signaling if buzzer_mode is explicitly set to "off" (2) */
            if (s_active_conditions[i].buzzer_mode == 2) {
                continue;
            }
            if (s_active_conditions[i].buzzer_mode == 1) {
                has_unack_continuous = true;
            }
            if (s_active_conditions[i].severity == ALARM_SEVERITY_DANGER)  has_unack_danger = true;
            if (s_active_conditions[i].severity == ALARM_SEVERITY_WARNING) has_unack_warning = true;
            if (s_active_conditions[i].severity == ALARM_SEVERITY_INFO)    has_unack_info = true;
        }
    }

    gui_header_set_alarm_state(s_has_active_alarms);

    if (alarm_is_muted()) {
        alarm_buzzer_stop();
        return;
    }

    if (has_unack_continuous) {
        alarm_buzzer_start(3);
    } else if (has_unack_danger) {
        alarm_buzzer_start(0);
    } else if (has_unack_warning) {
        alarm_buzzer_start(1);
    } else if (has_unack_info) {
        alarm_buzzer_start(2);
    } else {
        alarm_buzzer_stop();
    }
}
static void _alarm_persist_timer_cb(void *arg) {
    /* 1. Deferred loading: Trigger load as soon as time becomes valid (NTP/LoRa/Ersatzzeit) */
    if (!s_history_loaded && s_sd_card_ready && alarm_time_is_valid()) {
        ESP_LOGI(TAG, "Time synchronization acquired. Triggering deferred alarm history load.");
        s_history_loaded = true;
        alarm_history_load();
    }

    /* 2. Debounced save to SD */
    if (!s_history_dirty) return;

    int64_t now_ms = esp_timer_get_time() / 1000ULL;
    if (now_ms >= s_dirty_time_ms + ALARM_PERSIST_SETTLE_MS) {
        alarm_history_save();
    }
}

static void _alarm_shutdown_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data) {
    if (id == VIEW_EVENT_SHUTDOWN && s_history_dirty) {
        alarm_history_save();
    }
}

static void _alarm_sd_status_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data) {
    if (id == VIEW_EVENT_SD_INFO_RET) {
        struct sd_card_info *info = (struct sd_card_info *)event_data;
        if (!info) return;

        bool is_ready = (info->is_present && !info->is_usb_mounted && info->state >= SD_STATE_VALID_FAT32_LFS);
        s_sd_card_ready = is_ready;

        /* If time is already valid and we haven't loaded yet, load immediately */
        if (is_ready && !s_history_loaded && alarm_time_is_valid()) {
            ESP_LOGI(TAG, "SD ready and time valid. Triggering alarm history load.");
            s_history_loaded = true;
            alarm_history_load();
        }
    }
}

void alarm_model_init(void) {
    memset(s_ringbuffer, 0, sizeof(s_ringbuffer));
    s_ringbuffer_count = 0;
    s_ringbuffer_head = 0;
    s_ringbuffer_tail = 0;
    s_active_conditions_count = 0;

    s_ui_config = _get_default_ui_config();
    s_mute_until = 0;
    s_has_active_alarms = false;
    s_current_buzzer_pattern = 0xFF;

    const esp_timer_create_args_t persist_args = {
        .callback = &_alarm_persist_timer_cb,
        .name = "alarm_persist"
    };
    ESP_ERROR_CHECK(esp_timer_create(&persist_args, &s_persist_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_persist_timer, 1000000));

    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
        view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SHUTDOWN,
        _alarm_shutdown_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
        view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SD_INFO_RET,
        _alarm_sd_status_handler, NULL, NULL));

    ESP_LOGI(TAG, "Alarm Model initialized (Journal capacity: %d entries)", ALARM_RINGBUFFER_SIZE);
}

void alarm_model_shutdown(void) {
    alarm_buzzer_stop();
}

bool alarm_config_load(const char* path) {
    s_ui_config = _get_default_ui_config();
    return true;
}

bool alarm_config_save(const char* path) { return true; }

const alarm_ui_config_t* alarm_get_ui_config(void) {
    s_ui_config = _get_default_ui_config();
    return &s_ui_config;
}

void alarm_set_ui_config(const alarm_ui_config_t* config) {
    if (config) s_ui_config = *config;
}

alarm_ui_config_t alarm_get_default_ui_config(void) {
    return _get_default_ui_config();
}