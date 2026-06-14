#ifdef OPENEVSE_LITE
#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdlib.h>

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
#include "lite_override.h"
#include "lite_schedule.h"
#include <WiFi.h>
#include "lite_openevse_compat.h"

#include "mongoose.h"

// Reported as OpenEVSE `firmware`/`version` so the HA integration shows a value.
#ifndef LITE_FW_VERSION
#define LITE_FW_VERSION "lite-4"
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

// ---- /override (Slice 3b) volatile state (not persisted; resets on reboot) -------------
static LiteOverrideLimits s_ovrLimits;            // limits of the active override
static bool               s_ovrExpired  = false;  // a session limit fired -> sticky Disable
static bool               s_ovrEnabling = false;  // override resolves to Active (charging)
static bool               s_wasCharging = false;  // tracks the charge->idle falling edge

// ---- /schedule (Slice 4) -------------------------------------------------------------
static LiteSchedule s_schedule;            // persisted weekly schedule
static uint32_t     s_scheduleVersion = 0; // bumped on every mutation (exposed in /status)
static uint8_t      s_lastSchedState  = 0; // last schedule-resolved state applied (0/1/2)

static uint8_t sched_state_from_str(const char *s) {
  if (s && !strcmp(s, "active"))   return 1;
  if (s && !strcmp(s, "disabled")) return 2;
  return 0;
}
static const char *sched_state_str(uint8_t st) { return st == 1 ? "active" : "disabled"; }

// Serialize the whole schedule as a JSON array of {id,state,time,days}.
static void schedule_get_json(String &out) {
  // 16 events x {id,state,time,days[<=7]} -> ~2.1 KB worst case; size generously.
  StaticJsonDocument<2560> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint32_t i = 0; i < s_schedule.count && i < LITE_SCHEDULE_MAX_EVENTS; i++) {
    const LiteScheduleEvent &e = s_schedule.events[i];
    JsonObject o = arr.createNestedObject();
    o["id"] = e.id;
    o["state"] = sched_state_str(e.state);
    char tb[12]; lite_schedule_format_time(e.sec_of_day, tb, sizeof(tb));
    o["time"] = tb;
    JsonArray days = o.createNestedArray("days");
    for (int d = 0; d < 7; d++) if (e.day_mask & (1u << d)) days.add(lite_schedule_day_name(d));
  }
  serializeJson(doc, out);
}

// Parse one event from a JSON body. Returns false (and leaves *code) on validation error.
static bool schedule_parse(const char *body, size_t len, LiteScheduleEvent &e, int &code) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, body, len) != DeserializationError::Ok) { code = 400; return false; }
  if (!doc.containsKey("state") || !doc.containsKey("time") || !doc.containsKey("days")) {
    code = 400; return false;
  }
  memset(&e, 0, sizeof(e));
  uint8_t st = sched_state_from_str(doc["state"]);
  if (st == 0) { code = 400; return false; }
  e.state = st;
  uint32_t sod;
  if (!lite_schedule_parse_time(doc["time"], sod)) { code = 400; return false; }
  e.sec_of_day = sod;
  uint8_t mask = 0;
  for (JsonVariant v : doc["days"].as<JsonArray>()) {
    int di = lite_schedule_day_index(v.as<const char *>());
    if (di < 0) { code = 400; return false; }
    mask |= (uint8_t)(1u << di);
  }
  if (mask == 0) { code = 400; return false; }
  e.day_mask = mask;
  // id: client-provided, else auto-assign max+1 (1 when empty).
  if (doc.containsKey("id") && (uint32_t)doc["id"] != 0) {
    e.id = (uint32_t)doc["id"];
  } else {
    uint32_t mx = 0;
    for (uint32_t i = 0; i < s_schedule.count && i < LITE_SCHEDULE_MAX_EVENTS; i++)
      if (s_schedule.events[i].id > mx) mx = s_schedule.events[i].id;
    e.id = mx + 1;
  }
  return true;
}

// Parse a trailing /schedule/<id> id from the URI (0 if none).
static uint32_t schedule_id_from_uri(struct http_message *hm) {
  const char *pfx = "/schedule/";
  size_t pl = strlen(pfx);
  if (hm->uri.len > pl && memcmp(hm->uri.p, pfx, pl) == 0) {
    return (uint32_t)strtoul(hm->uri.p + pl, NULL, 10);
  }
  char idbuf[12];
  if (mg_get_http_var(&hm->query_string, "id", idbuf, sizeof(idbuf)) > 0)
    return (uint32_t)strtoul(idbuf, NULL, 10);
  return 0;
}

