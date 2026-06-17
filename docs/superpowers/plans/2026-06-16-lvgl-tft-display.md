# LVGL Stock-TFT Display — Implementation Plan

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax. Build gate per task; commit after each. Spec: `docs/superpowers/specs/2026-06-16-lvgl-tft-display-design.md`.

**Goal:** A single LVGL nightshift screen replacing the TFT_eSPI `screens/` renderer on the stock ILI9488 TFT (`openevse_wifi_tft_v1`), showing the same data as the original, read-only.

**Architecture:** New gate `ENABLE_SCREEN_LVGL_TFT`; a drop-in `LcdTask` (`lcd_lvgl.{h,cpp}`) with the same public API, driving LVGL via a productionized version of the spike's panel + screen. Internal-DRAM single partial buffer (no PSRAM), blocking flush (no DMA).

**Tech Stack:** ESP32 (Arduino/PlatformIO, core-3), LVGL 8.3.9, TFT_eSPI 2.5.43 (ILI9488), MicroTasks.

**Build gates (run from repo root):**
- LVGL renderer builds: `~/.platformio/penv/bin/pio run -e openevse_wifi_tft_v1`
- Unaffected: `~/.platformio/penv/bin/pio run -e openevse_wifi_v1` (4 MB) and `-e openevse_p4`

---

## Task 1: platformio.ini — split gfx flags, repoint env to LVGL

**Files:** `platformio.ini` (`gfx_display_build_flags` block ~line 154; `[env:openevse_wifi_tft_v1]` ~637)

- [ ] **Step 1:** Split the combined `gfx_display_build_flags` into `gfx_display_hw_flags` (driver/pins/SPI, everything *except* `-D ENABLE_SCREEN_LCD_TFT`) and keep `gfx_display_build_flags = ${common.gfx_display_hw_flags}` + `-D ENABLE_SCREEN_LCD_TFT` (so any other consumer is byte-identical).
- [ ] **Step 2:** In `[env:openevse_wifi_tft_v1]`, replace `${common.build_flags_openevse_tft}` usage so it pulls `gfx_display_hw_flags` (not the `_build_flags` enable), and add:
  ```ini
  -D ENABLE_SCREEN_LVGL_TFT
  -D LV_CONF_INCLUDE_SIMPLE
  -D LV_CONF_SUPPRESS_DEFINE_CHECK
  -I src/lvgl_tft
  ```
  Add `lvgl/lvgl@^8.3.9` to that env's `lib_deps`. (Cleanest: give `build_flags_openevse_tft` the hw flags + peripheral flags, and move the `-D ENABLE_SCREEN_LCD_TFT` out so the env picks its renderer.)
- [ ] **Step 3:** Build `-e openevse_wifi_tft_v1` → expect FAIL only on missing `lcd_lvgl.h` later; for now (no src yet) it should still compile the old way if the gate isn't referenced. Defer green build to Task 5. Build `-e openevse_p4` and `-e openevse_wifi_v1` → PASS (proves the split didn't disturb them).
- [ ] **Step 4:** Commit.

## Task 2: LVGL config + panel bring-up (lift from spike)

**Files:** create `src/lvgl_tft/lv_conf.h`, `src/lvgl_tft/lvgl_panel.{h,cpp}`

- [ ] **Step 1:** `lv_conf.h` = spike's `src/lvgl_spike/lv_conf.h` verbatim (LVGL 8.3, `LV_COLOR_DEPTH 16`, `LV_COLOR_16_SWAP 1`, `LV_MEM_SIZE 48K`, Arduino tick, Montserrat 14/28/48, arc+label). Drop `LV_USE_PERF_MONITOR`/`LV_USE_MEM_MONITOR` (set 0) — those were spike instrumentation.
- [ ] **Step 2:** `lvgl_panel.h`: `bool lvgl_panel_begin();` (gate `ENABLE_SCREEN_LVGL_TFT`).
- [ ] **Step 3:** `lvgl_panel.cpp` = spike's fixed `lvgl_display.cpp` (single `MALLOC_CAP_INTERNAL` buffer `SCREEN_W*32`, `setRotation(1)`, blocking `pushPixels`, returns bool, serial heap log) but **owning the only `TFT_eSPI` instance** and exposing it (the screen needs none — it draws via LVGL). Keep `flush_cb` identical.
- [ ] **Step 4:** Build `-e openevse_wifi_tft_v1` → compiles (unused). Commit.

## Task 3: The charge screen (widget tree + setters)

**Files:** create `src/lvgl_tft/charge_screen.{h,cpp}` (from spike `charge_mock`, extended)

