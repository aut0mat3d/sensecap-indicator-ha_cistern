/**
 * @file data_storage.cpp
 * @brief Non-Blocking Data Storage Engine using true LittleFS on SD Partition 2.
 *        Handles raw data (roundtrip), hourly, and daily aggregation
 *        with RAM accumulation, wall-clock alignment, and offline recovery.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "data_storage.hpp"
#include "littlefs_driver.hpp"
#include "littlefs_buffer.hpp"
#include "debug_log.hpp"
#include <ArduinoJson.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include <math.h>
#include <sys/time.h>

static bool g_time_valid = false;
static uint8_t g_time_flags = 0;

// ============================================================================
// Static State
// ============================================================================

static SdFat32* g_sd = nullptr;         // References Partition 1 (FAT32) for device JSON configs
static data_channel_t g_channels[64];   // Max 64 registered channels
static size_t g_channel_count = 0;

// ============================================================================
// Helper Functions & CSV Parsers
// ============================================================================

static bool strendswith(const char* str, const char* suffix) {
    if (!str || !suffix) return false;
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > str_len) return false;
    return strncmp(str + str_len - suffix_len, suffix, suffix_len) == 0;
}

static bool parse_channel_key(const char* channel_key, char* dev_addr, size_t dev_addr_size, 
                             char* decoder, size_t decoder_size, char* key, size_t key_size) {
    if (!channel_key) return false;
    
    const char* first_dot = strchr(channel_key, '.');
    const char* second_dot = first_dot ? strchr(first_dot + 1, '.') : nullptr;
    
    if (!first_dot || !second_dot) return false;
    
    size_t dev_addr_len = first_dot - channel_key;
    size_t decoder_len = second_dot - (first_dot + 1);
    size_t key_len = strlen(second_dot + 1);
    
    if (dev_addr_len >= dev_addr_size || decoder_len >= decoder_size || key_len >= key_size) {
        return false;
    }
    
    strncpy(dev_addr, channel_key, dev_addr_len);
    dev_addr[dev_addr_len] = '\0';
    
    strncpy(decoder, first_dot + 1, decoder_len);
    decoder[decoder_len] = '\0';
    
    strncpy(key, second_dot + 1, key_len);
    key[key_len] = '\0';
    
    return true;
}

static int32_t find_channel(const char* channel_key) {
    if (!channel_key) return -1;
    for (size_t i = 0; i < g_channel_count; i++) {
        if (strcasecmp(g_channels[i].config.channel_key, channel_key) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

/**
 * @brief Robust CSV parser for roundtrip.csv lines: <timestamp>,<value>,<unit>,<is_estimated>
 */
static bool parse_roundtrip_line(const char* line, uint32_t* ts, float* val, char* unit, size_t unit_size, uint8_t* is_est) {
    if (!line || line[0] == '#' || line[0] == '\0') return false;
    char* p = (char*)line;
    char* endp = nullptr;

    *ts = strtoul(p, &endp, 10);
    if (endp == p || *endp != ',') return false;
    p = endp + 1;

    *val = strtof(p, &endp);
    if (endp == p || *endp != ',') return false;
    p = endp + 1;

    char* comma = strchr(p, ',');
    if (!comma) return false;
    size_t ulen = comma - p;
    if (ulen >= unit_size) ulen = unit_size - 1;
    strncpy(unit, p, ulen);
    unit[ulen] = '\0';
    p = comma + 1;

    *is_est = (uint8_t)strtoul(p, nullptr, 10);
    return true;
}

/**
 * @brief Robust CSV parser for hourly/daily aggregated lines:
 *        Supports 6-column legacy: <start_ts>,<avg>,<min>,<max>,<count>,<has_estimated>
 *        and 7-column with TZ:     <start_ts>,<tz_offset>,<avg>,<min>,<max>,<count>,<has_estimated>
 */
static bool parse_aggregated_line(const char* line, uint32_t* ts, int32_t* tz_offset,
                                 float* avg, float* min_val, float* max_val, uint32_t* count, uint8_t* has_est) {
    if (!line || line[0] == '#' || line[0] == '\0') return false;

    /* Count commas to detect format (5 commas = 6 columns, 6 commas = 7 columns) */
    int commas = 0;
    for (const char* p = line; *p != '\0'; p++) {
        if (*p == ',') commas++;
    }

    char* p = (char*)line;
    char* endp = nullptr;

    *ts = strtoul(p, &endp, 10);
    if (endp == p || *endp != ',') return false;
    p = endp + 1;

    /* Extract optional tz_offset if present */
    if (commas >= 6) {
        long off = strtol(p, &endp, 10);
        if (endp == p || *endp != ',') return false;
        if (tz_offset) *tz_offset = (int32_t)off;
        p = endp + 1;
    } else {
        if (tz_offset) *tz_offset = 0;
    }

    *avg = strtof(p, &endp);
    if (endp == p || *endp != ',') return false;
    p = endp + 1;

    *min_val = strtof(p, &endp);
    if (endp == p || *endp != ',') return false;
    p = endp + 1;

    *max_val = strtof(p, &endp);
    if (endp == p || *endp != ',') return false;
    p = endp + 1;

    *count = strtoul(p, &endp, 10);
    if (endp == p || *endp != ',') return false;
    p = endp + 1;

    *has_est = (uint8_t)strtoul(p, nullptr, 10);
    return true;
}

/**
 * @brief 7-argument overload for hourly archives (where tz_offset is unused).
 */
static inline bool parse_aggregated_line(const char* line, uint32_t* ts,
                                        float* avg, float* min_val, float* max_val, 
                                        uint32_t* count, uint8_t* has_est) {
    return parse_aggregated_line(line, ts, nullptr, avg, min_val, max_val, count, has_est);
}

/**
 * @brief Math-based sanitizer ensuring intervals are clean divisors of 1 hour or 1 day
 */