static void handle_schedule(struct mg_connection *nc, struct http_message *hm) {
  int code = 200;
  String body;
  if (mg_vcmp(&hm->method, "POST") == 0) {
    LiteScheduleEvent e;
    if (schedule_parse(hm->body.p, hm->body.len, e, code)) {
      if (lite_schedule_upsert(s_schedule, e)) {
        bool saved = lite_config_save_schedule(s_schedule);
        s_scheduleVersion++;
        code = saved ? 201 : 503;
        StaticJsonDocument<64> r; r["id"] = e.id;
        serializeJson(r, body);
      } else {
        code = 507; body = "{\"msg\":\"Schedule full\"}";
      }
    } else {
      body = "{\"msg\":\"Bad schedule event\"}";
    }
  } else if (mg_vcmp(&hm->method, "DELETE") == 0) {
    uint32_t id = schedule_id_from_uri(hm);
    if (id != 0 && lite_schedule_remove(s_schedule, id)) {
      lite_config_save_schedule(s_schedule);
      s_scheduleVersion++;
      body = "{\"msg\":\"Deleted\"}";
    } else {
      code = 404; body = "{\"msg\":\"Not found\"}";
    }
  } else {
    schedule_get_json(body);   // GET
  }
  mg_send_head(nc, code, body.length(), "Content-Type: application/json");
  mg_printf(nc, "%s", body.c_str());
}

static const char *override_state_str(EvseState s) {
  switch (s) {
    case EvseState::Active:   return "active";
    case EvseState::Disabled: return "disabled";
    default:                  return nullptr;       // None -> omit
  }
}

// Claim a parsed override; capture its limits + enabling flag; clear the expired latch.
static void override_apply(EvseProperties &props, const LiteOverrideLimits &lim) {
  s_ovrLimits   = lim;
  s_ovrExpired  = false;
  s_ovrEnabling = (props.getState() == EvseState::Active);
  manual.claim(props);
}

static void override_clear() {
  s_ovrLimits   = LiteOverrideLimits();
  s_ovrExpired  = false;
  s_ovrEnabling = false;
  manual.release();
}

// Parse a JSON override body into props + limits. False on JSON parse error.
static bool override_parse(const char *body, size_t len,
                           EvseProperties &props, LiteOverrideLimits &lim) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body, len) != DeserializationError::Ok) return false;
  if (doc.containsKey("state")) {
    const char *s = doc["state"];
    if      (s && !strcmp(s, "active"))   props.setState(EvseState::Active);
    else if (s && !strcmp(s, "disabled")) props.setState(EvseState::Disabled);
    else if (s && !strcmp(s, "clear"))    props.setState(EvseState::None);
  }
  if (doc.containsKey("charge_current")) props.setChargeCurrent((uint32_t)doc["charge_current"]);
  if (doc.containsKey("max_current"))    props.setMaxCurrent((uint32_t)doc["max_current"]);
  if (doc.containsKey("auto_release"))   props.setAutoRelease((bool)doc["auto_release"]);
  if (doc.containsKey("energy_limit")) {
    lim.energy_limit_wh = (uint32_t)doc["energy_limit"]; lim.has_energy = true;
  }
  if (doc.containsKey("time_limit")) {
    lim.time_limit_s = (uint32_t)doc["time_limit"]; lim.has_time = true;
  }
  return true;
}

// Serialize the active override (or {}) into `out`.
static void override_get_json(String &out) {
  StaticJsonDocument<192> doc;
  if (manual.isActive()) {
    EvseProperties props;
    manual.getProperties(props);
    const char *st = override_state_str(props.getState());
    if (st) doc["state"] = st;
    if (props.hasChargeCurrent()) doc["charge_current"] = props.getChargeCurrent();
    if (props.hasMaxCurrent())    doc["max_current"]    = props.getMaxCurrent();
    doc["auto_release"] = props.isAutoRelease();
    if (s_ovrLimits.has_energy) doc["energy_limit"] = s_ovrLimits.energy_limit_wh;
    if (s_ovrLimits.has_time)   doc["time_limit"]   = s_ovrLimits.time_limit_s;
    doc["expired"] = s_ovrExpired;
  }
  serializeJson(doc, out);
}

static void handle_override(struct mg_connection *nc, struct http_message *hm) {
  int code = 200;
  String body;

  // Legacy bodyless convenience (same rationale as /config): ?state=active|disabled|
  // release|clear short-circuits to a claim/release regardless of method.
  char qstate[12];
  if (mg_get_http_var(&hm->query_string, "state", qstate, sizeof(qstate)) > 0) {
    if      (!strcmp(qstate, "active"))   { EvseProperties p(EvseState::Active);   LiteOverrideLimits l; override_apply(p, l); }
    else if (!strcmp(qstate, "disabled")) { EvseProperties p(EvseState::Disabled); LiteOverrideLimits l; override_apply(p, l); }
    else if (!strcmp(qstate, "release") || !strcmp(qstate, "clear")) { override_clear(); }
    override_get_json(body);
  } else if (mg_vcmp(&hm->method, "POST") == 0) {
    EvseProperties props;
    LiteOverrideLimits lim;
    if (override_parse(hm->body.p, hm->body.len, props, lim)) {
      override_apply(props, lim);
      code = 201; body = "{\"msg\":\"Created\"}";
    } else {
      code = 400; body = "{\"msg\":\"Failed to parse JSON\"}";
    }
  } else if (mg_vcmp(&hm->method, "DELETE") == 0) {
    override_clear();
    body = "{\"msg\":\"Deleted\"}";
  } else if (mg_vcmp(&hm->method, "PATCH") == 0) {
    manual.toggle();
    s_ovrLimits = LiteOverrideLimits(); s_ovrExpired = false;
    EvseProperties tp; s_ovrEnabling = manual.getProperties(tp) && tp.getState() == EvseState::Active;
    body = "{\"msg\":\"Updated\"}";
  } else {
    override_get_json(body);   // GET
  }

  mg_send_head(nc, code, body.length(), "Content-Type: application/json");
  mg_printf(nc, "%s", body.c_str());
}