- [ ] **Step 1:** `charge_screen.h`: `void charge_screen_build();` + a single rich setter
  ```c
  struct ChargeScreenData {
    uint8_t evse_state; bool vehicle_connected; bool charging;
    float power_kw; int pilot_a; float volts; float amps;
    uint32_t elapsed_s; double session_wh;
    bool temp_valid; float temp_c;
    bool wifi_client; bool wifi_connected; int rssi; int sta_count;
    const char *datetime; const char *msg_line;
  };
  void charge_screen_update(const ChargeScreenData &d);
  ```
- [ ] **Step 2:** `charge_screen.cpp`: build the tree (ring, big value+unit, status word label, top-strip labels for datetime/temp/wifi/car, ELAPSED/DELIVERED/V·A tiles, message label). `charge_screen_update` maps `evse_state` → status word + ring color (CHARGING/CONNECTED/SLEEPING/DISABLED/STARTING/NOT CONNECTED/FAULT) using `openevse.h` constants, sets center value (kW vs pilot A), fills tiles, shows/hides temp + message. Nightshift palette (spike approximations; refine to [[p4-eez-project-theme]] hexes if handy).
- [ ] **Step 3:** Build → compiles. Commit.

## Task 4: `lcd_lvgl` LcdTask

**Files:** create `src/lcd_lvgl.{h,cpp}`

- [ ] **Step 1:** `lcd_lvgl.h`: `LcdTask` class gated `ENABLE_SCREEN_LVGL_TFT`, same shape as `lcd_tft.h` (Message inner class, `_head/_tail`, `begin/display×3/setWifiMode`, `setup/loop`). Include `evse_man.h`, `scheduler.h`, `manual.h`. Hold `EvseManager* _evse`, wifi mode flags, backlight deadline, message-line buffer/time.
- [ ] **Step 2:** `lcd_lvgl.cpp`: Message queue + `display(...)` mechanics lifted from `lcd_tft.cpp` (identical). `begin()` stores `&evse` and `MicroTask.startTask(this)`. `loop()`: on first run (after networking) `lvgl_panel_begin()` + `charge_screen_build()` + `pinMode(LCD_BACKLIGHT_PIN)`; drain message queue into the message-line string (with timed clear); assemble `ChargeScreenData` from `EvseManager` + WiFi + `localtime_r`; `charge_screen_update(d)`; backlight wake/timeout logic lifted from `ScreenManager` (`TFT_BACKLIGHT_TIMEOUT_MS`/`_CHARGING_THRESHOLD`); `lv_timer_handler()`; return ~next whole second (cap 1000 ms). `setWifiMode` stores flags + wakes backlight.
- [ ] **Step 3:** Define global `LcdTask lcd;` under the gate. Build (not linked until Task 5). Commit.

## Task 5: Wire `lcd.h`, integrate, build green

**Files:** `src/lcd.h`

- [ ] **Step 1:** Add the third branch at the top of the gate ladder:
  ```c
  #if ENABLE_SCREEN_LVGL_TFT
  #include "lcd_lvgl.h"
  #elif ENABLE_SCREEN_LCD_TFT
  #include "lcd_tft.h"
  #else
  ... existing char-LCD LcdTask ...
  #endif
  ```
  (Keep `extern LcdTask lcd;` outside the branches.)
- [ ] **Step 2:** Build `-e openevse_wifi_tft_v1` → **PASS** (full integration: `lcd` is the LVGL task; `main.cpp`/`net`/`ocpp` link unchanged). Note RAM% + free-heap.
- [ ] **Step 3:** Build `-e openevse_wifi_v1` and `-e openevse_p4` → PASS (unaffected).
- [ ] **Step 4:** Commit.

## Task 6: HW validation (bench, USB)

- [ ] **Step 1:** Flash `-e openevse_wifi_tft_v1 -t upload`; confirm `[panel] display up … free internal heap=` and no crash.
- [ ] **Step 2:** Eyeball each state: boot messages (OpenEVSE WiFi + version) → idle/not-connected (pilot A) → car connected → charging (kW + ring) → sleeping/disabled → a fault if reproducible. Check temp, time, Wi-Fi/RSSI, ELAPSED/DELIVERED/V·A, backlight timeout. Note any color-swap (flip `LV_COLOR_16_SWAP`).
- [ ] **Step 3:** Report; iterate on layout/colors per your eye. Commit fixes.

---

## Self-review notes
- Public `LcdTask` API identical across `lcd_lvgl.h` and `lcd_tft.h` so `main.cpp`/`net`/`ocpp` are untouched.
- `ENABLE_SCREEN_LVGL_TFT` ≠ `ENABLE_SCREEN_LVGL` (P4) — no collision; both can coexist in the tree.
- TFT_eSPI instance owned solely by `lvgl_panel.cpp`; `screens/`+`lcd_tft.cpp` not compiled in this env (no second owner).
- LVGL/TFT init on first `loop()`, not `setup()` (networking-breaks-display ordering, per existing renderer).
