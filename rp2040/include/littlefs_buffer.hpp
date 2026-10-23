/**
 * @file littlefs_buffer.hpp
 * @brief LittleFS Partition Manager with Write-Through Buffer for SD Card
 *        Handles SD card removal/reinsertion by buffering writes in RAM
 *        and flushing to LittleFS when card becomes available again.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 *
 * Design Notes:
 * - Supports up to 16 LoRa devices (MAX_LORA_DEVICES)
 * - Dynamic source buffering (0-N sources depending on GUI config)
 * - 64KB RAM buffer for pending writes
 * - Automatic flush on SD card reconnect
 * - Thread-safe for RP2040 dual-core operation
 */

#ifndef LITTLEFS_BUFFER_HPP
#define LITTLEFS_BUFFER_HPP

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Forward declaration for SdFat
class SdFat32;

/**
 * @brief Maximum number of concurrent data sources to buffer
 *        Matches MAX_LORA_DEVICES (16) + some margin for GUI elements
 */
#define LITTLEFS_MAX_SOURCES 20

/**
 * @brief Total buffer size: 64KB for pending writes
 *        This allows buffering ~200-400 typical sensor data entries
 *        depending on entry size (avg ~160-320 bytes per entry)
 */
#define LITTLEFS_BUFFER_SIZE (65536)

/**
 * @brief Maximum path length for buffered files
 */
#define LITTLEFS_MAX_PATH_LEN 64

/**
 * @brief Maximum single write size (must be < BUFFER_SIZE)
 */
#define LITTLEFS_MAX_WRITE_SIZE (LITTLEFS_BUFFER_SIZE / 4)

/**
 * @brief Data types for buffered entries
 */
typedef enum {
    LFS_BUFFER_TYPE_SENSOR_DATA,    /**< Sensor measurement data */
    LFS_BUFFER_TYPE_LOG_ENTRY,      /**< System/log entries */
    LFS_BUFFER_TYPE_ALARM_EVENT,    /**< Alarm trigger/clear events */
    LFS_BUFFER_TYPE_GUI_CONFIG,      /**< GUI configuration changes */
    LFS_BUFFER_TYPE_CUSTOM          /**< User-defined data */
} lfs_buffer_entry_type_t;

/**
 * @brief Callback for custom buffer flush handling
 * @param path The file path being flushed
 * @param data Pointer to the buffered data
 * @param size Size of the data in bytes
 * @param user_ctx User context passed during buffer_append
 */
typedef void (*lfs_flush_callback_t)(const char* path, const uint8_t* data, size_t size, void* user_ctx);

/**
 * @brief Buffer entry structure for queued writes
 */
typedef struct {
    char path[LITTLEFS_MAX_PATH_LEN];     /**< Destination file path on LittleFS */
    lfs_buffer_entry_type_t type;         /**< Type of buffered data */
    uint8_t* data;                        /**< Pointer to buffered data (dynamic) */
    size_t size;                         /**< Size of data in bytes */
    uint32_t timestamp_ms;                /**< When this entry was buffered */
    void* user_ctx;                      /**< Optional user context for flush callback */
    lfs_flush_callback_t flush_cb;        /**< Optional custom flush handler */
    bool is_dynamic;                      /**< true if data was malloc'd by buffer system */
} lfs_buffer_entry_t;

/**
 * @brief Buffer statistics
 */
typedef struct {
    size_t entries_count;                 /**< Number of entries in buffer */
    size_t total_bytes;                   /**< Total bytes buffered */
    size_t max_entries;                   /**< Maximum entries configured */
    size_t max_bytes;                     /**< Maximum bytes configured */
    size_t overflow_count;                /**< Number of overflows (buffer full) */
    uint32_t oldest_entry_time_ms;        /**< Timestamp of oldest entry */
    uint32_t newest_entry_time_ms;        /**< Timestamp of newest entry */
} lfs_buffer_stats_t;

/**
 * @brief Initialize the LittleFS buffer system
 * @param sd Reference to the SdFat32 instance
 * @param littlefs_partition Partition number for LittleFS (typically 2)
 * @return true if LittleFS was mounted and buffer system initialized
 */
bool littlefs_buffer_init(SdFat32* sd, uint8_t littlefs_partition = 2);

/**
 * @brief Shutdown the LittleFS buffer system
 *        Flushes all pending writes and unmounts LittleFS
 */
void littlefs_buffer_shutdown(void);

/**
 * @brief Check if LittleFS partition is currently available
 * @return true if LittleFS is mounted and accessible
 */
bool littlefs_is_mounted(void);

/**
 * @brief Check if SD card is present and accessible
 * @return true if SD card is physically present
 */
bool littlefs_sd_is_present(void);

