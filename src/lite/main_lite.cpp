#ifdef OPENEVSE_LITE
#include <Arduino.h>
#include <WiFi.h>

#include "em_cmu.h"   // CMU_ClockEnable — GPIO clock for the PF11 reset-line drive
#include "em_gpio.h"  // GPIO_PinModeSet — hold the ATmega RESET deasserted (see setup())

#include "espal_lite.h"
#include "web_server_lite.h"
#include "lite_evse_backend.h"
#include "lite_config_store.h"
#include "lite_evse_manager.h"
#include "lite_clock.h"
#include "lite_energy_totals.h"
#include "manual.h"

#if defined(LITE_EVSE_BACKEND_JUICEBOX)
#include "juicebox_backend.h"
static JuiceBoxBackend s_backend(Serial);   // USART0 LOC1 (PE7=TX/PE6=RX) @ 9600 8N1
#else
#error "No lite EVSE backend selected (define LITE_EVSE_BACKEND_*)"
#endif

// Control seam: the manager owns the apply path; manual is the lifted canary
// claim client (referenced extern from web_server_lite.cpp for /override + status).
static LiteEvseManager s_manager(s_backend);
static LiteClock        s_clock;
static LiteEnergyTotals s_totals;
ManualOverride manual(s_manager);

// WiFi creds: real values arrive via PLATFORMIO_BUILD_FLAGS (-D LITE_WIFI_SSID=...).
// When unset, fall back to the placeholder *_DEFAULT macros from platformio.ini.
#ifndef LITE_WIFI_SSID
#define LITE_WIFI_SSID LITE_WIFI_SSID_DEFAULT
#endif
#ifndef LITE_WIFI_PASS
#define LITE_WIFI_PASS LITE_WIFI_PASS_DEFAULT
#endif

void setup()
{
  // ATmega EVSE-controller RESET (active-low) is wired to host GPIO PF11
  // (continuity-confirmed on the bench 2026-06-13). We previously never configured
  // PF11, so it floated at power-on (EFM32 GPIOs default to disabled/input) and the
  // line drifting/coupling low INTERMITTENTLY held the Atmel in reset — the "silent /
  // flapping comms" symptom. Drive it push-pull HIGH first thing, before anything
  // else, to hold the controller deasserted (running) for the whole session.
  CMU_ClockEnable(cmuClock_GPIO, true);
  GPIO_PinModeSet(gpioPortF, 11, gpioModePushPull, 1);

  // JuiceBox $-protocol line @ 115200 8N1 — HW-confirmed 2026-06-13 (clean $ES/$MD/$WR
  // frame decode at this rate; the earlier 9600 was a stale RAPI-era assumption). No
  // debug prints here: LibreTiny LT logging is LT_LEVEL_NONE so it can't corrupt framing.
  Serial.begin(115200);
  ESPAL.begin();

  WiFi.begin(LITE_WIFI_SSID, LITE_WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
  }

  s_backend.begin();
  lite_config_begin();              // mount FlashDB KVDB (kvs partition) before config load
  if (!lite_config_load_totals(s_totals)) {
    energy_totals_init(s_totals);   // first boot / key absent
  }
  web_server_lite_begin(s_manager, s_clock, s_totals); // loads config -> clamps -> seeds manager target

  // Now that Serial + the backend parser are up, give the Atmel a clean, synchronized
  // restart via its RESET line (PF11, active-low) so we capture its full boot burst —
  // the $HW/$FW/$PV identity and the $WC handshake nonce — from frame zero. (Held high
  // since the top of setup(), the Atmel already booted once before we were listening,
  // so its identity went unseen; this re-announces it with loop() about to run.)
  // Unconditional on every host boot (user-approved 2026-06-13): the WGM160P only
  // reboots on power-cycle/OTA/crash, so a clean comms re-sync is worth interrupting a
  // charge in those rare cases — the Atmel re-establishes its own safe state on reset.
  GPIO_PinOutClear(gpioPortF, 11);  // assert RESET — hold the Atmel
  delay(50);                        // well past the AVR min reset pulse width
  GPIO_PinOutSet(gpioPortF, 11);    // release RESET — Atmel boots fresh
  delay(100);                       // brief settle; loop() catches the boot frames
}

void loop()
{
  web_server_lite_loop();
  s_backend.loop();
  s_manager.loop();   // tick session-energy accumulator from live backend power
}
#endif
