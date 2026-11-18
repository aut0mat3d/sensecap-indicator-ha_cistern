/**
 * @file data_storage.cpp
 * @brief Non-Blocking Data Storage Engine using true LittleFS on SD Partition 2.
 *        Handles grid data (raw), hourly, and daily aggregation
 *        with RAM accumulation, wall-clock alignment, and offline recovery.
 *        Records are stored in segmented binary files (see archive_store.hpp).
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "data_storage.hpp"
#include "littlefs_driver.hpp"
#include "archive_store.hpp"
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

/** FNV-1a hash of a channel key (identifies queued records across config reloads) */
static uint32_t channel_key_hash(const char* key) {
    uint32_t h = 2166136261UL;
    for (const char* p = key; p && *p; p++) {
        h ^= (uint8_t)toupper((unsigned char)*p);
        h *= 16777619UL;
    }
    return h;
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

// ============================================================================
// Initialization & Loading from FAT32 Partition 1
// ============================================================================


void data_storage_init(SdFat32* sd) {
    g_sd = sd;
    g_channel_count = 0;
    memset(g_channels, 0, sizeof(g_channels));
    g_time_valid = false;
    g_time_flags = 0;
    LogSerial.println("[STORAGE] Data Storage Engine initialized.");
}

/** JSON names of the retention levels, indexed by retention_level_t */
static const char* const k_retention_names[RET_LEVEL_COUNT] = {
    "single", "hours", "days", "weeks", "months", "years"
};

/**
 * @brief Normalize a device address string ("05975dc9", "0x45") to 8 upper-case hex digits.
 *        Must match the "%08X" format used by the telemetry paths calling data_storage_add_point().
 */
static bool normalize_dev_addr(const char* raw, char* out, size_t out_size) {
    if (!raw || !raw[0] || !out || out_size < 9) return false;
    char* end = nullptr;
    unsigned long value = strtoul(raw, &end, 16); /* base 16 accepts an optional "0x" prefix */
    if (end == raw || *end != '\0') return false;
    snprintf(out, out_size, "%08lX", value);
    return true;
}

/**
 * @brief Open and parse /decoder/<name>.json (with legacy fallback directory).
 */
static bool load_decoder_doc(SdFat32* sd, const char* decoder, JsonDocument& doc) {
    char path[128];
    snprintf(path, sizeof(path), "/decoder/%s.json", decoder);
    File32 file = sd->open(path, O_READ);
    if (!file) {
        snprintf(path, sizeof(path), "/lora_p2p_configs/decoder/%s.json", decoder);
        file = sd->open(path, O_READ);
    }
    if (!file) {
        LogSerial.printf("[STORAGE_WARN] Decoder file not found: %s.json\r\n", decoder);
        return false;
    }

    /* Only the unit fields of the mappings are needed here: filtering keeps the heap footprint small */
    StaticJsonDocument<96> filter;
    JsonObject f_mapping = filter["mappings"].createNestedObject();
    f_mapping["key"] = true;
    f_mapping["unit"] = true;
    f_mapping["target_unit"] = true;

    DeserializationError err = deserializeJson(doc, file, DeserializationOption::Filter(filter));
    file.close();
    if (err) {
        LogSerial.printf("[STORAGE_ERR] Failed to parse decoder %s: %s (free heap: %d B)\r\n",
                      path, err.c_str(), rp2040.getFreeHeap());
        return false;
    }
    return true;
}

/**
 * @brief Resolve the storage unit of an archive key.
 *        1. Decoder mapping with matching "key" ("target_unit", else "unit")
 *        2. Device "virtual_decoder.<key>.unit"
 * @return false if the key is neither a decoder key nor a virtual key of this device.
 */
static bool resolve_archive_unit(const JsonDocument* decoder_doc, JsonObjectConst device,
                                 const char* key, char* unit_out, size_t unit_size) {
    unit_out[0] = '\0';

    if (decoder_doc) {
        JsonArrayConst mappings = (*decoder_doc)["mappings"];
        for (JsonObjectConst mapping : mappings) {
            const char* m_key = mapping["key"] | "";
            if (strcmp(m_key, key) != 0) continue;
            const char* unit = mapping["target_unit"] | "";
            if (!unit[0]) unit = mapping["unit"] | "";
            strncpy(unit_out, unit, unit_size - 1);
            unit_out[unit_size - 1] = '\0';
            return true;
        }
    }

    JsonObjectConst vd = device["virtual_decoder"][key];
    if (!vd.isNull()) {
        const char* unit = vd["unit"] | "";
        strncpy(unit_out, unit, unit_size - 1);
        unit_out[unit_size - 1] = '\0';
        return true;
    }
    return false;
}

/**
 * @brief Derive the effective retention (records to keep) of every archive level.
 *
 * The cascade reads each level from the next finer one (raw -> hour -> day).
 * A finer level that is disabled in the configuration but required as source is therefore
 * kept as a small staging level:
 *   - raw:  2 hours of buckets when hourly aggregation is active
 *   - hour: 26 entries when daily aggregation is active (covers 25 h DST days)
 * Direct accumulation without staging levels is part of the planned NTTW engine.
 */
static void compute_keep_entries(data_channel_config_t* cfg) {
    for (size_t lvl = 0; lvl < RET_LEVEL_COUNT; lvl++) {
        cfg->keep_entries[lvl] = 0;
    }

    cfg->keep_entries[RET_DAYS] = cfg->retention[RET_DAYS];

    uint16_t hours = cfg->retention[RET_HOURS];
    if (cfg->keep_entries[RET_DAYS] > 0 && hours < 26) hours = 26;
    cfg->keep_entries[RET_HOURS] = hours;

    uint32_t single = cfg->retention[RET_SINGLE];
    if (cfg->keep_entries[RET_HOURS] > 0) {
        uint32_t staging = (2 * 3600) / cfg->storage_interval;
        if (single < staging) single = staging;
    }
    if (single > 0) {
        /* The cold-start restore needs the newest REAL raw record for as long as its hold is
         * valid (expected interval + tolerance + P2P allowance). Sporadic keys (battery_soc once
         * a day) would otherwise lose it to the 5-minute hold records of the staging buffer, and
         * after a reboot nothing would be archived until the next packet. */
        uint32_t hold_span = cfg->roundtrip_interval + cfg->tolerance_sec + 6;
        uint32_t restore_keep = hold_span / cfg->storage_interval + 2;
        if (single < restore_keep) single = restore_keep;
    }
    cfg->keep_entries[RET_SINGLE] = (single > 0xFFFF) ? 0xFFFF : (uint16_t)single;

    if (cfg->retention[RET_WEEKS] || cfg->retention[RET_MONTHS] || cfg->retention[RET_YEARS]) {
        LogSerial.printf("[STORAGE] %s: weeks/months/years retention configured but not aggregated yet\r\n",
                      cfg->channel_key);
    }
}

/**
 * @brief Register all entries of the "archive" object of one parsed device JSON.
 */
static void register_device_archive(SdFat32* sd, JsonObjectConst device, const char* path) {
    JsonObjectConst archive = device["archive"];
    if (archive.isNull()) return; /* Archiving is opt-in per device */

    const char* raw_dev_addr = device["dev_addr"] | "";
    const char* decoder = device["decoder"] | "";
    uint32_t report_interval = device["report_interval_sec"] | 300;

    char dev_addr[17] = {0};
    if (!normalize_dev_addr(raw_dev_addr, dev_addr, sizeof(dev_addr)) || !decoder[0]) {
        LogSerial.printf("[STORAGE_WARN] Missing or invalid dev_addr/decoder in %s\r\n", path);
        return;
    }

    DynamicJsonDocument decoder_doc(2048);
    bool has_decoder = load_decoder_doc(sd, decoder, decoder_doc);

    for (JsonPairConst entry : archive) {
        const char* key = entry.key().c_str();
        JsonObjectConst arc = entry.value();
        if (!key[0] || arc.isNull()) continue;

        data_channel_config_t config;
        memset(&config, 0, sizeof(config));
        snprintf(config.channel_key, sizeof(config.channel_key), "%s.%s.%s", dev_addr, decoder, key);
        strncpy(config.dev_addr, dev_addr, sizeof(config.dev_addr) - 1);
        strncpy(config.decoder, decoder, sizeof(config.decoder) - 1);
        strncpy(config.key, key, sizeof(config.key) - 1);

        if (!resolve_archive_unit(has_decoder ? &decoder_doc : nullptr, device, key,
                                  config.unit, sizeof(config.unit))) {
            LogSerial.printf("[STORAGE_WARN] %s: archive key '%s' is neither a decoder nor a virtual key, skipped\r\n",
                          path, key);
            continue;
        }

        if (arc.containsKey("interval_sec")) {
            LogSerial.printf("[STORAGE_WARN] %s: 'interval_sec' is obsolete, use device 'report_interval_sec' "
                             "or 'expected_interval_sec' (ignored)\r\n", config.channel_key);
        }

        /* Expected update interval of this key: keys sent with every packet (depth) follow the
         * device transmit interval; sporadic keys (battery_soc) override it */
        uint32_t raw_interval = arc["expected_interval_sec"] | report_interval;
        config.roundtrip_interval = sanitize_roundtrip_interval(raw_interval, config.channel_key);
        config.storage_interval = (config.roundtrip_interval > 300) ? 300 : config.roundtrip_interval;
        config.tolerance_sec = arc["tolerance_sec"] | 15;
        config.max_estimated_count = arc["max_estimated_count"] | 0;

        JsonObjectConst quality = arc["quality"];
        uint32_t solid_pct = quality["solid_pct"] | QUALITY_DEFAULT_SOLID_PCT;
        uint32_t min_pct = quality["min_pct"] | QUALITY_DEFAULT_MIN_PCT;
        if (solid_pct > 100) solid_pct = 100;
        if (min_pct > solid_pct) min_pct = solid_pct;
        config.solid_pct = (uint8_t)solid_pct;
        config.min_pct = (uint8_t)min_pct;
        config.use_estimated = quality["use_estimated"] | QUALITY_DEFAULT_USE_ESTIMATED;

        JsonObjectConst retention = arc["retention"];
        bool any_level = false;
        for (size_t lvl = 0; lvl < RET_LEVEL_COUNT; lvl++) {
            uint32_t entries = retention[k_retention_names[lvl]] | 0;
            config.retention[lvl] = (entries > 0xFFFF) ? 0xFFFF : (uint16_t)entries;
            if (entries > 0) any_level = true;
        }
        if (!any_level) {
            LogSerial.printf("[STORAGE_WARN] %s: all retention levels are 0, nothing to store\r\n",
                          config.channel_key);
            continue;
        }

        compute_keep_entries(&config);
        data_storage_register_channel(&config);
    }
}

bool data_storage_load_devices(SdFat32* sd) {
    if (!sd) {
        g_sd = sd;
        return false;
    }
    g_sd = sd;

    /* Queued records survive the reload: they reference channels by key hash and are
     * written by process_buckets() once the channel is registered again */
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
        LogSerial.println("[STORAGE_WARN] Devices directory not found on FAT32 Partition 1!");
        return false;
    }

    LogSerial.printf("[STORAGE] Scanning %s for device archive definitions (free heap: %d B)...\r\n",
                  active_dir, rp2040.getFreeHeap());

    /* Parse only the fields the archive needs; device files can grow (keys, thresholds,
     * geometry) without increasing the RAM needed here */
    StaticJsonDocument<192> filter;
    filter["dev_addr"] = true;
    filter["decoder"] = true;
    filter["report_interval_sec"] = true;
    filter["archive"] = true;
    filter["virtual_decoder"]["*"]["unit"] = true;

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
                LogSerial.printf("[STORAGE_ERR] Cannot open device file: %s\r\n", path);
                entry.close();
                continue;
            }

            DynamicJsonDocument doc(3072);
            DeserializationError err = deserializeJson(doc, file, DeserializationOption::Filter(filter));
            file.close();
            if (err) {
                LogSerial.printf("[STORAGE_ERR] JSON parse failed on %s: %s (free heap: %d B)\r\n",
                              path, err.c_str(), rp2040.getFreeHeap());
                entry.close();
                continue;
            }

            register_device_archive(sd, doc.as<JsonObjectConst>(), path);
        }
        entry.close();
    }
    dir.close();
    LogSerial.printf("[STORAGE] Device scan complete. Registered storage channels: %u\r\n",
                  (unsigned)g_channel_count);
    return (g_channel_count > 0);
}

