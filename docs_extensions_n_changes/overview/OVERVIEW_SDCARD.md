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
| `config/mqtt.json` | ESP32 `mqtt_bridge` via `0x38` (after every config push) | MQTT broker and HA discovery settings, see below. Missing file = MQTT off |
| `config/units.json` | RP2040 `units_init` (optional) | additions / corrections to the unit table built into the RP2040 firmware (16 quantities, 78 units). Only `factor`, `offset`, `symbol` and `si_base` (new quantities) are read; entries identical to the builtin table are not stored, overrides are logged (`[UNITS] ... overrides ...`). Conversion: base = value × factor + offset. Delete the file if nothing needs to be added |
| `config/lang/<lang>/units.json` | not read yet (reserved for localized unit names in the GUI) | localized unit names |
| `decoder/<name>.json` | RP2040 `lorasensors_get_or_load_decoder` (RAM cache), `data_storage_load_decoder` | `format` (`IPSO_TLV`, `PACKED_BINARY`), `fport_config` (encryption), `commands` (downlinks), `reactions`, `mappings[]` (channel/type → `key`, `unit`, `datatype`, `context`: `measurement` / `diagnostic` / `history`) |
| `devices/<name>.json` | RP2040 `lorasensors_load_all` (devices, thresholds, virtual decoders, streamed to ESP32 via `0x3F`), `data_storage_load_devices` (archive channels) | see schema below |
| `gui/media_standards.json` | ESP32 `media_standards_load_from_sd()` (after config push, together with the language files); `cistern_view` applies `media_get_color(device.medium)` as main color of its graphs | `media_types.<id>`: name EN/DE, `color_hex` (user-defined main color), `unit_default` |
| `gui/lang/<lang>/common.json` | ESP32 `gui_i18n_reload_from_sd` via `0x38` | GUI string overrides |
| `gui/lang/<lang>/alarm_config.json` | ESP32 `gui_i18n` / `alarm_model` | alarm texts, HW button behavior (colors moved to `gui/theme.json`) |
| `gui/theme.json` | ESP32 `gui_theme_load_from_sd()` (together with the language files and media); built-in defaults if missing | `alarm.{danger,warning,info}`, `alarm_state.{incoming,going,muted,retriggered,acknowledged}` as `"0xRRGGBB"`. Language independent. After loading, every medium color closer than 120 (redmean distance) to the danger or warning color is reported in the log. |
| `alarm_history.bin` (root) | written by the RP2040 (`0xA9` chunks, staged as `alarm_history.tmp`, renamed after the last acknowledged chunk), streamed back with `0xAB` | binary alarm journal: `uint16 entry_size`, `uint16 count`, then `count` x `alarm_entry_t` (oldest first, max 50). Readable with an external tool. |

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
  "mqtt": {                              // optional, per key; only listed keys are published (OVERVIEW_DATAFLOW.md §7)
    "<key>": { "alarm": true },          // value + alarm state ("value": false = alarm state only)
    "<key2>": true,                      // value only
    "battery_soc": { "expire_after_sec": 172800 },   // optional HA expire_after override
    "rssi": {}, "snr": {}                // reception of this device at this SenseCAP (LoRa devices only)
  },
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

Thresholds: `direction` is mandatory (`"falling"` = alarm below the value, `"rising"` = alarm above); a threshold without a valid direction is skipped with a log message, the direction is never derived from the `id`. Percent values (`value_percent`, `hysteresis_percent`) refer to `limits[key]` (`val_min..val_max`), else to the tank depth. GUI edits rewrite exactly this entry in the device file (value, hysteresis, damping, severity, buzzer mode); `id`, `key` and `direction` are not editable.

File timestamps on the FAT32 partition: files written by the RP2040 (alarm journal, device JSON after threshold / offset edits) carry the real local time once the clock is valid (SdFat date callback, TZ from the global config); before that SdFat uses 1 January of the compile year.

MQTT keys: decoder keys, virtual keys, `rssi` and `snr`; at most 10 per device, key names 1..15 characters.
A listener SenseCAP (no keys) can publish the same sensor values; select each sensor key on one SenseCAP only.

`config/mqtt.json`:

```jsonc
{
  "enabled": true,                         // default true if the file exists
  "broker_uri": "mqtt://192.168.1.10:1883",// mqtt:// or mqtts://
  "username": "", "password": "",
  "client_id": "",                         // default: node id sensecap_xxxxxx
  "node_name": "SenseCAP Keller",          // HA device name of this SenseCAP (default "SenseCAP XXXXXX")
  "base_topic": "sensecap",
  "discovery": true, "discovery_prefix": "homeassistant",
  "retain_states": true,
  "local_interval_sec": 60,                // min. interval for onboard sensor values (5..3600)
  "keepalive_sec": 60,
  "ca_file": ""                            // mqtts:// only: PEM file on the SD card; empty = ESP-IDF CA bundle
}
```

Archive keys may be decoder keys (`depth`, `battery_soc`) or virtual keys (`volume`, `flowrate`).
Retention: number of entries kept per level; `0` disables the level. `weeks`, `months` and `years` are parsed but not aggregated yet (planned NTTW phase).

Retention limits (code-verified, `data_storage.cpp` parse / `compute_keep_entries`, `archive_store`):

| Level | Hard maximum | Time span at the maximum | Disk at the maximum |
|---|---|---|---|
| `single` | 65535 (larger values are clamped, `uint16_t`) | 65535 × grid (300 s) ≈ 227 days | 9 B/record, 455 per 4 KB segment → 145 segments ≈ 580 KB |
| `hours` | 65535 | ≈ 7.5 years | 23 B/record, 178 per segment → 369 segments ≈ 1.5 MB |
| `days` | 65535 | ≈ 179 years | as hours |

* Neither RAM nor the LittleFS partition (1 GB, 262144 blocks) is a practical limit; RAM per channel is
  independent of the retention.
* What a larger retention costs is **chart query time**: the trend query (LTTB) reads every part of the
  requested window from the finest level that still covers it (raw, then hour, then day). With
  `single` = 2016 (7 days) a 7-day window is read from ~2016 raw records (~5 segments); with 8928
  (31 days) a 31-day window reads ~20 segments (~80 KB) from the SD card. The cost is bounded by the
  window, not by the retention. The ESP32 waits at most `CHART_QUERY_TIMEOUT_MS` (2500 ms) per answer.
* Useful values for a 5-minute grid: 288 = 24 h, 2016 = 7 days, 8928 = 31 days.

Current limits of the engine (until the NTTW rework):
* Each level is aggregated from the next finer one. A disabled finer level that is needed as a source is kept as a small staging level: `raw` with 2 hours of buckets when hourly is active (at least the hold span interval + tolerance, so the newest real value survives for the cold-start restore even for hourly keys), `hour` with at least 26 entries when daily is active (covers 25-hour DST days).
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
