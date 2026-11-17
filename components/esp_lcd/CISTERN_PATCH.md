# Local override of ESP-IDF `esp_lcd` (RGB restart fix)

This directory is a copy of `components/esp_lcd` from **ESP-IDF v5.4** (tag `v5.4`, commit `67c1de1`),
without `test_apps`. A project component with the same name replaces the IDF component at build time.
Only `rgb/esp_lcd_panel_rgb.c` and `rgb/include/esp_lcd_panel_rgb.h` differ from the original; every
change is marked with `[CISTERN PATCH]`. The complete diff is in [cistern_rgb_restart.patch](cistern_rgb_restart.patch).

## Problem

Symptom on the SenseCAP Indicator (ESP32-S3, ST7701S 480x480 RGB panel, bounce buffer mode, frame
buffers in PSRAM): the whole picture is suddenly shifted left by 16 px, the cut-off part reappears
on the right edge, and the shift stays until reboot. It happens at random moments, mostly while
LVGL renders a lot (scrolling, dialogs), and also with the unmodified Seeed firmware.

Cause (verified on the device on 2026-11-17):

1. Under PSRAM load the bounce buffer ISR (which copies 28.8 KB from the PSRAM frame buffer per DMA
   EOF) is occasionally late; a DMA EOF is missed and the LCD FIFO underruns.
2. In the VSYNC ISR the driver detects the wrong EOF count and restarts GDMA + LCD FIFO while the
   LCD engine keeps running (`lcd_rgb_panel_try_restart_transmission`).
3. On ESP32-S3 the restart uses a separate "restart link" that starts
   `LCD_FIFO_PRESERVE_SIZE_PX = LCD_LL_FIFO_DEPTH + 1 = 17` pixels into the bounce buffer, assuming
   these pixels are still waiting in the LCD FIFO. After an underrun the FIFO is empty, the pixels
   are lost, and the continuous stream stays shifted.

Evidence: `esp_lcd_rgb_panel_restart()` (same restart path) and `CONFIG_LCD_RGB_RESTART_IN_VSYNC`
did not fix a visible shift; a panel hardware reset + re-init did not fix it either; a full
`esp_lcd_panel_init()` (engine stop/reset/start) fixed it immediately.

## Changes

1. **Full restart in bounce buffer mode** (`lcd_rgb_panel_try_restart_transmission`): when a restart
   is needed and `CONFIG_LCD_RGB_RESTART_IN_VSYNC` is off, the driver now performs the same full start
   as at initialization (`lcd_rgb_panel_start_transmission`: GDMA reset, LCD engine stop + reset,
   FIFO reset, bounce buffer prefill, GDMA start, engine start). Worst case is one disturbed frame
   instead of a permanent shift. `lcd_rgb_panel_start_transmission` is placed in IRAM for this.
2. **Backport upstream c0fb05b** ("fix(rgb_lcd): resync bounce buffer index when restarting DMA",
   [esp-idf#19070](https://github.com/espressif/esp-idf/issues/19070)): `bb_eof_count` is reset on
   every restart and on every start.
3. **Backport upstream 201ad05** ("fix(rgb_lcd): recover from excess EOF counts in bounce buffer
   mode"): restart on `bb_eof_count != expect_eof_count` instead of `<`.
4. **Diagnostics**: `esp_lcd_rgb_panel_get_restart_count()` returns the number of restarts; the
   firmware prints it in the `[RENDER]` log line.

The restart link itself is unchanged in ESP-IDF master as of 2026-11; the 17-pixel skip is not fixed
upstream. Reported as [espressif/esp-idf#19452](https://github.com/espressif/esp-idf/issues/19452)
(related: [#19329](https://github.com/espressif/esp-idf/pull/19329), restart loop after a dropped EOF).

Field test 2026-11-17: under continuous scrolling the driver performed 2 restarts in ~10 minutes
(`RGB restarts total` in the `[RENDER]` log); both recovered without a visible shift.

## Updating ESP-IDF

When the project moves to another ESP-IDF version:

1. Check whether upstream fixed the restart link behaviour (search `LCD_FIFO_PRESERVE_SIZE_PX` and
   `lcd_rgb_panel_try_restart_transmission` in `components/esp_lcd/rgb/esp_lcd_panel_rgb.c`).
2. If not: copy the new `components/esp_lcd` (without `test_apps`) here and re-apply
   `cistern_rgb_restart.patch` (`patch -p1 < cistern_rgb_restart.patch` inside this directory),
   resolving conflicts by hand.
3. If yes: delete this directory so the IDF component is used again, and remove the restart count
   from the `[RENDER]` log in `main/lv_port.c`.
