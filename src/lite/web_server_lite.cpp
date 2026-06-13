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
#include "lite_config_store.h"
#include "lite_charge_policy.h"

#include "mongoose.h"

// Mongoose manager kept static to this TU.
static struct mg_mgr s_mgr;

// Live LiteEvseBackend handle stashed at begin() (the handler is a C-style callback
// and cannot capture, so a static pointer is how it reaches device state).
static LiteEvseBackend *s_backend = NULL;

// Active EVSE config cached in RAM so /status and GET /config never touch flash.
// Seeded at web_server_lite_begin() from the store (or defaults) and updated on POST.
static LiteEvseConfig s_cfg = { 32, 32 }; // {soft, hard} defaults (smallest JuiceBox sold)

// Build the /status JSON from the live backend.
static void build_status_json(String &out)
{
  StaticJsonDocument<512> doc;
  if (s_backend) {
    doc["state"]  = (int)s_backend->getState();
    doc["amp"]    = s_backend->getAmps();
    doc["power"]  = s_backend->getPower();
    doc["temp"]   = s_backend->getTemp();
    doc["fault"]  = s_backend->getFault();
    doc["online"] = s_backend->isOnline() ? 1 : 0;
    s_backend->addStatusFields(doc);
    doc["max_current_soft"] = s_cfg.max_current_soft;
    doc["max_current_hard"] = s_cfg.max_current_hard;
  }
  doc["free_heap"] = ESPAL.getFreeHeap();
  doc["uptime"]    = (uint32_t)(millis() / 1000);
  serializeJson(doc, out);
}

// Serialize the cached config as the canonical /config response body.
static void config_json(String &out)
{
  StaticJsonDocument<64> doc;
  doc["max_current_soft"] = s_cfg.max_current_soft;
  doc["max_current_hard"] = s_cfg.max_current_hard;
  serializeJson(doc, out);
}

// POST /config?max_current_soft=N&max_current_hard=M (either or both).
static void handle_config_post(struct mg_connection *nc, struct http_message *hm)
{
  char val[8];
  bool any = false;
  LiteEvseConfig cfg = s_cfg; // start from current, allow partial update

  if (mg_get_http_var(&hm->query_string, "max_current_hard", val, sizeof(val)) > 0) {
    cfg.max_current_hard = atoi(val);
    any = true;
  }
  if (mg_get_http_var(&hm->query_string, "max_current_soft", val, sizeof(val)) > 0) {
    cfg.max_current_soft = atoi(val);
    any = true;
  }

  if (!any) {
    const char *body = "no params";
    mg_send_head(nc, 400, strlen(body), "Content-Type: text/plain");
    mg_printf(nc, "%s", body);
    return;
  }

  cfg.max_current_hard = lite_clamp_service_max(cfg.max_current_hard);
  cfg.max_current_soft = lite_clamp_charge_current(cfg.max_current_soft, cfg.max_current_hard);

  bool saved = lite_config_save_evse(cfg);

  // Apply + cache even if persistence failed (best effort).
  s_cfg = cfg;
  if (s_backend) {
    s_backend->setChargeCurrent(cfg.max_current_soft);
  }

  String body;
  config_json(body);
  // 503 signals "applied but not persisted" so the caller knows it won't survive reboot.
  mg_send_head(nc, saved ? 200 : 503, body.length(), "Content-Type: application/json");
  mg_printf(nc, "%s", body.c_str());
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
  } else if (mg_vcmp(&hm->uri, "/config") == 0) {
    if (mg_vcmp(&hm->method, "POST") == 0) {
      handle_config_post(nc, hm);
    } else {
      String body;
      config_json(body);
      mg_send_head(nc, 200, body.length(), "Content-Type: application/json");
      mg_printf(nc, "%s", body.c_str());
    }
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

  // Load persisted config (or keep the 32/32 defaults), clamp, apply to the backend.
  if (!lite_config_load_evse(s_cfg)) {
    s_cfg = (LiteEvseConfig){ 32, 32 };
  }
  s_cfg.max_current_hard = lite_clamp_service_max(s_cfg.max_current_hard);
  s_cfg.max_current_soft = lite_clamp_charge_current(s_cfg.max_current_soft, s_cfg.max_current_hard);
  backend.setChargeCurrent(s_cfg.max_current_soft);

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
