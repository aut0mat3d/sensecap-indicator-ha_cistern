/**
 * @file archive_store.cpp
 * @brief Segmented binary archive on LittleFS (see archive_store.hpp).
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#include "archive_store.hpp"
#include "littlefs_driver.hpp"
#include "debug_log.hpp"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char* const k_level_dirs[ARC_LEVEL_COUNT] = { "raw", "hour", "day" };

/* Legacy CSV archives of the previous format (removed on first scan, no migration) */
static const char* const k_legacy_files[] = { "roundtrip.csv", "hourly.csv", "daily.csv" };

size_t arc_record_size(arc_level_t level) {
    return (level == ARC_LEVEL_RAW) ? sizeof(arc_raw_record_t) : sizeof(arc_agg_record_t);
}

uint32_t arc_records_per_segment(arc_level_t level) {
    return ARC_SEGMENT_BYTES / (uint32_t)arc_record_size(level);
}

static void build_level_dir(const arc_channel_t* ch, arc_level_t level, char* out, size_t out_size) {
    snprintf(out, out_size, "%s/%s", ch->dir, k_level_dirs[level]);
}

static void build_segment_path(const arc_channel_t* ch, arc_level_t level, uint32_t seq,
                               char* out, size_t out_size) {
    snprintf(out, out_size, "%s/%s/%08lX.bin", ch->dir, k_level_dirs[level], (unsigned long)seq);
}

void arc_channel_setup(arc_channel_t* ch, const char* dev_addr, const char* key,
                       const uint32_t keep[ARC_LEVEL_COUNT]) {
    memset(ch, 0, sizeof(*ch));
    snprintf(ch->dir, sizeof(ch->dir), "/data/%s/%s", dev_addr, key);
    for (size_t l = 0; l < ARC_LEVEL_COUNT; l++) {
        ch->level[l].keep_records = keep[l];
    }
}

/* ── Segment discovery ──────────────────────────────────────────────────── */

typedef struct {
    bool     found;
    uint32_t min_seq;
    uint32_t max_seq;
    uint32_t max_size;
} seg_scan_ctx_t;

static bool parse_segment_name(const char* name, uint32_t* seq) {
    /* Expected: 8 hex digits + ".bin" */
    if (strlen(name) != 12 || strcmp(&name[8], ".bin") != 0) return false;
    char* end = nullptr;
    unsigned long v = strtoul(name, &end, 16);
    if (end != &name[8]) return false;
    *seq = (uint32_t)v;
    return true;
}

static bool seg_scan_visit(const char* name, uint32_t size, bool is_dir, void* ctx) {
    seg_scan_ctx_t* sc = (seg_scan_ctx_t*)ctx;
    uint32_t seq;
    if (is_dir || !parse_segment_name(name, &seq)) return true;

    if (!sc->found || seq < sc->min_seq) sc->min_seq = seq;
    if (!sc->found || seq > sc->max_seq) {
        sc->max_seq = seq;
        sc->max_size = size;
    }
    sc->found = true;
    return true;
}

bool arc_channel_scan(arc_channel_t* ch) {
    if (!ch || !littlefs_driver_is_mounted()) return false;

    /* Start with an empty archive: drop files of the former CSV format */
    for (size_t i = 0; i < sizeof(k_legacy_files) / sizeof(k_legacy_files[0]); i++) {
        char legacy[96];
        snprintf(legacy, sizeof(legacy), "%s/%s", ch->dir, k_legacy_files[i]);
        if (littlefs_driver_exists(legacy)) {
            littlefs_driver_remove(legacy);
            LogSerial.printf("[ARCHIVE] Removed legacy file %s\r\n", legacy);
        }
    }

    for (size_t l = 0; l < ARC_LEVEL_COUNT; l++) {
        arc_level_state_t* st = &ch->level[l];
        char dir[64];
        build_level_dir(ch, (arc_level_t)l, dir, sizeof(dir));

        seg_scan_ctx_t sc;
        memset(&sc, 0, sizeof(sc));
        littlefs_driver_list_dir(dir, seg_scan_visit, &sc);

        st->has_segments = sc.found;
        st->first_seq = sc.found ? sc.min_seq : 0;
        st->last_seq = sc.found ? sc.max_seq : 0;
        st->last_records = 0;
        if (sc.found) {
            size_t rec_size = arc_record_size((arc_level_t)l);
            st->last_records = sc.max_size / rec_size;
            /* A torn write leaves a partial record: continue in a fresh segment so
             * record offsets of new data stay aligned */
            if (sc.max_size % rec_size != 0) {
                st->last_records = arc_records_per_segment((arc_level_t)l);
                LogSerial.printf("[ARCHIVE] %s: partial record in segment %08lX, starting a new one\r\n",
                                 dir, (unsigned long)st->last_seq);
            }
        }
    }
    ch->scanned = true;
    return true;
}

/* ── Append & retention ─────────────────────────────────────────────────── */

