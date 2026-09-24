// src/lvgl_tft/lvgl_panel.h — LVGL + ILI9488 (TFT_eSPI) bring-up for the stock
// OpenEVSE color display. No PSRAM, no DMA: one internal-DRAM partial buffer,
// blocking flush. Validated by the spike (spike/lvgl-tft).
#ifndef __LVGL_PANEL_H
#define __LVGL_PANEL_H

#ifdef ENABLE_SCREEN_LVGL_TFT

// Initialise TFT_eSPI + LVGL and register the display driver. Call once, from
// the LcdTask's first loop() (AFTER networking is up — it breaks the display if
// done earlier). Returns false if the draw buffer could not be allocated, in
// which case no display is registered and the caller must NOT build any UI.
bool lvgl_panel_begin();

// Set the backlight brightness, 0..100%. Safe to call before lvgl_panel_begin()
// (no-op until the LEDC channel is attached). Drives active vs. standby dimming.
void lvgl_panel_set_backlight(uint8_t pct);

#ifdef EPOXY_DUINO
enum LvglPanelDisplayMode {
  LVGL_PANEL_DISPLAY_HEADLESS = 0,
  LVGL_PANEL_DISPLAY_WINDOW
};

void lvgl_panel_set_display_mode(LvglPanelDisplayMode mode);
LvglPanelDisplayMode lvgl_panel_get_display_mode();
const char *lvgl_panel_get_display_mode_name(LvglPanelDisplayMode mode);
void lvgl_panel_pump();

// Write the current native LVGL framebuffer to a binary PPM image. Returns false
// if the headless framebuffer is unavailable or the file could not be written.
bool lvgl_panel_write_ppm(const char *path);
#endif


// --- Running LVGL on a task of its own (LVGL_TASK) --------------------------
//
// The ILI9488 forces TFT_eSPI's SPI_18BIT_DRIVER, which compiles the library's
// DMA subsystem out, so pushing pixels is a blocking, CPU-driven SPI write:
// ~121 ms for a full frame at 40 MHz on the S3, of which ~92 ms is wire time
// that no amount of CPU cleverness avoids. Run from loopTask -- the same thread
// that drains every web response and services MQTT -- that is a long stall.
//
// Moving only the PUSH to a second task does NOT fix it, and this was measured
// rather than assumed: LVGL renders the next chunk into the second draw buffer
// and then BUSY-WAITS for the first to come back -- the `while(draw_buf->flushing)`
// in lv_refr.c's draw_buf_flush(). loopTask blocks for the push regardless,
// having also paid for a task switch and lost throughput to core contention
// (measured 32.7 ms/s against 28.1 ms/s for doing it inline).
//
// So lv_timer_handler() itself moves. It runs on lvgl_task, and everything else
// that touches LVGL state takes the lock below. The task holds it for the length
// of a render (~65 ms once a second on the charge screen); the screen updates
// hold it for microseconds, so they collide only occasionally.
//
// !! EVERY LVGL CALL OUTSIDE lvgl_task MUST HOLD THIS LOCK. !! That includes the
// screen build/update/destroy entry points and anything reading LVGL state. Use
// LvglLock rather than the bare calls -- LcdTask::loop has many early returns.
#ifdef LVGL_TASK
// Returns false if there is no mutex yet -- lvgl_panel_begin() creates it, and
// LcdTask has LVGL calls to make on either side of that. Pair with the return
// value, never unconditionally, or an LvglLock taken before the mutex existed
// would give a mutex it never took.
bool lvgl_lock();
void lvgl_unlock();

// RAII. The underlying mutex is recursive, so nesting is safe.
class LvglLock
{
  public:
    LvglLock() : _held(lvgl_lock()) { }
    ~LvglLock() { if(_held) { lvgl_unlock(); } }
    LvglLock(const LvglLock &) = delete;
    LvglLock &operator=(const LvglLock &) = delete;
  private:
    bool _held;
};
#else
// LVGL runs on the caller; the guard exists so callers need no #ifdef.
class LvglLock { };
#endif

#endif // ENABLE_SCREEN_LVGL_TFT
#endif // __LVGL_PANEL_H
