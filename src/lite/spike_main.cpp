#include <Arduino.h>
#include <WiFi.h>          // LibreTiny Arduino WiFi
// platform_custom.h is pulled in by mongoose.h (CS_P_CUSTOM path) via -I lib/MongooseLite.
// No -include flag: LibreTiny's SCons base.py chokes on -include as a CCFLAGS tuple.
#include "mongoose.h"

static struct mg_mgr s_mgr;

// MG_ENABLE_CALLBACK_USERDATA=1 (default in this mongoose build), so handlers
// take a 4th void* user_data argument.
static void ev_handler(struct mg_connection *nc, int ev, void *p, void *user_data) {
  (void)user_data;
  if (ev == MG_EV_HTTP_REQUEST) {
    mg_send_head(nc, 200, 5, "Content-Type: text/plain");
    mg_printf(nc, "hello");
    nc->flags |= MG_F_SEND_AND_CLOSE;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[lite-spike] boot\n");

  WiFi.begin(LITE_WIFI_SSID, LITE_WIFI_PASS);
  // Spike-only: blocks forever on bad creds. T4/T6 replace this with config-stored creds + softAP fallback.
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print('.'); }

  // LibreTiny's arduino::IPAddress has no toString()/c_str(); use printTo() or raw bytes.
  IPAddress ip = WiFi.localIP();
  Serial.printf("\n[lite-spike] WiFi up, IP=%u.%u.%u.%u\n",
                ip[0], ip[1], ip[2], ip[3]);

  mg_mgr_init(&s_mgr, NULL);
  // mg_bind takes (mgr, addr, handler, user_data) when MG_ENABLE_CALLBACK_USERDATA=1.
  struct mg_connection *c = mg_bind(&s_mgr, "80", ev_handler, NULL);
  if (!c) { Serial.println("[lite-spike] mg_bind FAILED"); return; }
  mg_set_protocol_http_websocket(c);
  Serial.println("[lite-spike] HTTP listening on :80");
}

void loop() {
  mg_mgr_poll(&s_mgr, 100);
}
