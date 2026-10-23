/**
 * @file littlefs_buffer.cpp
 * @brief LittleFS Partition Manager with Write-Through Buffer for SD Card
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "littlefs_buffer.hpp"
#include "littlefs_driver.hpp"
#include "sdcard.hpp"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <Arduino.h>

// ============================================================================
// Static State & Configuration
// ============================================================================

static bool s_sd_present = false;

// Buffer state: 256 entries in 64KB RAM
#define BUFFER_MAX_ENTRIES 256
static lfs_buffer_entry_t* s_buffer_entries = nullptr;
static size_t s_buffer_entry_count = 0;
static uint8_t s_buffer_memory[LITTLEFS_BUFFER_SIZE];
static size_t s_buffer_used = 0;
static size_t s_overflow_count = 0;

static lfs_sd_status_callback_t s_sd_status_callback = nullptr;
static void* s_sd_status_user_data = nullptr;

// ============================================================================
// Public API: Initialization & Shutdown
// ============================================================================

bool littlefs_buffer_init(SdFat32* sd, uint8_t littlefs_partition) {
    (void)sd;
    (void)littlefs_partition;

    if (!s_buffer_entries) {
        s_buffer_entries = (lfs_buffer_entry_t*)malloc(BUFFER_MAX_ENTRIES * sizeof(lfs_buffer_entry_t));
        if (!s_buffer_entries) {
            Serial.println("[LFS_BUF_ERR] Failed to allocate RAM buffer entries!");
            return false;
        }
    }

    memset(s_buffer_entries, 0, BUFFER_MAX_ENTRIES * sizeof(lfs_buffer_entry_t));
    memset(s_buffer_memory, 0, sizeof(s_buffer_memory));
    s_buffer_entry_count = 0;
    s_buffer_used = 0;
    s_overflow_count = 0;

    uint8_t is_present = 0, is_usb_mounted = 0;
    int8_t state = SD_STATE_UNCHECKED;
    uint32_t capacity_mb = 0;
    char label[12] = {0};
    sd_card_get_info(is_present, is_usb_mounted, state, capacity_mb, label);
    s_sd_present = (is_present != 0);

    Serial.printf("[LFS_BUF] Initialized: SD=%s, LFS=%s, RAM Buffer=%u entries (%u KB)\r\n",
                  s_sd_present ? "present" : "absent",
                  littlefs_driver_is_mounted() ? "mounted" : "unmounted",
                  BUFFER_MAX_ENTRIES, LITTLEFS_BUFFER_SIZE / 1024);

    return true;
}

void littlefs_buffer_shutdown(void) {
    if (s_buffer_entry_count > 0) {
        littlefs_buffer_flush_all();
    }

    littlefs_driver_unmount();

    if (s_buffer_entries) {
        free(s_buffer_entries);
        s_buffer_entries = nullptr;
    }
    s_buffer_entry_count = 0;
    s_buffer_used = 0;
}

bool littlefs_is_mounted(void) {
    return littlefs_driver_is_mounted();
}

bool littlefs_sd_is_present(void) {
    return s_sd_present;
}

// ============================================================================
// Buffer Operations
// ============================================================================

bool littlefs_buffer_append(
    const char* path,
    const uint8_t* data,
    size_t size,
    lfs_buffer_entry_type_t type,
    void* user_ctx,
    lfs_flush_callback_t flush_cb
) {
    if (!path || !data || size == 0) return false;

    /* Enqueue directly into RAM queue without repeating failed hardware SPI writes */
    if (!s_buffer_entries || s_buffer_entry_count >= BUFFER_MAX_ENTRIES || s_buffer_used + size > LITTLEFS_BUFFER_SIZE) {
        s_overflow_count++;
        Serial.printf("[LFS_BUF_ERR] Buffer overflow! Entries=%u/%u, Bytes=%u/%u\r\n",
                      (unsigned)s_buffer_entry_count, BUFFER_MAX_ENTRIES,
                      (unsigned)s_buffer_used, LITTLEFS_BUFFER_SIZE);
        return false;
    }

    lfs_buffer_entry_t* entry = &s_buffer_entries[s_buffer_entry_count];
    strncpy(entry->path, path, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';
    entry->type = type;
    entry->user_ctx = user_ctx;
    entry->flush_cb = flush_cb;
    entry->timestamp_ms = millis();

    entry->data = &s_buffer_memory[s_buffer_used];
    entry->size = size;
    entry->is_dynamic = false;
    memcpy(entry->data, data, size);

    s_buffer_used += size;
    s_buffer_entry_count++;

    Serial.printf("[LFS_BUF] Queued in RAM: %s (%u B), Total buffered: %u entries\r\n",
                  path, (unsigned)size, (unsigned)s_buffer_entry_count);
    return true;
}

