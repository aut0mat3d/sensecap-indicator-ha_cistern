# ESP32-S3 Firmware Overview (`main/`)

> Generated 2026-11-17 from the source tree. ESP-IDF 5.x, LVGL 9, FreeRTOS.
> Build: [main/CMakeLists.txt](../../main/CMakeLists.txt) globs every `*.c` in the listed module folders.
> Feature switches (compile definitions): `CONFIG_CISTERN_MODE`, `CONFIG_GUI_CONFIG_EXTERN`, `CONFIG_LORA_ENABLE`, `CONFIG_LORA_P2P`.

## Boot sequence

| Step | File | What happens |
|---|---|---|
| 1 | [main.c](../../main/main.c) | `bsp_board_init()` → `lv_port_init()` → `indicator_view_init()` → `indicator_model_init()` |
| 2 | [indicator_view.c](../../main/indicator_view.c) | `nav_init`, `settings_view_init`, `cistern_view_init`, `indicator_display_view_init`, `indicator_wifi_view_init`, `lora_diag_view_init`, `sd_view_init`, `alarm_view_init` (sensor and HA views are commented out) |
| 3 | [indicator_model.c](../../main/indicator_model.c) | NVS, button, display backlight, `esp32_rp2040_init`, sensors, `lora_model_init`, cmd, Wi-Fi, `mqtt_bridge_init`, `sd_model_init`, `alarm_model_init`, `alarm_evaluator_init` |

`indicator_enabler.h` is the umbrella header that pulls in all module headers.
`view_data.h` / `view_data_types.h` define the event bus (`VIEW_EVENT_BASE`) and its payload structs.
Note: `VIEW_EVENT_LORA_TELEMETRY` is a `#define` (0x7F) outside the enum in `view_data_types.h`.

## Modules

