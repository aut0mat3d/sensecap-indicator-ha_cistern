# RP2040 Co-Processor Overview (`rp2040/`)

> Generated 2026-11-17. PlatformIO, Arduino framework on the Earle Philhower core.
> Code lives in `rp2040/src/` and `rp2040/include/` (there is no `rp2040/main/`).
> Libraries: SdFat, LittleFS, PacketSerial (COBS, 2048 B buffer), ArduinoJson 6, Adafruit TinyUSB (USB-MSC), Sensirion drivers.

## Core split ([src/main.cpp](../../rp2040/src/main.cpp))

| Core | Entry | Work |
|---|---|---|
| Core 0 | `setup()` / `loop()` | SD + LittleFS buffer init, UART pins, I2C sensor power and detection, buzzer state machine (`buzzer_update`), sampling SCD41 / SGP40 / SHT41 (AHT20) / light → `lorasensors_dispatch_local()` |
| Core 1 | `setup1()` / `loop1()` | `packetSerial.update()` (UART RX → `onPacketReceived`), telemetry queue → `uart_link_process()` (the only place that writes to the ESP32, paced), `sd_card_update()`, alarm history stream step, system state machine: `BOOT → SD_CHECK → LOAD_CFG → STREAM_DEV_CFG → RUNNING (↔ RELOAD_CFG)`. RUNNING runs `data_storage_process_buckets()` every 1 s, hourly aggregation on UTC hour change, daily aggregation at local midnight. |

Both cores share `g_comm_mutex` around SD access, decoders and the archive. UART TX is decoupled: both cores only queue packets (`uart_link_send`), core 1 transmits them.

## Files

| File | Purpose | Main functions |
|---|---|---|
| `include/indicator_rp2040.hpp` | Umbrella header: packet IDs (`PKT_TYPE_*`), sensor/alarm/SD prototypes, PacketSerial 2048 B | – |
| `src/main.cpp` | Core loops + UART command dispatcher `onPacketReceived()` (handles 0x22, 0x30, 0x32, 0x34, 0x36–0x38, 0x3A, 0x3B, 0x3D, 0x40, 0x42, 0xA1–0xA4, 0xA6–0xAA) | `setup`, `loop`, `setup1`, `loop1` |
| `src/lorasensors.cpp/.hpp` | Loads `/config/*.json`, `/devices/*.json`, `/decoder/*.json` (decoders cached in RAM); IPSO-TLV / packed-binary decoding of decrypted frames; offsets, limits, tank geometry; pushes RF config + devices to ESP32 (`0x35`, `0x3F`, followed by the device's MQTT key selection `0x46`); patches thresholds/offsets on SD (`0x40`, `0x42`) | `lorasensors_load_all`, `lorasensors_process_raw_frame`, `lorasensors_dispatch_local`, `lorasensors_push_globals`, `lorasensors_stream_single_device`, `lorasensors_save_threshold_to_sd`, `lorasensors_save_offset_to_sd` |
| `src/virtual_decoder.cpp/.hpp` | Calculated keys from device `virtual_decoder` block, strictly scoped per `dev_addr` (decoders, source values and outputs of different devices never mix; max 24 decoders, 32 blackboard values): tank volume (rectangle, cylinders, frustum, LUT), derivatives (`cm/h`, `m³/h`), dewpoint, … Results are queued as telemetry to the ESP32. | `virtual_decoder_register`, `virtual_decoder_process_and_send`, `virtual_decoder_get_output` |
| `src/archive_store.cpp/.hpp` | Segmented binary archive: fixed-size records, append-only 4 KB segments per level (`raw`, `hour`, `day`), retention by deleting the oldest segment, range scan, last record | `arc_append`, `arc_scan_range`, `arc_read_last`, `arc_channel_scan` |
| `src/data_storage.cpp/.hpp` | Archive engine on LittleFS, configured per key by the `archive` block of each device JSON (retention, interval, `max_estimated_count`, `quality`); RAM queue while LittleFS is unavailable; cold-start restore of the last value: RAM bucket accumulator on wall-clock grid, hold/estimated values, hourly/daily aggregation with quality flags, chart queries for `0x3B`, last-time persistence (`/data/last_time.txt`) | `data_storage_load_devices`, `data_storage_add_point`, `data_storage_process_buckets`, `data_storage_aggregate_hourly/daily`, `data_storage_query_chart`, `data_storage_query_lttb`, `data_storage_save_time` |
| `include/littlefs_driver.hpp`, `src/littlefs_driver.cpp` | Block device + LittleFS on SD partition 2 (1 GB at LBA 4456448) | `littlefs_driver_mount/unmount/is_mounted`, `append_line`, `rewrite_file`, `open_read/read_line` |
| `src/sdcard.cpp/.hpp` | SD state machine: card detect, MBR dual partition (FAT32 + LittleFS), format, VBR patch, label check, USB-MSC, hot reload trigger | `sd_card_update`, `sd_card_format_start`, `sd_card_msc_toggle`, `sd_card_needs_config_reload` |
| `src/sensor_detect.cpp/.hpp` | Non-blocking I2C scan, builtin sensor presence | `sensor_detect_i2c_scan_step`, `sensor_detect_is_builtin_present` |
| `src/sensors.cpp` | Sensor drivers (AHT20, SHT41, SGP40, SCD4x, light) | `sensor_*_init/get` |
| `src/utility.cpp` | Buzzer / beep helpers, alarm pattern state machine | `alarm_set`, `alarm_mute`, `alarm_rearm`, `buzzer_update` |
| `src/units.cpp`, `include/units.h` | Unit conversion: complete builtin table in flash (no RAM), `/config/units.json` only adds or corrects units (filtered parse, only differences kept in RAM); exact name / symbol / alias match first, case-insensitive only for names of ≥ 3 characters; impossible conversions logged once per unit pair | `units_init`, `units_convert`, `units_get_quantity_type`, `units_get_symbol`, `units_resolve_target` |
| `src/telemetry_queue.hpp` | Thread-safe FIFO for virtual decoder telemetry (8 ms spacing, feeds the link queue) | `telemetry_tx_enqueue`, `telemetry_tx_process_queue` |
| `src/uart_link.cpp/.hpp` | Central paced TX queue to the ESP32 (6 KB ring, pacing by packet size) + link diagnostics (`UART_LINK_DIAG`) | `uart_link_send`, `uart_link_can_send`, `uart_link_process` |
| `src/debug_log.hpp/.cpp` | Non-blocking console `LogSerial` (drops output instead of stalling when the USB terminal stops reading) and domain debug macros (`DBG_LORA_PRINTF`, …) | `LogSerial` |

## Data path inside the RP2040

```
ESP32 0x36 (decrypted frame) ─► lorasensors_process_raw_frame()
                                   └─ per mapping: process_and_dispatch_rule()
                                        ├─ 0xBB telemetry to ESP32
                                        ├─ data_storage_add_point()      (contexts "measurement" + "diagnostic")
                                        └─ virtual_decoder_process_and_send()   (context "measurement")
                                              ├─ telemetry_tx_enqueue() (0xBB)
                                              └─ data_storage_add_point()   (virtual keys, e.g. volume)
data_storage keeps only keys listed in the device's "archive" block.
Core 0 I2C sample ─► lorasensors_dispatch_local() ─► same three sinks
data_storage: RAM bucket ─(grid)─► raw/*.bin ─(UTC hour)─► hour/*.bin ─(local midnight)─► day/*.bin   (via archive_store)
```
