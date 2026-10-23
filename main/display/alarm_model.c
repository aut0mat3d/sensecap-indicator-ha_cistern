/**
 * @file alarm_model.c
 * @brief Alarm system implementation: state management, persistence, and ringbuffer.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "alarm_model.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "gui_header.h"

static const char* TAG = "alarm_model";

// ============================================================================
// Static State
// ============================================================================

// Ringbuffer for last 50 alarms (in RAM)
static alarm_entry_t s_ringbuffer[ALARM_RINGBUFFER_SIZE];
static size_t s_ringbuffer_count = 0;
static size_t s_ringbuffer_head = 0;
static size_t s_ringbuffer_tail = 0;

// Full history (loaded from LittleFS on demand)
static alarm_entry_t* s_history = NULL;
static size_t s_history_count = 0;
static size_t s_history_capacity = 0;

// Alarm UI configuration
static alarm_ui_config_t s_ui_config;

// Mute state (stored in RTC RAM for persistence across reboots)
static uint32_t s_mute_until = 0; // 0 = not muted, otherwise Unix timestamp

// Active alarms that need buzzer attention
static bool s_has_active_alarms = false;

// ============================================================================
// Default UI Configuration
// ============================================================================

static alarm_ui_config_t _get_default_ui_config(void) {
    return (alarm_ui_config_t){
        .colors = {
            .color_danger = 0xE74C3C,     // Red
            .color_warning = 0xF39C12,    // Orange/Yellow
            .color_info = 0x3498DB,       // Blue
            .color_incoming = 0xE74C3C,   // Red (same as danger for incoming)
            .color_going = 0x2ECC71,      // Green
            .color_muted = 0x95A5A6,      // Gray
            .color_retriggered = 0x3498DB, // Blue
            .color_acknowledged = 0x7F8C8D // Dark Gray
        },
        .texts = {
            .danger = "Danger",
            .warning = "Warning",
            .info = "Info",
            .incoming = "Incoming",
            .going = "Going",
            .muted = "Muted",
            .retriggered = "Retriggered",
            .acknowledged = "Acknowledged"
        }
    };
}

// ============================================================================
// Ringbuffer Management
// ============================================================================

/**
 * @brief Add entry to ringbuffer
 * @param entry Alarm entry to add
 */
static void _ringbuffer_add(const alarm_entry_t* entry) {
    if (s_ringbuffer_count >= ALARM_RINGBUFFER_SIZE) {
        // Buffer full, overwrite oldest
        s_ringbuffer_head = (s_ringbuffer_head + 1) % ALARM_RINGBUFFER_SIZE;
        s_ringbuffer_count--;
    }

    // Copy entry to tail
    memcpy(&s_ringbuffer[s_ringbuffer_tail], entry, sizeof(alarm_entry_t));
    s_ringbuffer_tail = (s_ringbuffer_tail + 1) % ALARM_RINGBUFFER_SIZE;
    s_ringbuffer_count++;
}

/**
 * @brief Get entry from ringbuffer by index
 * @param index Index (0 = newest)
 * @return Pointer to alarm entry
 */
static alarm_entry_t* _ringbuffer_get(size_t index) {
    if (index >= s_ringbuffer_count) {
        return NULL;
    }
    
    // Calculate actual position in circular buffer
    size_t actual_index = (s_ringbuffer_tail - 1 - index + ALARM_RINGBUFFER_SIZE) % ALARM_RINGBUFFER_SIZE;
    return &s_ringbuffer[actual_index];
}

// ============================================================================
// History Management (LittleFS)
// ============================================================================

/**
 * @brief Load alarm history from LittleFS
 * @return true on success
 */
bool alarm_history_load(void) {
    // Free existing history
    if (s_history) {
        free(s_history);
        s_history = NULL;
        s_history_count = 0;
        s_history_capacity = 0;
    }

    // For now, we'll just return success
    // TODO: Implement actual loading from RP2040 via UART
    ESP_LOGI(TAG, "Alarm history load (stub implementation)");
    return true;
}

/**
 * @brief Save alarm history to LittleFS
 * @return true on success
 */