static uint32_t sanitize_roundtrip_interval(uint32_t raw_interval, const char* channel_name) {
    if (raw_interval > 86400) {
        DBG_LFS_PRINTF("%s: interval %lu s > 24h. Clamped to 86400 s", channel_name, (unsigned long)raw_interval);
        return 86400;
    }
    if (raw_interval < 60) {
        DBG_LFS_PRINTF("%s: interval %lu s < 1 min. Clamped to 60 s", channel_name, (unsigned long)raw_interval);
        return 60;
    }

    if (raw_interval >= 3600) {
        uint32_t hours = (raw_interval + 1800) / 3600;
        if (hours < 1) hours = 1;
        if (hours > 24) hours = 24;
        
        uint32_t cycles_per_day = (24 + (hours / 2)) / hours;
        if (cycles_per_day < 1) cycles_per_day = 1;
        uint32_t clean_interval = (24 / cycles_per_day) * 3600;
        
        if (clean_interval != raw_interval) {
            DBG_LFS_PRINTF("%s: uneven interval %lu s adjusted to %lu s (%lu h cycle)", 
                           channel_name, (unsigned long)raw_interval, (unsigned long)clean_interval, clean_interval / 3600);
        }
        return clean_interval;
    } else {
        uint32_t mins = (raw_interval + 30) / 60;
        if (mins < 1) mins = 1;
        if (mins > 60) mins = 60;

        uint32_t cycles_per_hour = (60 + (mins / 2)) / mins;
        if (cycles_per_hour < 1) cycles_per_hour = 1;
        uint32_t clean_interval = (60 / cycles_per_hour) * 60;

        if (clean_interval != raw_interval) {
            DBG_LFS_PRINTF("%s: uneven interval %lu s adjusted to %lu s (%lu min cycle)", 
                           channel_name, (unsigned long)raw_interval, (unsigned long)clean_interval, clean_interval / 60);
        }
        return clean_interval;
    }
}

// ============================================================================
// Path Building Functions (Schlank: /data/<dev_addr>/<key>/<archiv>.csv)
// ============================================================================

void data_storage_build_roundtrip_path(const char* channel_key, char* path, size_t path_size) {
    char dev_addr[17], decoder[32], key[32];
    if (parse_channel_key(channel_key, dev_addr, sizeof(dev_addr), decoder, sizeof(decoder), key, sizeof(key))) {
        snprintf(path, path_size, "/data/%s/%s/roundtrip.csv", dev_addr, key);
    } else {
        path[0] = '\0';
    }
}

void data_storage_build_hourly_path(const char* channel_key, char* path, size_t path_size) {
    char dev_addr[17], decoder[32], key[32];
    if (parse_channel_key(channel_key, dev_addr, sizeof(dev_addr), decoder, sizeof(decoder), key, sizeof(key))) {
        snprintf(path, path_size, "/data/%s/%s/hourly.csv", dev_addr, key);
    } else {
        path[0] = '\0';
    }
}

void data_storage_build_daily_path(const char* channel_key, char* path, size_t path_size) {
    char dev_addr[17], decoder[32], key[32];
    if (parse_channel_key(channel_key, dev_addr, sizeof(dev_addr), decoder, sizeof(decoder), key, sizeof(key))) {
        snprintf(path, path_size, "/data/%s/%s/daily.csv", dev_addr, key);
    } else {
        path[0] = '\0';
    }
}

// ============================================================================
// Time Synchronization State & Guards
// ============================================================================

void data_storage_set_time_valid(bool valid, uint8_t flags) {
    g_time_valid = valid;
    g_time_flags = flags;
}

bool data_storage_is_time_valid(void) {
    return g_time_valid && ((uint32_t)time(NULL) >= 1700000000UL);
}

uint8_t data_storage_get_time_flags(void) {
    return g_time_flags;
}

uint32_t data_storage_recover_last_archive_timestamp(void) {
    if (!littlefs_driver_is_mounted() || g_channel_count == 0) return 0;
    
    uint32_t highest_ts = 0;
    for (size_t i = 0; i < g_channel_count; i++) {
        char path[128];
        data_storage_build_roundtrip_path(g_channels[i].config.channel_key, path, sizeof(path));
        
        lfs_file_handle_t f = littlefs_driver_open_read(path);
        if (!f) continue;

        char line[128];
        uint32_t last_file_ts = 0;
        while (littlefs_driver_read_line(f, line, sizeof(line)) >= 0) {
            uint32_t ts;
            float val;
            char unit[8];
            uint8_t is_estimated;
            if (parse_roundtrip_line(line, &ts, &val, unit, sizeof(unit), &is_estimated)) {
                last_file_ts = ts;
            }
        }
        littlefs_driver_close(f);
        if (last_file_ts > highest_ts) {
            highest_ts = last_file_ts;
        }
    }
    return highest_ts;
}

// ============================================================================
// Initialization & Loading from FAT32 Partition 1
// ============================================================================

void data_storage_init(SdFat32* sd) {
    g_sd = sd;
    g_channel_count = 0;
    memset(g_channels, 0, sizeof(g_channels));
    g_time_valid = false;
    g_time_flags = 0;
    Serial.println("[STORAGE] Data Storage Engine initialized.");
}

