/**
 * @file alarm_model.h
 * @brief Alarm system data model, state management, and persistence.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef ALARM_MODEL_H
#define ALARM_MODEL_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// ============================================================================
// Constants
// ============================================================================

/** Maximum length for alarm source identifier */
#define ALARM_SOURCE_MAX_LEN   32

/** Maximum length for alarm message */
#define ALARM_MESSAGE_MAX_LEN  128

/** Maximum entries in RAM ringbuffer */
#define ALARM_RINGBUFFER_SIZE   50

/** Maximum alarms in full history (LittleFS) - 0 = unlimited */
#define ALARM_HISTORY_MAX      0

// ============================================================================
// Alarm States
// ============================================================================

/** Alarm states as per user specification */
typedef enum {
    ALARM_STATE_INCOMING,     /**< Incoming (new, active, red) */
    ALARM_STATE_GOING,        /**< Going (resolved, was active, green) */
    ALARM_STATE_MUTED,        /**< Muted (temporarily suppressed, gray) */
    ALARM_STATE_RETRIGGERED,  /**< Retriggered (reactivated after going, blue) */
    ALARM_STATE_ACKNOWLEDGED  /**< Acknowledged (user acknowledged, gray) */
} alarm_state_t;

// ============================================================================
// Alarm Severity Levels
// ============================================================================

/** Alarm severity levels */
typedef enum {
    ALARM_SEVERITY_DANGER,    /**< Critical (red) */
    ALARM_SEVERITY_WARNING,   /**< Warning (yellow) */
    ALARM_SEVERITY_INFO       /**< Information (blue/gray) */
} alarm_severity_t;

// ============================================================================
// Sort & Filter Options
// ============================================================================

/** Sort modes for alarm list */
typedef enum {
    ALARM_SORT_BY_TIME_ASC,       /**< Oldest first */
    ALARM_SORT_BY_TIME_DESC,      /**< Newest first (DEFAULT) */
    ALARM_SORT_BY_SEVERITY,      /**< Danger > Warning > Info */
    ALARM_SORT_BY_SOURCE         /**< Group by source */
} alarm_sort_mode_t;

/** Filter by severity */
typedef enum {
    ALARM_FILTER_ALL,            /**< Show all (DEFAULT) */
    ALARM_FILTER_DANGER,        /**< Only danger */
    ALARM_FILTER_WARNING,       /**< Only warnings */
    ALARM_FILTER_INFO           /**< Only info */
} alarm_filter_t;

// ============================================================================
// Alarm Entry Structure
// ============================================================================

/** Single alarm entry */
typedef struct {
    uint32_t timestamp;            /**< Unix timestamp when alarm was triggered */
    char source[ALARM_SOURCE_MAX_LEN];     /**< Source identifier (e.g., "Hochbehaelter") */
    char message[ALARM_MESSAGE_MAX_LEN];   /**< Alarm message text */
    alarm_severity_t severity;     /**< Alarm severity level */
    alarm_state_t state;           /**< Current alarm state */
    bool is_acknowledged;          /**< User has acknowledged this alarm */
    uint32_t acknowledged_at;      /**< Timestamp when acknowledged (0 = not acknowledged) */
    uint32_t triggered_count;      /**< Number of times this alarm was triggered */
} alarm_entry_t;

// ============================================================================
// Alarm Configuration (loaded from SD card JSON)
// ============================================================================

/** Color configuration for alarm UI */
typedef struct {
    uint32_t color_danger;       /**< LVGL color for danger alarms */
    uint32_t color_warning;      /**< LVGL color for warning alarms */
    uint32_t color_info;         /**< LVGL color for info alarms */
    uint32_t color_incoming;    /**< Color for incoming state */
    uint32_t color_going;        /**< Color for going state */
    uint32_t color_muted;        /**< Color for muted state */
    uint32_t color_retriggered;  /**< Color for retriggered state */
    uint32_t color_acknowledged; /**< Color for acknowledged state */
} alarm_color_config_t;