bool alarm_history_save(void) {
    // For now, we'll just return success
    // TODO: Implement actual saving to RP2040 via UART
    ESP_LOGI(TAG, "Alarm history save (stub implementation)");
    return true;
}

// ============================================================================
// Configuration Management
// ============================================================================

bool alarm_config_load(const char* path) {
    // Try to load from RP2040 via UART (SD card)
    // For now, use defaults since UART communication is not implemented yet
    s_ui_config = _get_default_ui_config();
    ESP_LOGI(TAG, "Alarm config loaded (using defaults)");
    return true;
}

bool alarm_config_save(const char* path) {
    // Save to RP2040 via UART
    // For now, just return success
    ESP_LOGI(TAG, "Alarm config save (stub implementation)");
    return true;
}

const alarm_ui_config_t* alarm_get_ui_config(void) {
    return &s_ui_config;
}

void alarm_set_ui_config(const alarm_ui_config_t* config) {
    if (config) {
        s_ui_config = *config;
    }
}

alarm_ui_config_t alarm_get_default_ui_config(void) {
    return _get_default_ui_config();
}

// ============================================================================
// Public API: Initialization & Shutdown
// ============================================================================

void alarm_model_init(void) {
    // Initialize ringbuffer
    memset(s_ringbuffer, 0, sizeof(s_ringbuffer));
    s_ringbuffer_count = 0;
    s_ringbuffer_head = 0;
    s_ringbuffer_tail = 0;

    // Initialize history
    s_history = NULL;
    s_history_count = 0;
    s_history_capacity = 0;

    // Load configuration (use defaults for now)
    s_ui_config = _get_default_ui_config();
    
    // TODO: Implement config loading from SD card based on language setting
    // 1. Check /config/globals.json for language
    // 2. If language is set (e.g., "de"), try to load /gui/lang/de/alarm_config.json
    // 3. If not found, fall back to /gui/alarm_config.json (English)
    // 4. If still not found, use builtin defaults

    // Load history
    alarm_history_load();

    // Initialize mute state
    s_mute_until = 0;
    s_has_active_alarms = false;
}

void alarm_model_shutdown(void) {
    // Save history
    alarm_history_save();

    // Free history buffer
    if (s_history) {
        free(s_history);
        s_history = NULL;
    }

    s_history_count = 0;
    s_history_capacity = 0;
}

// ============================================================================
// Public API: Alarm Management
// ============================================================================

alarm_entry_t* alarm_add(const char* source, const char* message, alarm_severity_t severity) {
    if (!source || !message) {
        return NULL;
    }

    // Create new alarm entry
    alarm_entry_t new_alarm = {
        .timestamp = (uint32_t)time(NULL),
        .state = ALARM_STATE_INCOMING,
        .severity = severity,
        .is_acknowledged = false,
        .acknowledged_at = 0,
        .triggered_count = 1
    };

    // Copy strings
    strncpy(new_alarm.source, source, sizeof(new_alarm.source) - 1);
    new_alarm.source[sizeof(new_alarm.source) - 1] = '\0';
    
    strncpy(new_alarm.message, message, sizeof(new_alarm.message) - 1);
    new_alarm.message[sizeof(new_alarm.message) - 1] = '\0';

    // Add to ringbuffer
    _ringbuffer_add(&new_alarm);

    // Add to history
    if (s_history_count < s_history_capacity) {
        s_history[s_history_count++] = new_alarm;
    }

    // Set flag for active alarms
    s_has_active_alarms = true;
    gui_header_set_alarm_state(true);

    // Return pointer to the entry in ringbuffer
    // Note: This pointer may become invalid after more additions
    return _ringbuffer_get(0);
}

