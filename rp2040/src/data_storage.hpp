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
#include "archive_store.hpp"

class SdFat32;

// ============================================================================
// Granularity Types
// ============================================================================

typedef enum {
    GRAN_ROUNDTRIP = 0,  /// Raw/Bucket-averaged data (configurable interval)
    GRAN_HOURLY    = 1,  /// Hourly aggregated values (retention "hours")
    GRAN_DAILY     = 2   /// Daily aggregated values (retention "days")
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
    uint32_t count;       /// Covered samples behind an aggregated point (hourly/daily), 0 for raw points
    bool is_gap;          /// True if no value exists for this slot (DATA_FLAG_NO_DATA)
    float value_min;      /// Lowest value behind the point (raw points: = value)
    float value_max;      /// Highest value behind the point (raw points: = value)
} data_point_t;

/** Aggregated data point for hourly/daily storage */
typedef struct {
    uint32_t start_timestamp;  ///< Start of aggregation interval (UTC)
    int32_t  tz_offset_sec;    ///< Timezone offset to local midnight in seconds (e.g. +7200)
    float    avg_value;        ///< Average value in interval
    float    min_value;        ///< Minimum value in interval
    float    max_value;        ///< Maximum value in interval
    uint32_t count;            ///< Covered samples in interval (real + valid hold, see quality model)
    bool     has_estimated;    ///< True if coverage is below solid_pct (degraded period)
} data_aggregated_t;

// ============================================================================
// Channel Configuration & Runtime Accumulator State
// ============================================================================

/**
 * @brief Retention levels of the archive pyramid (index into data_channel_config_t::retention).
 *        Configured per key in the device JSON: "archive": { "<key>": { "retention": { ... } } }
 */
typedef enum {
    RET_SINGLE = 0,  /// raw/ segments (bucket grid, min(interval, 300 s))
    RET_HOURS  = 1,  /// hour/ segments (full UTC hour)
    RET_DAYS   = 2,  /// day/ segments (local midnight)
    RET_WEEKS  = 3,  /// parsed, aggregation not implemented yet
    RET_MONTHS = 4,  /// parsed, aggregation not implemented yet
    RET_YEARS  = 5,  /// parsed, aggregation not implemented yet
    RET_LEVEL_COUNT
} retention_level_t;

/** Flag of a grid record (arc_raw_record_t::flag) */
#define RT_FLAG_REAL       0  ///< Bucket contains at least one received sample
#define RT_FLAG_ESTIMATED  1  ///< Substitute value after a missed packet (deadline exceeded)
#define RT_FLAG_HOLD       2  ///< No new sample in this bucket, but the next packet is not overdue yet

/** Defaults of the aggregation quality model ("quality" block of an archive key) */
#define QUALITY_DEFAULT_SOLID_PCT      66
#define QUALITY_DEFAULT_MIN_PCT        10
#define QUALITY_DEFAULT_USE_ESTIMATED  false

/**
 * @brief Per-channel storage configuration (source: "archive" block of /devices/<name>.json)
 */
typedef struct {
    char channel_key[64];        /// Unique key: "[dev_addr].[decoder].[key]"
    char dev_addr[17];          /// Device address, normalized to 8 hex digits (e.g. "05975DC9", "00000045")
    char decoder[32];           /// Decoder name (e.g. "EM500-SWL")
    char key[32];               /// Channel key (e.g. "depth"), decoder key or virtual key
    uint16_t retention[RET_LEVEL_COUNT]; /// Configured entries per level (0 = level disabled)
    uint16_t keep_entries[RET_LEVEL_COUNT]; /// Effective retention incl. staging levels (0 = level not written)
    uint16_t max_estimated_count; /// Max consecutive substitute values after a missed packet (0 = never)
    uint8_t  solid_pct;          /// Coverage >= solid_pct: aggregated value is regular
    uint8_t  min_pct;            /// Coverage >= min_pct: aggregated value flagged ESTIMATED; below: gap
    bool     use_estimated;      /// Substitute values enter aggregated averages (coverage never counts them)
    uint32_t roundtrip_interval;    /// Expected update interval of this key (expected_interval_sec, default: device report_interval_sec)
    uint32_t storage_interval;      /// Wall-clock storage grid in seconds (capped at 300s)
    uint32_t tolerance_sec;      /// Per-channel jitter tolerance from JSON
    char unit[8];               /// Unit string from decoder mapping or virtual decoder
} data_channel_config_t;

