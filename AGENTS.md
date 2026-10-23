# AGENTS.md - SenseCAP Indicator Cistern & Sensor System

## 1. Project Overview & Architecture

Firmware for the **Dual-MCU SenseCAP Indicator** (D1 / D1L / D1Pro) deployed as a resilient telemetry, display, and monitoring system[cite: 1, 15].

```
                     ┌──────────────────────────────────────────┐
                     │          MicroSD Card (SPI1)             │
                     ├────────────────────┬─────────────────────┤
                     │ Partition 1: FAT32 │ Partition 2: LittleFS│
                     │  Configs, Decoders,│  Rolling Archives:   │
                     │  GUI JSONs, Lang   │  roundtrip/hourly/  │
                     │  (/config, /gui)   │  daily.csv          │
                     └─────────┬──────────┴──────────┬──────────┘
                               │                     │
                               ▼                     ▼
┌────────────────────────────────────────────────────────────────────────┐
│ RP2040 Co-Processor (FreeRTOS / Earle Philhower Core)                   │
├────────────────────────────────────────────────────────────────────────┤
│ • Core 0: Real-time Grove I2C polling & autonomous Alarm Buzzer Engine │
│ • Core 1: SPI1 MicroSD Driver (MBR Dual-Partitioning)                  │
│ • Storage: Write-Through RAM Buffer (64 KB) + LittleFS Aggregation      │
│ • Telemetry: IPSO-TLV Decoders, Virtual Decoders (volume/flowrate)     │
│ • Time: POSIX timezone support, local midnight daily aggregation       │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ UART Link (COBS / PacketSerial @ 921600 Baud)
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ ESP32-S3 Primary Processor (ESP-IDF v5.x)                              │
├────────────────────────────────────────────────────────────────────────┤
│ • SX1262 LoRa Radio: LoRaWAN 1.0.x decryption & LoRa P2P Mesh Engine   │
│ • Time Sync: NTP Client + P2P Time Broadcast (Master/Slave Mesh)       │
│ • Display: LVGL 9 rendering engine                                     │
│ • Telemetry Cache: In-memory RAM State Cache                           │
│ • Alarms: Headless background Alarm Evaluation Task                    │
│ • GUI Engine: Dynamic JSON-driven multi-page Tileview (-n .. 0 .. +n)   │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Current Implementation Status (October 2026)

### Verified & Active Features
* **Storage Engine (RP2040)**:
  * Non-blocking LittleFS on Partition 2[cite: 2, 21].
  * Write-Through RAM Buffer (64 KB) prevents SPI timeouts and system freezes during SD card removal[cite: 12, 13, 21].
  * Immediate unmount upon card ejection (`SD_PIN_DET != LOW`)[cite: 21].
  * Grid-aligned aggregation: Sub-hourly (configurable, e.g., 300 s), Hourly (3600 s), and Daily (local midnight aligned via POSIX `TZ` with 7-column CSV format including UTC offset)[cite: 19, 21].
* **LoRa P2P Mesh & Decryption (ESP32)**:
  * Master Nodes: Decrypt sensor payloads with LoRaWAN session keys (`app_skey`) and retransmit decrypted raw TLVs via P2P Mesh (AES-CTR with shared `mesh.aes_key`)[cite: 25, 28].
  * Slave / Listener Nodes: Run without sensor keys (`app_skey == 0`). Direct sensor uplinks are ignored (no zero-key garbage decryption, no duplicate cache poisoning, no blind retransmission). Await decrypted P2P retransmit from Master[cite: 27, 28].
  * Automatic Time-Mesh: Master broadcasts UTC time (standalone type `0x03` or piggybacked on retransmit type `0x02`). Slaves sync system time without Wi-Fi/NTP[cite: 28].
* **Virtual Decoder Engine (RP2040)**:
  * Dynamic mathematical calculations: Tank Volume (rectangle, vertical/horizontal cylinder, cone frustum, LUT), Derivative Gain (`cm/h`), and Flowrate (`m³/h`)[cite: 14, 27].
  * Unit conversion pipeline based on `/config/units.json`[cite: 14, 25].
* **LVGL 9 Widget System (ESP32)**:
  * `horiz_bargraph`: Horizontal multi-zone bar chart with dynamic scaling, transparent gaps (`DATA_FLAG_NO_DATA`), 50% opacity for estimated/partial buckets (`DATA_FLAG_ESTIMATED`), and red alarm bars (`DATA_FLAG_ALARM_DANGER`)[cite: 1, 22].
  * Standard 31-slot layout: Zone 0 (7 days @ 86400 s) + Zone 1 (12x 2h @ 7200 s) + Zone 2 (11x 5m @ 300 s) + Zone 3 (1 live value)[cite: 1, 28].
  * `vert_graph`: Vertical bar indicator for levels/percentages with threshold markings[cite: 16].
  * `text_widget`: Dynamic multi-binding text tile with automatic unit conversion[cite: 16].

---

## 3. Architecture Roadmap: Dynamic GUI & Telemetry Framework

The legacy hardcoded view (`main/display/cistern_view.c`) is being refactored into a declarative, JSON-driven GUI framework[cite: 16, 31].

### Core Architectural Layers

#### Layer 1: Headless Alarm Engine (24/7 Autonomous)
* Completely decoupled from UI rendering or active page state[cite: 27].
* Parses `thresholds` from `/devices/*.json` at boot[cite: 27].
* Evaluates all incoming telemetry (LoRa, P2P, I2C, MQTT) against thresholds, damping counters, and hysteresis[cite: 14, 27].
* Directly triggers buzzer patterns via UART packet `0xA6` (`CMD_ALARM_SET`) to the RP2040[cite: 11].
* Sets `DATA_FLAG_ALARM_DANGER` in the RAM State Cache[cite: 22].

#### Layer 2: Global RAM State Cache & Event Broker
* Flat in-memory table storing the latest state for up to 64 telemetry keys:
  * `key`, `value`, `unit`, `timestamp`, `flags`, `is_valid`.
* When a page opens, all live widgets populate immediately from this cache (< 1 ms, zero load delay)[cite: 1].
* An event broker routes incoming live values exclusively to widgets on the **currently visible page**[cite: 1].

#### Layer 3: Multi-Page Tileview (-n .. 0 .. +n)
* Implemented using LVGL 9 `lv_tileview` for horizontal swipe navigation.
* Lifecycle events:
  * `PAGE_ON_LEAVE`: Unsubscribes widgets from the Event Broker (0% CPU load for hidden pages).
  * `PAGE_ON_ENTER`:
    1. Subscribes active widgets to the Event Broker.
    2. Populates widgets instantly from the RAM State Cache[cite: 1].
    3. Triggers historical chart archive queries[cite: 1].

#### Layer 4: Smart Bucket Scheduler (Grid-Synchronized Querying)
* Solves UART bus saturation for high-frequency telemetry (e.g., MQTT updates every 10 s):
  * **Live updates**: Real-time values update text tiles, vertical bars, and Zone 3 (live bar) on every packet[cite: 1, 16].
  * **Archive queries**: Chart widgets query LittleFS archives **only when crossing bucket boundaries**:
    $$\text{bucket}_{\text{current}} = \text{timestamp} - (\text{timestamp} \pmod{\text{interval\_sec}})$$
    Archive requests are dispatched only if $\text{bucket}_{\text{current}} > \text{bucket}_{\text{last\_queried}}$[cite: 21].

#### Layer 5: Declarative JSON Page Builder
* Pages are defined in `/gui/pages/<page_name>.json`[cite: 2].
* Contains layout, widget types, coordinates, bindings, and zone configurations[cite: 1].
* Instantiated dynamically via `widget_factory_create()`[cite: 1].

---

## 4. Directory Structure & File Rules

```
├── main/                       # ESP32-S3 Firmware (ESP-IDF)
│   ├── display/                # Legacy views & screen handlers
│   ├── gui/                    # Reusable LVGL 9 widgets (horiz_bargraph, vert_graph, text_widget)
│   ├── lora/                   # SX1262 driver, LoRaWAN decryption, P2P Mesh engine
│   ├── rp2040/                 # UART host interface to RP2040
│   └── wifi/                   # Wi-Fi & SNTP service
├── rp2040/                     # RP2040 Co-Processor Firmware (PlatformIO)
│   ├── include/                # Umbrella headers & interface contracts
│   └── src/                    # Data storage, LittleFS driver, sensor drivers, virtual decoders
├── sdcontent/                  # MicroSD Card Partition 1 Reference Content (TRACKED IN GIT)
│   ├── config/                 # System globals, LoRa mesh, units configuration
│   ├── decoder/                # IPSO-TLV JSON decoders (e.g. EM500-SWL.json)
│   ├── devices/                # Device configurations & tank geometry JSONs
│   └── gui/                    # i18n dictionaries & media standards
├── docs_extensions_n_changes/  # Architectural specs & hardware documentation
├── SDbackup/                   # PRIVATE BACKUP FOLDER (STRICTLY GIT-IGNORED - DO NOT TOUCH)
└── tmp/                        # TEMPORARY EXPERIMENTAL CODE (STRICTLY GIT-IGNORED - DO NOT TOUCH)
```

---

## 5. Agent Behavioral & Development Rules (MANDATORY)

### Code Style & Formatting
* **Preserve existing comments**: Never delete or rewrite existing comments unless directly modifying that logic.
* **New comments in English**: Write all new comments, function headers, and log messages in English.
* **No stubs or placeholders**: Do not leave empty functions, placeholder TODOs, or mock returns.
* **Complete context**: Deliver full updated functions or clear, unambiguous diffs. Never provide code blocks stripped of their surrounding scope.
* **Naming conventions**:
  * C: `snake_case` for functions/variables, `SCREAMING_SNAKE_CASE` for macros/enums.
  * C++: `camelCase` for methods/members, `PascalCase` for classes/types.

### MicroSD & Storage Protection
* **NEVER stage or commit `SDbackup/` or `tmp/`**.
* **NEVER store real LoRaWAN AppSKey/NwkSKey in `sdcontent/`**. Use dummy keys (e.g., `4A6F696E...`) for reference files[cite: 25, 27].
* **SD Card Removal Safety**: Always verify that write routines check `littlefs_driver_is_mounted()` and fallback to `littlefs_buffer_append()` without triggering SPI hardware timeouts[cite: 13, 21].

### LoRa & Mesh Rules
* **No blind retransmits**: Nodes without valid keys must never retransmit unparsed frames[cite: 28].
* **No zero-key decryption**: Never call `lorawan_cipher_payload` with an all-zero key[cite: 28].
* **Time-first processing**: When receiving P2P retransmit frames (type `0x02`), extract and apply the piggybacked UTC timestamp **before** checking the duplicate cache[cite: 28].

---

## 6. UART Protocol Reference (ESP32 <-> RP2040 @ 921600 Baud)

| Packet Type | ID | Direction | Payload Structure / Description |
| :--- | :--- | :--- | :--- |
| `CMD_SD_GET_INFO` | `0x30` | ESP32 $\rightarrow$ RP2040 | Request SD card status, capacity, and label[cite: 11] |
| `RES_SD_INFO` | `0x31` | RP2040 $\rightarrow$ ESP32 | `present(1B)`, `msc(1B)`, `state(1B)`, `mb(4B)`, `label(11B)`[cite: 11] |
| `CMD_PUSH_LORA_CFG` | `0x35` | RP2040 $\rightarrow$ ESP32 | Global RF config + array of active devices from SD[cite: 11, 28] |
| `CMD_LORA_RAW_FRAME` | `0x36` | ESP32 $\rightarrow$ RP2040 | `DevAddr(4B)`, `RSSI(2B)`, `SNR(1B)`, `Len(1B)`, `Payload(NB)`[cite: 11] |
| `CMD_SET_TIME_CFG` | `0x37` | ESP32 $\rightarrow$ RP2040 | `posix_tz[64]`, `time_format[8]` (Applies `TZ` and writes SD)[cite: 11, 15] |
| `CMD_SYNC_TIME` | `0x3A` | ESP32 $\rightarrow$ RP2040 | `utc_ts(4B)`, `flags(1B)` (Syncs RP2040 RTC and saves LittleFS)[cite: 11, 15] |
| `CMD_GET_CHART_DATA` | `0x3B` | ESP32 $\rightarrow$ RP2040 | Zone archive query: `req_id`, `dev_addr`, `key`, `start`, `end`, `step`, `pts`[cite: 11, 15] |
| `RES_CHART_DATA` | `0x3C` | RP2040 $\rightarrow$ ESP32 | `req_id(1B)`, `status(1B)`, `step(4B)`, `count(2B)`, `chart_data_point_t[]`[cite: 11, 15] |
| `CMD_GET_TIME` | `0x3D` | ESP32 $\rightarrow$ RP2040 | Request last saved timestamp from LittleFS[cite: 11, 15] |
| `RES_TIME` | `0x3E` | RP2040 $\rightarrow$ ESP32 | `last_utc_ts(4B)` (Used for offline clock recovery)[cite: 11, 15] |
| `CMD_ALARM_SET` | `0xA6` | ESP32 $\rightarrow$ RP2040 | `tON_ms(2B)`, `tOFF_ms(2B)` (Hardware buzzer control)[cite: 11, 15] |
| `CMD_ALARM_MUTE` | `0xA7` | ESP32 $\rightarrow$ RP2040 | `mute_sec(4B)`[cite: 11, 15] |
| `CMD_ALARM_REARM` | `0xA8` | ESP32 $\rightarrow$ RP2040 | Rearm muted buzzer[cite: 11, 15] |
| `LORA_TELEMETRY` | `0xBB` | RP2040 $\rightarrow$ ESP32 | Dynamic decoded telemetry: `DevAddr`, `FCnt`, `Key`, `Unit`, `Value`[cite: 11, 14] |
| `LOCAL_TELEMETRY` | `0xBC` | RP2040 $\rightarrow$ ESP32 | Onboard Grove sensor readings (SCD41, SHT41, SGP40, Light)[cite: 11, 14] |
