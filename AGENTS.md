# AGENTS.md - SenseCAP Indicator Cistern & Sensor System

Firmware for the dual-MCU **SenseCAP Indicator** (D1 / D1L / D1Pro): a resilient telemetry, display and
monitoring system for tanks and sensors (LoRaWAN sensors, LoRa P2P mesh, onboard I2C sensors, SD card archive).

Detailed, code-verified maps (read these before larger changes):

| Document | Content |
|---|---|
| [docs_extensions_n_changes/overview/OVERVIEW_ESP32.md](docs_extensions_n_changes/overview/OVERVIEW_ESP32.md) | ESP32 boot sequence, modules, widgets |
| [docs_extensions_n_changes/overview/OVERVIEW_RP2040.md](docs_extensions_n_changes/overview/OVERVIEW_RP2040.md) | RP2040 core split, files, data path |
| [docs_extensions_n_changes/overview/OVERVIEW_SDCARD.md](docs_extensions_n_changes/overview/OVERVIEW_SDCARD.md) | SD card layout, decoder vs. device JSON, `archive` schema |
| [docs_extensions_n_changes/overview/OVERVIEW_DATAFLOW.md](docs_extensions_n_changes/overview/OVERVIEW_DATAFLOW.md) | End-to-end flows, P2P frame types, complete UART packet table |
| [docs_extensions_n_changes/NEXT_STEPS.md](docs_extensions_n_changes/NEXT_STEPS.md) | **Start here in a new session:** uncommitted work, running verifications, next feature spec, backlog |

Keep these documents in sync when changing architecture, protocol or JSON schemas.

---

## 1. Architecture

```
┌──────────────────────────────── MicroSD (SPI, owned by RP2040) ───────────────────────────────┐
│ Partition 1 FAT32: /config, /decoder, /devices, /gui     │ Partition 2 LittleFS: /data archive │
└──────────────────────────────────────────────┬───────────────────────────────────────────────┘
                                               ▼
┌ RP2040 co-processor (PlatformIO, Arduino / Earle Philhower core) ─────────────────────────────┐
│ Core 0: I2C sensors (SCD41, SGP40, SHT41/AHT20, light), buzzer/alarm pattern state machine    │
│ Core 1: UART link, SD state machine (MBR, format, USB-MSC, hot reload), config parsing,       │
│         IPSO-TLV decoding, virtual decoders (per device), archive engine (data_storage)       │
└──────────────────────────────────────────────┬───────────────────────────────────────────────┘
                                               │ UART 921600 Bd, COBS (PacketSerial), XOR checksum
┌ ESP32-S3 main processor (ESP-IDF 5.x, LVGL 9, FreeRTOS) ──────────────────────────────────────┐
│ SX1262: LoRaWAN decryption (master) + P2P mesh (retransmit, time broadcast, claim)            │
│ Time: SNTP / P2P time / last saved time on LittleFS; headless alarm evaluator; LVGL GUI       │
└───────────────────────────────────────────────────────────────────────────────────────────────┘
```

* **Master node:** has the sensor session keys, decrypts uplinks, forwards the decrypted frame to the RP2040 (`0x36`) and retransmits it via P2P (type `0x02`, mesh AES-CTR, piggybacked UTC).
* **Listener node:** no keys (`app_skey` all zero). Ignores direct sensor uplinks and only uses the master's P2P retransmit.
* **GUI:** `main/display/cistern_view.c` is today's static start page. Target: pages generated from JSON on SD (`/gui/pages/*.json`) using the generic widgets in `main/gui/`. Settings, Wi-Fi, LoRa diagnostics, SD management, alarm list and display settings stay builtin.

---

## 2. Directory Structure