void alarm_acknowledge(alarm_entry_t* entry) {
    if (!entry) {
        return;
    }

    entry->state = ALARM_STATE_ACKNOWLEDGED;
    entry->is_acknowledged = true;
    entry->acknowledged_at = (uint32_t)time(NULL);

    // Update in history if exists
    for (size_t i = 0; i < s_history_count; i++) {
        if (s_history[i].timestamp == entry->timestamp &&
            strcmp(s_history[i].source, entry->source) == 0 &&
            strcmp(s_history[i].message, entry->message) == 0) {
            s_history[i] = *entry;
            break;
        }
    }

    // Check if we still have active alarms
    s_has_active_alarms = false;
    for (size_t i = 0; i < s_ringbuffer_count; i++) {
        alarm_entry_t* e = _ringbuffer_get(i);
        if (e && !e->is_acknowledged && e->state == ALARM_STATE_INCOMING) {
            s_has_active_alarms = true;
            break;
        }
    }
    gui_header_set_alarm_state(s_has_active_alarms);
}

void alarm_mute(uint32_t seconds) {
    if (seconds == 0) {
        s_mute_until = 0xFFFFFFFF; // Indefinite mute
    } else {
        s_mute_until = (uint32_t)time(NULL) + seconds;
    }
}

void alarm_unmute(void) {
    s_mute_until = 0;
}

bool alarm_is_muted(void) {
    if (s_mute_until == 0) {
        return false;
    }
    if (s_mute_until == 0xFFFFFFFF) {
        return true; // Indefinitely muted
    }
    return (uint32_t)time(NULL) < s_mute_until;
}

uint32_t alarm_get_mute_remaining(void) {
    if (s_mute_until == 0) {
        return 0;
    }
    if (s_mute_until == 0xFFFFFFFF) {
        return 0xFFFFFFFF; // Indefinite
    }
    uint32_t now = (uint32_t)time(NULL);
    return (s_mute_until > now) ? (s_mute_until - now) : 0;
}

void alarm_set_state(alarm_entry_t* entry, alarm_state_t state) {
    if (!entry) {
        return;
    }

    entry->state = state;

    // Update in history if exists
    for (size_t i = 0; i < s_history_count; i++) {
        if (s_history[i].timestamp == entry->timestamp &&
            strcmp(s_history[i].source, entry->source) == 0 &&
            strcmp(s_history[i].message, entry->message) == 0) {
            s_history[i].state = state;
            break;
        }
    }

    // Check active alarms flag
    s_has_active_alarms = false;
    for (size_t i = 0; i < s_ringbuffer_count; i++) {
        alarm_entry_t* e = _ringbuffer_get(i);
        if (e && !e->is_acknowledged && e->state == ALARM_STATE_INCOMING) {
            s_has_active_alarms = true;
            break;
        }
    }
    gui_header_set_alarm_state(s_has_active_alarms);
}

// ============================================================================
// Public API: List Access
// ============================================================================

/**
 * @brief Comparison function for sorting alarms by time (newest first)
 */
static int _compare_by_time_desc(const void* a, const void* b) {
    const alarm_entry_t* ea = (const alarm_entry_t*)a;
    const alarm_entry_t* eb = (const alarm_entry_t*)b;
    return (eb->timestamp - ea->timestamp);
}

/**
 * @brief Comparison function for sorting alarms by time (oldest first)
 */
static int _compare_by_time_asc(const void* a, const void* b) {
    const alarm_entry_t* ea = (const alarm_entry_t*)a;
    const alarm_entry_t* eb = (const alarm_entry_t*)b;
    return (ea->timestamp - eb->timestamp);
}

/**
 * @brief Comparison function for sorting alarms by severity
 */
static int _compare_by_severity(const void* a, const void* b) {
    const alarm_entry_t* ea = (const alarm_entry_t*)a;
    const alarm_entry_t* eb = (const alarm_entry_t*)b;
    return (ea->severity - eb->severity);
}

/**
 * @brief Comparison function for sorting alarms by source
 */
static int _compare_by_source(const void* a, const void* b) {
    const alarm_entry_t* ea = (const alarm_entry_t*)a;
    const alarm_entry_t* eb = (const alarm_entry_t*)b;
    return strcmp(ea->source, eb->source);
}

/**
 * @brief Filter function for alarms
 */
static bool _filter_alarm(const alarm_entry_t* entry, alarm_filter_t filter) {
    if (filter == ALARM_FILTER_ALL) {
        return true;
    }
    return (entry->severity == filter);
}