bool data_storage_load_devices(SdFat32* sd) {
    if (!sd) {
        g_sd = sd;
        return false;
    }
    g_sd = sd;
    g_channel_count = 0;

    const char* dev_dirs[] = {"/devices/", "/lora_p2p_configs/devices/"};
    const char* active_dir = nullptr;
    FatFile dir;

    for (size_t d = 0; d < 2; d++) {
        if (dir.open(dev_dirs[d])) {
            active_dir = dev_dirs[d];
            break;
        }
    }

    if (!active_dir) {
        Serial.println("[STORAGE_WARN] Devices directory not found on FAT32 Partition 1!");
        return false;
    }

    Serial.printf("[STORAGE] Scanning %s for device definitions...\r\n", active_dir);

    FatFile entry;
    while (entry.openNext(&dir, O_READ)) {
        char name[128];
        if (!entry.getName(name, sizeof(name))) {
            entry.close();
            continue;
        }
        
        if (strendswith(name, ".json")) {
            char path[128];
            snprintf(path, sizeof(path), "%s%s", active_dir, name);

            File32 file = sd->open(path, O_READ);
            if (!file) {
                Serial.printf("[STORAGE_ERR] Cannot open device file: %s\r\n", path);
                entry.close();
                continue;
            }

            DynamicJsonDocument doc(2048);
            DeserializationError err = deserializeJson(doc, file);
            file.close();
            if (err) {
                Serial.printf("[STORAGE_ERR] JSON parse failed on %s: %s\r\n", path, err.c_str());
                entry.close();
                continue;
            }

            const char* raw_dev_addr = doc["dev_addr"] | "";
            const char* decoder = doc["decoder"] | "";

            char dev_addr[17] = {0};
            for (size_t c = 0; raw_dev_addr[c] && c < 16; c++) {
                dev_addr[c] = (char)toupper((unsigned char)raw_dev_addr[c]);
            }

            if (dev_addr[0] && decoder[0]) {
                data_storage_load_decoder(sd, dev_addr, decoder);
            } else {
                Serial.printf("[STORAGE_WARN] Missing dev_addr or decoder in %s\r\n", path);
            }
        }
        entry.close();
    }
    dir.close();
    Serial.printf("[STORAGE] Device scan complete. Registered storage channels: %u\r\n", 
                  (unsigned)g_channel_count);
    return (g_channel_count > 0);
}

void data_storage_load_decoder(SdFat32* sd, const char* dev_addr, const char* decoder) {
    if (!sd || !dev_addr || !decoder) return;

    char path[128];
    snprintf(path, sizeof(path), "/decoder/%s.json", decoder);
    File32 file = sd->open(path, O_READ);
    if (!file) {
        snprintf(path, sizeof(path), "/lora_p2p_configs/decoder/%s.json", decoder);
        file = sd->open(path, O_READ);
    }
    if (!file) {
        Serial.printf("[STORAGE_WARN] Decoder file not found: %s.json\r\n", decoder);
        return;
    }

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err) {
        Serial.printf("[STORAGE_ERR] Failed to parse decoder %s: %s\r\n", path, err.c_str());
        return;
    }
    if (!doc.containsKey("mappings")) {
        Serial.printf("[STORAGE_WARN] Decoder %s has no 'mappings' key!\r\n", path);
        return;
    }

    JsonArray mappings = doc["mappings"];
    for (JsonObject mapping : mappings) {
        const char* key = mapping["key"] | "";
        if (!key[0]) continue;

        bool store_roundtrip = mapping["store_roundtrip"] | false;
        if (!store_roundtrip) continue;

        data_channel_config_t config;
        snprintf(config.channel_key, sizeof(config.channel_key), "%s.%s.%s", dev_addr, decoder, key);
        strncpy(config.dev_addr, dev_addr, sizeof(config.dev_addr));
        strncpy(config.decoder, decoder, sizeof(config.decoder));
        strncpy(config.key, key, sizeof(config.key));
        
        config.store_longterm = mapping["store_longterm"] | false;
        config.store_roundtrip = true;
        
        uint32_t raw_interval = mapping["roundtrip_interval_sec"] | 300;
        config.roundtrip_interval = sanitize_roundtrip_interval(raw_interval, config.channel_key);
        config.tolerance_sec = mapping["tolerance_sec"] | 15;

        const char* unit = mapping["unit"] | "";
        strncpy(config.unit, unit, sizeof(config.unit));

        data_storage_register_channel(&config);
    }
}

bool data_storage_register_channel(const data_channel_config_t* config) {
    if (!config || g_channel_count >= 64) return false;
    
    for (size_t i = 0; i < g_channel_count; i++) {
        if (strcmp(g_channels[i].config.channel_key, config->channel_key) == 0) {
            return false;
        }
    }

    data_channel_t* channel = &g_channels[g_channel_count++];
    channel->config = *config;
    
    channel->state.current_bucket = 0;
    channel->state.accumulator = 0.0;
    channel->state.sample_count = 0;
    channel->state.last_stored_time = 0;
    channel->state.last_value = 0.0f;
    channel->state.has_last_value = false;

    char roundtrip_path[128];
    data_storage_build_roundtrip_path(config->channel_key, roundtrip_path, sizeof(roundtrip_path));
    if (littlefs_driver_is_mounted()) {
        littlefs_driver_ensure_dir(roundtrip_path);
    }

    Serial.printf("[STORAGE] Registered channel: %s (interval=%lu s, unit=%s)\r\n",
                  config->channel_key, (unsigned long)config->roundtrip_interval, config->unit);
    return true;
}

// ============================================================================
// Low-Level Data Point Write Functions (LittleFS + RAM Fallback)
// ============================================================================

static bool write_roundtrip_point(const char* channel_key, uint32_t timestamp, float value, const char* unit, bool is_estimated) {
    char line[128];
    snprintf(line, sizeof(line), "%lu,%.4f,%s,%u\n", (unsigned long)timestamp, value, unit, is_estimated ? 1 : 0);

    char path[128];
    data_storage_build_roundtrip_path(channel_key, path, sizeof(path));

    if (littlefs_driver_is_mounted()) {
        bool ok = littlefs_driver_append_line(path, line);
        if (!ok) {
            DBG_LFS_PRINTF("Write error on %s, queuing to RAM buffer...", path);
            littlefs_buffer_append(path, (const uint8_t*)line, strlen(line), LFS_BUFFER_TYPE_SENSOR_DATA);
        }
        return ok;
    } else {
        DBG_LFS_PRINTF("LittleFS unmounted: queuing %s in RAM buffer...", path);
        return littlefs_buffer_append(path, (const uint8_t*)line, strlen(line), LFS_BUFFER_TYPE_SENSOR_DATA);
    }
}

