# NEXT_STEPS - handoff and todo list

Living document: status, running verifications, next feature and backlog. Update it at the end of a
session; remove items once they are committed and verified.

Last update: 2026-11-19. Base: `d976c94` (first beta revision); the fixes below are committed on top
of it ("fix: radio ownership, touch read errors ...") and still under field verification.

---

## 1. Fixes under verification (committed 2026-11-19)

| Area | Files | What / why |
|---|---|---|
| LoRa freeze (IRQ race) | `components/lora/radio.c`, `main/lora/lora_model.c` | SX1262 IRQ register was read and cleared by two tasks (poll + DIO1 path via IO expander); a lost TxDone left the radio in standby, listener stopped receiving. `RadioOnDioIrq` only sets `IrqFired`, the LoRa task owns all SPI. TX watchdog (8 s), RX watchdog (chip mode every 2 s). |
| LoRa: foreign-task radio access | `main/lora/lora_model.c/.h` | Found 2026-11-19 by the RX watchdog (chip mode 2 right after each hourly SNTP sync): `lora_model_enqueue_time_broadcast()` ran in the SNTP task and called `Radio.Random()`, which switches the SX1262 to STDBY_RC with all radio IRQs disabled. Jitter now via `esp_random()` (all 4 call sites); SNTP only hands the UTC over (`s_pending_time_bcast_utc`), the LoRa task enqueues. |
| Ghost touches | `components/bsp/src/indev/indev.c`, `indev_tp.c`, `components/i2c_devices/touch_panel/ft5x06.c`, `main/lv_port.c` | I2C read error returned `ESP_OK` with uninitialized `indev_data` → random / clamped (0,0) press = settings gear. Errors now propagate, last valid state kept, FT5x06 status masked (1..5 points, lift-up / no-event ignored). `[RENDER]` line shows `touch read errors N`. |
| Docs | `docs_extensions_n_changes/overview/OVERVIEW_ESP32.md` | Radio ownership, watchdogs, RNG rule. |

Flash state: slave = LoRa IRQ fix + touch fix (not yet the time-broadcast / RNG fix). Master = LoRa IRQ
fix only (kept on purpose as ghost-touch reference).

## 2. Running verification

Slave log: `picocom -b 115200 /dev/ttyUSB0 -g ~/sensecap_$(date +%F).log`

```sh
# -a is required: picocom logs contain binary bytes, plain grep only says "binary file matches"
grep -anE "watchdog|touch read errors [1-9]" ~/sensecap_*.log
```

| Observation | Meaning / next action |
|---|---|
| `RX watchdog ... chip mode 2` right after `Enqueued Standalone P2P Time Broadcast` | Expected on firmware without the RNG fix; must disappear after flashing it. |
| Any other `TX watchdog` / `RX watchdog` | A further radio stall path exists; look at the 50 lines before it. |
| `touch read errors N` > 0 | I2C read errors happen; each would have been a ghost press before the fix. |
| Ghost press seen, counter stays 0 | Not an I2C error: check FT5x06 thresholds (`ft5x06_init`: `THGROUP 70`, `THPEAK 60`, `THDIFF 20`), touch sensitivity was raised earlier; log raw coordinates of presses. |

Night 2026-11-18 → 19 (slave): 0 touch read errors, no ghost seen, 2 RX watchdog hits (the SNTP case above),
no freeze.

---

## 3. Next feature: flowrate trend page (user request 2026-11-19)

### 3.1 Goal

Tap on the `vert_graph` in `cistern_view` opens a full-screen page (like the battery icon → detail page)
with a trend graph of `flowrate` of the cistern device.

* Ranges: **24 h (default)**, 1 week, 1 month.
* Values are positive or negative (sign: negative = net outflow, derivative of `volume`), magnitude
  unknown → Y axis must auto-zoom.
* Purpose: find the warning sweet spot by observation. Typical state is ~0 (cistern full, inflow >
  draw). In winter two taps run permanently against freezing: too much must be noticed. Later: daily
  draw peaks of 4 households, recovery periods, summer shortage.
* Below the graph a scrollable list of the drawn data points (date, time, value), compacted
  (runs of identical values merged into a range, estimated values filtered) → new generic widget
  `autocompact_listview`, sized in grid cells.

### 3.2 Findings in the current code

1. **`flowrate` archive: done 2026-11-19** (`single` 2016 = 7 days raw, `hours` 48, `days` 32) in
   `sdcontent/devices/Wasser_Hochbehaelter.json` and on the test unit's SD card. History starts from
   that day. Before, `archive` only had `depth` and `battery_soc`.
   Proposal (24 h raw at 5 min = 288 records, week from hours, month from days):
   ```json
   "flowrate": { "tolerance_sec": 15, "max_estimated_count": 0,
                 "retention": { "single": 288, "hours": 192, "days": 62, "weeks": 0, "months": 0, "years": 0 } }
   ```
   Limits checked 2026-11-19 (see `OVERVIEW_SDCARD.md`, retention limits): max 65535 per level, disk
   is no issue, cost is chart query time. User keeps hours 48 / days 32 and wants more raw values;
   recommendation `single` = 2016 (7 days), 8928 (31 days) only after measuring the query time.
   The test unit's SD card is updated by the user (`SDbackup/` stays untouched).
2. **vert_graph tap is taken:** `cistern_view.c` `on_vert_graph_clicked()` opens
   `device_config_controller_open_thresholds(CISTERN_DEV_ADDR)`. **Decision needed:** e.g. tap →
   flowrate page and a "thresholds" button on that page, or long press → thresholds.