static void enforce_retention(arc_channel_t* ch, arc_level_t level) {
    arc_level_state_t* st = &ch->level[level];
    uint32_t cap = arc_records_per_segment(level);

    /* Delete the oldest segment while the remaining ones still hold keep_records */
    while (st->last_seq > st->first_seq) {
        uint32_t remaining = (st->last_seq - st->first_seq - 1) * cap + st->last_records;
        if (remaining < st->keep_records) break;

        char path[96];
        build_segment_path(ch, level, st->first_seq, path, sizeof(path));
        littlefs_driver_remove(path);
        DBG_LFS_PRINTF("Retention: removed %s", path);
        st->first_seq++;
    }
}

bool arc_append(arc_channel_t* ch, arc_level_t level, const void* record) {
    if (!ch || !record || level >= ARC_LEVEL_COUNT || !littlefs_driver_is_mounted()) return false;
    if (!ch->scanned && !arc_channel_scan(ch)) return false;

    arc_level_state_t* st = &ch->level[level];
    uint32_t cap = arc_records_per_segment(level);

    if (!st->has_segments) {
        st->first_seq = 0;
        st->last_seq = 0;
        st->last_records = 0;
        st->has_segments = true;
    } else if (st->last_records >= cap) {
        st->last_seq++;
        st->last_records = 0;
    }

    char path[96];
    build_segment_path(ch, level, st->last_seq, path, sizeof(path));
    if (!littlefs_driver_append_bytes(path, record, arc_record_size(level))) {
        return false;
    }
    st->last_records++;

    enforce_retention(ch, level);
    return true;
}

/* ── Reading ────────────────────────────────────────────────────────────── */

static uint32_t record_ts(const uint8_t* rec) {
    uint32_t ts;
    memcpy(&ts, rec, sizeof(ts)); /* ts is the first field of both record types */
    return ts;
}

/**
 * @brief Read the last complete record of one segment file.
 */
static bool read_segment_last(const char* path, size_t rec_size, uint8_t* out) {
    int32_t size = littlefs_driver_file_size(path);
    if (size < (int32_t)rec_size) return false;
    uint32_t offset = (uint32_t)(size - (size % (int32_t)rec_size) - (int32_t)rec_size);
    return littlefs_driver_read_at(path, offset, out, rec_size) == (int32_t)rec_size;
}

size_t arc_scan_range(arc_channel_t* ch, arc_level_t level, uint32_t from_ts, uint32_t to_ts,
                      arc_visit_fn visit, void* ctx) {
    if (!ch || !visit || level >= ARC_LEVEL_COUNT || !littlefs_driver_is_mounted()) return 0;
    if (!ch->scanned && !arc_channel_scan(ch)) return 0;

    arc_level_state_t* st = &ch->level[level];
    if (!st->has_segments) return 0;

    size_t rec_size = arc_record_size(level);
    uint8_t rec[sizeof(arc_agg_record_t)];
    size_t visited = 0;

    for (uint32_t seq = st->first_seq; seq <= st->last_seq; seq++) {
        char path[96];
        build_segment_path(ch, level, seq, path, sizeof(path));

        /* Skip segments that end before the requested range (cheap: one record read) */
        if (seq < st->last_seq && read_segment_last(path, rec_size, rec) && record_ts(rec) < from_ts) {
            continue;
        }

        lfs_file_handle_t f = littlefs_driver_open_read(path);
        if (!f) continue;

        while (littlefs_driver_read_bytes(f, rec, rec_size) == (int32_t)rec_size) {
            uint32_t ts = record_ts(rec);
            if (ts < from_ts || ts >= to_ts) continue;
            visited++;
            if (!visit(rec, ctx)) {
                littlefs_driver_close(f);
                return visited;
            }
        }
        littlefs_driver_close(f);
    }
    return visited;
}

bool arc_read_first(arc_channel_t* ch, arc_level_t level, void* record_out) {
    if (!ch || !record_out || level >= ARC_LEVEL_COUNT || !littlefs_driver_is_mounted()) return false;
    if (!ch->scanned && !arc_channel_scan(ch)) return false;

    arc_level_state_t* st = &ch->level[level];
    if (!st->has_segments) return false;

    size_t rec_size = arc_record_size(level);
    /* The oldest segment can be empty right after a rollover: try the next one as well */
    for (uint32_t seq = st->first_seq; seq <= st->last_seq && seq <= st->first_seq + 1; seq++) {
        char path[96];
        build_segment_path(ch, level, seq, path, sizeof(path));
        if (littlefs_driver_read_at(path, 0, (uint8_t*)record_out, rec_size) == (int32_t)rec_size) return true;
    }
    return false;
}

bool arc_read_last(arc_channel_t* ch, arc_level_t level, void* record_out) {
    if (!ch || !record_out || level >= ARC_LEVEL_COUNT || !littlefs_driver_is_mounted()) return false;
    if (!ch->scanned && !arc_channel_scan(ch)) return false;

    arc_level_state_t* st = &ch->level[level];
    if (!st->has_segments) return false;

    size_t rec_size = arc_record_size(level);
    /* The newest segment can be empty right after a rollover: fall back to the previous one */
    for (uint32_t seq = st->last_seq + 1; seq-- > st->first_seq;) {
        char path[96];
        build_segment_path(ch, level, seq, path, sizeof(path));
        if (read_segment_last(path, rec_size, (uint8_t*)record_out)) return true;
        if (st->last_seq - seq >= 1) break;
    }
    return false;
}