static bool write_aggregated_point(const char* channel_key, granularity_t gran, const data_aggregated_t* agg) {
    if (!agg) return false;

    char line[128];
    char path[128];

    if (gran == GRAN_HOURLY) {
        snprintf(line, sizeof(line), "%lu,%.4f,%.4f,%.4f,%lu,%u\n", 
                 (unsigned long)agg->start_timestamp,
                 agg->avg_value,
                 agg->min_value,
                 agg->max_value,
                 (unsigned long)agg->count,
                 agg->has_estimated ? 1 : 0);
        data_storage_build_hourly_path(channel_key, path, sizeof(path));
    } else {
        /* Store UTC start timestamp plus local timezone offset in seconds */
        snprintf(line, sizeof(line), "%lu,%ld,%.4f,%.4f,%.4f,%lu,%u\n", 
                 (unsigned long)agg->start_timestamp,
                 (long)agg->tz_offset_sec,
                 agg->avg_value,
                 agg->min_value,
                 agg->max_value,
                 (unsigned long)agg->count,
                 agg->has_estimated ? 1 : 0);
        data_storage_build_daily_path(channel_key, path, sizeof(path));
    }

    if (littlefs_driver_is_mounted()) {
        bool ok = littlefs_driver_append_line(path, line);
        if (!ok) {
            DBG_LFS_PRINTF("Failed write agg point to %s, buffering in RAM...", path);
            littlefs_buffer_append(path, (const uint8_t*)line, strlen(line), LFS_BUFFER_TYPE_SENSOR_DATA);
        }
        return ok;
    } else {
        return littlefs_buffer_append(path, (const uint8_t*)line, strlen(line), LFS_BUFFER_TYPE_SENSOR_DATA);
    }
}

// ============================================================================
// Public API: RAM Accumulation & Bucket Flushing
// ============================================================================

void data_storage_add_point(
    const char* dev_addr,
    const char* decoder,
    const char* key,
    uint32_t timestamp,
    float value,
    const char* unit
) {
    if (!dev_addr || !decoder || !key) return;

    if (!data_storage_is_time_valid()) {
        DBG_LFS_PRINTF("Blocked write: time not valid (%lu)", (unsigned long)timestamp);
        return;
    }

    char channel_key[64];
    snprintf(channel_key, sizeof(channel_key), "%s.%s.%s", dev_addr, decoder, key);

    int32_t idx = find_channel(channel_key);
    if (idx < 0) return;

    data_channel_t* channel = &g_channels[idx];
    if (!channel->config.store_roundtrip) return;

    /* Assign incoming value to RAM accumulator */
    uint32_t target_bucket = timestamp - (timestamp % channel->config.roundtrip_interval);
    
    if (channel->state.current_bucket != target_bucket) {
        /* Edge Case: Flush missed if timestamp jumped unexpectedly */
        if (channel->state.sample_count > 0) {
            float avg = (float)(channel->state.accumulator / channel->state.sample_count);
            write_roundtrip_point(channel->config.channel_key, channel->state.current_bucket, avg, channel->config.unit, false);
            channel->state.last_stored_time = channel->state.current_bucket;
            channel->state.last_value = avg;
            channel->state.has_last_value = true;
        }
        channel->state.current_bucket = target_bucket;
        channel->state.accumulator = 0.0;
        channel->state.sample_count = 0;
    }

    channel->state.accumulator += value;
    channel->state.sample_count++;
    DBG_LFS_PRINTF("Acc: %s added %.2f (cnt=%u) to bucket %lu", 
                   channel_key, value, channel->state.sample_count, (unsigned long)target_bucket);
}

void data_storage_process_buckets(void) {
    if (!data_storage_is_time_valid()) return;

    uint32_t now = (uint32_t)time(NULL);

    for (size_t i = 0; i < g_channel_count; i++) {
        data_channel_t* channel = &g_channels[i];
        if (!channel->config.store_roundtrip) continue;

        uint32_t interval = channel->config.roundtrip_interval;
        uint32_t tolerance = channel->config.tolerance_sec > 0 ? channel->config.tolerance_sec : 15;
        uint32_t active_bucket = now - (now % interval);

        /* Initial bucket setup */
        if (channel->state.current_bucket == 0) {
            channel->state.current_bucket = active_bucket;
            continue;
        }

        /* Flush bucket only after the interval has elapsed plus per-channel tolerance */
        if (now >= (channel->state.current_bucket + interval + tolerance)) {
            if (channel->state.sample_count > 0) {
                /* Regular measured value (real) */
                float avg = (float)(channel->state.accumulator / channel->state.sample_count);
                bool ok = write_roundtrip_point(channel->config.channel_key, channel->state.current_bucket, 
                                                avg, channel->config.unit, false);
                DBG_LFS_PRINTF("Flush %s: %s -> %.2f %s (cnt=%u)", ok ? "SUCCESS" : "QUEUED", 
                               channel->config.channel_key, avg, channel->config.unit, channel->state.sample_count);
                
                channel->state.last_stored_time = channel->state.current_bucket;
                channel->state.last_value = avg;
                channel->state.has_last_value = true;
            } else if (channel->state.has_last_value) {
                /* Missing packet timeout: write held value as estimated */
                write_roundtrip_point(channel->config.channel_key, channel->state.current_bucket, 
                                      channel->state.last_value, channel->config.unit, true);
                DBG_LFS_PRINTF("Estimated missing packet for %s at ts=%lu",
                               channel->config.channel_key, (unsigned long)channel->state.current_bucket);
                channel->state.last_stored_time = channel->state.current_bucket;
            }

            /* Advance to next cleanly aligned bucket */
            channel->state.current_bucket = active_bucket;
            channel->state.accumulator = 0.0;
            channel->state.sample_count = 0;
        }
    }
}

// ============================================================================
// File Trimming & Aggregation Functions (LittleFS v2)
// ============================================================================

