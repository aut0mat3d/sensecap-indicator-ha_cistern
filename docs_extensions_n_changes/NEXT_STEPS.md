# NEXT_STEPS - handoff and todo list

Living document: status, running verifications, next feature and backlog. Update it at the end of a
session; remove items once they are committed and verified.

Last update: 2026-11-20. **Beta 2** (commit "feat: beta 2 ..."): the fixes of section 1 are verified, the
detail page of section 3 is verified on the test unit, `interval_sec` (3.4) still needs its field test. The outdated upstream
`TASKS.md` and the archived `docs_extensions_n_changes/old/` were removed (git history keeps them).

---

## 1. Fixes of 2026-11-19 (committed, verified 2026-11-20)

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

**Verified 2026-11-20** (slave with all fixes incl. RNG, log 19.11. ~09:30 → 20.11. 09:16, ~23.7 h):
0 watchdog hits across 23 hourly time broadcasts, 0 touch read errors, no ghost touch, no missed P2P
packet while observed. The master (old firmware) had ghost touches on the `vert_graph` the same night.
→ Fixes count as proven; flash the master too. Optional beta tag possible (backlog).

---

## 3. Flow rate / level detail page (beta 2, 2026-11-20)

### 3.1 Spec (user, 2026-11-20; replaces the 2026-11-19 draft with the list widget)

* Tap on the `horiz_bargraph` ("Verlauf Pegel") in `cistern_view` opens a full-screen page (like the
  battery page). The `vert_graph` tap keeps opening the thresholds.
* Two `trend_graph`s of 10 × 4 cells: `flowrate` on top, `depth` below, same time range. Ranges
  24 h (default), 7 days, 1 month. No list (the `autocompact_listview` idea is dropped).
* Both Y axes automatic (flowrate signed, mostly 0.0, range unknown). Dashed lines at the window
  maximum / minimum.
* Touch zones per graph: head line = next range (both graphs), plot = drag cursor with time + value
  (both graphs show the same time), X label row = dead zone.

### 3.2 Implementation

| Part | Files |
|---|---|
| Window extremes: LTTB pass 1 tracks min / max of every record; `0x3C` trailer `chart_window_extremes_t` (9 B) in LTTB mode | `rp2040/src/data_storage.cpp/.hpp`, `rp2040/src/main.cpp` |
| Parser accepts the trailer (length check, older RP2040 firmware still works); scheduler stores it per slot, `chart_query_scheduler_get_window_extremes()`, fallback from the points | `main/rp2040/rp2040.c/.h`, `main/rp2040/chart_query_scheduler.c/.h` |
| `calc_derivative`: no value (instead of a fake 0.0) for the first sample after boot / reload and for non-increasing timestamps | `rp2040/src/virtual_decoder.cpp` |
| `trend_graph`: touch zones, cursor + coupling callbacks, automatic scale (zero inclusion, minimum span, nice step), zero line, extreme lines, unit | `main/gui/trend_graph.c/.h`, `main/gui/docs/trend_graph_readme.md` |
| `key_detail_view_show_ex()`: one key (unchanged battery page, 31 days) or two coupled keys | `main/display/key_detail_view.c/.h` |
| Wiring: `on_horiz_graph_clicked` (flowrate with 0 in the scale); Y span from the data only, no per-key constants | `main/display/cistern_view.c` |
| `horiz_bargraph`: chart and zone dividers no longer swallow the tap | `main/gui/horiz_bargraph.c` |
| i18n `telemetry.flowrate`, `trend.1m`, `trend.max`, `trend.min` (EN + DE) | `main/gui/i18n/gui_i18n_builtin_*.c` |
| `archive.depth.retention.single` 144 → 2016 (same resolution as flowrate) | `sdcontent/devices/Wasser_Hochbehaelter.json`, `Guest_Wasser_Hochbehaelter.json`; **test unit SD: user** |

Both MCUs build. Verified on the test unit 2026-11-20: Y scaling to the window min / max, cursor,
coupled ranges; battery page unchanged incl. cursor.

### 3.3 Verification on the device (slave first; it then also gets the RNG fix of §1)

* Tap level history → page with both graphs, 24 h; head tap switches both 24 h → 7 days → 1 month;
  a tap on the X label row of the upper graph does nothing.
* Flow rate ~0: symmetric axis around 0 with zero line; min / max lines match the extremes.
* Drag the cursor: both graphs show time + value at the same time; gone 10 s after the last touch.
* `vert_graph` tap still opens the thresholds; battery page unchanged (31 days, info rows).
* Log: `RES_CHART_DATA` without "length too short"; note the 7-day query time (raw level) before
  raising `single` further.
* After a reboot: no 0.0 as first flowrate value in archive / MQTT.

### 3.4 Open points

* **Flow rate resolution (log 2026-11-19/20, 24 h):** depth arrives in whole cm (119 / 120 / 121),
  tank 2 × 2 m → 1 cm = 40 l. One step per 5 min report = **480 l/h** (`flowrate` ±0.48 m³/h; 276 × 0.00,
  5 × -0.48, 4 × +0.48 = level jitter by 1 cm). A trickle of tens of l/h is invisible per report.
  The step is level resolution × area / time span: report interval 60 min → 40 l/h, a rate over 6 h →
  6.7 l/h.
* **`interval_sec` implemented 2026-11-20** (beta 2, field test pending): one computation per local
  raster slot, first packet of the slot triggers, no intermediate values; sensor slower than the raster
  → every packet computes (warning at load). Effective interval feeds archive `expected_interval_sec`
  default and HA `expire_after`. The cistern keeps `interval_sec: 0` (user decision). Details:
  `OVERVIEW_SDCARD.md`. To test: a device with e.g. `"interval_sec": 3600` on a derivative → one value
  per local hour, archive without gaps, HA not "unavailable".
* Unit: the cistern files configure `m³/h`; `"unit": "l/h"` works as well (units table: `l` = liter).
* While the cistern overflows (depth at max) the net flow is 0 regardless of the draw: the level
  derivative only shows draw when inflow < draw.
* Existing threshold `alarm_frate`: danger at -2.0 m³/h falling, hysteresis 0.4, damping 2: tune it
  from the observations on the new page.

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