/**
 * @brief Per-channel runtime accumulator state (RAM-based, zero SD wear)
 */
typedef struct {
    uint32_t current_bucket;    /// Start timestamp of current wall-clock window
    double   accumulator;       /// High-precision IEEE-754 sum of samples in window
    uint16_t sample_count;      /// Number of sensor samples received in window (cnt)
    uint32_t last_rx_time;      /// Unix timestamp of the last real sensor packet received
    uint32_t last_stored_time;  /// Timestamp of last written record on LittleFS
    float    last_value;        /// Last known value (used for hold / missing estimation)
    bool     has_last_value;    /// True if last_value is valid
    uint16_t consecutive_estimated; /// Substitute values written since the last real packet
} data_channel_state_t;

/**
 * @brief Combined channel configuration and state
 */
typedef struct {
    data_channel_config_t config;
    data_channel_state_t state;
    arc_channel_t arc;          /// Segment archive of this channel (/data/<dev_addr>/<key>/...)
    uint32_t key_hash;          /// Hash of config.channel_key (references queued records)
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

/**
 * @brief Scan all device JSON files in /devices/ and register one archive channel per entry of the
 *        optional "archive" object. Keys without an "archive" entry are not stored.
 */
bool data_storage_load_devices(SdFat32* sd);
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

/**
 * @brief Write records queued in RAM while LittleFS was unavailable (card removed, USB-MSC).
 *        Called periodically from process_buckets() with a small bound per tick.
 * @param max_records Upper bound per call (0 = all; blocks the caller accordingly).
 * @return Number of records written.
 */
size_t data_storage_flush_pending(size_t max_records);

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


// ============================================================================
// Chart Data Export Types
// ============================================================================

#define DATA_FLAG_REAL          0x00  ///< Measured real value
#define DATA_FLAG_ESTIMATED     0x01  ///< Interpolated / held replacement value
#define DATA_FLAG_AVERAGED      0x02  ///< Aggregated value over time window
#define DATA_FLAG_ALARM_WARN    0x04  ///< Warning threshold active
#define DATA_FLAG_ALARM_DANGER  0x08  ///< Danger / Alarm threshold active

/* Severity of the chart query alarm levels (CMD_GET_CHART_DATA byte 44, same bits on the ESP32):
 * a set bit marks the level as warning (DATA_FLAG_ALARM_WARN), a cleared bit as danger */
#define CHART_ALARM_MIN_IS_WARNING  0x01
#define CHART_ALARM_MAX_IS_WARNING  0x02

/* Chart query mode (CMD_GET_CHART_DATA byte 45, same values on the ESP32) */
#define CHART_QUERY_MODE_GRID  0   ///< one value per interval bucket (fixed grid)
#define CHART_QUERY_MODE_LTTB  1   ///< LTTB-reduced representative points with real timestamps
/* LTTB time buckets for max_points: points plus one gap marker per gap always fit into max_points */
#define CHART_LTTB_BUCKETS(max_points)  ((((max_points) * 2) / 3) < 2 ? 2 : (((max_points) * 2) / 3))
#define LTTB_MAX_POINTS        100 ///< upper bound for max_points in LTTB mode
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
    uint8_t alarm_flags,
    uint32_t* actual_interval_sec,
    size_t* count
);

/**
 * @brief Reduce the archived values of [start_timestamp, end_timestamp[ to at most max_points
 *        representative points (Largest-Triangle-Three-Buckets on equal time buckets).
 *
 * Sources: daily, hourly and raw records joined without overlap (each level is used from where
 * the next finer one ends). Two streaming passes over the archive, memory independent of the
 * amount of stored data. Each emitted point keeps its real timestamp; a run of empty buckets
 * between data yields one DATA_FLAG_NO_DATA marker (line break). Alarm flags come from the
 * minimum / maximum of the bucket, so an episode is marked even if another point is chosen.
 *
 * @param actual_interval_sec Out: bucket width in seconds.
 * @return Malloc'd array (caller frees) or nullptr.
 */
chart_data_point_t* data_storage_query_lttb(
    const char* channel_key,
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    uint32_t max_points,
    float alarm_min,
    float alarm_max,
    uint8_t alarm_flags,
    uint32_t* actual_interval_sec,
    size_t* count
);


#endif // DATA_STORAGE_HPP