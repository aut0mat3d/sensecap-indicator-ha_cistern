/**
 * @file littlefs_driver.hpp
 * @brief Hardware BlockDevice driver and LittleFS filesystem manager for SD Partition 2.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef LITTLEFS_DRIVER_HPP
#define LITTLEFS_DRIVER_HPP

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Read file contents into a pre-allocated buffer.
 * @param path Full file path.
 * @param buffer Output buffer.
 * @param max_len Maximum bytes to read.
 * @return Number of bytes read, or -1 on error.
 */
int32_t littlefs_driver_read_file(const char* path, uint8_t* buffer, size_t max_len);

#ifdef __cplusplus
extern "C" {
#endif

/* ── Partition 2 Constants (matches MBR in sdcard.cpp) ─────────────────── */
#define LFS_P2_START_LBA       4456448UL   ///< Start sector of Partition 2 in MBR (offset ~2.12 GB)
#define LFS_P2_SECTOR_COUNT    2097152UL   ///< 1024 MB in 512-byte sectors
#define LFS_BLOCK_SIZE         4096UL      ///< 4KB LittleFS block size (8 SD sectors per block)
#define LFS_SECTORS_PER_BLOCK  (LFS_BLOCK_SIZE / 512UL) ///< 8 sectors per block
#define LFS_TOTAL_BLOCKS       (LFS_P2_SECTOR_COUNT / LFS_SECTORS_PER_BLOCK) ///< 262144 blocks

/**
 * @brief Opaque file handle for reading lines from LittleFS files.
 */
typedef void* lfs_file_handle_t;

/**
 * @brief Initialize block driver and attempt to mount LittleFS on Partition 2.
 * @return true if Partition 2 is successfully mounted, false otherwise.
 */
bool littlefs_driver_mount(void);

/**
 * @brief Format Partition 2 with LittleFS file system structures and mount it.
 * @return true on success, false on I/O failure.
 */
bool littlefs_driver_format(void);

/**
 * @brief Unmount LittleFS filesystem safely.
 */
void littlefs_driver_unmount(void);

/**
 * @brief Check if LittleFS on Partition 2 is currently mounted and operational.
 */
bool littlefs_driver_is_mounted(void);

/**
 * @brief Recursively ensure that all parent directories along the path exist.
 * @param path File path (e.g. "/data/05975DC9/EM500-SWL/depth/roundtrip.csv")
 * @return true if directories exist, false on error.
 */
bool littlefs_driver_ensure_dir(const char* path);

/**
 * @brief Append a single text line to a file on LittleFS Partition 2.
 * Automatically creates parent directories if needed.
 * @param path Full file path
 * @param line Null-terminated string (including newline)
 * @return true on success, false on I/O error.
 */
bool littlefs_driver_append_line(const char* path, const char* line);

/**
 * @brief Overwrite / rewrite an entire file with a list of text lines.
 * Useful for trimming files without dynamic memory fragmentation.
 * @param path Full file path
 * @param lines Array of string pointers
 * @param line_count Number of lines to write
 * @return true on success, false on error.
 */
bool littlefs_driver_rewrite_file(const char* path, const char* const* lines, size_t line_count);

/**
 * @brief Open an existing file for line-by-line reading.
 * @param path File path
 * @return File handle, or NULL if file not found or mount error.
 */
lfs_file_handle_t littlefs_driver_open_read(const char* path);

/**
 * @brief Read a single line from an open LittleFS file handle.
 * @param handle Handle returned from littlefs_driver_open_read
 * @param line_buf Output buffer for line
 * @param max_len Maximum length of buffer
 * @return Number of characters read, or -1 on EOF/error.
 */
int32_t littlefs_driver_read_line(lfs_file_handle_t handle, char* line_buf, size_t max_len);

/**
 * @brief Close an open LittleFS file handle.
 */
void littlefs_driver_close(lfs_file_handle_t handle);

/**
 * @brief Check if a file or directory exists on LittleFS.
 */
bool littlefs_driver_exists(const char* path);

/**
 * @brief Remove a file from LittleFS.
 */
bool littlefs_driver_remove(const char* path);

/**
 * @brief Append raw bytes to a file (binary safe). Creates parent directories if needed.
 * @return true if all bytes were written and synced.
 */
bool littlefs_driver_append_bytes(const char* path, const void* data, size_t len);

/**
 * @brief Read raw bytes from an open file handle (shares the read-ahead buffer with read_line).
 * @return Number of bytes read (0 at EOF), or -1 on error.
 */
int32_t littlefs_driver_read_bytes(lfs_file_handle_t handle, void* buf, size_t len);

/**
 * @brief Read len bytes at a byte offset of a file (open, seek, read, close).
 * @return Number of bytes read, or -1 on error.
 */
int32_t littlefs_driver_read_at(const char* path, uint32_t offset, void* buf, size_t len);

/**
 * @brief Size of a file in bytes, or -1 if it does not exist.
 */
int32_t littlefs_driver_file_size(const char* path);

/**
 * @brief Directory entry callback for littlefs_driver_list_dir().
 * @return false to stop the iteration.
 */
typedef bool (*lfs_dir_visit_fn)(const char* name, uint32_t size, bool is_dir, void* ctx);

/**
 * @brief Iterate the entries of a directory ("." and ".." are skipped).
 * @return Number of visited entries, or -1 if the directory cannot be opened.
 */
int32_t littlefs_driver_list_dir(const char* path, lfs_dir_visit_fn visit, void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* LITTLEFS_DRIVER_HPP */