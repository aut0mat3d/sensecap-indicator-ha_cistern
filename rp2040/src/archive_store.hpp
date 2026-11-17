/**
 * @file archive_store.hpp
 * @brief Segmented binary archive on LittleFS (SD partition 2).
 *
 * Layout (one directory per device and data point, one sub-directory per level):
 *   /data/<DEVADDR>/<key>/raw/00000000.bin, 00000001.bin, ...   grid values (arc_raw_record_t)
 *   /data/<DEVADDR>/<key>/hour/...                              hourly aggregates (arc_agg_record_t)
 *   /data/<DEVADDR>/<key>/day/...                               daily aggregates (arc_agg_record_t)
 *
 * Every segment holds at most ARC_SEGMENT_BYTES of fixed-size records (one LittleFS block).
 * Records are only appended; when a segment is full a new one is started, and the oldest
 * segment is deleted once the remaining segments still hold the configured retention.
 * Retention is therefore a guaranteed minimum (exceeded by at most one segment).
 * No file is ever rewritten, so no trim buffer is needed and retention is not limited by RAM.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#ifndef ARCHIVE_STORE_HPP
#define ARCHIVE_STORE_HPP

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/** Segment size: one LittleFS block (see LFS_BLOCK_SIZE) */
#define ARC_SEGMENT_BYTES 4096U

typedef enum {
    ARC_LEVEL_RAW  = 0,  /// Grid values (5-minute grid or finer)
    ARC_LEVEL_HOUR = 1,  /// Hourly aggregates (full UTC hour)
    ARC_LEVEL_DAY  = 2,  /// Daily aggregates (local midnight)
    ARC_LEVEL_COUNT
} arc_level_t;

/** Grid value: 9 bytes */
typedef struct __attribute__((packed)) {
    uint32_t ts;     /// Bucket start (UTC)
    float    value;
    uint8_t  flag;   /// RT_FLAG_REAL / RT_FLAG_ESTIMATED / RT_FLAG_HOLD
} arc_raw_record_t;

#define ARC_AGG_FLAG_DEGRADED  0x01  /// Coverage below solid_pct (shown as estimated)

/** Aggregated value: 23 bytes */
typedef struct __attribute__((packed)) {
    uint32_t ts;             /// Period start (UTC)
    float    avg;
    float    min;
    float    max;
    uint32_t count;          /// Covered grid samples (quality model)
    int16_t  tz_offset_min;  /// Local UTC offset at period start in minutes (daily level)
    uint8_t  flags;          /// ARC_AGG_FLAG_*
} arc_agg_record_t;

/** Per-level segment bookkeeping (RAM) */
typedef struct {
    uint32_t first_seq;     /// Oldest existing segment number
    uint32_t last_seq;      /// Newest segment number (append target)
    uint32_t last_records;  /// Records in the newest segment
    uint32_t keep_records;  /// Retention: records to keep at least (0 = level disabled)
    bool     has_segments;  /// At least one segment exists
} arc_level_state_t;

/** One archived data point of one device */
typedef struct {
    char dir[48];           /// "/data/<DEVADDR>/<key>"
    bool scanned;           /// Segment state has been read from LittleFS
    arc_level_state_t level[ARC_LEVEL_COUNT];
} arc_channel_t;

/**
 * @brief Visitor for arc_scan_range(); record points to an arc_raw_record_t or arc_agg_record_t.
 * @return false to stop the scan.
 */
typedef bool (*arc_visit_fn)(const void* record, void* ctx);

size_t arc_record_size(arc_level_t level);
uint32_t arc_records_per_segment(arc_level_t level);

/**
 * @brief Initialize the RAM descriptor (no I/O). keep[] holds the retention per level.
 */
void arc_channel_setup(arc_channel_t* ch, const char* dev_addr, const char* key,
                       const uint32_t keep[ARC_LEVEL_COUNT]);

/**
 * @brief Read the existing segments of all levels from LittleFS and remove legacy CSV files.
 *        Called automatically by the other functions when needed.
 * @return false if LittleFS is not mounted.
 */
bool arc_channel_scan(arc_channel_t* ch);

/**
 * @brief Append one record to a level and enforce its retention.
 * @return false if LittleFS is not mounted or the write failed (caller keeps the record).
 */
bool arc_append(arc_channel_t* ch, arc_level_t level, const void* record);

/**
 * @brief Visit all records with from_ts <= ts < to_ts in chronological segment order.
 * @return Number of visited records.
 */
size_t arc_scan_range(arc_channel_t* ch, arc_level_t level, uint32_t from_ts, uint32_t to_ts,
                      arc_visit_fn visit, void* ctx);

/**
 * @brief Read the newest record of a level.
 * @return false if the level is empty or not readable.
 */
bool arc_read_last(arc_channel_t* ch, arc_level_t level, void* record_out);

#endif /* ARCHIVE_STORE_HPP */