| Folder | Purpose | Key files / public API |
|---|---|---|
| `alarm/` | Headless alarm engine + alarm list UI | `alarm_evaluator.c`: rules are registered from the streamed device config (`0x3F`, `lora_model_on_rp2040_device_received`) and after GUI edits (`device_config_controller`); percent thresholds are resolved by `lora_model_resolve_threshold` (reference `limits[key]`, else tank depth). Evaluates every `VIEW_EVENT_LORA_TELEMETRY` with damping/hysteresis (`alarm_evaluator_evaluate`). `alarm_model.c`: alarm entries, mute/ack, ring buffer, persistence via RP2040 (acknowledged upload `0xA9`/`0xAC`, load `0xAA`/`0xAB`), buzzer patterns; never takes the LVGL lock. `alarm_view.c`: LVGL alarm list (sort, filter, ack, HW button), refreshed by an LVGL timer after `alarm_view_notify_change()`. |
| `btn/` | Physical button | `indicator_btn_init` |
| `cistern/` | Cistern constants | `cistern_config.h` |
| `cmd/` | UART console commands (debug), can inject telemetry events | `indicator_cmd_init` |
| `display/` | **Static start page** + backlight/sleep + detail page | `key_detail_view.c` (full-screen detail page for any dev_addr + key: trend graph 24 h / 7 d / 31 d, value, time, status, expected interval; opened from the battery icon in header slot `HEADER_SLOT_LEFT_2`), `cistern_view.c` (current hard-wired home screen: vert_graph, horiz_bargraph with 4 zones, keyvalue cards; subscribes 3 chart zones in `chart_query_scheduler`), `display_model.c` (LEDC PWM, sleep timer, NVS), `display_view.c` (brightness/sleep settings). Has its own `README.md`. |
| `gui/` | Reusable LVGL 9 widgets | `horiz_bargraph` (multi-zone history chart), `vert_graph` (level bar with alarm marks), `keyvalue_card` (multi-row text tile), `sensor_card`, `collapsible_list` (accordion), `input_dialog` (modal numeric/text input), `config_controller` (schema-driven settings editor), `gui_header` / `gui_footer` (global bars: title, slots, Wi-Fi, LoRa, alarm state), `gui_theme` (alarm severity and journal colors from `/gui/theme.json`, alarm levels `GUI_ALARM_NONE/WARNING/DANGER` used by the widgets), `battery_icon` (standing battery bound to dev_addr + key: continuous fill, colors from the key's thresholds or fallback bands 25/10 %, "?" without value, dimmed after 2 x expected interval; requests the newest archived value via `0x44`), `trend_graph` (long-term trend as point/line chart on a time axis: LTTB-reduced data (100 points), points at their timestamps, gaps break the line, estimated points at 50 %, alarm points in theme colors; up to 4 tap-to-cycle presets, one scheduler slot, fixed calendar frame following the clock, level lines, `set_custom_range`; any size on the 48 px grid, minimum 4 × 3 cells), `widget_base.h` (common widget interface). Docs in `gui/docs/`. |
| `gui/i18n/` | GUI translation engine | `gui_i18n.c`: builtin EN/DE tables (`gui_i18n_builtin_en.c`, `_de.c`) + SD overrides from `/gui/lang/<lang>/common.json` and `alarm_config.json` (read via RP2040 `0x38`). API: `gui_i18n_get(key)`, `gui_i18n_reload_from_sd(lang)`. |
| `gui_ext/` | Empty (reserved) | – |
| `lora/` | SX1262 radio, LoRaWAN decryption, P2P mesh | `lora_model.c` (task `lora_model_task`): RF config from RP2040 (`0x35/0x3F`), RX handler, LoRaWAN AES/MIC (`lorawan_cipher_payload`, `lorawan_calculate_mic`), mesh AES-CTR (`lora_mesh_cipher_payload`), duplicate cache, TX queue (downlinks, P2P CLAIM `0x01`, RETRANSMIT `0x02`, TIME `0x03`). `lora_diag_view.c`: builtin LoRa debug page. |
| `mqtt/` | MQTT publisher + Home Assistant discovery | `mqtt_bridge.c` (broker settings from `/config/mqtt.json`, key selection per device from packet `0x46`, bridge task with PSRAM queue, discovery sync / stale removal, alarm state poll, `mqtt` console command), `mqtt_ha_classes.c` (device_class / state_class / HA unit from key and unit). Replaces the upstream `ha/` module (fixed topics, NVS broker settings, default connection to a public test broker). Details: OVERVIEW_DATAFLOW.md §7 |
| `nav/` | `lv_tileview` horizontal navigation | `nav_init`, `nav_get_tile`, `nav_go_tile` |
| `rp2040/` | UART link to RP2040 (COBS, 921600 Bd) | `rp2040.c`: UART task pinned to core 1 above LVGL priority (RX dispatcher → view events with bounded post timeouts, overflow detection, alarm journal uploader, link diagnostics `RP2040_LINK_DIAG`) and TX helpers (`rp2040_send_sync_time`, `rp2040_request_chart_data`, `rp2040_alarm_*`, `rp2040_sd_*`, `rp2040_read_file_from_sd`). `chart_query_scheduler.c`: serialized FIFO of chart archive queries (one in flight, 100 ms gap); 24 slots × 100 points in PSRAM; widgets subscribe per zone (`chart_query_scheduler_subscribe`), choose GRID or LTTB mode (`chart_query_scheduler_set_query_mode`), queries fire on bucket boundaries or on `chart_query_scheduler_poll_range`. Flow control rules: OVERVIEW_DATAFLOW.md §5a. |
| `sdcard/` | SD card management page (builtin) | `sd_view.c`: info, format, USB-MSC toggle |
| `sensor/` | Legacy onboard sensor view (currently not initialized) | `sensor_model.c`, `sensor_view.c` |
| `settings/` | Builtin settings page | `settings_view.c` |
| `storage/` | NVS + SD model | `storage_nvs.c`, `sd_model.c` |
| `util/` | Helpers | `cobs.c`, `debug_log.h`, `device_config_controller.c` (threshold editor connector → RP2040 `0x40`), `i18n.c` (older page-scoped i18n with fallback cascade), `indicator_util.c`, `media_standards.c` (medium → user-defined main color/unit, loaded from `/gui/media_standards.json`) |
| `wifi/` | Wi-Fi + SNTP (builtin pages) | `wifi_model.c`, `wifi_view.c`, `wifi_list_screen.c`, `wifi_connect_screen.c`, `sntp_service.c` |

## Display pipeline

* **Panel:** ST7701S 480x480, 16-bit RGB565 (panel in RGB666 mode), 12 MHz pixel clock, `PCLK_ACTIVE_NEG = 1`. The panel is reset (IO expander IO0_5) and configured over bit-banged 3-wire SPI (GPIO41/48, shared with the SX1262 SPI bus) in `bsp_board_sensecap_indicator_init()` **before** `bsp_lcd_init()` starts the RGB stream.
* **Driver:** ESP-IDF RGB panel in bounce buffer mode (2 x 30 lines in internal RAM, two frame buffers in PSRAM). The project uses a patched local copy of the ESP-IDF v5.4 `esp_lcd` component (`components/esp_lcd/`, see `CISTERN_PATCH.md`): after a bounce buffer underrun the driver restarts the LCD engine completely instead of using the ESP32-S3 restart link, which left the picture permanently shifted by 16 px.
* **LVGL port** (`main/lv_port.c`): esp_lvgl_port with `avoid_tearing`, LVGL **direct mode** (only invalidated areas are rendered and synced into the other frame buffer), LVGL task on core 1 (priority 6).
* **Covered screen:** while an opaque full-screen modal (radius 0) on `lv_layer_top()` is visible, `lv_port.c` hides the active screen in `LV_EVENT_REFR_START`; LVGL would otherwise render the complete page underneath on every frame (`LV_PORT_HIDE_COVERED_SCREEN`).
* **Diagnostics:** `[RENDER]` log every 30 s (frames, render time, total RGB restarts; `LV_PORT_RENDER_DIAG`), console commands `lcd_restart` and `lcd_rgb_reinit`.

## GUI direction

* **Today:** `display/cistern_view.c` is a static, hard-wired start page (device address and key are compile-time constants).
* **Target:** pages generated from JSON on SD (`/gui/pages/*.json`, see AGENTS.md §3 Layer 5), built from the generic widgets in `gui/`.
* **Stays builtin:** settings, Wi-Fi, LoRa diagnostics, SD management, alarm list, display settings.
* Builtin GUI languages: EN and DE. More languages are loaded from SD (`/gui/lang/<lang>/`).

## Two i18n engines (worth consolidating later)

* `gui/i18n/gui_i18n.c` – key lookup with builtin EN/DE + SD override (used by newer widgets).
* `util/i18n.c` – `i18n_get(page_id, key)` with 5-step fallback (older).