/**
 * @brief Collects the newest real sample of the raw level (cold-start restore).
 */
typedef struct {
    bool     found;
    uint32_t ts;
    float    value;
} restore_ctx_t;

static bool restore_visit(const void* record, void* ctx) {
    const arc_raw_record_t* rec = (const arc_raw_record_t*)record;
    restore_ctx_t* rc = (restore_ctx_t*)ctx;
    if (rec->flag == RT_FLAG_REAL && (!rc->found || rec->ts >= rc->ts)) {
        rc->found = true;
        rc->ts = rec->ts;
        rc->value = rec->value;
    }
    return true;
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
    channel->key_hash = channel_key_hash(config->channel_key);
    if (channel->config.storage_interval == 0) {
        channel->config.storage_interval = (channel->config.roundtrip_interval > 300) ? 300 : channel->config.roundtrip_interval;
    }

    channel->state.current_bucket = 0;
    channel->state.accumulator = 0.0;
    channel->state.sample_count = 0;
    channel->state.last_rx_time = 0;
    channel->state.last_stored_time = 0;
    channel->state.last_value = 0.0f;
    channel->state.has_last_value = false;
    /* Cold start: no substitute values until the first real packet resets the counter */
    channel->state.consecutive_estimated = channel->config.max_estimated_count;

    const uint16_t* ke = channel->config.keep_entries;
    const uint32_t keep[ARC_LEVEL_COUNT] = { ke[RET_SINGLE], ke[RET_HOURS], ke[RET_DAYS] };
    arc_channel_setup(&channel->arc, channel->config.dev_addr, channel->config.key, keep);

    /* Cold-start restore: continue holding the last real value of the archive, so a reboot
     * within the expected interval leaves no gap (and sporadic keys keep their last value).
     * consecutive_estimated stays at max: no substitutes are invented after a reboot. */
    if (littlefs_driver_is_mounted() && arc_channel_scan(&channel->arc)) {
        restore_ctx_t rc = {};
        arc_scan_range(&channel->arc, ARC_LEVEL_RAW, 0, UINT32_MAX, restore_visit, &rc);
        if (rc.found) {
            channel->state.last_value = rc.value;
            channel->state.last_rx_time = rc.ts;
            channel->state.has_last_value = true;
            LogSerial.printf("[STORAGE] %s: restored last value %.2f %s (ts=%lu)\r\n",
                             channel->config.channel_key, rc.value, channel->config.unit,
                             (unsigned long)rc.ts);
        }
    }

    LogSerial.printf("[STORAGE] Registered channel: %s (expected=%lu s, grid=%lu s, unit=%s, max_est=%u, "
                  "quality=%u/%u/%s, keep: raw=%u hour=%u day=%u)\r\n",
                  config->channel_key, (unsigned long)channel->config.roundtrip_interval,
                  (unsigned long)channel->config.storage_interval, config->unit,
                  (unsigned)channel->config.max_estimated_count,
                  (unsigned)channel->config.solid_pct, (unsigned)channel->config.min_pct,
                  channel->config.use_estimated ? "est" : "real",
                  (unsigned)ke[RET_SINGLE], (unsigned)ke[RET_HOURS], (unsigned)ke[RET_DAYS]);
    return true;
}