// Build the /status JSON in the OpenEVSE local-API shape the firstof9/openevse
// Home Assistant integration consumes (it polls this every 60 s). Keys it reads
// but a JuiceBox can't provide (divert/shaper/OCPP/GFCI counts/etc.) are simply
// omitted — the integration's .get() defaults tolerate absent keys.
static void build_status_json(String &out)
{
  StaticJsonDocument<1280> doc;

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
  doc["schedule_version"] = s_scheduleVersion;

  if (s_clock && s_clock->valid()) {
    char isobuf[24];
    lite_clock_iso8601(s_clock->nowUtc(millis()), isobuf, sizeof(isobuf));
    doc["time"] = isobuf;                        // ISO-8601 UTC; omitted until first sync
  }
  if (s_totals) {
    doc["total_energy"]   = s_totals->lifetime_wh / 1000.0;  // kWh
    doc["total_day"]      = s_totals->day_wh    / 1000.0;
    doc["total_week"]     = s_totals->week_wh   / 1000.0;
    doc["total_month"]    = s_totals->month_wh  / 1000.0;
    doc["total_year"]     = s_totals->year_wh   / 1000.0;
    doc["total_switches"] = s_totals->switches;
  }

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

  char tzbuf[8];
  if (mg_get_http_var(&hm->query_string, "tz_offset_min", tzbuf, sizeof(tzbuf)) > 0) {
    LiteClockConfig cc; lite_config_load_clock(cc);
    cc.tz_offset_min = atoi(tzbuf);
    lite_config_save_clock(cc);
    if (s_clock) s_clock->setTzOffsetMinutes(cc.tz_offset_min);
  }
  char hostbuf[48];
  if (mg_get_http_var(&hm->query_string, "sntp_hostname", hostbuf, sizeof(hostbuf)) > 0) {
    LiteClockConfig cc; lite_config_load_clock(cc);
    cc.sntp_hostname = hostbuf;
    lite_config_save_clock(cc);
    s_sntpHost = hostbuf;
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
    handle_override(nc, hm);
  } else if ((hm->uri.len == 9  && memcmp(hm->uri.p, "/schedule", 9)  == 0) ||
             (hm->uri.len > 10 && memcmp(hm->uri.p, "/schedule/", 10) == 0)) {
    handle_schedule(nc, hm);
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

  if (!lite_config_load_schedule(s_schedule)) {
    memset(&s_schedule, 0, sizeof(s_schedule));
  }

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

  // Slice 3b: enforce override session limits + auto-release. Pure decision in
  // lite_override_evaluate; this is the thin wiring to the manager seam.
  if (s_mgr_ctrl) {
    bool charging    = s_mgr_ctrl->isCharging();
    bool fallingEdge = (s_wasCharging && !charging);
    s_wasCharging    = charging;
    if (manual.isActive() && !s_ovrExpired) {
      EvseProperties cur; bool haveCur = manual.getProperties(cur);
      LiteOverrideAction act = lite_override_evaluate(
          s_ovrLimits,
          s_mgr_ctrl->getSessionWattHours(),
          s_mgr_ctrl->getSessionElapsed(),
          true, s_ovrEnabling, haveCur && cur.isAutoRelease(), fallingEdge);
      if (act == LiteOverrideAction::Stop) {
        EvseProperties p(EvseState::Disabled);
        manual.claim(p);
        s_ovrExpired = true; s_ovrEnabling = false;   // sticky: stays stopped until DELETE
      } else if (act == LiteOverrideAction::Release) {
        override_clear();
      }
    }
  }

  // Slice 4: time-of-day schedule. Resolve the active scheduled state from the local
  // wall-clock and claim it via the Schedule client (priority below a manual override).
  // Released while the clock is unsynced (no time basis). Re-claim only on change.
  if (s_mgr_ctrl && s_clock) {
    uint8_t st = 0;
    if (s_clock->valid()) {
      uint32_t local = s_clock->nowLocal(millis());
      int dow = (int)(((local / 86400u) + 4u) % 7u);  // 1970-01-01 = Thursday; Sun=0
      uint32_t sod = local % 86400u;
      st = lite_schedule_active_state(s_schedule, dow, sod);
    }
    if (st != s_lastSchedState) {
      if (st == 0) {
        s_mgr_ctrl->release(EvseClient_OpenEVSE_Schedule);
      } else {
        EvseProperties p(st == 1 ? EvseState::Active : EvseState::Disabled);
        s_mgr_ctrl->claim(EvseClient_OpenEVSE_Schedule, EvseManager_Priority_Timer, p);
      }
      s_lastSchedState = st;
    }
  }

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
