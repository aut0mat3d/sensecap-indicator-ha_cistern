# End-to-End Data Flow & UART Protocol

> Generated 2026-11-17. Packet IDs verified against `main/rp2040/rp2040.h` and `rp2040/include/indicator_rp2040.hpp` (both identical).

## 1. Boot & configuration

```
RP2040 Core 1: SD_CHECK ─► LOAD_CFG: lorasensors_load_all() + data_storage_load_devices()
   ├─ 0x35 PUSH_LORA_CFG  (globals, RF, mesh, device count)   ─► ESP32 lora_model_on_rp2040_cfg_received()
   └─ 0x3F PUSH_LORA_DEV  (one device per packet, ≥ 15 ms apart, only when the link queue has room) ─► ESP32 lora_model_on_rp2040_device_received()
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
scheduler (bucket boundary crossed) ─► 0x3B GET_CHART_DATA ─► RP2040 data_storage_query_chart()   (mode GRID)
                                                              └► RP2040 data_storage_query_lttb()   (mode LTTB)
RP2040 ─► 0x3C RES_CHART_DATA (req_id, status, step, count, chart_data_point_t[]) ─► chart callback ─► widget
```
`cistern_view.c` subscribes 3 zones for `depth`: 7 × 86400 s, 12 × 7200 s, 11 × 300 s (zone 3 = live value).
The scheduler has 24 subscription slots of up to 100 points each (`CHART_SUB_MAX_POINTS`), held in PSRAM.

**Query modes (byte 45 of `0x3B`):**

* `CHART_QUERY_MODE_GRID` (0): one value per grid step, right-aligned, empty steps as `DATA_FLAG_NO_DATA` (bar charts).
* `CHART_QUERY_MODE_LTTB` (1): Largest-Triangle-Three-Buckets reduction for point/line charts (`trend_graph`).
  The window is split into `CHART_LTTB_BUCKETS(pts)` = 2/3 · pts time buckets (66 for 100 points, so gap
  markers still fit); `step` in the answer is the bucket width. Day, hour and raw levels are joined
  chronologically (day records before the first hour record, hour records before the first raw record).
  Two streaming passes over the archive (bucket statistics, then selection), no record buffer:
  one point per non-empty bucket at its real timestamp, the newest value is always kept, a
  `DATA_FLAG_NO_DATA` marker separates records further apart than the expected spacing of their level
  (raw: update interval + tolerance, hour 1.5 h, day 1.5 d, never less than one bucket). Alarm flags come
  from the bucket minimum / maximum as in GRID mode; `DATA_FLAG_ESTIMATED` from the selected record.

**Alarm coloring of history bars:** the archive stores values and quality flags only, no alarm state.
Each query carries the current alarm levels of the key and their severities; the RP2040 marks a
bucket `DATA_FLAG_ALARM_WARN` or `DATA_FLAG_ALARM_DANGER` when any value behind it crossed a level
(bucket minimum for the falling level, maximum for the rising level, so episodes inside an hour or day
are visible). Hysteresis and damping are not applied here; the alarm journal records what actually
alarmed. The view re-queries the history when the device config arrives or a threshold is changed.
Only one query is in flight at a time; after each answer the scheduler waits `CHART_QUERY_GAP_MS` (100 ms) before the next one, so pages with many charts fill gradually instead of in one burst.

## 5a. Link flow control (both directions)

Rule: a page may take longer to fill, but neither MCU may stall. No side sends large blobs at once, no side blocks while holding a shared lock.

**RP2040 → ESP32** (`rp2040/src/uart_link.cpp`)
* Every packet goes through `uart_link_send()` into a 6 KB ring buffer (both cores). Only core 1 transmits, in `uart_link_process()`.
* Pacing: after a packet of *n* bytes the next one waits `max(2 ms, n / 32 B/ms)` (≈ 32 KB/s, about a third of the line rate). A 40 B telemetry packet is followed by 2 ms of silence, a 1 KB file packet by 32 ms.
* Large or many packets are produced incrementally: the alarm history stream sends one entry per loop pass when `uart_link_can_send()` allows it (no `delay()`), device configs (`0x3F`) are streamed only when the whole chunk fits.
* Maximum packet size `UART_LINK_MAX_PACKET` = 1536 B (below the 2048 B receive buffers on both sides; checked at compile time for `0x3F`).

**ESP32 → RP2040**
* The alarm journal is uploaded by the UART task as acknowledged chunks of 4 entries: chunk *k+1* is sent only after `0xAC RES_ALARM_SAVE` for chunk *k* (the RP2040 writes each chunk to SD first). Missing or negative acks restart the upload from chunk 0 (3 attempts), then the journal stays dirty and is retried later.
* Chart queries: one in flight (see above). Threshold / offset writes: one request, waiting for its response.

**ESP32 receive path** (`main/rp2040/rp2040.c`)
* The UART task (and with it the UART ISR) is pinned to core 1 at priority 7, above LVGL (6): the RGB bounce-buffer ISR on core 0 copies 28.8 KB from PSRAM per interrupt and starved the UART ISR under render load. The RX FIFO interrupt fires at 64 of 128 bytes (default 120 left only ~87 µs). Slow work is deferred out of the task (the SX1262 init for `0x35` runs in the LoRa task). The task never takes the LVGL lock: views pick up model changes with LVGL timers (`alarm_view_notify_change()`, `gui_header_set_alarm_state()` only set flags).
* Events are posted with a timeout (telemetry 50 ms, control events 500 ms) instead of `portMAX_DELAY`; when the event loop (queue 32) stays congested, telemetry is dropped and counted rather than stalling reception.
* UART driver events are monitored: on a FIFO / ring buffer overflow the input is flushed, the COBS framer resynchronized and a warning logged.