// ============================================================================
// Archive Writes with RAM Queue (card removed / USB-MSC active)
// ============================================================================

/**
 * Records that cannot be written (LittleFS unmounted) are queued in a compact RAM ring
 * and written as soon as LittleFS is back. When the ring is full the oldest record is
 * dropped, so the most recent history survives a long card removal.
 * Capacity: 1024 records (~28 KB) = ~8 h for 10 channels on a 5-minute grid.
 * Records reference their channel by key hash, so they survive a config reload.
 * The queue is drained in small portions by process_buckets() to keep the UART responsive.
 */
#define PENDING_CAPACITY 1024
#define PENDING_FLUSH_PER_TICK 32

typedef struct __attribute__((packed)) {
    uint32_t channel_hash;                   /* channel_key_hash() of the owning channel */
    uint8_t  level;                          /* arc_level_t */
    uint8_t  data[sizeof(arc_agg_record_t)]; /* arc_raw_record_t or arc_agg_record_t */
} pending_record_t;

static pending_record_t g_pending[PENDING_CAPACITY];
static size_t g_pending_head = 0;   /* Oldest queued record */
static size_t g_pending_count = 0;
static uint32_t g_pending_dropped = 0;

static data_channel_t* find_channel_by_hash(uint32_t hash) {
    for (size_t i = 0; i < g_channel_count; i++) {
        if (g_channels[i].key_hash == hash) return &g_channels[i];
    }
    return nullptr;
}

static void pending_push(uint32_t channel_hash, arc_level_t level, const void* record) {
    if (g_pending_count >= PENDING_CAPACITY) {
        g_pending_head = (g_pending_head + 1) % PENDING_CAPACITY;
        g_pending_count--;
        g_pending_dropped++;
    }
    pending_record_t* p = &g_pending[(g_pending_head + g_pending_count) % PENDING_CAPACITY];
    p->channel_hash = channel_hash;
    p->level = (uint8_t)level;
    memcpy(p->data, record, arc_record_size(level));
    g_pending_count++;
}

size_t data_storage_flush_pending(size_t max_records) {
    if (g_pending_count == 0 || !littlefs_driver_is_mounted()) return 0;

    size_t written = 0;
    while (g_pending_count > 0 && (max_records == 0 || written < max_records)) {
        pending_record_t* p = &g_pending[g_pending_head];
        data_channel_t* ch = find_channel_by_hash(p->channel_hash);
        /* Records of channels removed by a config change are dropped */
        if (ch && !arc_append(&ch->arc, (arc_level_t)p->level, p->data)) {
            break; /* Write failed: keep the record for the next attempt */
        }
        g_pending_head = (g_pending_head + 1) % PENDING_CAPACITY;
        g_pending_count--;
        written++;
    }

    if (written > 0) {
        LogSerial.printf("[STORAGE] Flushed %u queued records (%u left, %lu dropped while offline)\r\n",
                         (unsigned)written, (unsigned)g_pending_count, (unsigned long)g_pending_dropped);
        if (g_pending_count == 0) g_pending_dropped = 0;
    }
    return written;
}

static bool archive_write(data_channel_t* channel, arc_level_t level, const void* record) {
    /* Keep chronological order: while records are queued, new ones are queued behind them */
    if (g_pending_count == 0 && arc_append(&channel->arc, level, record)) {
        return true;
    }

    pending_push(channel->key_hash, level, record);
    DBG_LFS_PRINTF("LittleFS unavailable: queued %s record of %s (%u pending)",
                   (level == ARC_LEVEL_RAW) ? "raw" : (level == ARC_LEVEL_HOUR) ? "hour" : "day",
                   channel->config.channel_key, (unsigned)g_pending_count);
    return false;
}

static bool write_roundtrip_point(data_channel_t* channel, uint32_t timestamp, float value, uint8_t rt_flag) {
    arc_raw_record_t rec;
    rec.ts = timestamp;
    rec.value = value;
    rec.flag = rt_flag;
    return archive_write(channel, ARC_LEVEL_RAW, &rec);
}

static bool write_aggregated_point(data_channel_t* channel, granularity_t gran, const data_aggregated_t* agg) {
    if (!agg) return false;

    arc_agg_record_t rec;
    rec.ts = agg->start_timestamp;
    rec.avg = agg->avg_value;
    rec.min = agg->min_value;
    rec.max = agg->max_value;
    rec.count = agg->count;
    /* Local timezone offset in minutes (daily level; 0 for hourly UTC periods) */
    rec.tz_offset_min = (int16_t)(agg->tz_offset_sec / 60);
    rec.flags = agg->has_estimated ? ARC_AGG_FLAG_DEGRADED : 0;

    return archive_write(channel, (gran == GRAN_HOURLY) ? ARC_LEVEL_HOUR : ARC_LEVEL_DAY, &rec);
}

// ============================================================================
// Public API: RAM Accumulation & Bucket Flushing
// ============================================================================

