# LVGL Stock-TFT Display — Design

**Status:** Approved 2026-06-16. Branch `feature/lvgl-tft-display` off `feature/esp32-modernization`.

## Goal

Replace the hand-rolled TFT_eSPI on-device UI for OpenEVSE's stock color display
(ESP32-WROOM / **ILI9488 320×480 SPI**, env `openevse_wifi_tft_v1`) with a single
**LVGL** screen in the nightshift visual language. Same information the original
charge screen showed — restyled, modern. **No touch** (read-only display).

De-risked by the spike (`spike/lvgl-tft`, HW-validated 2026-06-16): LVGL renders
on this no-PSRAM hardware from a single internal-DRAM partial buffer; the ~9 fps
full-frame number is a bus ceiling (ILI9488 18-bit, no DMA) identical for any
renderer, and irrelevant for a ~1 Hz status screen with small dirty rects.

## Hardware (HW-confirmed)

- ESP32-D0WD-V3, 16 MB flash, **NO PSRAM** (`denky32`'s `BOARD_HAS_PSRAM` is a
  misconfig for this part, harmless because nothing allocates from SPIRAM).
- ILI9488 320×480, rotated to **480×320 landscape**; SPI @ 40 MHz, **no DMA**
  (18-bit driver disables it). Backlight on `TFT_BL` (GPIO 27). XPT2046 touch
  present but **unused**.

## Architecture

New build gate **`ENABLE_SCREEN_LVGL_TFT`** (distinct from the P4's
`ENABLE_SCREEN_LVGL`, a different MIPI-DSI path). Env `openevse_wifi_tft_v1`
switches from `ENABLE_SCREEN_LCD_TFT` → `ENABLE_SCREEN_LVGL_TFT`. The TFT_eSPI
`screens/` + `lcd_tft.cpp` stay in the tree, compiled out — a flag-flip fallback.

**Drop-in `LcdTask`.** `lcd.h` already selects its `LcdTask` definition by screen
gate. Add a third branch → `lcd_lvgl.{h,cpp}` exposing the **same public API**
(`begin(evse, scheduler, manual)`, the three `display(...)` overloads,
`setWifiMode(client, connected)`) so `main.cpp`, `net`, and `ocpp` link
unchanged. The boot messages (`main.cpp:239-240`) still work via the queue.

```
src/lcd.h                       (modify) third branch: ENABLE_SCREEN_LVGL_TFT -> lcd_lvgl.h
src/lcd_lvgl.h        (create)  LcdTask class (gate ENABLE_SCREEN_LVGL_TFT)
src/lcd_lvgl.cpp      (create)  task: message queue, LVGL pump, EvseManager poll, backlight
src/lvgl_tft/lv_conf.h          (create)  LVGL 8.3 config (16-bit, internal-RAM buffer, Arduino tick)
src/lvgl_tft/lvgl_panel.{h,cpp} (create)  LVGL + ILI9488 bring-up, internal-DRAM buffer, blocking flush
src/lvgl_tft/charge_screen.{h,cpp} (create) the one screen: widget tree + value/state setters
platformio.ini       (modify)  split gfx hw/renderer flags; repoint openevse_wifi_tft_v1 to LVGL
```

The panel + lv_conf are productionized straight from the spike (single
`MALLOC_CAP_INTERNAL` partial buffer ~30 KB, `setRotation(1)`, blocking
`pushPixels`). The screen widget tree starts from the spike's `charge_mock` and
gains the full data set + all states.

## The one screen (nightshift palette, refresh ~1 Hz)

Data source: `EvseManager` read directly (same getters the `screens/` use:
`getEvseState`, `getPower`, `getChargeCurrent`, `getVoltage`, `getAmps`,
`getSessionElapsed`, `getSessionEnergy`, `getTemperature(EVSE_MONITOR_TEMP_MONITOR)`,
`isVehicleConnected`, `isTemperatureValid`). State constants from `openevse.h`.

| Element | Content |
|---|---|
| Power ring + big center value | kW when `OPENEVSE_STATE_CHARGING`, else pilot **A** (`getChargeCurrent`) — mirrors `screen_charge.cpp` |
| Status word + ring color | CHARGING / CONNECTED / SLEEPING / DISABLED / STARTING / NOT CONNECTED / FAULT (faults = the 7 error states) |
| Top strip | date/time (`localtime_r`), EVSE temp (if valid), Wi-Fi icon-glyph + RSSI / AP station count, car-connected indicator |
| Stat tiles | ELAPSED (`HH:MM:SS`), DELIVERED (Wh, scaled), V·A (`getVoltage` / `getAmps`) |
| Message line | text from `lcd.display(...)` (boot/OTA/status) — a dedicated label, auto-cleared after the message's time |

State→appearance map covers every `OPENEVSE_STATE_*` the original handled; fault
states show FAULT + a warning color. Unknown/default → neutral.

## Backlight

Replicate `ScreenManager`'s behavior (it's user-facing): `TFT_BACKLIGHT_TIMEOUT_MS`
(600 s) wakes on state/vehicle change, stays on while charging above
`TFT_BACKLIGHT_CHARGING_THRESHOLD` and during faults, times off when idle.

## Reuse / EEZ decision

For a single 480×320 screen, hand-build the LVGL widget tree (the spike already
does) rather than retarget the P4's EEZ Studio export — simpler, maintainable.
Shared-with-P4 element is the **nightshift palette/fonts**, not generated code.
([[p4-eez-project-theme]] for the canonical hexes; spike's approximation is the
starting point.)

## Out of scope

Touch input; menus/multiple screens; settings UI; any change to the P4 path or
the char-LCD (`lcd.cpp`) path; reworking `EvseManager`.

## Testing

No host unit tests (display glue, like the existing `screens/`). Gate:
`pio run -e openevse_wifi_tft_v1` builds with the LVGL renderer; the 4 MB and
P4 envs build unaffected. Runtime: flash to the bench unit, eyeball each EVSE
state + boot messages + backlight timeout.

## Risk / mitigations

- **Internal-DRAM pressure** (no PSRAM): spike showed 113 KB free with WiFi up
  and a 30 KB buffer — comfortable. Watch free-heap log on first integration build.
- **Color byte order**: if colors look swapped, flip `LV_COLOR_16_SWAP` (spike note).
- **Init ordering**: LVGL/TFT must init *after* networking (the existing renderer
  notes this) — bring up on first `loop()`, not in `setup()`.