bool data_storage_trim_file(const char* path, uint32_t max_entries) {
    if (!littlefs_driver_is_mounted() || !path || max_entries == 0) return false;

    lfs_file_handle_t f = littlefs_driver_open_read(path);
    if (!f) return false;

    #define TRIM_MAX_LINES 96
    uint32_t limit = (max_entries <= TRIM_MAX_LINES) ? max_entries : TRIM_MAX_LINES;
    static char s_stored_lines[TRIM_MAX_LINES][128];
    size_t total_lines = 0;

    char line[128];
    while (littlefs_driver_read_line(f, line, sizeof(line)) >= 0) {
        if (line[0] == '\0') continue;
        strncpy(s_stored_lines[total_lines % limit], line, sizeof(s_stored_lines[0]) - 1);
        s_stored_lines[total_lines % limit][sizeof(s_stored_lines[0]) - 1] = '\0';
        total_lines++;
    }
    littlefs_driver_close(f);

    if (total_lines <= limit) return true;

    size_t start = total_lines % limit;
    const char* write_ptrs[TRIM_MAX_LINES];
    for (size_t i = 0; i < limit; i++) {
        write_ptrs[i] = s_stored_lines[(start + i) % limit];
    }

    return littlefs_driver_rewrite_file(path, write_ptrs, limit);
}

static void aggregate_to_hourly(const char* channel_key) {
    if (!littlefs_driver_is_mounted() || !channel_key) return;

    char roundtrip_path[128];
    char hourly_path[128];
    data_storage_build_roundtrip_path(channel_key, roundtrip_path, sizeof(roundtrip_path));
    data_storage_build_hourly_path(channel_key, hourly_path, sizeof(hourly_path));

    uint32_t now = (uint32_t)time(NULL);
    uint32_t current_hour = now - (now % 3600);
    uint32_t previous_hour = current_hour - 3600;

    lfs_file_handle_t f = littlefs_driver_open_read(roundtrip_path);
    if (!f) return;

    data_aggregated_t hourly = {0};
    hourly.start_timestamp = previous_hour;
    bool has_data = false;

    char line[128];
    while (littlefs_driver_read_line(f, line, sizeof(line)) >= 0) {
        uint32_t ts;
        float val;
        char unit[8];
        uint8_t is_estimated;
        if (parse_roundtrip_line(line, &ts, &val, unit, sizeof(unit), &is_estimated)) {
            if (ts >= previous_hour && ts < current_hour) {
                hourly.avg_value += val;
                if (!has_data || val < hourly.min_value) hourly.min_value = val;
                if (!has_data || val > hourly.max_value) hourly.max_value = val;
                hourly.count++;
                if (is_estimated) hourly.has_estimated = true;
                has_data = true;
            }
        }
    }
    littlefs_driver_close(f);

    if (has_data && hourly.count > 0) {
        hourly.avg_value /= hourly.count;
        write_aggregated_point(channel_key, GRAN_HOURLY, &hourly);
        data_storage_trim_file(hourly_path, 48);
        DBG_LFS_PRINTF("Hourly aggregated for %s: avg=%.2f, count=%lu",
                       channel_key, hourly.avg_value, (unsigned long)hourly.count);
    }
}

static void aggregate_to_daily(const char* channel_key) {
    if (!littlefs_driver_is_mounted() || !channel_key) return;

    char hourly_path[128];
    char daily_path[128];
    data_storage_build_hourly_path(channel_key, hourly_path, sizeof(hourly_path));
    data_storage_build_daily_path(channel_key, daily_path, sizeof(daily_path));

    /* Calculate local midnight boundaries using active POSIX timezone */
    time_t now = time(NULL);
    struct tm ltm;
    localtime_r(&now, &ltm);
    ltm.tm_hour = 0;
    ltm.tm_min = 0;
    ltm.tm_sec = 0;
    ltm.tm_isdst = -1; /* Let mktime determine active DST state */
    time_t today_midnight = mktime(&ltm);

    ltm.tm_mday -= 1; /* Yesterday's local calendar day */
    ltm.tm_isdst = -1;
    time_t yesterday_midnight = mktime(&ltm);

    /* Determine local timezone offset relative to UTC at yesterday's midnight */
    struct tm g_tm, l_tm;
    gmtime_r(&yesterday_midnight, &g_tm);
    localtime_r(&yesterday_midnight, &l_tm);
    long day_diff = l_tm.tm_mday - g_tm.tm_mday;
    if (day_diff > 1) day_diff = -1;
    else if (day_diff < -1) day_diff = 1;
    int32_t tz_offset_sec = (int32_t)(day_diff * 86400 + (l_tm.tm_hour - g_tm.tm_hour) * 3600 + (l_tm.tm_min - g_tm.tm_min) * 60);

    lfs_file_handle_t f = littlefs_driver_open_read(hourly_path);
    if (!f) return;

    data_aggregated_t daily = {0};
    daily.start_timestamp = (uint32_t)yesterday_midnight;
    daily.tz_offset_sec = tz_offset_sec;
    bool has_data = false;

    char line[128];
    while (littlefs_driver_read_line(f, line, sizeof(line)) >= 0) {
        uint32_t start_ts;
        int32_t dummy_tz = 0;
        float avg, min_val, max_val;
        uint32_t count;
        uint8_t has_estimated;
        if (parse_aggregated_line(line, &start_ts, &dummy_tz, &avg, &min_val, &max_val, &count, &has_estimated)) {
            /* Hourly points in interval [yesterday 00:00 local .. today 00:00 local[ */
            if (start_ts >= (uint32_t)yesterday_midnight && start_ts < (uint32_t)today_midnight) {
                daily.avg_value += avg * count;
                if (!has_data || min_val < daily.min_value) daily.min_value = min_val;
                if (!has_data || max_val > daily.max_value) daily.max_value = max_val;
                daily.count += count;
                if (has_estimated) daily.has_estimated = true;
                has_data = true;
            }
        }
    }
    littlefs_driver_close(f);

    if (has_data && daily.count > 0) {
        daily.avg_value /= daily.count;
        write_aggregated_point(channel_key, GRAN_DAILY, &daily);
        data_storage_trim_file(daily_path, 32);
        DBG_LFS_PRINTF("Daily aggregated for %s: start=%lu, tz_off=%ld s, avg=%.2f, count=%lu",
                       channel_key, (unsigned long)daily.start_timestamp, (long)daily.tz_offset_sec,
                       daily.avg_value, (unsigned long)daily.count);
    }
}

void data_storage_aggregate_hourly(void) {
    if (!data_storage_is_time_valid()) return;

    for (size_t i = 0; i < g_channel_count; i++) {
        if (g_channels[i].config.store_longterm) {
            aggregate_to_hourly(g_channels[i].config.channel_key);
        }
    }
}

