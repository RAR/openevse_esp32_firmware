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
#include "lite_evse_manager.h"
#include "lite_clock.h"
#include "lite_energy_totals.h"
#include "manual.h"
#include "web_server_lite.h"
#include "espal_lite.h"
#include "lite_config_store.h"
#include "lite_charge_policy.h"
#include <WiFi.h>
#include "lite_openevse_compat.h"

#include "mongoose.h"

// Reported as OpenEVSE `firmware`/`version` so the HA integration shows a value.
#ifndef LITE_FW_VERSION
#define LITE_FW_VERSION "lite-3a"
#endif

// Manual override is defined in main_lite.cpp; reached here for /override + status.
extern ManualOverride manual;

// Mongoose manager kept static to this TU.
static struct mg_mgr s_mgr;

// Live LiteEvseManager handle stashed at begin() (the handler is a C-style callback
// and cannot capture, so a static pointer is how it reaches device state).
static LiteEvseManager *s_mgr_ctrl = NULL;

static LiteClock        *s_clock  = nullptr;
static LiteEnergyTotals *s_totals = nullptr;
static String            s_sntpHost = "pool.ntp.org";
static unsigned long     s_lastSntpAttemptMs = 0;
static const unsigned long SNTP_RETRY_MS = 30000;  // re-attempt cadence while unsynced/resync-due

// Active EVSE config cached in RAM so /status and GET /config never touch flash.
// Seeded at web_server_lite_begin() from the store (or defaults) and updated on POST.
static LiteEvseConfig s_cfg = { 32, 32 }; // {soft, hard} defaults (smallest JuiceBox sold)

// Build the /status JSON in the OpenEVSE local-API shape the firstof9/openevse
// Home Assistant integration consumes (it polls this every 60 s). Keys it reads
// but a JuiceBox can't provide (divert/shaper/OCPP/GFCI counts/etc.) are simply
// omitted — the integration's .get() defaults tolerate absent keys.
static void build_status_json(String &out)
{
  StaticJsonDocument<1024> doc;

  if (s_mgr_ctrl) {
    LiteEvseState dev   = s_mgr_ctrl->getDeviceState();
    bool disabled       = (s_mgr_ctrl->getState() == EvseState::Disabled);

    // State (int + string), per the OpenEVSE contract.
    doc["state"]  = openevse_state_code(dev, disabled);
    doc["status"] = openevse_status_str(dev, disabled);

    // Live telemetry.
    int amp   = s_mgr_ctrl->getAmps();
    int power = s_mgr_ctrl->getPower();
    doc["amp"]               = amp;
    doc["pilot"]             = (uint32_t)s_mgr_ctrl->getChargeCurrent(); // advertised setpoint
    doc["power"]             = power;
    doc["tempt"]             = s_mgr_ctrl->getTemperature();
    doc["temp2"]             = s_mgr_ctrl->getTemperature();
    doc["max_current_soft"]  = (uint32_t)s_mgr_ctrl->getChargeCurrent();
    doc["max_current_hard"]  = s_mgr_ctrl->getMaxHardwareCurrent();
    doc["min_current_hard"]  = s_mgr_ctrl->getMinCurrent();
    doc["available_current"] = (uint32_t)s_mgr_ctrl->getMaxCurrent();
    doc["manual_override"]   = manual.isActive() ? 1 : 0;
    doc["mode"]              = "fast";

    // Derived voltage: power / amps while charging, else nominal 240 V. Keeps
    // HA's V x I ~= power consistent without a sensor the JuiceBox lacks.
    doc["voltage"] = (amp > 0 && power > 0) ? (power / amp) : 240;

    // Session energy (host-side accumulator).
    doc["wattsec"]        = (uint32_t)s_mgr_ctrl->getSessionWattSeconds();
    doc["watthour"]       = (uint32_t)s_mgr_ctrl->getSessionWattHours();
    doc["session_energy"] = (uint32_t)s_mgr_ctrl->getSessionWattHours();
    doc["elapsed"]        = (uint32_t)s_mgr_ctrl->getSessionElapsed();

    // Backend-specific extras (hw/fw/protocol/md/wc/wr/line + state_str). The
    // `wr` key carries the raw $WR fault string (the fault detail for state 8).
    s_mgr_ctrl->addStatusFields(doc);

    // Control/claim diagnostics (retained from the prior status body).
    doc["claims"] = (uint32_t)s_mgr_ctrl->activeClaimCount();
    doc["manual"] = manual.isActive() ? 1 : 0;
  }

  // Identity / system.
  doc["firmware"]  = LITE_FW_VERSION;
  doc["version"]   = LITE_FW_VERSION;
  { IPAddress ip = WiFi.localIP();
    char ipbuf[16];
    snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    doc["ipaddress"] = ipbuf; }
  doc["ssid"]      = WiFi.SSID();
  doc["srssi"]     = WiFi.RSSI();
  doc["free_heap"] = ESPAL.getFreeHeap();
  doc["freeram"]   = ESPAL.getFreeHeap();
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

// GET or POST /config[?max_current_soft=N&max_current_hard=M].
// Params present (on EITHER method) => SET (clamp -> persist -> apply); absent
// => read current. Accepting the set on GET means no request body is ever
// required, so bodyless clients work: `curl 'http://ip/config?max_current_soft=20'`,
// evcc's generic-charger GET URLs, and the HA app all hit it without a body
// (a no-Content-Length POST otherwise wedges Mongoose waiting for a body).
static void handle_config(struct mg_connection *nc, struct http_message *hm)
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

  int status = 200;
  if (any) {
    cfg.max_current_hard = lite_clamp_service_max(cfg.max_current_hard);
    cfg.max_current_soft = lite_clamp_charge_current(cfg.max_current_soft, cfg.max_current_hard);

    bool saved = lite_config_save_evse(cfg);
    // Apply + cache even if persistence failed (best effort).
    s_cfg = cfg;
    if (s_mgr_ctrl) {
      s_mgr_ctrl->setTargetMaxCurrent((uint32_t)cfg.max_current_hard);
      s_mgr_ctrl->setTargetChargeCurrent((uint32_t)cfg.max_current_soft);
    }
    // 503 signals "applied but not persisted" so the caller knows it won't survive reboot.
    if (!saved) {
      status = 503;
    }
  }

  // Always echo the (now-current) clamped config so the caller sees what took effect.
  String body;
  config_json(body);
  mg_send_head(nc, status, body.length(), "Content-Type: application/json");
  mg_printf(nc, "%s", body.c_str());
}