**Diagnostics** (switchable): `UART_LINK_DIAG` (`rp2040/src/uart_link.hpp`), `RP2040_LINK_DIAG` (`main/rp2040/rp2040.h`), `LV_PORT_RENDER_DIAG` (`main/lv_port.h`). Every 30 s, only when there was traffic:
* RP2040 `[LINK]`: TX packets/bytes, drops, queue high-water mark, RX packets/bytes, overflows, slowest packet handler.
* ESP32 `[LINK]`: RX packets/bytes, decode errors, oversized fragments, overflows, line errors, slowest handler, event drops and longest post wait, TX packets/bytes, longest blocking write.
* ESP32 `[RENDER]`: rendered frames, average and maximum render time.

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
| `0x38` | CMD_READ_FILE | E→R | offset (uint32) + path; one chunk per request (stop-and-wait) |
| `0x39` | RES_FILE_DATA | R→E | status, chunk_len (uint16), total size (uint32), offset (uint32), data (max 512 B); the ESP32 requests the next offset until the file is complete |
| `0x3A` | CMD_SYNC_TIME | E→R | utc_ts(4) + flags(1) |
| `0x3B` | CMD_GET_CHART_DATA | E→R | req_id, dev_addr, key, start, end, step, pts, alarm_min, alarm_max, alarm flags (byte 44, `CHART_ALARM_MIN/MAX_IS_WARNING`), query mode (byte 45, `CHART_QUERY_MODE_GRID/LTTB`, §5) |
| `0x3C` | RES_CHART_DATA | R→E | req_id, status, step, count, points[] |
| `0x3D` | CMD_GET_TIME | E→R | – |
| `0x3E` | RES_TIME | R→E | last_utc(4) |
| `0x3F` | PUSH_LORA_DEV | R→E | one device config |
| `0x40` | CMD_SET_THRESHOLD | E→R | patch threshold in device JSON on SD |
| `0x41` | RES_SET_THRESHOLD | R→E | 1 = OK, 0 = ERR |
| `0x42` | CMD_SET_OFFSET | E→R | patch offset in device JSON on SD |
| `0x43` | RES_SET_OFFSET | R→E | 1 = OK, 0 = ERR |
| `0x44` | CMD_GET_LAST_VALUE | E→R | dev_addr(4) + key[16]: newest archived value of a key |
| `0x45` | RES_LAST_VALUE | R→E | dev_addr, key[16], status (1 = value), value (float), timestamp, expected interval (s), unit[8]; posted on the ESP32 as `VIEW_EVENT_ARCHIVE_LAST_VALUE` |
| `0xA0` | CMD_COLLECT_INTERVAL | E→R | defined, not handled |
| `0xA1/0xA2` | CMD_BEEP_ON/OFF | E→R | legacy click |
| `0xA3/0xA4` | CMD_SHUTDOWN / POWER_ON | E→R | sensor power |
| `0xA5` | CMD_RESCAN_GROVE | E→R | defined, not handled |
| `0xA6` | CMD_ALARM_SET | E→R | tON_ms(2) + tOFF_ms(2) |
| `0xA7` | CMD_ALARM_MUTE | E→R | mute_sec(4), 0 = indefinite |
| `0xA8` | CMD_ALARM_REARM | E→R | – |
| `0xA9` | CMD_ALARM_SAVE | E→R | chunk_idx, total, count (≤ 4), entries[]; next chunk only after `0xAC` |
| `0xAA` | CMD_ALARM_LOAD | E→R | – |
| `0xAB` | RES_ALARM_LOAD | R→E | chunk_idx, total, count, entries[] (one entry per packet, paced) |
| `0xAC` | RES_ALARM_SAVE | R→E | chunk_idx, status (1 = written to SD); ack per `0xA9` chunk, flow control |
| `0xBB` | LORA_TELEMETRY | R→E | DevAddr, FCnt, type, context, key, unit, value |
| `0xBC` | LOCAL_TELEMETRY | R→E | onboard/Grove sensors |

Free IDs for planned extensions: `0x46`–`0x47` (CLEAR_CHANNEL_HISTORY per NTTW roadmap).

## 7. Differences found vs. AGENTS.md

* AGENTS.md §6 lists only 15 packets; the table above is complete (0x20–0x22, 0x32–0x34, 0x38/0x39, 0x3F–0x43, 0xA9–0xAB are missing there).
* P2P type `0x01` (CLAIM) is not mentioned in AGENTS.md.
* RP2040 code lives in `rp2040/src` and `rp2040/include`, not `rp2040/main`.
* Virtual decoder outputs (`volume`, `flowrate`) are sent to the ESP32 but were not archived (fixed with the `archive` block, see OVERVIEW_SDCARD.md).
* `sdcontent/gui/media_standards.json` was not loaded by any code (fixed: loaded by `media_standards_load_from_sd()`).
* Virtual decoders were not scoped per device (fixed: `virtual_decoder_config_t.dev_addr`, blackboard keyed by device + key).
* `main/AGENTS.md` and `main/ARCHITECTURE_PRE-FORK.md` describe the upstream Seeed fork (partly Chinese) and are outdated relative to the root AGENTS.md.