static void flush_current_bucket(data_channel_t* channel) {
    if (!channel || channel->state.current_bucket == 0) return;

    uint32_t storage_interval = channel->config.storage_interval;
    uint32_t tolerance = channel->config.tolerance_sec > 0 ? channel->config.tolerance_sec : 15;
    uint32_t bucket_start = channel->state.current_bucket;
    uint32_t bucket_end = bucket_start + storage_interval;

    if (channel->state.sample_count > 0) {
        /* Regular measured value (real) */
        float avg = (float)(channel->state.accumulator / channel->state.sample_count);
        bool ok = write_roundtrip_point(channel, bucket_start, avg, RT_FLAG_REAL);
        DBG_LFS_PRINTF("Flush %s: %s -> %.2f %s (cnt=%u)", ok ? "SUCCESS" : "QUEUED",
                       channel->config.channel_key, avg, channel->config.unit, channel->state.sample_count);

        channel->state.last_stored_time = bucket_start;
        channel->state.last_value = avg;
        channel->state.has_last_value = true;
        channel->state.consecutive_estimated = 0;
    } else if (channel->state.has_last_value) {
        /* Hold last valid value as non-estimated until expected next packet deadline expires
         * (handles intervals > 300s as well as P2P relay jitter across bucket boundaries) */
        const uint32_t p2p_jitter_allowance_sec = 6;
        uint32_t deadline = channel->state.last_rx_time + channel->config.roundtrip_interval + tolerance + p2p_jitter_allowance_sec;
        bool is_estimated = (channel->state.last_rx_time == 0) || (bucket_end > deadline);

        if (is_estimated) {
            /* Substitute value cap: after max_estimated_count substitutes, write nothing so the
             * chart shows a real gap (DATA_FLAG_NO_DATA) instead of an endless flat line */
            if (channel->state.consecutive_estimated >= channel->config.max_estimated_count) {
                DBG_LFS_PRINTF("Gap for %s at ts=%lu (substitute limit %u reached)",
                               channel->config.channel_key, (unsigned long)bucket_start,
                               (unsigned)channel->config.max_estimated_count);
                return;
            }
            channel->state.consecutive_estimated++;
        }

        write_roundtrip_point(channel, bucket_start, channel->state.last_value,
                              is_estimated ? RT_FLAG_ESTIMATED : RT_FLAG_HOLD);
        DBG_LFS_PRINTF("Hold value for %s at ts=%lu (val=%.2f, mode=%s, est=%u/%u)",
                       channel->config.channel_key, (unsigned long)bucket_start,
                       channel->state.last_value, is_estimated ? "ESTIMATED" : "VALID_HOLD",
                       (unsigned)channel->state.consecutive_estimated,
                       (unsigned)channel->config.max_estimated_count);
        channel->state.last_stored_time = bucket_start;
    }
}

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
    if (channel->config.keep_entries[RET_SINGLE] == 0) return;

    /* Assign incoming value to RAM accumulator on storage_interval grid */
    uint32_t storage_interval = channel->config.storage_interval;
    uint32_t target_bucket = timestamp - (timestamp % storage_interval);
    
    if (channel->state.current_bucket != target_bucket) {
        /* Flush previous bucket if a boundary was crossed before periodic process_buckets() ran */
        if (channel->state.current_bucket != 0) {
            flush_current_bucket(channel);
        }
        channel->state.current_bucket = target_bucket;
        channel->state.accumulator = 0.0;
        channel->state.sample_count = 0;
    }

    channel->state.accumulator += value;
    channel->state.sample_count++;
    channel->state.last_rx_time = timestamp;
    DBG_LFS_PRINTF("Acc: %s added %.2f (cnt=%u) to bucket %lu", 
                   channel_key, value, channel->state.sample_count, (unsigned long)target_bucket);
}

void data_storage_process_buckets(void) {
    if (!data_storage_is_time_valid()) return;

    uint32_t now = (uint32_t)time(NULL);

    /* Write records queued while LittleFS was unavailable (bounded per tick) */
    if (g_pending_count > 0) {
        data_storage_flush_pending(PENDING_FLUSH_PER_TICK);
    }

    for (size_t i = 0; i < g_channel_count; i++) {
        data_channel_t* channel = &g_channels[i];
        if (channel->config.keep_entries[RET_SINGLE] == 0) continue;

        uint32_t storage_interval = channel->config.storage_interval;
        uint32_t tolerance = channel->config.tolerance_sec > 0 ? channel->config.tolerance_sec : 15;
        uint32_t active_bucket = now - (now % storage_interval);

        /* Initial bucket setup */
        if (channel->state.current_bucket == 0) {
            channel->state.current_bucket = active_bucket;
            continue;
        }

        /* Flush bucket only after the storage interval has elapsed plus per-channel tolerance */
        if (now >= (channel->state.current_bucket + storage_interval + tolerance)) {
            flush_current_bucket(channel);

            /* Advance to next cleanly aligned bucket */
            channel->state.current_bucket = active_bucket;
            channel->state.accumulator = 0.0;
            channel->state.sample_count = 0;
        }
    }
}

// ============================================================================
// Aggregation Functions (raw -> hour -> day)
// ============================================================================

/**
 * @brief Quality model for aggregated periods (hours, days, query-time multi-hour buckets).
 *
 * coverage = covered samples / expected samples of the period, where a covered sample is a
 * grid bucket with a real value or a valid hold (next packet not overdue). Substitute values
 * (RT_FLAG_ESTIMATED) never count as coverage; with use_estimated they still enter the average.
 *   coverage >= solid_pct -> regular value
 *   coverage >= min_pct   -> value flagged ESTIMATED (50 % opacity in the GUI)
 *   below min_pct         -> no entry (gap)
 * Coverage propagates upwards: a day sums the covered samples of its hours.
 */
typedef enum {
    AGG_QUALITY_GAP = 0,
    AGG_QUALITY_DEGRADED,
    AGG_QUALITY_SOLID
} agg_quality_t;

static agg_quality_t classify_coverage(const data_channel_config_t* cfg, uint32_t covered, uint32_t expected) {
    uint32_t solid_pct = cfg ? cfg->solid_pct : QUALITY_DEFAULT_SOLID_PCT;
    uint32_t min_pct = cfg ? cfg->min_pct : QUALITY_DEFAULT_MIN_PCT;
    if (expected == 0) expected = 1;

    uint32_t pct = (covered * 100U) / expected;
    if (pct >= solid_pct) return AGG_QUALITY_SOLID;
    if (pct >= min_pct) return AGG_QUALITY_DEGRADED;
    return AGG_QUALITY_GAP;
}

static const char* quality_name(agg_quality_t q) {
    return (q == AGG_QUALITY_SOLID) ? "SOLID" : (q == AGG_QUALITY_DEGRADED) ? "ESTIMATED" : "GAP";
}

/** Accumulator shared by the aggregation visitors */
typedef struct {
    bool     use_estimated;
    double   weighted_sum;
    uint32_t weight_total;
    uint32_t covered;
    uint32_t estimated;
    uint32_t periods;      /* Source records used (hours for a day) */
    float    min_value;
    float    max_value;
} agg_acc_t;

static void acc_add(agg_acc_t* acc, float value, uint32_t weight, float min_v, float max_v) {
    if (acc->weight_total == 0 || min_v < acc->min_value) acc->min_value = min_v;
    if (acc->weight_total == 0 || max_v > acc->max_value) acc->max_value = max_v;
    acc->weighted_sum += (double)value * weight;
    acc->weight_total += weight;
}

static bool raw_agg_visit(const void* record, void* ctx) {
    const arc_raw_record_t* rec = (const arc_raw_record_t*)record;
    agg_acc_t* acc = (agg_acc_t*)ctx;
    if (rec->flag == RT_FLAG_ESTIMATED) {
        acc->estimated++;
        if (!acc->use_estimated) return true;
    } else {
        acc->covered++;
    }
    acc_add(acc, rec->value, 1, rec->value, rec->value);
    return true;
}

static bool hour_agg_visit(const void* record, void* ctx) {
    const arc_agg_record_t* rec = (const arc_agg_record_t*)record;
    agg_acc_t* acc = (agg_acc_t*)ctx;
    /* Weight by covered samples; an hour made only of substitutes (use_estimated) weighs 1 */
    acc_add(acc, rec->avg, (rec->count > 0) ? rec->count : 1, rec->min, rec->max);
    acc->covered += rec->count;
    acc->periods++;
    return true;
}