```
├── main/                  ESP32-S3 firmware (ESP-IDF component, all module folders globbed in CMakeLists.txt)
│   ├── alarm/             alarm evaluator (thresholds), alarm model + list view
│   ├── display/           cistern_view (static start page), backlight / sleep
│   ├── gui/               reusable LVGL 9 widgets, header/footer, i18n (builtin EN + DE)
│   ├── lora/              SX1262 model, LoRaWAN crypto, P2P mesh, LoRa diagnostics page
│   ├── rp2040/            UART link + chart_query_scheduler
│   └── ...                btn, cmd, mqtt (MQTT + HA discovery), nav, sdcard, sensor, settings, storage, util, wifi
├── components/
│   ├── bsp/               board support (Seeed/Espressif BSP): ST7701S panel init, RGB timings, IO expander
│   └── esp_lcd/           patched copy of ESP-IDF v5.4 esp_lcd (RGB restart fix), see esp_lcd/CISTERN_PATCH.md
├── rp2040/                RP2040 firmware (PlatformIO): code in rp2040/src and rp2040/include
├── sdcontent/             reference content of SD partition 1 (tracked in git, dummy keys only)
├── docs_extensions_n_changes/   architecture docs (overview/) and the handoff NEXT_STEPS.md
├── SDbackup/              PRIVATE backup of a test unit SD card - never read into commits, never modify
└── tmp/                   private experiments - never modify, never commit
```

`main/ARCHITECTURE_PRE-FORK.md` describes the upstream Seeed firmware before the fork; treat it as historical.

---

## 3. Rules (mandatory)

### Language & style
* Code identifiers, comments and log messages: **English**.
* Existing comments in other languages (German, Chinese) are translated when touched; their content is kept. Never delete existing comments unless the logic they describe changes.
* Builtin GUI languages are English and German: every new user-visible string needs entries in `main/gui/i18n/gui_i18n_builtin_en.c` **and** `_de.c`. Further languages come from `/gui/lang/<lang>/*.json` on SD.
* C: `snake_case` functions/variables, `SCREAMING_SNAKE_CASE` macros/enums. C++: `camelCase` methods, `PascalCase` types.
* No stubs, placeholder TODOs or mock returns. Deliver complete functions.

### SD card JSON roles
* `decoder/*.json` = **hardware abstraction** of a sensor model, shared by every device using that hardware. Only payload decoding belongs here (mappings, datatypes, fport/encryption, commands, reactions).
* `devices/*.json` = **one measuring installation**: address, keys, decoder reference, offsets, `virtual_decoder`, `thresholds`, `limits`, `display_config`, `archive`. Everything application-specific goes here, never into a decoder.
* Everything derived from a device (virtual decoder values, archive channels, alarms, telemetry) is always bound to its `dev_addr`. Identical keys of different devices must never mix.

### Storage & time safety
* Never store real LoRaWAN keys in `sdcontent/`; use dummy keys.
* Archive writes go through `data_storage` → `archive_store` (segmented binary files). When LittleFS is unavailable records are queued in RAM and written later in small portions; never block on SPI when the card is removed, never block the UART loop with long flushes.
* RP2040 console output goes through `LogSerial` (non-blocking); never use `Serial.print*` directly (a terminal that stops reading would stall both cores).
* There is no hardware RTC. Archive writes and time-dependent logic require a synchronized or recovered time (`data_storage_is_time_valid()` on the RP2040, `alarm_time_is_valid()` on the ESP32). Never work with the epoch start time after a reboot.
* RP2040 RAM is limited (~150 KB of 264 KB statically used). Parse SD JSON with ArduinoJson **filters** and small documents; avoid large heap allocations and new big static tables.

### LoRa & mesh
* No blind retransmits: nodes without valid keys never retransmit unparsed frames.
* Never call `lorawan_cipher_payload` with an all-zero key.
* P2P retransmit (type `0x02`): apply the piggybacked UTC **before** the duplicate check.

### Git
* Never stage or commit `SDbackup/` or `tmp/`.
* Commit only when asked.

---

## 4. Build

* ESP32: ESP-IDF 5.x from the repository root (`idf.py build`).
* RP2040: `cd rp2040 && pio run` (PlatformIO env `indicator_rp2040`).

---

## 5. UART protocol

The complete, code-verified packet table lives in [OVERVIEW_DATAFLOW.md](docs_extensions_n_changes/overview/OVERVIEW_DATAFLOW.md) §6.
Packet IDs are defined twice and must stay identical: `main/rp2040/rp2040.h` and `rp2040/include/indicator_rp2040.hpp`.
