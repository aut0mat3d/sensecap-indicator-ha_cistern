/**
 * @file data_storage.hpp
 * @brief Non-Blocking Data Storage Engine for LittleFS
 *        Handles raw data (roundtrip), hourly, and daily aggregation
 *        with RAM accumulation, wall-clock alignment, and offline recovery.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef DATA_STORAGE_HPP
#define DATA_STORAGE_HPP

#include <stdint.h>
#include <stdbool.h>
#include <SdFat.h>

class SdFat32;

// ============================================================================
// Granularity Types
// ============================================================================

typedef enum {
    GRAN_ROUNDTRIP = 0,  /// Raw/Bucket-averaged data (configurable interval)
    GRAN_HOURLY    = 1,  /// Hourly aggregated values (max 48 entries)
    GRAN_DAILY     = 2   /// Daily aggregated values (max 32 entries)
} granularity_t;

// ============================================================================
// Data Point Structures
// ============================================================================

/** Single raw/bucket data point for roundtrip storage */
typedef struct {
    uint32_t timestamp;    /// Unix timestamp (UTC)
    float value;          /// Sensor value
    char unit[8];        /// Unit string (e.g. "cm", "%", "m³")
    bool is_estimated;    /// True if this is an estimated/replacement value
} data_point_t;

/** Aggregated data point for hourly/daily storage */
typedef struct {
    uint32_t start_timestamp;  ///< Start of aggregation interval (UTC)
    int32_t  tz_offset_sec;    ///< Timezone offset to local midnight in seconds (e.g. +7200)
    float    avg_value;        ///< Average value in interval
    float    min_value;        ///< Minimum value in interval
    float    max_value;        ///< Maximum value in interval
    uint32_t count;            ///< Number of raw values in interval
    bool     has_estimated;    ///< True if any estimated values in interval
} data_aggregated_t;

// ============================================================================
// Channel Configuration & Runtime Accumulator State
// ============================================================================

/**
 * @brief Per-channel storage configuration
 */
typedef struct {
    char channel_key[64];        /// Unique key: "[dev_addr].[decoder].[key]"
    char dev_addr[17];          /// Device address (e.g. "05975DC9")
    char decoder[32];           /// Decoder name (e.g. "EM500-SWL")
    char key[32];               /// Channel key (e.g. "depth")
    bool store_longterm;         /// Enable long-term storage (hourly/daily)
    bool store_roundtrip;        /// Enable roundtrip storage
    uint32_t roundtrip_interval;    /// Wall-clock aligned interval in seconds
    uint32_t tolerance_sec;      /// Per-channel jitter tolerance from JSON
    char unit[8];               /// Unit string from mapping
} data_channel_config_t;

/**
 * @brief Per-channel runtime accumulator state (RAM-based, zero SD wear)
 */
typedef struct {
    uint32_t current_bucket;    /// Start timestamp of current wall-clock window
    double   accumulator;       /// High-precision IEEE-754 sum of samples in window
    uint16_t sample_count;      /// Number of sensor samples received in window (cnt)
    uint32_t last_stored_time;  /// Timestamp of last written record on LittleFS
    float    last_value;        /// Last known value (used for missing estimation)
    bool     has_last_value;    /// True if last_value is valid
} data_channel_state_t;

/**
 * @brief Combined channel configuration and state
 */
typedef struct {
    data_channel_config_t config;
    data_channel_state_t state;
} data_channel_t;

// ============================================================================
// Time Synchronization State & Guards
// ============================================================================

#define TIME_FLAG_ACCURATE   0x01  ///< Synced from NTP or LoRa Time Broadcast
#define TIME_FLAG_ESTIMATED  0x02  ///< Recovered from Archive/Uptime offset (Offline)

void data_storage_set_time_valid(bool valid, uint8_t flags = TIME_FLAG_ACCURATE);
bool data_storage_is_time_valid(void);
uint8_t data_storage_get_time_flags(void);
/** Save and read persistent timestamp on LittleFS Partition 2 */
void data_storage_save_time(uint32_t timestamp);
uint32_t data_storage_get_saved_time(void);

// ============================================================================
// Public API
// ============================================================================

void data_storage_init(SdFat32* sd);
bool data_storage_load_devices(SdFat32* sd);
void data_storage_load_decoder(SdFat32* sd, const char* dev_addr, const char* decoder);
bool data_storage_register_channel(const data_channel_config_t* config);

/**
 * @brief Ingest incoming telemetry sample into RAM accumulator (Zero SD I/O)
 */
void data_storage_add_point(
    const char* dev_addr,
    const char* decoder,
    const char* key,
    uint32_t timestamp,
    float value,
    const char* unit
);

/**
 * @brief Wall-clock tick: flushes completed RAM buckets to LittleFS
 *        Call periodically (e.g. every 1-2 seconds) from loop1()
 */
void data_storage_process_buckets(void);

void data_storage_aggregate_hourly(void);
void data_storage_aggregate_daily(void);

data_point_t* data_storage_query(
    const char* channel_key,
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    uint32_t interval_sec,
    uint32_t max_points,
    uint32_t* actual_interval_sec,
    size_t* count
);

const data_channel_config_t* data_storage_get_config(const char* channel_key);
data_channel_state_t* data_storage_get_state(const char* channel_key);

void data_storage_build_roundtrip_path(const char* channel_key, char* path, size_t path_size);
void data_storage_build_hourly_path(const char* channel_key, char* path, size_t path_size);
void data_storage_build_daily_path(const char* channel_key, char* path, size_t path_size);
bool data_storage_trim_file(const char* path, uint32_t max_entries);

// ============================================================================
// Chart Data Export Types
// ============================================================================

#define DATA_FLAG_REAL          0x00  ///< Measured real value
#define DATA_FLAG_ESTIMATED     0x01  ///< Interpolated / held replacement value
#define DATA_FLAG_AVERAGED      0x02  ///< Aggregated value over time window
#define DATA_FLAG_ALARM_WARN    0x04  ///< Warning threshold active
#define DATA_FLAG_ALARM_DANGER  0x08  ///< Danger / Alarm threshold active
#define DATA_FLAG_NO_DATA       0x10  ///< Gap / missing interval (e.g. power outage)

typedef struct __attribute__((packed)) {
    uint32_t timestamp;  ///< UTC Unix timestamp
    float    value;      ///< Measured, estimated, or averaged value
    uint8_t  flags;      ///< Bitmask of DATA_FLAG_*
} chart_data_point_t;

chart_data_point_t* data_storage_query_chart(
    const char* channel_key,
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    uint32_t interval_sec,
    uint32_t max_points,
    float alarm_min,
    float alarm_max,
    uint32_t* actual_interval_sec,
    size_t* count
);


#endif // DATA_STORAGE_HPP