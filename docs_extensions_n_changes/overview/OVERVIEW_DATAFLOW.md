# End-to-End Data Flow & UART Protocol

> Generated 2026-11-17. Packet IDs verified against `main/rp2040/rp2040.h` and `rp2040/include/indicator_rp2040.hpp` (both identical).

## 1. Boot & configuration

```
RP2040 Core 1: SD_CHECK ─► LOAD_CFG: lorasensors_load_all() + data_storage_load_devices()
   ├─ 0x35 PUSH_LORA_CFG  (globals, RF, mesh, device count)   ─► ESP32 lora_model_on_rp2040_cfg_received()
   └─ 0x3F PUSH_LORA_DEV  (one device per packet, 15 ms pacing) ─► ESP32 lora_model_on_rp2040_device_received()
ESP32 → 0x3D GET_TIME → RP2040 → 0x3E RES_TIME (last saved UTC, offline clock recovery)
ESP32 → 0x37 SET_TIME_CFG (POSIX TZ) / 0x3A SYNC_TIME (after NTP or P2P time)
```
On SD insert or after USB-MSC, `sd_card_needs_config_reload()` triggers the same sequence again (RELOAD_CFG).

## 2. LoRa uplink (master node with keys)

```
SX1262 RX (ESP32 lora_model) ─► duplicate cache ─► MIC/AES decrypt with app_skey/nwk_skey
   ├─ 0x36 LORA_RAW_FRAME (decrypted) ─► RP2040 lorasensors_process_raw_frame()
   │       ├─ decode mappings ─► 0xBB LORA_TELEMETRY per key ─► ESP32 VIEW_EVENT_LORA_TELEMETRY
   │       ├─ data_storage_add_point()       (archive)
   │       └─ virtual_decoder ─► 0xBB for volume / flowrate / ...
   └─ P2P RETRANSMIT (type 0x02, mesh AES-CTR, piggybacked UTC) to listener nodes
```
Listener nodes (no keys) ignore direct sensor uplinks and only accept the master's P2P retransmit.

## 3. P2P mesh frame types (first bytes: sync word, type)

| Type | Name | Purpose |
|---|---|---|
| `0x01` | P2P_CLAIM | Display claims a device / RX window (6 bytes) |
| `0x02` | P2P_RETRANSMIT | Decrypted sensor payload + UTC timestamp (time is applied **before** the duplicate check) |
| `0x03` | P2P_TIME | Standalone UTC broadcast (8 bytes) |

## 4. Live telemetry on the ESP32

`VIEW_EVENT_LORA_TELEMETRY` consumers: `alarm/alarm_evaluator.c` (thresholds → `0xA6` buzzer), `display/cistern_view.c` (live widgets), `lora/lora_diag_view.c`, `cmd/cmd.c` (injects test values).

## 5. Chart archive queries

```
widget ─► chart_query_scheduler_subscribe(dev_addr, key, zone, step, points, ...)
scheduler (bucket boundary crossed) ─► 0x3B GET_CHART_DATA ─► RP2040 data_storage_query_chart()
RP2040 ─► 0x3C RES_CHART_DATA (req_id, status, step, count, chart_data_point_t[]) ─► chart callback ─► widget
```
`cistern_view.c` subscribes 3 zones for `depth`: 7 × 86400 s, 12 × 7200 s, 11 × 300 s (zone 3 = live value).

## 6. UART packet table (complete)