static void aggregate_to_hourly(data_channel_t* channel) {
    if (!littlefs_driver_is_mounted() || !channel) return;
    const data_channel_config_t* cfg = &channel->config;

    uint32_t now = (uint32_t)time(NULL);
    uint32_t current_hour = now - (now % 3600);
    uint32_t previous_hour = current_hour - 3600;

    /* Expected samples = grid buckets per hour */
    uint32_t interval = (cfg->storage_interval > 0) ? cfg->storage_interval : 300;
    uint32_t expected_points = 3600 / interval;
    if (expected_points == 0) expected_points = 1;

    agg_acc_t acc = {};
    acc.use_estimated = cfg->use_estimated;
    arc_scan_range(&channel->arc, ARC_LEVEL_RAW, previous_hour, current_hour, raw_agg_visit, &acc);

    if (acc.weight_total == 0) return; /* Nothing usable in this hour: gap */

    agg_quality_t q = classify_coverage(cfg, acc.covered, expected_points);
    DBG_LFS_PRINTF("Hourly %s: covered=%lu/%lu, est=%lu -> %s",
                   cfg->channel_key, (unsigned long)acc.covered, (unsigned long)expected_points,
                   (unsigned long)acc.estimated, quality_name(q));
    if (q == AGG_QUALITY_GAP) return;

    data_aggregated_t hourly = {0};
    hourly.start_timestamp = previous_hour;
    hourly.avg_value = (float)(acc.weighted_sum / acc.weight_total);
    hourly.min_value = acc.min_value;
    hourly.max_value = acc.max_value;
    hourly.count = acc.covered;
    hourly.has_estimated = (q == AGG_QUALITY_DEGRADED);

    write_aggregated_point(channel, GRAN_HOURLY, &hourly);
    DBG_LFS_PRINTF("Hourly aggregated for %s: avg=%.2f (min=%.2f, max=%.2f)",
                   cfg->channel_key, hourly.avg_value, hourly.min_value, hourly.max_value);
}

static void aggregate_to_daily(data_channel_t* channel) {
    if (!littlefs_driver_is_mounted() || !channel) return;
    const data_channel_config_t* cfg = &channel->config;

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

    /* Expected samples of this local day (23 h / 25 h on DST switch days) */
    uint32_t interval = (cfg->storage_interval > 0) ? cfg->storage_interval : 300;
    uint32_t day_len = (uint32_t)(today_midnight - yesterday_midnight);
    uint32_t expected_points = day_len / interval;

    /* Hourly points in interval [yesterday 00:00 local .. today 00:00 local[ */
    agg_acc_t acc = {};
    arc_scan_range(&channel->arc, ARC_LEVEL_HOUR, (uint32_t)yesterday_midnight, (uint32_t)today_midnight,
                   hour_agg_visit, &acc);

    if (acc.periods == 0 || acc.weight_total == 0) return;

    agg_quality_t q = classify_coverage(cfg, acc.covered, expected_points);
    DBG_LFS_PRINTF("Daily %s: hours=%lu, covered=%lu/%lu -> %s", cfg->channel_key,
                   (unsigned long)acc.periods, (unsigned long)acc.covered,
                   (unsigned long)expected_points, quality_name(q));
    if (q == AGG_QUALITY_GAP) return;

    data_aggregated_t daily = {0};
    daily.start_timestamp = (uint32_t)yesterday_midnight;
    daily.tz_offset_sec = tz_offset_sec;
    daily.avg_value = (float)(acc.weighted_sum / acc.weight_total);
    daily.min_value = acc.min_value;
    daily.max_value = acc.max_value;
    daily.count = acc.covered;
    daily.has_estimated = (q == AGG_QUALITY_DEGRADED);

    write_aggregated_point(channel, GRAN_DAILY, &daily);
    DBG_LFS_PRINTF("Daily aggregated for %s: start=%lu, tz_off=%ld s, avg=%.2f",
                   cfg->channel_key, (unsigned long)daily.start_timestamp, (long)daily.tz_offset_sec, daily.avg_value);
}

void data_storage_aggregate_hourly(void) {
    if (!data_storage_is_time_valid()) return;

    for (size_t i = 0; i < g_channel_count; i++) {
        if (g_channels[i].config.keep_entries[RET_HOURS] > 0) {
            aggregate_to_hourly(&g_channels[i]);
        }
    }
}

void data_storage_aggregate_daily(void) {
    if (!data_storage_is_time_valid()) return;

    for (size_t i = 0; i < g_channel_count; i++) {
        if (g_channels[i].config.keep_entries[RET_DAYS] > 0) {
            aggregate_to_daily(&g_channels[i]);
        }
    }
}

// ============================================================================
// Query Functions (Reading from LittleFS Partition 2)
// ============================================================================

/** Collects records of one level into a bounded data_point_t array */
typedef struct {
    data_point_t* points;
    size_t        count;
    size_t        capacity;
    const char*   unit;
} collect_ctx_t;

static void set_point_unit(data_point_t* p, const char* unit) {
    strncpy(p->unit, unit ? unit : "", sizeof(p->unit) - 1);
    p->unit[sizeof(p->unit) - 1] = '\0';
}

static bool raw_collect_visit(const void* record, void* ctx) {
    const arc_raw_record_t* rec = (const arc_raw_record_t*)record;
    collect_ctx_t* cc = (collect_ctx_t*)ctx;
    if (cc->count >= cc->capacity) return false;
    data_point_t* p = &cc->points[cc->count++];
    p->timestamp = rec->ts;
    p->value = rec->value;
    set_point_unit(p, cc->unit);
    /* RT_FLAG_HOLD is a valid value (next packet not overdue yet) */
    p->is_estimated = (rec->flag == RT_FLAG_ESTIMATED);
    p->count = 0;
    p->is_gap = false;
    p->value_min = rec->value;
    p->value_max = rec->value;
    return true;
}

static bool agg_collect_visit(const void* record, void* ctx) {
    const arc_agg_record_t* rec = (const arc_agg_record_t*)record;
    collect_ctx_t* cc = (collect_ctx_t*)ctx;
    if (cc->count >= cc->capacity) return false;
    data_point_t* p = &cc->points[cc->count++];
    p->timestamp = rec->ts;
    p->value = rec->avg;
    set_point_unit(p, cc->unit);
    p->is_estimated = (rec->flags & ARC_AGG_FLAG_DEGRADED) != 0;
    p->count = rec->count;
    p->is_gap = false;
    p->value_min = rec->min;
    p->value_max = rec->max;
    return true;
}

/**
 * @brief Load all records of a level within [start, end[ (bounded by max_points).
 */
static data_point_t* load_level(data_channel_t* channel, arc_level_t level,
                                uint32_t start_timestamp, uint32_t end_timestamp,
                                size_t max_points, size_t* count) {
    *count = 0;
    if (!channel || max_points == 0) return nullptr;

    data_point_t* points = (data_point_t*)malloc(max_points * sizeof(data_point_t));
    if (!points) {
        LogSerial.printf("[STORAGE_ERR] OOM loading %s (%lu bytes)\r\n", channel->config.channel_key,
                         (unsigned long)(max_points * sizeof(data_point_t)));
        return nullptr;
    }

    collect_ctx_t cc = { points, 0, max_points, channel->config.unit };
    arc_scan_range(&channel->arc, level, start_timestamp, end_timestamp,
                   (level == ARC_LEVEL_RAW) ? raw_collect_visit : agg_collect_visit, &cc);

    DBG_LFS_PRINTF("%s/%s: Found %u points in range [%lu..%lu]", channel->arc.dir,
                   (level == ARC_LEVEL_RAW) ? "raw" : (level == ARC_LEVEL_HOUR) ? "hour" : "day",
                   (unsigned)cc.count, (unsigned long)start_timestamp, (unsigned long)end_timestamp);

    if (cc.count == 0) {
        free(points);
        return nullptr;
    }
    *count = cc.count;
    return points;
}

