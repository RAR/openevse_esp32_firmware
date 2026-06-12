#ifdef OPENEVSE_LITE
#include <Arduino.h>
#include <WiFi.h>          // LibreTiny Arduino WiFi
#include <MicroTasks.h>

#include "espal_lite.h"
#include "web_server_lite.h"

// RAPI line for the lite build: USART0 LOC1 (PE7=TX/PE6=RX) is the only USART
// the LibreTiny EFM32 fork wires, and it's where the OpenEVSE controller lives.
// debug.h (pulled via evse_man.h) defines RAPI_PORT == SerialEvse, the firmware's
// StreamSpy wrapper around the raw serial; debug_lite.cpp wires SerialEvse to
// Serial @ 9600 8N1 for us. There is NO debug console this slice (any
// Serial.print* would corrupt RAPI framing). Observability = WiFi + curl /status.
#include "debug.h"        // RAPI_PORT == SerialEvse; extern SerialEvse/SerialDebug
#include "evse_man.h"
#include "event_log.h"

// WiFi creds: real values arrive via PLATFORMIO_BUILD_FLAGS (-D LITE_WIFI_SSID=...).
// When unset, fall back to the placeholder *_DEFAULT macros from platformio.ini. Using a
// separate default macro (rather than re-defining LITE_WIFI_SSID in the .ini) keeps the real
// value from colliding with a placeholder on the command line — no "redefined" warning.
#ifndef LITE_WIFI_SSID
#define LITE_WIFI_SSID LITE_WIFI_SSID_DEFAULT
#endif
#ifndef LITE_WIFI_PASS
#define LITE_WIFI_PASS LITE_WIFI_PASS_DEFAULT
#endif

EventLog eventLog;
EvseManager evse(RAPI_PORT, eventLog);

void setup()
{
  // Bring up SerialEvse (the RAPI StreamSpy) on Serial @ 9600 8N1. No debug prints.
  debug_setup();

  ESPAL.begin();

  // WiFi creds come from the LITE_WIFI_SSID/PASS build flags this slice
  // (persistence deferred — the fork ships no LittleFS).
  WiFi.begin(LITE_WIFI_SSID, LITE_WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
  }

  evse.begin();
  web_server_lite_begin(evse);
}

void loop()
{
  web_server_lite_loop();

  // Drive the EVSE core exactly as the full firmware does each loop: pump the
  // RAPI sender, then tick the MicroTasks scheduler (which runs EvseManager /
  // EvseMonitor). Subsystems tied to excluded code (web_server/ota/ocpp/...)
  // are intentionally omitted.
  evse.getSender().loop();
  MicroTask.update();
}
#endif
