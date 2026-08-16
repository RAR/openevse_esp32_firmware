// src/lvgl_tft/lv_conf.h — LVGL 8.3 config for the stock OpenEVSE ILI9488 TFT
// (ESP32-WROOM, no PSRAM). Selected via -I src/lvgl_tft on env openevse_wifi_tft_v1.
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// 16-bit colour. TFT_eSPI pushes via pushPixels on-device, so that path keeps
// the byte-swapped buffer order. The native headless build snapshots LVGL's
// pixels directly, so it must keep the host-order RGB565 bytes unswapped.
#define LV_COLOR_DEPTH 16
#ifdef EPOXY_DUINO
#define LV_COLOR_16_SWAP 0
#else
#define LV_COLOR_16_SWAP 1
#endif

// LVGL widget/render scratch pool. Static (BSS) array of this size — the partial
// draw buffer is separate and lives in internal DRAM (lvgl_panel.cpp). No PSRAM
// on this board, so it competes directly with the WiFi/TLS/Mongoose stack and
// the ~30 KB draw buffer.
#define LV_MEM_CUSTOM 0
// 32 KB, raised from 24 KB. The 24 KB figure rested on an 8.6 KB peak measured
// with the previous, sparser charge screen; the reworked screen adds a second
// arc, three tile containers and half again as many labels.
//
// Re-measured on hardware with this build: lv_used_max 30%, lv_frag_max 23% of
// 32 KB, i.e. a ~9.6 KB peak. That sample includes two live theme switches,
// which is the genuine worst case -- charge_screen_build() loads the new screen
// before deleting the old, so both widget trees are briefly allocated.
//
// 24 KB would still fit that peak (40%), but the margin is not worth reclaiming
// 8 KB on a unit with 80 KB+ free: undersizing does not fail gracefully. LVGL
// answers pool exhaustion with an assert loop that the task watchdog turns into
// a reboot, and this runs on the live charger.
#define LV_MEM_SIZE (32U * 1024U)

// Tick from Arduino millis() on-device. The native/EpoxyDuino host build advances
// LVGL explicitly from lcd_lvgl.cpp so the C-only LVGL sources don't need to
// include the C++ Arduino headers.
#ifdef EPOXY_DUINO
#define LV_TICK_CUSTOM 0
#else
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#endif

#define LV_DPI_DEF 130

// No on-screen instrumentation in production (those were spike overlays).
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

#define LV_USE_LOG 0
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

// Fonts. Sized for reading the panel from across a garage, not from a desk:
//   14  tile captions, host/IP strip      20  pilot line, kW unit, long state words
//   18  clock, status chips, SoC readout  28  state word
//   36  tile values                       48  live kW
// Each size is ~10-25 KB of flash; the 16 MB partition is at ~34%, so the trade
// is worth it. Revisit only if a 4 MB build ever needs this env.
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// lv_label_set_text_fmt()/lv_snprintf need this for %f.
#define LV_SPRINTF_USE_FLOAT 1

// Widgets used: label, arc, bar (boot-splash progress).
#define LV_USE_ARC 1
#define LV_USE_LABEL 1
#define LV_USE_BAR 1

// QR code (setup screen — phone scans to join the softAP).
#define LV_USE_QRCODE 1

#endif // LV_CONF_H