static void set_gap(data_point_t* p, uint32_t ts) {
    p->timestamp = ts;
    p->value = 0.0f;
    p->unit[0] = '\0';
    p->is_estimated = false;
    p->count = 0;
    p->is_gap = true;
    p->value_min = 0.0f;
    p->value_max = 0.0f;
}

static data_point_t* load_and_aggregate_roundtrip(
    data_channel_t* channel,
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    uint32_t interval_sec,
    uint32_t max_points,
    size_t* count
) {
    *count = 0;
    if (!channel || max_points == 0) return nullptr;

    /* Grid records needed for the requested window (+1 bucket margin) */
    uint32_t grid = (channel->config.storage_interval > 0) ? channel->config.storage_interval : 300;
    size_t raw_capacity = (size_t)((end_timestamp - start_timestamp) / grid) + 2;
    if (raw_capacity > 400) raw_capacity = 400;

    size_t raw_count = 0;
    data_point_t* raw_points = load_level(channel, ARC_LEVEL_RAW, start_timestamp, end_timestamp,
                                          raw_capacity, &raw_count);

    data_point_t* result = (data_point_t*)malloc(max_points * sizeof(data_point_t));
    if (!result) {
        if (raw_points) free(raw_points);
        return nullptr;
    }

    const data_channel_state_t* live_state = &channel->state;
    const char* default_unit = channel->config.unit;

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
        float vmin = 0.0f;
        float vmax = 0.0f;

        while (k < raw_count && raw_points[k].timestamp < (current_bucket + interval_sec)) {
            float v = raw_points[k].value;
            if (bucket_count == 0 || v < vmin) vmin = v;
            if (bucket_count == 0 || v > vmax) vmax = v;
            sum += v;
            bucket_count++;
            if (raw_points[k].is_estimated) has_estimated = true;
            k++;
        }

        /* Fallback to unflushed RAM accumulator if query arrives right on the bucket boundary */
        if (bucket_count == 0 && live_state->sample_count > 0 &&
            live_state->current_bucket >= current_bucket &&
            live_state->current_bucket < (current_bucket + interval_sec)) {
            sum = (float)live_state->accumulator;
            bucket_count = live_state->sample_count;
            has_estimated = false;
            /* The accumulator keeps no extremes: use its average */
            vmin = vmax = sum / (float)bucket_count;
        }

        if (bucket_count > 0) {
            data_point_t* p = &result[result_count];
            p->timestamp = current_bucket;
            p->value = sum / (float)bucket_count;
            set_point_unit(p, default_unit);
            p->is_estimated = has_estimated;
            p->count = bucket_count;
            p->is_gap = false;
            p->value_min = vmin;
            p->value_max = vmax;
            j = k;
        } else {
            /* Missing grid interval: flag as empty gap */
            set_gap(&result[result_count], current_bucket);
        }

        result_count++;
        current_bucket += interval_sec;
    }

    if (raw_points) free(raw_points);
    *count = result_count;
    return result;
}

static data_point_t* load_from_hourly(
    data_channel_t* channel,
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    uint32_t max_points,
    size_t* count
) {
    /* Hourly records sit exactly on the hour; the end tolerance keeps the last slot */
    return load_level(channel, ARC_LEVEL_HOUR, start_timestamp, end_timestamp + 31, max_points, count);
}

static data_point_t* load_and_aggregate_hourly(
    data_channel_t* channel,
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    uint32_t interval_sec,
    uint32_t max_points,
    size_t* count
) {
    *count = 0;
    if (!channel || max_points == 0) return nullptr;

    size_t raw_count = 0;
    data_point_t* raw_points = load_level(channel, ARC_LEVEL_HOUR, start_timestamp, end_timestamp, 400, &raw_count);

    data_point_t* result = (data_point_t*)malloc(max_points * sizeof(data_point_t));
    if (!result) {
        if (raw_points) free(raw_points);
        return nullptr;
    }

    const data_channel_config_t* cfg = &channel->config;

    size_t result_count = 0;
    uint32_t current_bucket = start_timestamp - (start_timestamp % interval_sec);
    size_t j = 0;

    /* Expected grid samples per multi-hour bucket (e.g. 2 h at 300 s grid = 24) */
    uint32_t grid = (cfg->storage_interval > 0) ? cfg->storage_interval : 300;
    uint32_t expected_samples = interval_sec / grid;
    if (expected_samples == 0) expected_samples = 1;

    /* Loop strictly over all requested bucket slots to guarantee exact max_points */
    while (result_count < max_points && current_bucket < end_timestamp) {
        while (j < raw_count && raw_points[j].timestamp < current_bucket) {
            j++;
        }

        size_t k = j;
        double weighted_sum = 0.0;
        uint32_t weight_total = 0;
        uint32_t covered = 0;
        float vmin = 0.0f;
        float vmax = 0.0f;

        while (k < raw_count && raw_points[k].timestamp < (current_bucket + interval_sec)) {
            if (weight_total == 0 || raw_points[k].value_min < vmin) vmin = raw_points[k].value_min;
            if (weight_total == 0 || raw_points[k].value_max > vmax) vmax = raw_points[k].value_max;
            uint32_t weight = (raw_points[k].count > 0) ? raw_points[k].count : 1;
            weighted_sum += (double)raw_points[k].value * weight;
            weight_total += weight;
            covered += raw_points[k].count;
            k++;
        }
        j = k;

        /* Same quality model as the archived hours/days (coverage of grid samples) */
        agg_quality_t q = (weight_total > 0) ? classify_coverage(cfg, covered, expected_samples)
                                             : AGG_QUALITY_GAP;

        if (q != AGG_QUALITY_GAP) {
            data_point_t* p = &result[result_count];
            p->timestamp = current_bucket;
            p->value = (float)(weighted_sum / weight_total);
            set_point_unit(p, cfg->unit);
            p->is_estimated = (q == AGG_QUALITY_DEGRADED);
            p->count = covered;
            p->is_gap = false;
            p->value_min = vmin;
            p->value_max = vmax;
        } else {
            /* Blackout / gap slot: coverage below min_pct, flag as empty gap */
            set_gap(&result[result_count], current_bucket);
        }

        result_count++;
        current_bucket += interval_sec;
    }

    if (raw_points) free(raw_points);
    *count = result_count;
    return result;
}