3. **`key_detail_view` is already generic** (`key_detail_view_show(dev_addr, key)`): trend graph 10 x 5
   cells, presets 24 h / 7 d / 31 d, threshold lines from `thresholds[]`, Y scale from `limits[key]`,
   media color. Gaps:
   * Initial preset is hard-coded to 31 d (`configure_trend`: `preset_idx = 2`) → make it a parameter
     (options struct or `key_detail_view_show_ex`).
   * Lower part shows 5 info rows; the flowrate page wants the list there → layout variant
     (e.g. trend 10 x 4 + list 10 x 4, or trend 10 x 5 + list 10 x 3; content area is 10 x 8 cells,
     `GRID_CONTENT_Y_OFFSET` 64).
4. **Auto scale is not suited to small signed values** (`trend_graph.c` `compute_scale`): margin is at
   least an absolute `1.0`, so a trickle of -0.05 m³/h on a -1.05 .. 1.0 axis is a flat line. Needed:
   relative margin with a unit-independent minimum span, **0 always inside the range**, a zero line,
   rounded ("nice") min/max labels; flat 0 data must not divide by zero.
5. **Aggregates carry min/max:** hour/day records store `avg/min/max`, `chart_data_point_t` has
   `value_min/value_max`, the graph draws only `value` (= avg). For week/month a min/max band or
   whiskers keep short draw peaks visible that averaging would hide. Decide per page or per widget flag.
6. **Derivative quality** (`rp2040/src/virtual_decoder.cpp` `calc_derivative`):
   * The first value after boot / reload returns `0.0` → a fake 0 point is published and archived.
     It should produce no value instead.
   * `virtual_decoder.flowrate.interval_sec` is parsed nowhere → either implement a smoothing window
     (rate over N seconds) or remove it from the schema / docs.
   * Resolution: rate step = level resolution × tank area / report interval (5 min). Check whether the
     trickle of two taps is above that step at all; otherwise a longer window is required.
   * Existing threshold `alarm_frate`: danger at -2.0 m³/h falling, hysteresis 0.4, damping 2.
7. LTTB delivers ≤ 100 points per window (`CHART_SUB_MAX_POINTS`); the 24 h preset (step 3600 × 24)
   gives ~14 min buckets over 5 min raw records, so the list has at most ~100 source rows and needs
   no own query: it can read `trend_graph_t.points` after each update.

### 3.3 `autocompact_listview` widget (proposal)

* Generic LVGL widget in `main/gui/` + `main/gui/docs/autocompact_listview_readme.md`, placed on the
  48 px grid (`x/y/width/height` in cells via `GRID_*`), minimum size e.g. 4 x 2 cells, row height
  from the font.
* Input: array of `chart_data_point_t` (time, value, min/max, flags) + unit + decimals; the widget does
  the compaction, the caller does not.
* Compaction rules (configurable):
  * consecutive points whose values are equal at display precision → one row
    `from – to   value   (n)`;
  * `DATA_FLAG_ESTIMATED` hidden (or shown dimmed, option);
  * `DATA_FLAG_NO_DATA` / gap markers → one row "no data from – to";
  * alarm flags colored with the theme warning / danger color.
* Time format by window: 24 h → `HH:MM`, day change as a separator row; week / month → `dd.mm. HH:MM`
  or `dd.mm.` for daily points; local time (`tz_offset`).
* Newest first; scrollable; updates keep the scroll position when the user has scrolled.
* Later option: tap a row → highlight the point in the trend graph.
* i18n: new strings in `gui_i18n_builtin_en.c` and `_de.c`.

### 3.4 Suggested order

1. Decide the vert_graph tap / thresholds access (3.2.2) and the page layout (3.2.3).
2. ~~`archive.flowrate`~~ done (cistern and Guest file). After some days, measure the 7-day trend
   query time (raw level) before raising `single` further.
3. RP2040 `calc_derivative`: no fake 0 after boot, decide `interval_sec`.
4. `trend_graph` auto scale for signed small values + zero line (+ optional min/max band); update
   `main/gui/docs/trend_graph_readme.md`.
5. `key_detail_view`: initial preset parameter + layout variant with list.
6. `autocompact_listview` widget + readme.
7. Wire it up in `cistern_view.c`; build both MCUs; test on the slave first.

---

## 4. Backlog

* Optional beta git tag once section 1 is verified.
* HA → SenseCAP direction: show HA entities (Fronius Symo GEN24, battery, Wattpilot).
* Logging cleanup (WARN default, keep the 30 s `[LINK]` / `[RENDER]` diagnostics switchable).
* Move the "button" settings out of `alarm_config.json`.
* Wi-Fi core split.
* Collapsible list scroll range.
* CSV export of the archive; NTTW aggregation (weeks / months / years retention is parsed but not aggregated).
* JSON-generated pages from `/gui/pages/*.json`.
* `lorasensors.cpp` device JSON doc is 4 KB, `Wasser_Hochbehaelter.json` uses ~3 KB (headroom shrinks
  with every new block, e.g. `archive.flowrate`).
* Python theme color tool (low priority).

## 5. Working notes for a new session

* Read `AGENTS.md` and the four overview documents first.
* ESP32 build: `source ~/esp/esp-idf/export.sh && idf.py build`; RP2040: `cd rp2040 && pio run`.
* Release binaries for click deploy: `click_deploy/sync_from_build.sh`.
* Never touch / stage `SDbackup/`, `tmp/`, `assets/fonts/*.bak`; dummy keys only in `sdcontent/`.
* Radio rule: only `lora_model_task` talks to the SX1262; no `Radio.Random()` (use `esp_random()`).