/** Text configuration for alarm UI */
typedef struct {
    char danger[32];
    char warning[32];
    char info[32];
    char incoming[32];
    char going[32];
    char muted[32];
    char retriggered[32];
    char acknowledged[32];
} alarm_text_config_t;

/** Combined UI configuration */
typedef struct {
    alarm_color_config_t colors;
    alarm_text_config_t texts;
} alarm_ui_config_t;

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Initialize alarm system
 */
void alarm_model_init(void);

/**
 * @brief Shutdown alarm system
 */
void alarm_model_shutdown(void);

/**
 * @brief Add a new alarm to the system
 * @param source Alarm source (e.g., "Hochbehaelter")
 * @param message Alarm message
 * @param severity Alarm severity
 * @return Pointer to the new alarm entry, or NULL on error
 */
alarm_entry_t* alarm_add(const char* source, const char* message, alarm_severity_t severity);

/**
 * @brief Acknowledge an alarm (set state to ACKNOWLEDGED)
 * @param entry Alarm entry to acknowledge
 */
void alarm_acknowledge(alarm_entry_t* entry);

/**
 * @brief Mute alarms for a period (stops buzzer, but alarms still trigger)
 * @param seconds Mute duration in seconds (0 = mute indefinitely)
 */
void alarm_mute(uint32_t seconds);

/**
 * @brief Unmute alarms
 */
void alarm_unmute(void);

/**
 * @brief Check if alarms are currently muted
 * @return true if muted
 */
bool alarm_is_muted(void);

/**
 * @brief Get remaining mute time
 * @return Seconds remaining, or 0 if not muted
 */
uint32_t alarm_get_mute_remaining(void);

/**
 * @brief Set alarm state
 * @param entry Alarm entry
 * @param state New state
 */
void alarm_set_state(alarm_entry_t* entry, alarm_state_t state);

/**
 * @brief Get current alarm list with sorting and filtering
 * @param sort_mode Sort mode
 * @param filter Filter mode
 * @param count Output: number of alarms in result
 * @return Array of alarm entries (pointer to internal buffer)
 * @note Result is valid until next call to alarm_get_list
 */
alarm_entry_t* alarm_get_list(alarm_sort_mode_t sort_mode, alarm_filter_t filter, size_t* count);

/**
 * @brief Get ringbuffer alarm list (last ALARM_RINGBUFFER_SIZE alarms)
 * @param count Output: number of alarms returned
 * @return Array of alarm entries
 */
alarm_entry_t* alarm_get_ringbuffer(size_t* count);

/**
 * @brief Load alarm configuration from LittleFS
 * @param path Path to config file on LittleFS (e.g., "/gui/alarm_config.json")
 * @return true on success
 */
bool alarm_config_load(const char* path);

/**
 * @brief Save alarm configuration to LittleFS
 * @param path Path to config file on LittleFS
 * @return true on success
 */
bool alarm_config_save(const char* path);

/**
 * @brief Load alarm history from LittleFS
 * @return true on success
 */
bool alarm_history_load(void);

/**
 * @brief Save alarm history to LittleFS
 * @return true on success
 */
bool alarm_history_save(void);

/**
 * @brief Get alarm UI configuration (colors and texts)
 * @return Pointer to current UI config
 */
const alarm_ui_config_t* alarm_get_ui_config(void);

/**
 * @brief Set custom UI configuration
 * @param config Configuration to set
 */
void alarm_set_ui_config(const alarm_ui_config_t* config);

/**
 * @brief Get default UI configuration
 * @return Default configuration
 */
alarm_ui_config_t alarm_get_default_ui_config(void);

/**
 * @brief Notify RP2040 to start alarm buzzer
 * @param pattern Buzzer pattern (0 = danger, 1 = warning, 2 = info)
 */
void alarm_buzzer_start(uint8_t pattern);

/**
 * @brief Notify RP2040 to stop alarm buzzer
 */
void alarm_buzzer_stop(void);

/**
 * @brief Update buzzer state based on active alarms
 */
void alarm_buzzer_update(void);

#endif /* ALARM_MODEL_H */
