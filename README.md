# SenseCAP Indicator — Cistern & Sensor Firmware

Firmware for the dual-MCU [Seeed Studio SenseCAP Indicator D1](https://www.seeedstudio.com/SenseCAP-Indicator-D1-p-5643.html)
that turns it into a self-contained, unattended monitoring display for tanks and sensors:
LoRaWAN sensors are decrypted on the device, values are decoded by SD-card-defined decoders,
archived on the SD card with a data quality model, shown as live values and history charts,
and checked against alarm thresholds with buzzer and alarm journal.

**Status:** early beta — in field test on real hardware.

Origin: forked from [Love4yzp/sensecap-indicator-ha](https://github.com/Love4yzp/sensecap-indicator-ha)
(Seeed's Home Assistant MQTT panel, v1.1.0). Little of that firmware remains; the MQTT/Home Assistant
part is still in the tree but not yet adapted (see [Home Assistant / MQTT](#home-assistant--mqtt)).

---

## Table of Contents

- [Features](#features)
- [Architecture](#architecture)
- [SD card](#sd-card)
- [Build & Flash](#build--flash)
- [Toolchain versions](#toolchain-versions)
- [Display: critical settings and the local esp_lcd patch](#display-critical-settings-and-the-local-esp_lcd-patch)
- [Console commands](#console-commands)
- [Diagnostics](#diagnostics)
- [Home Assistant / MQTT](#home-assistant--mqtt)
- [Documentation](#documentation)
- [Version](#version)

---

## Features

- [x] **LoRaWAN reception without gateway**: the SX1262 receives sensor uplinks, the master node decrypts them with the sensor's session keys (MIC check + AES)
- [x] **LoRa P2P mesh**: master retransmits decrypted frames (AES-CTR, piggybacked UTC) to listener displays; P2P time broadcast and claim frames
- [x] **SD-card-driven configuration**: decoders (hardware abstraction per sensor model) and devices (one measuring installation each) as JSON, hot reload on card change
- [x] **Decoding on the RP2040**: IPSO-TLV / packed binary mappings, offsets, limits, per-device virtual decoders (tank volume for rectangle / cylinders / frustum / LUT, flow rate, dew point, …)
- [x] **Archive** on a LittleFS partition of the SD card: append-only binary segments per device / key / level (raw grid, hourly, daily), retention per level, RAM queue while the card is missing, cold-start restore
- [x] **Data quality model**: hold values within the expected interval, limited substitute values, coverage-based hourly/daily aggregates (solid / degraded / gap)
- [x] **Charts**: multi-zone history bar graph (7 days / 24 h / 55 min + live), serialized chart queries
- [x] **Alarms**: percent or absolute thresholds with direction, hysteresis and damping, editable on the device and written back to the device JSON; buzzer patterns, mute, acknowledge, persistent alarm journal
- [x] **Onboard sensors**: SCD41 (CO₂), SGP40 (VOC), SHT41 / AHT20, ambient light
- [x] **Offline time**: SNTP, P2P time, or last known time from the SD card; no time-dependent work before the time is valid
- [x] **Robust MCU link**: paced, chunked and acknowledged UART protocol (see [OVERVIEW_DATAFLOW.md §5a](docs_extensions_n_changes/overview/OVERVIEW_DATAFLOW.md))
- [x] GUI languages: English and German built in, more from the SD card
- [ ] Pages generated from JSON on the SD card (today the start page is the static cistern view)
- [ ] Export of archive data as CSV, weekly / monthly / yearly aggregation
- [ ] MQTT / Home Assistant integration for the new data model

---

## Architecture

```
┌──────────────────────────────── MicroSD (SPI, owned by RP2040) ──────────────────────────────┐
│ Partition 1 FAT32: /config, /decoder, /devices, /gui     │ Partition 2 LittleFS: /data archive│
└──────────────────────────────────────────────┬──────────────────────────────────────────────┘
                                               ▼
┌ RP2040 (PlatformIO, Arduino-Pico) ──────────────────────────────────────────────────────────┐
│ Core 0: I2C sensors, buzzer / alarm patterns                                                 │
│ Core 1: UART link (paced TX queue), SD state machine, config parsing, decoding,              │
│         virtual decoders, archive engine                                                     │
└──────────────────────────────────────────────┬──────────────────────────────────────────────┘
                                               │ UART 921600 Bd, COBS, XOR checksum
┌ ESP32-S3 (ESP-IDF 5.4, LVGL 9, FreeRTOS) ─────────────────────────────────────────────────────┐
│ SX1262 LoRaWAN decryption + P2P mesh, time service, alarm evaluator, LVGL GUI                │
│ Core 0: RGB LCD bounce buffer ISR    Core 1: LVGL rendering, UART task + ISR    (rest: free) │
└───────────────────────────────────────────────────────────────────────────────────────────────┘
```

The detailed, code-verified maps are in [`docs_extensions_n_changes/overview/`](docs_extensions_n_changes/overview/).
Rules for contributors (human and AI) are in [`AGENTS.md`](AGENTS.md).

---

## SD card

The SD card carries the whole application configuration. A reference set (dummy keys only) is in
[`sdcontent/`](sdcontent/); the layout, the JSON schemas and the split between `decoder/*.json`
(sensor hardware) and `devices/*.json` (one installation: keys, offsets, virtual decoders,
thresholds, limits, archive) are described in [OVERVIEW_SDCARD.md](docs_extensions_n_changes/overview/OVERVIEW_SDCARD.md).

The card is formatted by the device (MBR with FAT32 + LittleFS partition); it can be exposed to a PC
as USB mass storage from the settings.

---

## Build & Flash

Both MCUs must run matching firmware: the UART protocol changes between versions.

### ESP32-S3

**Prerequisites:** ESP-IDF **v5.4** with `IDF_PATH` exported ([setup](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/get-started/)).

```bash
./dev build          # idf.py build
./dev flash          # auto-detects port
./dev monitor        # Ctrl-] to exit
./dev fullclean      # reset build directory
```

or manually:

```bash
. "$IDF_PATH/export.sh"
idf.py build
idf.py -p /dev/ttyUSB0 -b 460800 flash monitor
```

After adding or removing a project component (e.g. `components/esp_lcd`), run `idf.py reconfigure` once.

### RP2040

```bash
./dev rp2040 build        # or: cd rp2040 && pio run
./dev rp2040 upload
./dev rp2040 monitor
```

---

## Toolchain versions

| Component | Version | Notes |
|-----------|---------|-------|
| ESP-IDF | **v5.4** (tag, commit `67c1de1`) | `esp_lcd` is overridden locally, see below |
| LVGL | 9.6.0 | managed component |
| esp_lvgl_port | 2.9.0 | managed component |
| RP2040 core | Arduino-Pico (Earle Philhower) via PlatformIO | env `indicator_rp2040` |
| ArduinoJson (RP2040) | 6.21 | filtered parsing to save RAM |

---

## Display: critical settings and the local esp_lcd patch

The 480×480 ST7701S panel has no frame memory; the ESP32-S3 streams every frame over the 16-bit RGB
interface from two frame buffers in PSRAM through internal bounce buffers.

### Recurring 16 px display shift — fixed

For a long time (also with the unmodified Seeed firmware, on several devices) the picture
occasionally jumped 16 px to the left, wrapping around to the right edge, and stayed there until
reboot. Cause: under PSRAM load the bounce buffer interrupt is occasionally late, the LCD FIFO
underruns, and the ESP-IDF v5.4 RGB driver recovers with an ESP32-S3-specific "restart link" that
skips 17 pixels assumed to be still in the LCD FIFO. After an underrun the FIFO is empty, so the
stream ends up permanently shifted.

The project therefore contains a **patched copy of the ESP-IDF v5.4 `esp_lcd` component** in
[`components/esp_lcd/`](components/esp_lcd/): the recovery performs a full LCD engine restart (at most
one disturbed frame), plus two upstream fixes from the same function. Details, evidence and the
update procedure: [components/esp_lcd/CISTERN_PATCH.md](components/esp_lcd/CISTERN_PATCH.md).

Reported upstream: [espressif/esp-idf#19452](https://github.com/espressif/esp-idf/issues/19452).
Related: [#19070](https://github.com/espressif/esp-idf/issues/19070) (bounce buffer index resync),
[#19329](https://github.com/espressif/esp-idf/pull/19329) (restart loop after a dropped EOF).

### Settings that must not be changed without testing on the device

| Setting | Value | Why |
|---------|-------|-----|
| `CONFIG_SPIRAM_SPEED_80M` | 80 MHz octal | 120 MHz octal PSRAM is experimental in ESP-IDF (timing point chosen at power-on, accesses can fail after ~20 °C temperature change); the devices sit on window sills |
| `CONFIG_ESPTOOLPY_FLASHFREQ_80M` | 80 MHz | flash shares the MSPI core clock with PSRAM |
| `CONFIG_LCD_LVGL_DIRECT_MODE` | direct mode, 2 PSRAM frame buffers | LVGL redraws only changed areas; full refresh re-rendered 480×480 px for every change |
| `CONFIG_LCD_EVB_SCREEN_FREQ` | 12 MHz pixel clock | lowers the bounce buffer copy load |
| `bounce_buffer_size_px` | `480 * 30` (`bsp_lcd.c`) | ~1.4 ms slack for the bounce buffer ISR |
| `PCLK_ACTIVE_NEG` | 1 (`sensecap_indicator_board.c`) | 0 caused pixel noise and jumping lines on our panels |
| Panel init order | reset (IO expander IO0_5) + ST7701S init **before** the RGB stream starts | defined panel state after warm and cold starts |
| `CONFIG_LV_USE_CLIB_MALLOC` | y | LVGL 9 must allocate from the ESP heap; the built-in 64 KB pool live-locks the render task |
| `CONFIG_LCD_RGB_RESTART_IN_VSYNC` | off | would restart (now fully) on every frame |

`scripts/test_lcd_rgb_config.py` checks several of these settings.

Hardware note: the panel's 3-wire SPI (GPIO41 SCK / GPIO48 SDA, CS on the IO expander) shares
GPIO41/48 with the SX1262 SPI bus. The panel may only be (re)initialized before the radio's SPI bus
is set up (at boot).

---

## Console commands

Serial console on the ESP32-S3 USB port (prompt `Indicator>`):

| Command | Description |
|---------|-------------|
| `telem_inject <dev_hex> <key> <val> [unit]` | Inject a telemetry value into the event loop and UI |
| `lora_inject <dev_hex> [fport] <hex_payload>` | Inject a decrypted LoRa payload into the RP2040 decoder cascade |
| `chart_poll <dev_hex> <key> <zone>` | Trigger a history query through the chart scheduler |
| `alarm_mute [seconds]` | Mute the buzzer (default 7200 s, 0 = indefinite) |
| `alarm_ack` | Acknowledge all active alarms |
| `lcd_restart` | Diagnostics: driver restart of the RGB stream (GDMA/FIFO) at the next VSYNC |
| `lcd_rgb_reinit` | Diagnostics: full restart of the RGB LCD engine |
| `haconfig`, `mqtthelp`, `setmqtt …` | MQTT configuration (from the original firmware) |

---

## Diagnostics

Every 30 s (only when there was activity), switchable by defines:

| Log line | MCU | Define | Content |
|----------|-----|--------|---------|
| `[LINK]` | RP2040 | `UART_LINK_DIAG` (`rp2040/src/uart_link.hpp`) | TX/RX packets and bytes, queue high-water mark, drops, overflows, slowest packet handler |
| `[LINK]` | ESP32 | `RP2040_LINK_DIAG` (`main/rp2040/rp2040.h`) | RX/TX, decode errors, UART overflows, slowest handler, event drops |
| `[RENDER]` | ESP32 | `LV_PORT_RENDER_DIAG` (`main/lv_port.h`) | rendered frames, render time avg/max, total RGB stream restarts, longest gap between touch reads while pressed, longest touch read, longest LVGL lock hold by another task (with task name) |

A growing "RGB restarts total" means bounce buffer underruns happened (each is now recovered automatically).

---

## Home Assistant / MQTT

The MQTT client, the console commands and the Home Assistant examples in
[`examples/homeassistant/`](examples/homeassistant/) come from the original firmware (topics
`indicator/sensor`, `indicator/switch/set`, `indicator/switch/state`). They are not yet connected to
the new device / archive data model; an MQTT interface for the decoded and virtual values is planned.

---

## Documentation

| Document | Content |
|----------|---------|
| [AGENTS.md](AGENTS.md) | Rules and architecture summary for contributors |
| [OVERVIEW_ESP32.md](docs_extensions_n_changes/overview/OVERVIEW_ESP32.md) | ESP32 boot sequence, modules, widgets |
| [OVERVIEW_RP2040.md](docs_extensions_n_changes/overview/OVERVIEW_RP2040.md) | RP2040 core split, files, data path |
| [OVERVIEW_SDCARD.md](docs_extensions_n_changes/overview/OVERVIEW_SDCARD.md) | SD card layout, JSON schemas, archive |
| [OVERVIEW_DATAFLOW.md](docs_extensions_n_changes/overview/OVERVIEW_DATAFLOW.md) | End-to-end flows, P2P frames, UART packet table, flow control |
| [components/esp_lcd/CISTERN_PATCH.md](components/esp_lcd/CISTERN_PATCH.md) | Local ESP-IDF RGB driver patch |

---

## Version

| Component | Version |
|-----------|---------|
| Firmware (ESP32-S3 + RP2040) | early beta (branch `feat/binary-archive-device-config`) |
| Based on | Love4yzp/sensecap-indicator-ha v1.1.0 |

License: MIT (see [LICENSE](LICENSE)).
