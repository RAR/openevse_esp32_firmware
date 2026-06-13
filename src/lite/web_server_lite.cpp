#ifdef OPENEVSE_LITE
#include <Arduino.h>
#include <ArduinoJson.h>

// IMPORTANT include order: pull in all the C++ / ArduinoJson headers FIRST,
// then mongoose.h LAST. mongoose.h -> platform_custom.h forces
// LWIP_COMPAT_SOCKETS=1, which makes lwIP define function-like macros named
// bind/read/write/send/recv. Those clobber std::bind and the Print/Stream
// read()/write() method declarations if any C++ header is parsed afterwards.
// Including them before mongoose keeps the lwIP compat macros scoped to the
// mongoose code that actually wants the bare socket names.
#include "lite_evse_backend.h"  // ArduinoJson before mongoose — see above
#include "web_server_lite.h"
#include "espal_lite.h"

#include "mongoose.h"

// Mongoose manager kept static to this TU.
static struct mg_mgr s_mgr;

// Live LiteEvseBackend handle stashed at begin() (the handler is a C-style callback
// and cannot capture, so a static pointer is how it reaches device state).
static LiteEvseBackend *s_backend = NULL;

// Build the /status JSON from the live backend.
static void build_status_json(String &out)
{
  StaticJsonDocument<256> doc;
  if (s_backend) {
    doc["state"]  = (int)s_backend->getState();
    doc["amp"]    = s_backend->getAmps();
    doc["power"]  = s_backend->getPower();
    doc["temp"]   = s_backend->getTemp();
    doc["fault"]  = s_backend->getFault();
    doc["online"] = s_backend->isOnline() ? 1 : 0;
    s_backend->addStatusFields(doc);
  }
  doc["free_heap"] = ESPAL.getFreeHeap();
  doc["uptime"]    = (uint32_t)(millis() / 1000);
  serializeJson(doc, out);
}

// MG_ENABLE_CALLBACK_USERDATA=1 (default in this mongoose build), so handlers
// take a 4th void* user_data argument.
static void ev_handler(struct mg_connection *nc, int ev, void *p, void *user_data)
{
  (void)user_data;
  if (ev != MG_EV_HTTP_REQUEST) {
    return;
  }

  struct http_message *hm = (struct http_message *)p;

  if (mg_vcmp(&hm->uri, "/status") == 0) {
    String body;
    build_status_json(body);
    mg_send_head(nc, 200, body.length(), "Content-Type: application/json");
    mg_printf(nc, "%s", body.c_str());
  } else if (mg_vcmp(&hm->uri, "/") == 0) {
    const char *body = "openevse-lite";
    mg_send_head(nc, 200, strlen(body), "Content-Type: text/plain");
    mg_printf(nc, "%s", body);
  } else {
    const char *body = "not found";
    mg_send_head(nc, 404, strlen(body), "Content-Type: text/plain");
    mg_printf(nc, "%s", body);
  }

  nc->flags |= MG_F_SEND_AND_CLOSE;
}

void web_server_lite_begin(LiteEvseBackend &backend)
{
  s_backend = &backend;
  mg_mgr_init(&s_mgr, NULL);
  // mg_bind takes (mgr, addr, handler, user_data) when MG_ENABLE_CALLBACK_USERDATA=1.
  // No Serial.print on failure — Serial is the JuiceBox $-protocol line this slice.
  struct mg_connection *c = mg_bind(&s_mgr, "80", ev_handler, NULL);
  if (c) {
    mg_set_protocol_http_websocket(c);
  }
}

void web_server_lite_loop()
{
  mg_mgr_poll(&s_mgr, 0);
}
#endif