// Mongoose SNTP callback: a reply carries unix time as a double in mg_sntp_message.time.
static void sntp_ev_handler(struct mg_connection *nc, int ev, void *p, void *user_data) {
  (void)nc; (void)user_data;
  if (ev == MG_SNTP_REPLY && s_clock) {
    struct mg_sntp_message *m = (struct mg_sntp_message *)p;
    s_clock->setEpoch((uint32_t)m->time, millis());
  }
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
    handle_config(nc, hm);
  } else if (mg_vcmp(&hm->uri, "/override") == 0) {
    char val[12];
    if (mg_get_http_var(&hm->query_string, "state", val, sizeof(val)) > 0) {
      if (strcmp(val, "active") == 0) {
        EvseProperties p(EvseState::Active);  manual.claim(p);
      } else if (strcmp(val, "disabled") == 0) {
        EvseProperties p(EvseState::Disabled); manual.claim(p);
      } else if (strcmp(val, "release") == 0) {
        manual.release();
      }
    }
    String body;
    StaticJsonDocument<96> doc;
    doc["manual"] = manual.isActive() ? 1 : 0;
    doc["state"]  = (int)(s_mgr_ctrl ? (int)s_mgr_ctrl->getState() : 0);
    doc["charge_current"] = (uint32_t)(s_mgr_ctrl ? s_mgr_ctrl->getChargeCurrent() : 0);
    serializeJson(doc, body);
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

void web_server_lite_begin(LiteEvseManager &mgr, LiteClock &clock, LiteEnergyTotals &totals)
{
  s_mgr_ctrl = &mgr;
  s_clock    = &clock;
  s_totals   = &totals;

  LiteClockConfig cc;
  lite_config_load_clock(cc);
  s_sntpHost = cc.sntp_hostname;
  clock.setTzOffsetMinutes(cc.tz_offset_min);

  // Load persisted config (or keep the 32/32 defaults), clamp, seed the manager target.
  if (!lite_config_load_evse(s_cfg)) {
    s_cfg = (LiteEvseConfig){ 32, 32 };
  }
  s_cfg.max_current_hard = lite_clamp_service_max(s_cfg.max_current_hard);
  s_cfg.max_current_soft = lite_clamp_charge_current(s_cfg.max_current_soft, s_cfg.max_current_hard);
  mgr.setTargetMaxCurrent((uint32_t)s_cfg.max_current_hard);
  mgr.setTargetChargeCurrent((uint32_t)s_cfg.max_current_soft);

  mg_mgr_init(&s_mgr, NULL);
  // mg_bind takes (mgr, addr, handler, user_data) when MG_ENABLE_CALLBACK_USERDATA=1.
  // No Serial.print on failure — Serial is the JuiceBox $-protocol line this slice.
  struct mg_connection *c = mg_bind(&s_mgr, "80", ev_handler, NULL);
  if (c) {
    mg_set_protocol_http_websocket(c);
  }
}

// Reap connections wedged open by an incomplete request — e.g. a POST with no
// Content-Length, where Mongoose waits forever for a body that never arrives.
// Left unchecked these accumulate and exhaust the connection pool (symptom: the
// server stops answering even GETs). A normal request/response on this server
// lasts milliseconds, so any non-listening connection idle past this window is
// stuck. mg_time() is wall-clock seconds; last_io_time updates on each socket IO.
static const double LITE_CONN_IDLE_SECS = 10.0;

void web_server_lite_loop()
{
  mg_mgr_poll(&s_mgr, 0);

  unsigned long nowMs = millis();
  if (s_clock && s_clock->resyncDue(nowMs) &&
      (nowMs - s_lastSntpAttemptMs) >= SNTP_RETRY_MS) {
    s_lastSntpAttemptMs = nowMs;
    mg_sntp_get_time(&s_mgr, sntp_ev_handler, s_sntpHost.c_str(), NULL);
  }

  double now = mg_time();
  struct mg_connection *c, *tmp;
  for (c = mg_next(&s_mgr, NULL); c != NULL; c = tmp) {
    tmp = mg_next(&s_mgr, c); // fetch next before a possible close invalidates c
    if (!(c->flags & MG_F_LISTENING) &&
        (now - (double)c->last_io_time) > LITE_CONN_IDLE_SECS) {
      c->flags |= MG_F_CLOSE_IMMEDIATELY;
    }
  }
}
#endif