void data_storage_aggregate_daily(void) {
    if (!data_storage_is_time_valid()) return;

    for (size_t i = 0; i < g_channel_count; i++) {
        if (g_channels[i].config.store_longterm) {
            aggregate_to_daily(g_channels[i].config.channel_key);
        }
    }
}

// ============================================================================
// Query Functions (Reading from LittleFS Partition 2)
// ============================================================================

static data_point_t* load_from_roundtrip(
    const char* channel_key,
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    uint32_t interval_sec,
    uint32_t max_points,
    size_t* count
) {
    *count = 0;
    if (!littlefs_driver_is_mounted() || !channel_key || max_points == 0) return nullptr;

    char path[128];
    data_storage_build_roundtrip_path(channel_key, path, sizeof(path));
    
    lfs_file_handle_t f = littlefs_driver_open_read(path);
    if (!f) {
        DBG_LFS_PRINTF("File not found on LittleFS: %s", path);
        return nullptr;
    }

    data_point_t* result = (data_point_t*)malloc(max_points * sizeof(data_point_t));
    if (!result) {
        littlefs_driver_close(f);
        return nullptr;
    }

    size_t result_count = 0;
    char line[128];
    while (littlefs_driver_read_line(f, line, sizeof(line)) >= 0) {
        uint32_t ts;
        float val;
        char unit[8];
        uint8_t is_estimated;

        if (parse_roundtrip_line(line, &ts, &val, unit, sizeof(unit), &is_estimated)) {
            if (ts >= start_timestamp && ts <= (end_timestamp + 31) && result_count < max_points) {
                result[result_count].timestamp = ts;
                result[result_count].value = val;
                strncpy(result[result_count].unit, unit, sizeof(result[result_count].unit));
                result[result_count].is_estimated = (is_estimated != 0);
                result_count++;
            }
        }
    }
    littlefs_driver_close(f);

    DBG_LFS_PRINTF("%s: Found %u points in range [%lu..%lu]",
                   path, (unsigned)result_count, (unsigned long)start_timestamp, (unsigned long)end_timestamp);

    if (result_count == 0) {
        free(result);
        return nullptr;
    }

    *count = result_count;
    return result;
}

static data_point_t* load_and_aggregate_roundtrip(
    const char* channel_key,
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    uint32_t interval_sec,
    uint32_t max_points,
    size_t* count
) {
    *count = 0;
    if (!channel_key || max_points == 0) return nullptr;

    size_t raw_count = 0;
    data_point_t* raw_points = load_from_roundtrip(channel_key, start_timestamp, end_timestamp, interval_sec, 1000, &raw_count);

    data_point_t* result = (data_point_t*)malloc(max_points * sizeof(data_point_t));
    if (!result) {
        if (raw_points) free(raw_points);
        return nullptr;
    }

    const data_channel_config_t* cfg = data_storage_get_config(channel_key);
    const char* default_unit = cfg ? cfg->unit : "";

    size_t result_count = 0;
    uint32_t current_bucket = start_timestamp - (start_timestamp % interval_sec);
    size_t j = 0;

    /* Loop strictly over all requested bucket slots to guarantee exact max_points */
    while (result_count < max_points && current_bucket < end_timestamp) {
        while (j < raw_count && raw_points[j].timestamp < current_bucket) {
            j++;
        }

        size_t k = j;
        float sum = 0.0f;
        uint32_t bucket_count = 0;
        bool has_estimated = false;
        const char* unit_ptr = default_unit;

        while (k < raw_count && raw_points[k].timestamp < (current_bucket + interval_sec)) {
            sum += raw_points[k].value;
            bucket_count++;
            if (raw_points[k].is_estimated) has_estimated = true;
            if (raw_points[k].unit[0] != '\0') unit_ptr = raw_points[k].unit;
            k++;
        }

        result[result_count].timestamp = current_bucket;
        if (bucket_count > 0) {
            result[result_count].value = sum / (float)bucket_count;
            strncpy(result[result_count].unit, unit_ptr, sizeof(result[result_count].unit) - 1);
            result[result_count].unit[sizeof(result[result_count].unit) - 1] = '\0';
            result[result_count].is_estimated = has_estimated;
            j = k;
        } else {
            /* Missing 5-minute interval: flag as empty gap */
            result[result_count].value = 0.0f;
            result[result_count].unit[0] = '\0';
            result[result_count].is_estimated = false;
        }

        result_count++;
        current_bucket += interval_sec;
    }

    if (raw_points) free(raw_points);
    *count = result_count;
    return result;
}

static data_point_t* load_from_hourly(
    const char* channel_key,
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    uint32_t max_points,
    size_t* count
) {
    *count = 0;
    if (!littlefs_driver_is_mounted() || !channel_key || max_points == 0) return nullptr;

    char path[128];
    data_storage_build_hourly_path(channel_key, path, sizeof(path));
    
    lfs_file_handle_t f = littlefs_driver_open_read(path);
    if (!f) {
        DBG_LFS_PRINTF("File not found on LittleFS: %s", path);
        return nullptr;
    }

    data_point_t* result = (data_point_t*)malloc(max_points * sizeof(data_point_t));
    if (!result) {
        littlefs_driver_close(f);
        return nullptr;
    }

    size_t result_count = 0;
    char line[128];
    while (littlefs_driver_read_line(f, line, sizeof(line)) >= 0) {
        uint32_t start_ts;
        float avg, min_val, max_val;
        uint32_t count_val;
        uint8_t has_estimated;

        if (parse_aggregated_line(line, &start_ts, &avg, &min_val, &max_val, &count_val, &has_estimated)) {
            if (start_ts >= start_timestamp && start_ts <= (end_timestamp + 31) && result_count < max_points) {
                result[result_count].timestamp = start_ts;
                result[result_count].value = avg;
                const data_channel_config_t* cfg = data_storage_get_config(channel_key);
                if (cfg) {
                    strncpy(result[result_count].unit, cfg->unit, sizeof(result[result_count].unit));
                } else {
                    result[result_count].unit[0] = '\0';
                }
                result[result_count].is_estimated = (has_estimated != 0);
                result_count++;
            }
        }
    }
    littlefs_driver_close(f);

    DBG_LFS_PRINTF("%s: Found %u points in range [%lu..%lu]",
                   path, (unsigned)result_count, (unsigned long)start_timestamp, (unsigned long)end_timestamp);

    if (result_count == 0) {
        free(result);
        return nullptr;
    }

    *count = result_count;
    return result;
}

