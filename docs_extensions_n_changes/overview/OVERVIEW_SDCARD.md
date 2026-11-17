# MicroSD Card Content Overview (`sdcontent/`)

> Generated 2026-11-17. `sdcontent/` is the git-tracked reference for **partition 1 (FAT32)**.
> Partition 2 (LittleFS) holds runtime archives under `/data/` and is not tracked.
> `SDbackup/` is a private backup of a test unit: never modify or commit it.

## Roles: decoder vs. device

| Folder | Role | Example |
|---|---|---|
| `decoder/` | **Hardware abstraction** ("LoRa driver"). Describes how to decode the payload of one sensor model. It is shared by every device that uses this hardware. | `EM500-SWL.json`, `SHT41.json` |
| `devices/` | **One measuring installation.** Address, keys, which decoder, offsets, tank geometry (`virtual_decoder`), `thresholds`, `limits`, display config and (new) `archive`. | `Wasser_Hochbehaelter.json`: an EM500-SWL measuring a water tank |

Four EM500-SWL units measuring a cistern, a grey-water tank, a fuel tank and a grain silo share **one** decoder file but have four device files, each with its own geometry, alarms and archiving needs.
Application-specific settings therefore never belong in a decoder file.

## Files

| Path | Read by | Content |
|---|---|---|
| `config/globals.json` | RP2040 `lorasensors_load_all` → ESP32 via `0x35` | language, POSIX timezone, NTP server, time format, P2P time sync |
| `config/lora_globals.json` | RP2040 `lorasensors_load_all` → ESP32 via `0x35` | RF parameters, sync word, `p2p_mesh` (network id, AES key, TTL) |
| `config/units.json` | RP2040 `units_init` (ESP32 has a builtin copy in `units.c`) | quantity types, units, factors/offsets |
| `config/lang/<lang>/units.json` | RP2040 units | localized unit names (override only `name`) |
| `decoder/<name>.json` | RP2040 `lorasensors_get_or_load_decoder` (RAM cache), `data_storage_load_decoder` | `format` (`IPSO_TLV`, `PACKED_BINARY`), `fport_config` (encryption), `commands` (downlinks), `reactions`, `mappings[]` (channel/type → `key`, `unit`, `datatype`, `context`: `measurement` / `diagnostic` / `history`) |
| `devices/<name>.json` | RP2040 `lorasensors_load_all` (devices, thresholds, virtual decoders, streamed to ESP32 via `0x3F`), `data_storage_load_devices` (archive channels), ESP32 `alarm_evaluator_load_device_json` | see schema below |
| `gui/media_standards.json` | ESP32 `media_standards_load_from_sd()` (after config push, together with the language files); `cistern_view` applies `media_get_color(device.medium)` as main color of its graphs | `media_types.<id>`: name EN/DE, `color_hex` (user-defined main color), `unit_default` |
| `gui/lang/<lang>/common.json` | ESP32 `gui_i18n_reload_from_sd` via `0x38` | GUI string overrides |
| `gui/lang/<lang>/alarm_config.json` | ESP32 `gui_i18n` / `alarm_model` | alarm colors, texts, HW button behavior |

## Device JSON schema (`devices/*.json`)