static data_point_t* load_from_daily(
    data_channel_t* channel,
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    uint32_t max_points,
    size_t* count
) {
    *count = 0;
    if (!channel || max_points == 0) return nullptr;

    data_point_t* result = (data_point_t*)malloc(max_points * sizeof(data_point_t));
    if (!result) return nullptr;

    /* 1. Read daily archive entries of the window (+/- 2 h for DST shifted midnights) */
    size_t raw_count = 0;
    uint32_t from_ts = (start_timestamp > 7200) ? start_timestamp - 7200 : 0;
    data_point_t* raw_daily = load_level(channel, ARC_LEVEL_DAY, from_ts, end_timestamp + 7200,
                                         max_points + 4, &raw_count);

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

        set_gap(&result[i], (uint32_t)slot_midnight);

        /* Match archive record within 2 hours tolerance (covers DST shifts) */
        for (size_t r = 0; r < raw_count; r++) {
            long diff = (long)raw_daily[r].timestamp - (long)slot_midnight;
            if (diff >= -7200 && diff <= 7200) {
                result[i] = raw_daily[r];
                result[i].timestamp = (uint32_t)slot_midnight;
                break;
            }
        }
    }

    if (raw_daily) free(raw_daily);
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

    int32_t idx = find_channel(channel_key);
    if (idx < 0) {
        *count = 0;
        return nullptr;
    }
    data_channel_t* channel = &g_channels[idx];
    bool has_hourly = channel->config.keep_entries[RET_HOURS] > 0;
    bool has_daily = channel->config.keep_entries[RET_DAYS] > 0;

    /* Sub-hourly query: aggregate roundtrip on bucket grid */
    if (interval_sec < 3600 || !has_hourly) {
        *actual_interval_sec = interval_sec;
        return load_and_aggregate_roundtrip(channel, start_timestamp, end_timestamp, *actual_interval_sec, max_points, count);
    }
    /* Exactly 1 hour requested: load directly from hourly archive */
    else if (interval_sec == 3600) {
        *actual_interval_sec = 3600;
        return load_from_hourly(channel, start_timestamp, end_timestamp, max_points, count);
    }
    /* Multi-hour aggregation (e.g. 2h, 4h, 6h): aggregate from hourly archive.
     * Also used for daily steps when the channel keeps no daily archive. */
    else if (interval_sec < 86400 || !has_daily) {
        *actual_interval_sec = interval_sec;
        return load_and_aggregate_hourly(channel, start_timestamp, end_timestamp, interval_sec, max_points, count);
    }
    /* Daily or multi-day intervals: load from daily archive */
    else {
        *actual_interval_sec = 86400;
        return load_from_daily(channel, start_timestamp, end_timestamp, max_points, count);
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
    uint8_t alarm_flags,
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
        if (raw_pts[i].is_gap) {
            chart_pts[i].flags = DATA_FLAG_NO_DATA;
        } else {
            if (raw_pts[i].is_estimated) {
                chart_pts[i].flags |= DATA_FLAG_ESTIMATED; /* 50% opacity in LVGL */
            }
            if (is_averaged) {
                chart_pts[i].flags |= DATA_FLAG_AVERAGED;
            }
            /* Alarm coloring: an aggregated bucket is marked when ANY value behind it crossed
             * the level (minimum for the falling, maximum for the rising level), so an episode
             * shows up in hours and days even if the average stays inside the limits */
            if (alarm_min > 0.0f && raw_pts[i].value_min < alarm_min) {
                chart_pts[i].flags |= (alarm_flags & CHART_ALARM_MIN_IS_WARNING) ? DATA_FLAG_ALARM_WARN
                                                                                 : DATA_FLAG_ALARM_DANGER;
            }
            if (alarm_max > 0.0f && raw_pts[i].value_max > alarm_max) {
                chart_pts[i].flags |= (alarm_flags & CHART_ALARM_MAX_IS_WARNING) ? DATA_FLAG_ALARM_WARN
                                                                                 : DATA_FLAG_ALARM_DANGER;
            }
        }
    } 

    free(raw_pts);
    *count = raw_count;
    return chart_pts;
}

// ============================================================================
// LTTB (Largest-Triangle-Three-Buckets) reduction for trend graphs
// ============================================================================

typedef struct {
    double   sum;
    uint32_t n;
    float    vmin;
    float    vmax;
    uint8_t  next;              /* next non-empty bucket (own index if none) */
} lttb_bucket_t;

/* Static: core 1 has only 8 KB stack; queries run one at a time under g_comm_mutex */
static lttb_bucket_t s_lttb_buckets[LTTB_MAX_POINTS];

typedef struct {
    uint32_t start;
    uint32_t width;
    uint32_t m;
    int      pass;              /* 1 = bucket statistics, 2 = point selection */
    arc_level_t level;          /* level of the records currently visited */
    uint32_t raw_gap_sec;       /* raw records further apart than this are a data gap */
    /* Pass 2: previous visited record (gap detection) */
    bool     have_prev;
    uint32_t prev_ts;
    arc_level_t prev_level;
    /* Pass 2 */
    int32_t  cur;               /* bucket being selected (-1 = none yet) */
    int32_t  last_nonempty;
    bool     have_a;            /* previous selected point exists */
    double   a_t, a_v;
    bool     have_best;
    double   best_t, best_v, best_area;
    bool     best_est;
    bool     prev_emitted_gap;
    bool     any_emitted;
    chart_data_point_t* out;
    size_t   out_count;
    size_t   out_capacity;
    float    alarm_min, alarm_max;
    uint8_t  alarm_flags;
} lttb_ctx_t;

/* Unpack one archive record of the current level into time / value / extremes / estimated */
static void lttb_record(const lttb_ctx_t* c, const void* record, uint32_t* ts, float* v, float* vmin,
                        float* vmax, bool* est) {
    if (c->level == ARC_LEVEL_RAW) {
        const arc_raw_record_t* r = (const arc_raw_record_t*)record;
        *ts = r->ts;
        *v = *vmin = *vmax = r->value;
        *est = (r->flag == RT_FLAG_ESTIMATED);
    } else {
        const arc_agg_record_t* a = (const arc_agg_record_t*)record;
        *ts = a->ts;
        *v = a->avg;
        *vmin = a->min;
        *vmax = a->max;
        *est = (a->flags & ARC_AGG_FLAG_DEGRADED) != 0;
    }
}

/* Average point (C) of the next non-empty bucket; the bucket itself for the newest one */
static void lttb_next_avg(const lttb_ctx_t* c, int32_t b, double* t, double* v) {
    int32_t nb = s_lttb_buckets[b].next;
    *t = (double)c->start + (double)c->width * ((double)nb + 0.5);
    *v = s_lttb_buckets[nb].sum / (double)s_lttb_buckets[nb].n;
}

static void lttb_emit_point(lttb_ctx_t* c, int32_t b) {
    if (!c->have_best || c->out_count >= c->out_capacity) return;
    chart_data_point_t* p = &c->out[c->out_count++];
    p->timestamp = (uint32_t)c->best_t;
    p->value = (float)c->best_v;
    p->flags = c->best_est ? DATA_FLAG_ESTIMATED : 0;
    const lttb_bucket_t* bk = &s_lttb_buckets[b];
    if (c->alarm_min > 0.0f && bk->vmin < c->alarm_min) {
        p->flags |= (c->alarm_flags & CHART_ALARM_MIN_IS_WARNING) ? DATA_FLAG_ALARM_WARN : DATA_FLAG_ALARM_DANGER;
    }
    if (c->alarm_max > 0.0f && bk->vmax > c->alarm_max) {
        p->flags |= (c->alarm_flags & CHART_ALARM_MAX_IS_WARNING) ? DATA_FLAG_ALARM_WARN : DATA_FLAG_ALARM_DANGER;
    }
    c->have_a = true;
    c->a_t = c->best_t;
    c->a_v = c->best_v;
    c->prev_emitted_gap = false;
    c->any_emitted = true;
}