static data_point_t* load_and_aggregate_hourly(
    const char* channel_key,
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    uint32_t interval_sec,
    uint32_t max_points,
    size_t* count
) {
    *count = 0;
    if (!channel_key || max_points == 0) return nullptr;

    size_t raw_count = 0;
    data_point_t* raw_points = load_from_hourly(channel_key, start_timestamp, end_timestamp, 200, &raw_count);

    data_point_t* result = (data_point_t*)malloc(max_points * sizeof(data_point_t));
    if (!result) {
        if (raw_points) free(raw_points);
        return nullptr;
    }

    const data_channel_config_t* cfg = data_storage_get_config(channel_key);
    const char* default_unit = cfg ? cfg->unit : "";

    size_t result_count = 0;
    uint32_t current_bucket = start_timestamp - (start_timestamp % interval_sec);
    size_t j = 0;

    /* For 2-hour intervals, we expect 2 hourly points per bucket */
    uint32_t expected_hourly_points = interval_sec / 3600;
    if (expected_hourly_points == 0) expected_hourly_points = 1;

    /* Loop strictly over all requested bucket slots to guarantee exact max_points */
    while (result_count < max_points && current_bucket < end_timestamp) {
        while (j < raw_count && raw_points[j].timestamp < current_bucket) {
            j++;
        }

        size_t k = j;
        float sum = 0.0f;
        uint32_t b_count = 0;
        bool has_estimated = false;
        const char* unit_ptr = default_unit;

        while (k < raw_count && raw_points[k].timestamp < (current_bucket + interval_sec)) {
            sum += raw_points[k].value;
            b_count++;
            if (raw_points[k].is_estimated) has_estimated = true;
            if (raw_points[k].unit[0] != '\0') unit_ptr = raw_points[k].unit;
            k++;
        }

        result[result_count].timestamp = current_bucket;
        if (b_count > 0) {
            result[result_count].value = sum / (float)b_count;
            strncpy(result[result_count].unit, unit_ptr, sizeof(result[result_count].unit) - 1);
            result[result_count].unit[sizeof(result[result_count].unit) - 1] = '\0';

            /* If hours are missing within this 2h bucket, mark as estimated (50% transparent)! */
            if (b_count < expected_hourly_points) {
                has_estimated = true;
            }
            result[result_count].is_estimated = has_estimated;
            j = k;
        } else {
            /* Blackout / gap slot: full 2 hours missing, flag as empty gap */
            result[result_count].value = 0.0f;
            result[result_count].unit[0] = '\0';
            result[result_count].is_estimated = false;
        }

        result_count++;
        current_bucket += interval_sec;
    }

    if (raw_points) free(raw_points);
    *count = result_count;
    return result;
}

static data_point_t* load_from_daily(
    const char* channel_key,
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    uint32_t max_points,
    size_t* count
) {
    *count = 0;
    if (!channel_key || max_points == 0) return nullptr;

    data_point_t* result = (data_point_t*)malloc(max_points * sizeof(data_point_t));
    if (!result) return nullptr;

    const data_channel_config_t* cfg = data_storage_get_config(channel_key);
    const char* default_unit = cfg ? cfg->unit : "";

    /* 1. Read existing daily archive entries into temporary buffer */
    #define MAX_RAW_DAILY 32
    data_point_t raw_daily[MAX_RAW_DAILY];
    size_t raw_count = 0;

    char path[128];
    data_storage_build_daily_path(channel_key, path, sizeof(path));
    lfs_file_handle_t f = littlefs_driver_open_read(path);
    if (f) {
        char line[128];
        while (littlefs_driver_read_line(f, line, sizeof(line)) >= 0 && raw_count < MAX_RAW_DAILY) {
            uint32_t start_ts;
            int32_t tz_off = 0;
            float avg, min_val, max_val;
            uint32_t count_val;
            uint8_t has_estimated;

            if (parse_aggregated_line(line, &start_ts, &tz_off, &avg, &min_val, &max_val, &count_val, &has_estimated)) {
                raw_daily[raw_count].timestamp = start_ts;
                raw_daily[raw_count].value = avg;
                raw_daily[raw_count].is_estimated = (has_estimated != 0);
                strncpy(raw_daily[raw_count].unit, default_unit, sizeof(raw_daily[raw_count].unit) - 1);
                raw_daily[raw_count].unit[sizeof(raw_daily[raw_count].unit) - 1] = '\0';
                raw_count++;
            }
        }
        littlefs_driver_close(f);
        DBG_LFS_PRINTF("%s: Found %u points in daily archive", path, (unsigned)raw_count);
    }

    /* 2. Grid-locked iteration over calendar days ending at local midnight */
    time_t ref_time = (time_t)end_timestamp;
    struct tm ltm_ref;
    localtime_r(&ref_time, &ltm_ref);
    ltm_ref.tm_hour = 0;
    ltm_ref.tm_min = 0;
    ltm_ref.tm_sec = 0;
    ltm_ref.tm_isdst = -1;
    time_t today_midnight = mktime(&ltm_ref);

    for (size_t i = 0; i < max_points; i++) {
        /* Slot i: Day -(max_points - i). Last slot (i = max_points - 1) is yesterday (-1 day) */
        struct tm ltm_slot;
        localtime_r(&today_midnight, &ltm_slot);
        ltm_slot.tm_mday -= (max_points - i);
        ltm_slot.tm_isdst = -1;
        time_t slot_midnight = mktime(&ltm_slot);

        result[i].timestamp = (uint32_t)slot_midnight;
        result[i].value = 0.0f;
        result[i].unit[0] = '\0';
        result[i].is_estimated = false;

        /* Match archive record within 2 hours tolerance (covers DST shifts) */
        for (size_t r = 0; r < raw_count; r++) {
            long diff = (long)raw_daily[r].timestamp - (long)slot_midnight;
            if (diff >= -7200 && diff <= 7200) {
                result[i].value = raw_daily[r].value;
                strncpy(result[i].unit, raw_daily[r].unit, sizeof(result[i].unit) - 1);
                result[i].unit[sizeof(result[i].unit) - 1] = '\0';
                result[i].is_estimated = raw_daily[r].is_estimated;
                break;
            }
        }
    }

    *count = max_points;
    return result;
}

