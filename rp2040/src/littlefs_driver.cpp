/**
 * @file littlefs_driver.cpp
 * @brief Hardware BlockDevice driver and LittleFS filesystem manager for SD Partition 2.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "littlefs_driver.hpp"
#include "debug_log.hpp"
#include "sdcard.hpp"
#include <LittleFS.h>
#include <Arduino.h>

// Static LittleFS filesystem state
static lfs_t s_lfs;
static struct lfs_config s_cfg;
static bool s_mounted = false;

// LittleFS cache buffers (static allocation in RP2040 RAM)
static uint8_t s_read_buffer[512];
static uint8_t s_prog_buffer[512];
static uint8_t s_lookahead_buffer[128];

/* ── Block Device Driver Callbacks (LittleFS v2 -> SdFat SPI1) ─────────── */

static int lfs_sd_read(const struct lfs_config *c, lfs_block_t block,
                       lfs_off_t off, void *buffer, lfs_size_t size) {
    if (!sd_card_get_fs().card()) return LFS_ERR_IO;
    if (block >= c->block_count) return LFS_ERR_INVAL;

    uint32_t sector = LFS_P2_START_LBA + (block * LFS_SECTORS_PER_BLOCK) + (off / 512);
    size_t count = size / 512;

    if (!sd_card_get_fs().card()->readSectors(sector, (uint8_t*)buffer, count)) {
        LogSerial.printf("[LFS_DRV_ERR] Read error: LBA %lu, count %u\r\n", (unsigned long)sector, (unsigned)count);
        return LFS_ERR_IO;
    }
    return LFS_ERR_OK;
}

static int lfs_sd_prog(const struct lfs_config *c, lfs_block_t block,
                       lfs_off_t off, const void *buffer, lfs_size_t size) {
    if (!sd_card_get_fs().card()) return LFS_ERR_IO;
    if (block >= c->block_count) return LFS_ERR_INVAL;

    uint32_t sector = LFS_P2_START_LBA + (block * LFS_SECTORS_PER_BLOCK) + (off / 512);
    size_t count = size / 512;

    if (!sd_card_get_fs().card()->writeSectors(sector, (const uint8_t*)buffer, count)) {
        LogSerial.printf("[LFS_DRV_ERR] Prog error: LBA %lu, count %u\r\n", (unsigned long)sector, (unsigned)count);
        return LFS_ERR_IO;
    }
    return LFS_ERR_OK;
}

static int lfs_sd_erase(const struct lfs_config *c, lfs_block_t block) {
    (void)c;
    (void)block;
    /* SD cards handle sector overwrite internally via their on-board flash translation layer */
    return LFS_ERR_OK;
}

static int lfs_sd_sync(const struct lfs_config *c) {
    (void)c;
    if (!sd_card_get_fs().card()) return LFS_ERR_IO;
    return sd_card_get_fs().card()->syncDevice() ? LFS_ERR_OK : LFS_ERR_IO;
}

static void _init_cfg(void) {
    memset(&s_cfg, 0, sizeof(s_cfg));

    s_cfg.context = nullptr;

    // Block device operations
    s_cfg.read  = lfs_sd_read;
    s_cfg.prog  = lfs_sd_prog;
    s_cfg.erase = lfs_sd_erase;
    s_cfg.sync  = lfs_sd_sync;

    // Block device geometry
    s_cfg.read_size = 512;
    s_cfg.prog_size = 512;
    s_cfg.block_size = LFS_BLOCK_SIZE;          // 4096 bytes
    s_cfg.block_count = LFS_TOTAL_BLOCKS;        // 262144 blocks (1024 MB)
    s_cfg.block_cycles = 500;
    s_cfg.cache_size = 512;
    s_cfg.lookahead_size = 128;

    s_cfg.read_buffer = s_read_buffer;
    s_cfg.prog_buffer = s_prog_buffer;
    s_cfg.lookahead_buffer = s_lookahead_buffer;
}

/* ── Public API Implementation ─────────────────────────────────────────── */

bool littlefs_driver_mount(void) {
    if (s_mounted) return true;
    if (!sd_card_get_fs().card()) {
        LogSerial.println("[LFS_DRV] Mount skipped: SD card hardware not initialized");
        return false;
    }

    _init_cfg();

    LogSerial.printf("[LFS_DRV] Attempting mount on Partition 2 (LBA %lu..%lu, %lu blocks)...\r\n",
                  LFS_P2_START_LBA, LFS_P2_START_LBA + LFS_P2_SECTOR_COUNT - 1, LFS_TOTAL_BLOCKS);

    int err = lfs_mount(&s_lfs, &s_cfg);
    if (err) {
        LogSerial.printf("[LFS_DRV] Partition 2 mount failed (code %d: unformatted or mismatch)\r\n", err);
        s_mounted = false;
        return false;
    }

    s_mounted = true;
    LogSerial.println("[LFS_DRV] Mount SUCCESS: LittleFS on Partition 2 is ready!");
    return true;
}