```jsonc
{
  "device_name": "Hochbehälter",
  "medium": "water",                    // key into gui/media_standards.json
  "enabled": true,
  "dev_addr": "05975DC9",               // LoRaWAN DevAddr (hex) or I2C address ("0x45")
  "dev_sn": "...", "dev_eui": "...",
  "keys": { "nwk_skey": "...", "app_skey": "...", "d2d_key": "..." },  // omit or zero on listener nodes
  "decoder": "EM500-SWL",               // file name in decoder/
  "report_interval_sec": 300,           // configured transmit interval of the sensor (60..86400, default 300):
                                        // ESP32 offline watchdog + default expected interval of archived keys
  "offsets": { "<key>": { "value": 50, "unit": "mm" } },
  "virtual_decoder": { "<out_key>": { "source_key": "depth", "type"|"calculation": "...", ... } },
  "thresholds": [ { "id", "key", "severity", "value_percent"|"value_threshold", "direction", "damping", "hysteresis[_percent]", "buzzer_mode" } ],
  "limits": { "<key>": { "val_min", "val_max", "cap_min", "cap_max" } },
  "display_config": { "unit_system": "metric", "volume_unit": "m3" },
  "archive": {                           // optional, per key; keys without entry are not archived
    "<key>": {
      "expected_interval_sec": 86400,    // optional: update interval of this key if it differs from
                                         // report_interval_sec (e.g. battery_soc once a day)
      "tolerance_sec": 15,               // jitter tolerance (default 15; P2P adds a fixed 6 s allowance)
      "max_estimated_count": 3,          // max substitute values after a missed packet (default 0 = never)
      "quality": { "solid_pct": 66, "min_pct": 10, "use_estimated": false },   // defaults shown
      "retention": { "single": 144, "hours": 48, "days": 32, "weeks": 0, "months": 0, "years": 0 }
    }
  }
}
```

Archive keys may be decoder keys (`depth`, `battery_soc`) or virtual keys (`volume`, `flowrate`).
Retention: number of entries kept per level; `0` disables the level. `weeks`, `months` and `years` are parsed but not aggregated yet (planned NTTW phase).

Current limits of the engine (until the NTTW rework):
* Each level is aggregated from the next finer one. A disabled finer level that is needed as a source is kept as a small staging level: `raw` with 2 hours of buckets when hourly is active, `hour` with at least 26 entries when daily is active (covers 25-hour DST days).
* Grid: values are stored on a wall-clock grid of `min(expected interval, 300 s)`. Each grid bucket gets a flag: `0` real sample, `2` hold (no new sample, but the next packet is not overdue yet: covers intervals longer than the grid and the asynchronous sensor clock), `1` substitute (packet overdue).
* Quality model for hours, days and multi-hour chart buckets: coverage = (real + hold buckets) / expected buckets. Coverage ≥ `solid_pct` → regular value; ≥ `min_pct` → value flagged ESTIMATED (50 % opacity); below → gap. Substitutes never count as coverage; with `use_estimated` they enter the average. A day sums the coverage of its hours.
* Substitute values: after a missed packet the last value is held without the ESTIMATED flag until `interval_sec + tolerance_sec + 6 s` has passed. After that, up to `max_estimated_count` buckets are written with the ESTIMATED flag, then nothing (chart gap). After a reboot no substitutes are written until the first real packet arrives.
* The RP2040 parses each device JSON into a 4 KB document (`lorasensors.cpp`); keep device files below roughly 3 KB of JSON content.

## LittleFS layout (partition 2)

```
/data/last_time.txt                          last known UTC (offline clock recovery)
/data/<DEVADDR>/<key>/raw/<SEQ>.bin          grid records, 9 B:  ts(u32) value(f32) flag(u8: 0 real, 1 substitute, 2 hold)
/data/<DEVADDR>/<key>/hour/<SEQ>.bin         aggregates, 23 B:   ts(u32) avg min max(f32) count(u32) tz_off_min(i16) flags(u8: bit0 degraded)
/data/<DEVADDR>/<key>/day/<SEQ>.bin          same 23 B records, period start = local midnight
```
* `<SEQ>` = 8 hex digits, ascending. Each segment holds at most 4096 B (one LittleFS block: 455 grid records or 178 aggregates). Records are only appended; a full segment is followed by a new one; the oldest segment is deleted once the newer ones hold the retention. Retention is a guaranteed minimum (plus at most one segment) and is not limited by RAM.
* Little-endian, packed structs (`archive_store.hpp`). Legacy CSV files are deleted on the first scan (no migration).
* While LittleFS is unavailable (card removed, USB-MSC), records are queued in RAM (1024 records ≈ 8 h for 10 channels) and written in portions of 32 per second afterwards; on overflow the oldest queued record is dropped.
* After a reboot each channel restores the newest real grid value and keeps holding it until its deadline (no gap after a short reboot, no invented substitutes after a long one).
