#ifdef OPENEVSE_LITE
#include <Arduino.h>
#include <WiFi.h>

#include "espal_lite.h"
#include "web_server_lite.h"
#include "lite_evse_backend.h"

#if defined(LITE_EVSE_BACKEND_JUICEBOX)
#include "juicebox_backend.h"
static JuiceBoxBackend s_backend(Serial);   // USART0 LOC1 (PE7=TX/PE6=RX) @ 9600 8N1
#else
#error "No lite EVSE backend selected (define LITE_EVSE_BACKEND_*)"
#endif

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
  web_server_lite_begin(s_backend);
}

void loop()
{
  web_server_lite_loop();
  s_backend.loop();
}
#endif