bool littlefs_driver_format(void) {
    if (!sd_card_get_fs().card()) {
        LogSerial.println("[LFS_DRV_ERR] Format failed: SD card not ready");
        return false;
    }

    if (s_mounted) {
        lfs_unmount(&s_lfs);
        s_mounted = false;
    }

    _init_cfg();

    LogSerial.printf("[LFS_DRV] Formatting Partition 2 (1024 MB LittleFS v2)...\r\n");
    int err = lfs_format(&s_lfs, &s_cfg);
    if (err) {
        LogSerial.printf("[LFS_DRV_ERR] lfs_format failed with code %d!\r\n", err);
        return false;
    }

    LogSerial.println("[LFS_DRV] Format SUCCESS! Mounting filesystem...");
    err = lfs_mount(&s_lfs, &s_cfg);
    if (err) {
        LogSerial.printf("[LFS_DRV_ERR] Mount after format failed: code %d\r\n", err);
        return false;
    }

    s_mounted = true;
    LogSerial.println("[LFS_DRV] Partition 2 formatted and mounted successfully.");
    return true;
}

void littlefs_driver_unmount(void) {
    if (s_mounted) {
        lfs_unmount(&s_lfs);
        s_mounted = false;
        LogSerial.println("[LFS_DRV] LittleFS Partition 2 unmounted.");
    }
}

bool littlefs_driver_is_mounted(void) {
    return s_mounted;
}

bool littlefs_driver_ensure_dir(const char* path) {
    if (!s_mounted || !path) return false;

    char temp[128];
    strncpy(temp, path, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    char* last_slash = strrchr(temp, '/');
    if (!last_slash) return true;
    *last_slash = '\0';

    for (char* p = temp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            int err = lfs_mkdir(&s_lfs, temp);
            if (err != LFS_ERR_OK && err != LFS_ERR_EXIST) {
                LogSerial.printf("[LFS_DRV_ERR] mkdir failed for %s (code %d)\r\n", temp, err);
                return false;
            }
            *p = '/';
        }
    }

    int err = lfs_mkdir(&s_lfs, temp);
    if (err != LFS_ERR_OK && err != LFS_ERR_EXIST) {
        LogSerial.printf("[LFS_DRV_ERR] mkdir failed for %s (code %d)\r\n", temp, err);
        return false;
    }
    return true;
}

bool littlefs_driver_append_bytes(const char* path, const void* data, size_t len) {
    if (!s_mounted || !path || !data) {
        LogSerial.printf("[LFS_DRV_ERR] Cannot append: mounted=%d, path=%s\r\n", s_mounted, path ? path : "NULL");
        return false;
    }

    if (!littlefs_driver_ensure_dir(path)) {
        return false;
    }

    lfs_file_t file;
    int err = lfs_file_open(&s_lfs, &file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND);
    if (err) {
        LogSerial.printf("[LFS_DRV_ERR] Open failed for append: %s (code %d)\r\n", path, err);
        return false;
    }

    lfs_ssize_t written = lfs_file_write(&s_lfs, &file, data, len);
    int sync_err = lfs_file_sync(&s_lfs, &file);
    lfs_file_close(&s_lfs, &file);

    if (written < 0 || (size_t)written != len || sync_err) {
        LogSerial.printf("[LFS_DRV_ERR] Write mismatch on %s (expected %u, wrote %d, sync %d)\r\n",
                         path, (unsigned)len, (int)written, sync_err);
        return false;
    }

    return true;
}

bool littlefs_driver_append_line(const char* path, const char* line) {
    if (!line) return false;
    return littlefs_driver_append_bytes(path, line, strlen(line));
}

bool littlefs_driver_rewrite_file(const char* path, const char* const* lines, size_t line_count) {
    if (!s_mounted || !path) return false;
    littlefs_driver_ensure_dir(path);

    lfs_file_t file;
    int err = lfs_file_open(&s_lfs, &file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err != LFS_ERR_OK) {
        LogSerial.printf("[LFS_DRV_ERR] Rewrite open failed for %s (code %d)\r\n", path, err);
        return false;
    }

    for (size_t i = 0; i < line_count; i++) {
        if (lines[i] && lines[i][0] != '\0') {
            size_t len = strlen(lines[i]);
            lfs_file_write(&s_lfs, &file, lines[i], len);
            if (lines[i][len - 1] != '\n') {
                lfs_file_write(&s_lfs, &file, "\n", 1);
            }
        }
    }

    lfs_file_sync(&s_lfs, &file);
    lfs_file_close(&s_lfs, &file);
    return true;
}

typedef struct {
    lfs_file_t file;
    uint8_t    buf[512];
    size_t     buf_pos;
    size_t     buf_len;
} lfs_buffered_file_t;

lfs_file_handle_t littlefs_driver_open_read(const char* path) {
    if (!s_mounted || !path) return nullptr;
    lfs_buffered_file_t* bf = (lfs_buffered_file_t*)malloc(sizeof(lfs_buffered_file_t));
    if (!bf) return nullptr;

    bf->buf_pos = 0;
    bf->buf_len = 0;

    int err = lfs_file_open(&s_lfs, &bf->file, path, LFS_O_RDONLY);
    if (err != LFS_ERR_OK) {
        free(bf);
        return nullptr;
    }
    return (lfs_file_handle_t)bf;
}