/* NO_DATA marker between two records (breaks the line), placed just after the older one */
static void lttb_emit_gap(lttb_ctx_t* c, uint32_t before_ts, uint32_t after_ts) {
    if (!c->any_emitted || c->prev_emitted_gap || c->out_count >= c->out_capacity) return;
    chart_data_point_t* p = &c->out[c->out_count++];
    p->timestamp = (after_ts - before_ts > 1) ? before_ts + 1 : before_ts;
    p->value = 0.0f;
    p->flags = DATA_FLAG_NO_DATA;
    c->prev_emitted_gap = true;
}

/*
 * Records further apart than the expected spacing of their level (raw: update interval incl.
 * tolerance, hour / day: 1.5 periods) are separated by a gap marker. A gap is never shorter than
 * one bucket: sparse data in narrow buckets (e.g. hourly values in 22 min buckets) stays connected,
 * and missing samples below the chart resolution do not break the line.
 */
static uint32_t lttb_gap_limit(const lttb_ctx_t* c, arc_level_t level) {
    uint32_t limit;
    if (level == ARC_LEVEL_RAW) limit = c->raw_gap_sec;
    else if (level == ARC_LEVEL_HOUR) limit = 3600 + 1800;
    else limit = 86400 + 43200;
    return (limit > c->width) ? limit : c->width;
}

static bool lttb_visit(const void* record, void* ctx) {
    lttb_ctx_t* c = (lttb_ctx_t*)ctx;
    uint32_t ts;
    float v, vmin, vmax;
    bool est;
    lttb_record(c, record, &ts, &v, &vmin, &vmax, &est);
    if (ts < c->start) return true;
    uint32_t b = (ts - c->start) / c->width;
    if (b >= c->m) return true;

    if (c->pass == 1) {
        lttb_bucket_t* bk = &s_lttb_buckets[b];
        if (bk->n == 0 || vmin < bk->vmin) bk->vmin = vmin;
        if (bk->n == 0 || vmax > bk->vmax) bk->vmax = vmax;
        bk->sum += v;
        bk->n++;
        return true;
    }

    /* Pass 2: bucket change -> emit the selected point of the finished bucket, mark gaps.
       A gap limit >= width guarantees that a gap always coincides with a bucket change. */
    if ((int32_t)b != c->cur) {
        if (c->cur >= 0) lttb_emit_point(c, c->cur);
        if (c->have_prev && ts - c->prev_ts > lttb_gap_limit(c, c->prev_level)) {
            lttb_emit_gap(c, c->prev_ts, ts);
        }
        c->cur = (int32_t)b;
        c->have_best = false;
    }
    c->have_prev = true;
    c->prev_ts = ts;
    c->prev_level = c->level;

    bool take;
    if ((int32_t)b == c->last_nonempty) {
        take = true; /* newest bucket: always keep the newest value */
    } else if (!c->have_a) {
        take = !c->have_best; /* first bucket: its first point */
    } else {
        double ct, cv;
        lttb_next_avg(c, (int32_t)b, &ct, &cv);
        double area = fabs((c->a_t - ct) * ((double)v - c->a_v) - (c->a_t - (double)ts) * (cv - c->a_v));
        take = !c->have_best || area > c->best_area;
        if (take) c->best_area = area;
    }
    if (take) {
        c->have_best = true;
        c->best_t = (double)ts;
        c->best_v = (double)v;
        c->best_est = est;
    }
    return true;
}

/* Visit the joined levels in chronological order: day [start, hour_from[, hour [.., raw_from[, raw */
static void lttb_scan(data_channel_t* channel, lttb_ctx_t* c, uint32_t start, uint32_t end) {
    arc_raw_record_t raw_first;
    arc_agg_record_t hour_first;
    uint32_t raw_from = arc_read_first(&channel->arc, ARC_LEVEL_RAW, &raw_first) ? raw_first.ts : end;
    uint32_t hour_from = arc_read_first(&channel->arc, ARC_LEVEL_HOUR, &hour_first) ? hour_first.ts : raw_from;
    if (hour_from > raw_from) hour_from = raw_from;

    if (start < hour_from) {
        c->level = ARC_LEVEL_DAY;
        arc_scan_range(&channel->arc, ARC_LEVEL_DAY, start, (hour_from < end) ? hour_from : end, lttb_visit, c);
    }
    if (hour_from < end && hour_from < raw_from) {
        c->level = ARC_LEVEL_HOUR;
        arc_scan_range(&channel->arc, ARC_LEVEL_HOUR, (start > hour_from) ? start : hour_from,
                       (raw_from < end) ? raw_from : end, lttb_visit, c);
    }
    if (raw_from < end) {
        c->level = ARC_LEVEL_RAW;
        arc_scan_range(&channel->arc, ARC_LEVEL_RAW, (start > raw_from) ? start : raw_from, end, lttb_visit, c);
    }
}

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
) {
    *count = 0;
    if (!channel_key || !littlefs_driver_is_mounted() || end_timestamp <= start_timestamp || max_points == 0) {
        return nullptr;
    }
    int32_t idx = find_channel(channel_key);
    if (idx < 0) return nullptr;
    data_channel_t* channel = &g_channels[idx];

    uint32_t capacity = (max_points > LTTB_MAX_POINTS) ? LTTB_MAX_POINTS : max_points;
    uint32_t m = CHART_LTTB_BUCKETS(capacity);
    uint32_t width = (end_timestamp - start_timestamp + m - 1) / m;
    if (width == 0) width = 1;
    *actual_interval_sec = width;

    memset(s_lttb_buckets, 0, sizeof(s_lttb_buckets));
    lttb_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.start = start_timestamp;
    c.width = width;
    c.m = m;
    c.alarm_min = alarm_min;
    c.alarm_max = alarm_max;
    c.alarm_flags = alarm_flags;
    c.raw_gap_sec = channel->config.roundtrip_interval + channel->config.tolerance_sec;
    if (c.raw_gap_sec < channel->config.storage_interval) c.raw_gap_sec = channel->config.storage_interval;

    /* Pass 1: bucket statistics */
    c.pass = 1;
    lttb_scan(channel, &c, start_timestamp, end_timestamp);

    c.last_nonempty = -1;
    int32_t next = -1;
    for (int32_t b = (int32_t)m - 1; b >= 0; b--) {
        s_lttb_buckets[b].next = (uint8_t)((next >= 0) ? next : b);
        if (s_lttb_buckets[b].n > 0) {
            if (c.last_nonempty < 0) c.last_nonempty = b;
            next = b;
        }
    }
    if (c.last_nonempty < 0) return nullptr;

    /* Pass 2: point selection (one point per non-empty bucket, gap markers in between) */
    c.out_capacity = capacity;
    c.out = (chart_data_point_t*)malloc(c.out_capacity * sizeof(chart_data_point_t));
    if (!c.out) return nullptr;
    c.pass = 2;
    c.cur = -1;
    lttb_scan(channel, &c, start_timestamp, end_timestamp);
    if (c.cur >= 0) lttb_emit_point(&c, c.cur);

    *count = c.out_count;
    if (c.out_count == 0) {
        free(c.out);
        return nullptr;
    }
    return c.out;
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