// ============================================================================
// Public API: Multi-Archive Dispatcher & High-Level Chart Query
// ============================================================================

data_point_t* data_storage_query(
    const char* channel_key,
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    uint32_t interval_sec,
    uint32_t max_points,
    uint32_t* actual_interval_sec,
    size_t* count
) {
    if (!channel_key || !littlefs_driver_is_mounted()) {
        *count = 0;
        return nullptr;
    }

    const data_channel_config_t* cfg = data_storage_get_config(channel_key);
    bool has_longterm = cfg ? cfg->store_longterm : false;

    /* Sub-hourly query: aggregate roundtrip on bucket grid */
    if (interval_sec < 3600 || !has_longterm) {
        *actual_interval_sec = interval_sec;
        return load_and_aggregate_roundtrip(channel_key, start_timestamp, end_timestamp, *actual_interval_sec, max_points, count);
    }
    /* Exactly 1 hour requested: load directly from hourly archive */
    else if (interval_sec == 3600) {
        *actual_interval_sec = 3600;
        return load_from_hourly(channel_key, start_timestamp, end_timestamp, max_points, count);
    }
    /* Multi-hour aggregation (e.g. 2h, 4h, 6h): aggregate from hourly archive */
    else if (interval_sec < 86400) {
        *actual_interval_sec = interval_sec;
        return load_and_aggregate_hourly(channel_key, start_timestamp, end_timestamp, interval_sec, max_points, count);
    }
    /* Daily or multi-day intervals: load from daily archive */
    else {
        *actual_interval_sec = 86400;
        return load_from_daily(channel_key, start_timestamp, end_timestamp, max_points, count);
    }
}

const data_channel_config_t* data_storage_get_config(const char* channel_key) {
    int32_t idx = find_channel(channel_key);
    return (idx >= 0) ? &g_channels[idx].config : nullptr;
}

data_channel_state_t* data_storage_get_state(const char* channel_key) {
    int32_t idx = find_channel(channel_key);
    return (idx >= 0) ? &g_channels[idx].state : nullptr;
}

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
) {
    *count = 0;
    if (!channel_key || !littlefs_driver_is_mounted() || max_points == 0) return nullptr;

    /* Snap to completed time intervals */
    uint32_t end_bucket = end_timestamp - (end_timestamp % interval_sec);
    uint32_t start_bucket = end_bucket - (max_points * interval_sec);

    size_t raw_count = 0;
    /* respect Bucket-boundaries */
    data_point_t* raw_pts = data_storage_query(
        channel_key, start_bucket, end_bucket, interval_sec, max_points, actual_interval_sec, &raw_count);

    if (!raw_pts || raw_count == 0) {
        if (raw_pts) free(raw_pts);
        return nullptr;
    }

    chart_data_point_t* chart_pts = (chart_data_point_t*)malloc(raw_count * sizeof(chart_data_point_t));
    if (!chart_pts) {
        free(raw_pts);
        return nullptr;
    }

    const data_channel_config_t* cfg = data_storage_get_config(channel_key);
    uint32_t base_interval = cfg ? cfg->roundtrip_interval : 300;
    bool is_averaged = (*actual_interval_sec >= 3600) || (*actual_interval_sec > base_interval);

    for (size_t i = 0; i < raw_count; i++) {
        chart_pts[i].timestamp = raw_pts[i].timestamp;
        chart_pts[i].value = raw_pts[i].value;
        chart_pts[i].flags = 0;

        /* Empty slot (gap from power outage or missing history) */
        if (raw_pts[i].value == 0.0f && !raw_pts[i].is_estimated && raw_pts[i].unit[0] == '\0') {
            chart_pts[i].flags = DATA_FLAG_NO_DATA;
        } else {
            if (raw_pts[i].is_estimated) {
                chart_pts[i].flags |= DATA_FLAG_ESTIMATED; /* 50% opacity in LVGL */
            }
            if (is_averaged) {
                chart_pts[i].flags |= DATA_FLAG_AVERAGED;
            }
            if (alarm_min > 0.0f && raw_pts[i].value < alarm_min) {
                chart_pts[i].flags |= DATA_FLAG_ALARM_DANGER;
            }
            if (alarm_max > 0.0f && raw_pts[i].value > alarm_max) {
                chart_pts[i].flags |= DATA_FLAG_ALARM_DANGER;
            }
        }
    } 

    free(raw_pts);
    *count = raw_count;
    return chart_pts;
}

static const char* TIME_FILE_PATH = "/data/last_time.txt";

void data_storage_save_time(uint32_t timestamp) {
    if (!littlefs_driver_is_mounted() || timestamp < 1700000000UL) return;

    char line[32];
    snprintf(line, sizeof(line), "%lu\n", (unsigned long)timestamp);

    const char* lines[1] = { line };
    littlefs_driver_rewrite_file(TIME_FILE_PATH, lines, 1);
}

uint32_t data_storage_get_saved_time(void) {
    if (!littlefs_driver_is_mounted()) return 0;

    lfs_file_handle_t f = littlefs_driver_open_read(TIME_FILE_PATH);
    if (!f) return 0;

    char line[32];
    uint32_t ts = 0;
    if (littlefs_driver_read_line(f, line, sizeof(line)) > 0) {
        ts = strtoul(line, nullptr, 10);
    }
    littlefs_driver_close(f);
    return ts;
}