void littlefs_driver_close(lfs_file_handle_t handle) {
    if (!s_mounted || !handle) return;
    lfs_buffered_file_t* bf = (lfs_buffered_file_t*)handle;
    lfs_file_close(&s_lfs, &bf->file);
    free(bf);
}

int32_t littlefs_driver_read_line(lfs_file_handle_t handle, char* line_buf, size_t max_len) {
    if (!s_mounted || !handle || !line_buf || max_len == 0) return -1;
    lfs_buffered_file_t* bf = (lfs_buffered_file_t*)handle;

    size_t idx = 0;
    while (idx < max_len - 1) {
        if (bf->buf_pos >= bf->buf_len) {
            lfs_ssize_t res = lfs_file_read(&s_lfs, &bf->file, bf->buf, sizeof(bf->buf));
            if (res <= 0) {
                if (idx == 0) return -1; /* EOF reached */
                break;
            }
            bf->buf_pos = 0;
            bf->buf_len = (size_t)res;
        }

        char c = (char)bf->buf[bf->buf_pos++];
        if (c == '\r') continue;
        line_buf[idx++] = c;
        if (c == '\n') break;
    }
    line_buf[idx] = '\0';
    return (int32_t)idx;
}

bool littlefs_driver_exists(const char* path) {
    if (!s_mounted || !path) return false;
    struct lfs_info info;
    return (lfs_stat(&s_lfs, path, &info) == LFS_ERR_OK);
}

bool littlefs_driver_remove(const char* path) {
    if (!s_mounted || !path) return false;
    return (lfs_remove(&s_lfs, path) == LFS_ERR_OK);
}

int32_t littlefs_driver_read_file(const char* path, uint8_t* buffer, size_t max_len) {
    if (!s_mounted || !path || !buffer || max_len == 0) return -1;

    lfs_file_t file;
    int err = lfs_file_open(&s_lfs, &file, path, LFS_O_RDONLY);
    if (err != LFS_ERR_OK) return -1;

    lfs_ssize_t bytes_read = lfs_file_read(&s_lfs, &file, buffer, max_len);
    lfs_file_close(&s_lfs, &file);

    return (bytes_read >= 0) ? (int32_t)bytes_read : -1;
}
int32_t littlefs_driver_read_bytes(lfs_file_handle_t handle, void* buf, size_t len) {
    if (!s_mounted || !handle || !buf) return -1;
    lfs_buffered_file_t* bf = (lfs_buffered_file_t*)handle;
    uint8_t* out = (uint8_t*)buf;
    size_t done = 0;

    while (done < len) {
        if (bf->buf_pos >= bf->buf_len) {
            lfs_ssize_t res = lfs_file_read(&s_lfs, &bf->file, bf->buf, sizeof(bf->buf));
            if (res < 0) return (done > 0) ? (int32_t)done : -1;
            if (res == 0) break; /* EOF */
            bf->buf_pos = 0;
            bf->buf_len = (size_t)res;
        }
        size_t chunk = bf->buf_len - bf->buf_pos;
        if (chunk > len - done) chunk = len - done;
        memcpy(&out[done], &bf->buf[bf->buf_pos], chunk);
        bf->buf_pos += chunk;
        done += chunk;
    }
    return (int32_t)done;
}

int32_t littlefs_driver_read_at(const char* path, uint32_t offset, void* buf, size_t len) {
    if (!s_mounted || !path || !buf) return -1;

    lfs_file_t file;
    if (lfs_file_open(&s_lfs, &file, path, LFS_O_RDONLY) != LFS_ERR_OK) return -1;

    int32_t result = -1;
    if (lfs_file_seek(&s_lfs, &file, (lfs_soff_t)offset, LFS_SEEK_SET) >= 0) {
        lfs_ssize_t n = lfs_file_read(&s_lfs, &file, buf, len);
        result = (n >= 0) ? (int32_t)n : -1;
    }
    lfs_file_close(&s_lfs, &file);
    return result;
}

int32_t littlefs_driver_file_size(const char* path) {
    if (!s_mounted || !path) return -1;
    struct lfs_info info;
    if (lfs_stat(&s_lfs, path, &info) != LFS_ERR_OK || info.type != LFS_TYPE_REG) return -1;
    return (int32_t)info.size;
}

int32_t littlefs_driver_list_dir(const char* path, lfs_dir_visit_fn visit, void* ctx) {
    if (!s_mounted || !path || !visit) return -1;

    lfs_dir_t dir;
    if (lfs_dir_open(&s_lfs, &dir, path) != LFS_ERR_OK) return -1;

    int32_t visited = 0;
    struct lfs_info info;
    while (lfs_dir_read(&s_lfs, &dir, &info) > 0) {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) continue;
        visited++;
        if (!visit(info.name, (uint32_t)info.size, info.type == LFS_TYPE_DIR, ctx)) break;
    }
    lfs_dir_close(&s_lfs, &dir);
    return visited;
}