| ID | Name | Dir | Payload / meaning |
|---|---|---|---|
| `0x20` | CMD_GET_SYS_INFO | E→R | defined, not handled on RP2040 |
| `0x21` | RES_SYS_INFO | R→E | defined, unused |
| `0x22` | CMD_SET_ESP_MAC | E→R | 6 B MAC + 11 B expected SD label |
| `0x30` | CMD_SD_GET_INFO | E→R | – |
| `0x31` | RES_SD_INFO | R→E | present, usb_mounted, state, total_mb, label |
| `0x32` | CMD_SD_FORMAT | E→R | format_type (0 = LittleFS, 1 = full SD) + label[12] |
| `0x33` | RES_SD_FORMAT | R→E | success |
| `0x34` | CMD_USB_MSC_TOGGLE | E→R | enable |
| `0x35` | PUSH_LORA_CFG | R→E | globals + device count |
| `0x36` | CMD_LORA_RAW_FRAME | E→R | DevAddr(4) RSSI(2) SNR(1) Len(1) Payload |
| `0x37` | CMD_SET_TIME_CFG | E→R | posix_tz[64] + time_format[8] |
| `0x38` | CMD_READ_FILE | E→R | null-terminated path (FAT32) |
| `0x39` | RES_FILE_DATA | R→E | status + len(2) + data |
| `0x3A` | CMD_SYNC_TIME | E→R | utc_ts(4) + flags(1) |
| `0x3B` | CMD_GET_CHART_DATA | E→R | req_id, dev_addr, key, start, end, step, pts |
| `0x3C` | RES_CHART_DATA | R→E | req_id, status, step, count, points[] |
| `0x3D` | CMD_GET_TIME | E→R | – |
| `0x3E` | RES_TIME | R→E | last_utc(4) |
| `0x3F` | PUSH_LORA_DEV | R→E | one device config |
| `0x40` | CMD_SET_THRESHOLD | E→R | patch threshold in device JSON on SD |
| `0x41` | RES_SET_THRESHOLD | R→E | 1 = OK, 0 = ERR |
| `0x42` | CMD_SET_OFFSET | E→R | patch offset in device JSON on SD |
| `0x43` | RES_SET_OFFSET | R→E | 1 = OK, 0 = ERR |
| `0xA0` | CMD_COLLECT_INTERVAL | E→R | defined, not handled |
| `0xA1/0xA2` | CMD_BEEP_ON/OFF | E→R | legacy click |
| `0xA3/0xA4` | CMD_SHUTDOWN / POWER_ON | E→R | sensor power |
| `0xA5` | CMD_RESCAN_GROVE | E→R | defined, not handled |
| `0xA6` | CMD_ALARM_SET | E→R | tON_ms(2) + tOFF_ms(2) |
| `0xA7` | CMD_ALARM_MUTE | E→R | mute_sec(4), 0 = indefinite |
| `0xA8` | CMD_ALARM_REARM | E→R | – |
| `0xA9` | CMD_ALARM_SAVE | E→R | chunk_idx, total, count, entries[] |
| `0xAA` | CMD_ALARM_LOAD | E→R | – |
| `0xAB` | RES_ALARM_LOAD | R→E | chunk_idx, total, count, entries[] |
| `0xBB` | LORA_TELEMETRY | R→E | DevAddr, FCnt, type, context, key, unit, value |
| `0xBC` | LOCAL_TELEMETRY | R→E | onboard/Grove sensors |

Free IDs for planned extensions: `0x44`–`0x47` (GET_LAST_VALUE, CLEAR_CHANNEL_HISTORY per NTTW roadmap).

## 7. Differences found vs. AGENTS.md

* AGENTS.md §6 lists only 15 packets; the table above is complete (0x20–0x22, 0x32–0x34, 0x38/0x39, 0x3F–0x43, 0xA9–0xAB are missing there).
* P2P type `0x01` (CLAIM) is not mentioned in AGENTS.md.
* RP2040 code lives in `rp2040/src` and `rp2040/include`, not `rp2040/main`.
* Virtual decoder outputs (`volume`, `flowrate`) are sent to the ESP32 but were not archived (fixed with the `archive` block, see OVERVIEW_SDCARD.md).
* `sdcontent/gui/media_standards.json` was not loaded by any code (fixed: loaded by `media_standards_load_from_sd()`).
* Virtual decoders were not scoped per device (fixed: `virtual_decoder_config_t.dev_addr`, blackboard keyed by device + key).
* `main/AGENTS.md` and `main/ARCHITECTURE_PRE-FORK.md` describe the upstream Seeed fork (partly Chinese) and are outdated relative to the root AGENTS.md.