alarm_entry_t* alarm_get_list(alarm_sort_mode_t sort_mode, alarm_filter_t filter, size_t* count) {
    // First, collect all alarms that match the filter
    alarm_entry_t* filtered = NULL;
    size_t filtered_count = 0;
    
    // Allocate temporary buffer for filtered list
    alarm_entry_t temp_list[ALARM_RINGBUFFER_SIZE];
    
    // Copy from ringbuffer
    for (size_t i = 0; i < s_ringbuffer_count; i++) {
        alarm_entry_t* entry = _ringbuffer_get(i);
        if (entry && _filter_alarm(entry, filter)) {
            temp_list[filtered_count++] = *entry;
        }
    }
    
    // Sort the filtered list
    switch (sort_mode) {
        case ALARM_SORT_BY_TIME_ASC:
            qsort(temp_list, filtered_count, sizeof(alarm_entry_t), _compare_by_time_asc);
            break;
        case ALARM_SORT_BY_TIME_DESC:
            qsort(temp_list, filtered_count, sizeof(alarm_entry_t), _compare_by_time_desc);
            break;
        case ALARM_SORT_BY_SEVERITY:
            qsort(temp_list, filtered_count, sizeof(alarm_entry_t), _compare_by_severity);
            break;
        case ALARM_SORT_BY_SOURCE:
            qsort(temp_list, filtered_count, sizeof(alarm_entry_t), _compare_by_source);
            break;
    }
    
    // Allocate result buffer
    filtered = (alarm_entry_t*)malloc(filtered_count * sizeof(alarm_entry_t));
    if (!filtered) {
        *count = 0;
        return NULL;
    }
    
    memcpy(filtered, temp_list, filtered_count * sizeof(alarm_entry_t));
    *count = filtered_count;
    
    return filtered;
}

alarm_entry_t* alarm_get_ringbuffer(size_t* count) {
    *count = s_ringbuffer_count;
    
    // Allocate buffer for result
    alarm_entry_t* result = (alarm_entry_t*)malloc(s_ringbuffer_count * sizeof(alarm_entry_t));
    if (!result) {
        *count = 0;
        return NULL;
    }
    
    // Copy from ringbuffer (newest first)
    for (size_t i = 0; i < s_ringbuffer_count; i++) {
        alarm_entry_t* entry = _ringbuffer_get(i);
        if (entry) {
            result[i] = *entry;
        }
    }
    
    return result;
}

// ============================================================================
// Buzzer Control (via RP2040 UART)
// ============================================================================

/**
 * @brief Notify RP2040 to start alarm buzzer
 * @param pattern Buzzer pattern (0 = danger, 1 = warning, 2 = info)
 */
void alarm_buzzer_start(uint8_t pattern) {
    // Send command to RP2040 via PacketSerial
    // This would be implemented in the main communication loop
    // For now, just a placeholder
}

/**
 * @brief Notify RP2040 to stop alarm buzzer
 */
void alarm_buzzer_stop(void) {
    // Send command to RP2040
    // Placeholder
}

/**
 * @brief Update buzzer state based on active alarms
 */
void alarm_buzzer_update(void) {
    if (alarm_is_muted()) {
        alarm_buzzer_stop();
        return;
    }
    
    if (s_has_active_alarms) {
        // Check for danger alarms
        for (size_t i = 0; i < s_ringbuffer_count; i++) {
            alarm_entry_t* entry = _ringbuffer_get(i);
            if (entry && entry->severity == ALARM_SEVERITY_DANGER && 
                !entry->is_acknowledged && entry->state == ALARM_STATE_INCOMING) {
                alarm_buzzer_start(0); // Danger pattern
                return;
            }
        }
        
        // Check for warning alarms
        for (size_t i = 0; i < s_ringbuffer_count; i++) {
            alarm_entry_t* entry = _ringbuffer_get(i);
            if (entry && entry->severity == ALARM_SEVERITY_WARNING && 
                !entry->is_acknowledged && entry->state == ALARM_STATE_INCOMING) {
                alarm_buzzer_start(1); // Warning pattern
                return;
            }
        }
        
        // Only info alarms
        alarm_buzzer_start(2); // Info pattern
    } else {
        alarm_buzzer_stop();
    }
}