int littlefs_buffer_flush_all(void) {
    if (s_buffer_entry_count == 0) return 0;

    /* STRICT OWNERSHIP GUARD: Verify SD card matches device before flushing! */
    uint8_t is_present = 0, is_usb_mounted = 0;
    int8_t state = SD_STATE_UNCHECKED;
    uint32_t capacity_mb = 0;
    char volume_label[12] = {0};
    sd_card_get_info(is_present, is_usb_mounted, state, capacity_mb, volume_label);

    if (state != SD_STATE_VALID_FAT32_LFS || !littlefs_driver_is_mounted()) {
        return -1;
    }

    Serial.printf("[LFS_BUF] Flushing %u buffered entries to LittleFS Partition 2...\r\n", 
                  (unsigned)s_buffer_entry_count);

    size_t flushed_count = 0;
    for (size_t i = 0; i < s_buffer_entry_count; i++) {
        lfs_buffer_entry_t* entry = &s_buffer_entries[i];

        char line[256];
        size_t copy_len = (entry->size < sizeof(line) - 1) ? entry->size : (sizeof(line) - 1);
        memcpy(line, entry->data, copy_len);
        line[copy_len] = '\0';

        if (littlefs_driver_append_line(entry->path, line)) {
            flushed_count++;
        } else {
            Serial.printf("[LFS_BUF_ERR] Flush failed at entry %u, stopping to protect unwritten data!\r\n", (unsigned)i);
            break;
        }
    }

    /* Clean up flushed entries safely without losing unwritten ones */
    if (flushed_count == s_buffer_entry_count) {
        s_buffer_entry_count = 0;
        s_buffer_used = 0;
    } else if (flushed_count > 0) {
        size_t bytes_removed = 0;
        for (size_t i = 0; i < flushed_count; i++) {
            bytes_removed += s_buffer_entries[i].size;
        }
        size_t remaining_entries = s_buffer_entry_count - flushed_count;
        memmove(&s_buffer_entries[0], &s_buffer_entries[flushed_count], remaining_entries * sizeof(lfs_buffer_entry_t));
        memmove(&s_buffer_memory[0], &s_buffer_memory[bytes_removed], s_buffer_used - bytes_removed);
        s_buffer_entry_count = remaining_entries;
        s_buffer_used -= bytes_removed;
    }

    Serial.printf("[LFS_BUF] Flush complete: %u written, %u retained in RAM.\r\n", 
                  (unsigned)flushed_count, (unsigned)s_buffer_entry_count);
    return (int)flushed_count;
}

void littlefs_notify_sd_status(bool sd_present, bool littlefs_ready) {
    bool was_present = s_sd_present;
    s_sd_present = sd_present;

    if (!was_present && sd_present && littlefs_ready) {
        Serial.println("[LFS_BUF] SD reconnected and validated. Flushing RAM buffer...");
        littlefs_buffer_flush_all();
    }
}

bool littlefs_write_direct(const char* path, const uint8_t* data, size_t size) {
    if (!littlefs_driver_is_mounted() || !path || !data || size == 0) return false;
    char line[256];
    size_t copy_len = (size < sizeof(line) - 1) ? size : (sizeof(line) - 1);
    memcpy(line, data, copy_len);
    line[copy_len] = '\0';
    return littlefs_driver_append_line(path, line);
}

int littlefs_read_direct(const char* path, uint8_t* buffer, size_t max_size) {
    return littlefs_driver_read_file(path, buffer, max_size);
}

bool littlefs_exists(const char* path) {
    return littlefs_driver_exists(path);
}

bool littlefs_delete(const char* path) {
    return littlefs_driver_remove(path);
}

void littlefs_buffer_clear(void) {
    s_buffer_entry_count = 0;
    s_buffer_used = 0;
    Serial.println("[LFS_BUF] RAM Buffer cleared.");
}

void littlefs_buffer_get_stats(lfs_buffer_stats_t* stats) {
    if (!stats) return;
    stats->entries_count = s_buffer_entry_count;
    stats->total_bytes = s_buffer_used;
    stats->max_entries = BUFFER_MAX_ENTRIES;
    stats->max_bytes = LITTLEFS_BUFFER_SIZE;
    stats->overflow_count = s_overflow_count;
}

bool littlefs_format_partition(uint8_t partition) {
    (void)partition;
    return littlefs_driver_format();
}

bool littlefs_create_structure(void) {
    if (!littlefs_driver_is_mounted()) return false;
    littlefs_driver_ensure_dir("/data/test.tmp");
    return true;
}