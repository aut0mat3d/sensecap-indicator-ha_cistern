# ESP32-S3 Firmware Overview (`main/`)

> Generated 2026-11-17 from the source tree. ESP-IDF 5.x, LVGL 9, FreeRTOS.
> Build: [main/CMakeLists.txt](../../main/CMakeLists.txt) globs every `*.c` in the listed module folders.
> Feature switches (compile definitions): `CONFIG_CISTERN_MODE`, `CONFIG_GUI_CONFIG_EXTERN`, `CONFIG_LORA_ENABLE`, `CONFIG_LORA_P2P`.

## Boot sequence

| Step | File | What happens |
|---|---|---|
| 1 | [main.c](../../main/main.c) | `bsp_board_init()` → `lv_port_init()` → `indicator_view_init()` → `indicator_model_init()` |
| 2 | [indicator_view.c](../../main/indicator_view.c) | `nav_init`, `settings_view_init`, `cistern_view_init`, `indicator_display_view_init`, `indicator_wifi_view_init`, `lora_diag_view_init`, `sd_view_init`, `alarm_view_init` (sensor and HA views are commented out) |
| 3 | [indicator_model.c](../../main/indicator_model.c) | NVS, button, display backlight, `esp32_rp2040_init`, sensors, `lora_model_init`, cmd, Wi-Fi, MQTT, HA, `sd_model_init`, `alarm_model_init`, `alarm_evaluator_init` |

`indicator_enabler.h` is the umbrella header that pulls in all module headers.
`view_data.h` / `view_data_types.h` define the event bus (`VIEW_EVENT_BASE`) and its payload structs.
Note: `VIEW_EVENT_LORA_TELEMETRY` is a `#define` (0x7F) outside the enum in `view_data_types.h`.

## Modules

| Folder | Purpose | Key files / public API |
|---|---|---|
| `alarm/` | Headless alarm engine + alarm list UI | `alarm_evaluator.c`: parses `thresholds` of a device JSON (`alarm_evaluator_load_device_json`), evaluates every `VIEW_EVENT_LORA_TELEMETRY` with damping/hysteresis (`alarm_evaluator_evaluate`). `alarm_model.c`: alarm entries, mute/ack, ring buffer, persistence via RP2040 (`0xA9/0xAA/0xAB`), buzzer patterns. `alarm_view.c`: LVGL alarm list (sort, filter, ack, HW button). |
| `btn/` | Physical button | `indicator_btn_init` |
| `cistern/` | Cistern constants | `cistern_config.h` |
| `cmd/` | UART console commands (debug), can inject telemetry events | `indicator_cmd_init` |
| `display/` | **Static start page** + backlight/sleep | `cistern_view.c` (current hard-wired home screen: vert_graph, horiz_bargraph with 4 zones, keyvalue cards; subscribes 3 chart zones in `chart_query_scheduler`), `display_model.c` (LEDC PWM, sleep timer, NVS), `display_view.c` (brightness/sleep settings). Has its own `README.md`. |
| `gui/` | Reusable LVGL 9 widgets | `horiz_bargraph` (multi-zone history chart), `vert_graph` (level bar with alarm marks), `keyvalue_card` (multi-row text tile), `sensor_card`, `collapsible_list` (accordion), `input_dialog` (modal numeric/text input), `config_controller` (schema-driven settings editor), `gui_header` / `gui_footer` (global bars: title, slots, Wi-Fi, LoRa, alarm state), `widget_base.h` (common widget interface). Docs in `gui/docs/`. |
| `gui/i18n/` | GUI translation engine | `gui_i18n.c`: builtin EN/DE tables (`gui_i18n_builtin_en.c`, `_de.c`) + SD overrides from `/gui/lang/<lang>/common.json` and `alarm_config.json` (read via RP2040 `0x38`). API: `gui_i18n_get(key)`, `gui_i18n_reload_from_sd(lang)`. |
| `gui_ext/` | Empty (reserved) | – |
| `ha/` | Home Assistant over MQTT (largely disabled in cistern mode) | `ha_mqtt.c`, `ha_sensor.c`, `ha_switch.c`, `ha_switch_screen.c`, `ha_config.c` |
| `lora/` | SX1262 radio, LoRaWAN decryption, P2P mesh | `lora_model.c` (task `lora_model_task`): RF config from RP2040 (`0x35/0x3F`), RX handler, LoRaWAN AES/MIC (`lorawan_cipher_payload`, `lorawan_calculate_mic`), mesh AES-CTR (`lora_mesh_cipher_payload`), duplicate cache, TX queue (downlinks, P2P CLAIM `0x01`, RETRANSMIT `0x02`, TIME `0x03`). `lora_diag_view.c`: builtin LoRa debug page. |
| `mqtt/` | Shared MQTT client | `indicator_mqtt_init` |
| `nav/` | `lv_tileview` horizontal navigation | `nav_init`, `nav_get_tile`, `nav_go_tile` |
| `rp2040/` | UART link to RP2040 (COBS, 921600 Bd) | `rp2040.c`: TX helpers (`rp2040_send_sync_time`, `rp2040_request_chart_data`, `rp2040_alarm_*`, `rp2040_sd_*`, `rp2040_read_file_from_sd`) and RX dispatcher → view events. `chart_query_scheduler.c`: serialized FIFO of chart archive queries; widgets subscribe per zone (`chart_query_scheduler_subscribe`), queries fire on bucket boundaries. |
| `sdcard/` | SD card management page (builtin) | `sd_view.c`: info, format, USB-MSC toggle |
| `sensor/` | Legacy onboard sensor view (currently not initialized) | `sensor_model.c`, `sensor_view.c` |
| `settings/` | Builtin settings page | `settings_view.c` |
| `storage/` | NVS + SD model | `storage_nvs.c`, `sd_model.c` |
| `util/` | Helpers | `cobs.c`, `debug_log.h`, `device_config_controller.c` (threshold editor connector → RP2040 `0x40`), `i18n.c` (older page-scoped i18n with fallback cascade), `indicator_util.c`, `media_standards.c` (medium → user-defined main color/unit, loaded from `/gui/media_standards.json`) |
| `wifi/` | Wi-Fi + SNTP (builtin pages) | `wifi_model.c`, `wifi_view.c`, `wifi_list_screen.c`, `wifi_connect_screen.c`, `sntp_service.c` |
| root | Units | `units.c`: unit conversion with builtin `units.json` |

## GUI direction

* **Today:** `display/cistern_view.c` is a static, hard-wired start page (device address and key are compile-time constants).
* **Target:** pages generated from JSON on SD (`/gui/pages/*.json`, see AGENTS.md §3 Layer 5), built from the generic widgets in `gui/`.
* **Stays builtin:** settings, Wi-Fi, LoRa diagnostics, SD management, alarm list, display settings.
* Builtin GUI languages: EN and DE. More languages are loaded from SD (`/gui/lang/<lang>/`).

## Two i18n engines (worth consolidating later)

* `gui/i18n/gui_i18n.c` – key lookup with builtin EN/DE + SD override (used by newer widgets).
* `util/i18n.c` – `i18n_get(page_id, key)` with 5-step fallback (older).