/**
 * @brief Append data to the write buffer
 * @param path Destination path on LittleFS (e.g., "/logs/sensor_05975dc9.json")
 * @param data Pointer to data to buffer
 * @param size Size of data in bytes
 * @param type Type of data being buffered
 * @param user_ctx Optional user context for flush callback
 * @param flush_cb Optional custom flush callback (NULL = default file write)
 * @return true if data was buffered successfully, false if buffer is full
 */
bool littlefs_buffer_append(
    const char* path,
    const uint8_t* data,
    size_t size,
    lfs_buffer_entry_type_t type = LFS_BUFFER_TYPE_CUSTOM,
    void* user_ctx = nullptr,
    lfs_flush_callback_t flush_cb = nullptr
);

/**
 * @brief Append formatted string to buffer
 * @param path Destination path
 * @param format printf-style format string
 * @param ... Format arguments
 * @return true if buffered successfully
 */
bool littlefs_buffer_append_fmt(
    const char* path,
    lfs_buffer_entry_type_t type,
    const char* format,
    ...
);

/**
 * @brief Force flush of all buffered entries to LittleFS
 *        This is called automatically on SD reconnect, but can be
 *        triggered manually if needed.
 * @return Number of entries flushed, or -1 on error
 */
int littlefs_buffer_flush_all(void);

/**
 * @brief Flush only entries for a specific path prefix
 * @param path_prefix Path prefix to match (e.g., "/logs/" to flush all logs)
 * @return Number of entries flushed
 */
int littlefs_buffer_flush_prefix(const char* path_prefix);

/**
 * @brief Flush only entries of a specific type
 * @param type Buffer entry type to flush
 * @return Number of entries flushed
 */
int littlefs_buffer_flush_type(lfs_buffer_entry_type_t type);

/**
 * @brief Clear all buffered entries without flushing
 *        Use with caution - data will be lost!
 */
void littlefs_buffer_clear(void);

/**
 * @brief Get current buffer statistics
 * @param stats Output struct for statistics
 */
void littlefs_buffer_get_stats(lfs_buffer_stats_t* stats);

/**
 * @brief Register a callback for SD card status changes
 * @param callback Function to call when SD status changes
 * @param user_data User data passed to callback
 */
typedef void (*lfs_sd_status_callback_t)(bool sd_present, bool littlefs_mounted, void* user_data);
void littlefs_register_sd_status_callback(lfs_sd_status_callback_t callback, void* user_data);

/**
 * @brief Manually notify the buffer system of SD card status change
 *        Call this from your existing SD card detection code
 * @param sd_present true if SD card is physically present
 * @param littlefs_ready true if LittleFS partition is ready
 */
void littlefs_notify_sd_status(bool sd_present, bool littlefs_ready);

/**
 * @brief Write directly to LittleFS (bypasses buffer)
 *        Use when SD is known to be present and you need immediate write
 * @param path File path
 * @param data Data to write
 * @param size Size of data
 * @return true on success
 */
bool littlefs_write_direct(const char* path, const uint8_t* data, size_t size);

/**
 * @brief Read directly from LittleFS
 * @param path File path
 * @param buffer Output buffer for data
 * @param max_size Maximum bytes to read
 * @return Number of bytes read, or -1 on error
 */
int littlefs_read_direct(const char* path, uint8_t* buffer, size_t max_size);

/**
 * @brief Check if a file exists on LittleFS
 * @param path File path
 * @return true if file exists
 */
bool littlefs_exists(const char* path);

/**
 * @brief Delete a file from LittleFS
 * @param path File path
 * @return true on success
 */
bool littlefs_delete(const char* path);

/**
 * @brief List files in a directory on LittleFS
 * @param path Directory path
 * @param callback Function called for each file found
 * @param user_data User data for callback
 * @return Number of files listed, or -1 on error
 */
typedef void (*lfs_list_callback_t)(const char* filename, uint32_t size, void* user_data);
int littlefs_list(const char* path, lfs_list_callback_t callback, void* user_data);

/**
 * @brief Get free space on LittleFS partition
 * @return Free space in bytes, or -1 on error
 */
int32_t littlefs_free_space(void);

/**
 * @brief Get total space on LittleFS partition
 * @return Total space in bytes, or -1 on error
 */
int32_t littlefs_total_space(void);

/**
 * @brief Format the LittleFS partition (creates file system structure)
 * @param partition Partition number to format (typically 2)
 * @return true on success
 */
bool littlefs_format_partition(uint8_t partition);

/**
 * @brief Create basic directory structure on LittleFS
 * @return true on success
 */
bool littlefs_create_structure(void);

#endif /* LITTLEFS_BUFFER_HPP */
