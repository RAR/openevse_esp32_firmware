# Standard-firmware → JuiceBox-lite lift audit (2026-06-13)

Question: how much of the standard OpenEVSE firmware can run on the JuiceBox 40
(WGM160P / EFM32GG11 / LibreTiny) with feature gates, vs. needing reimplementation?

**Headline:** a naive "compile out the bad parts" doesn't work — 30/57 `.cpp` lean on the
RAPI/`EvseManager` control object and 21/57 on the ArduinoMongoose C++ wrapper. But hard
*platform* (ESP-IDF) coupling is only ~6 files. The firmware routes through a small number of
**seams** (control object, config, HTTP/MQTT transport). Replace those and ~half the app's
*logic* lifts. **The binding constraint is the read-only `LiteEvseBackend` control seam**, not the
platform.

## Three strategic levers
1. **Control seam (BIGGEST unlock).** `LiteEvseBackend` is read-only today. The whole control
   cluster (manual/limit/temp_throttle/divert/current_shaper/scheduler/mqtt) drives the EVSE via
   `claim(client,priority,props)` / `release` / `clientHasClaim` / setpoint writes. Grow the seam
   into a **claim/setpoint surface** (minimal EvseManager claim-table + EvseProperties/State, or a
   "highest-priority setpoint wins" resolver) plus `setChargeCurrent/enable/disable`,
   `getChargeCurrent/MinCurrent/MaxHardwareCurrent/Voltage`, session counters
   (`getSessionElapsed/Energy`), temp validity, and MicroTasks `onStateChange`/`onSessionComplete`
   events. JuiceBox `$SL` can set current + pause/resume, so it's feasible. This single addition
   converts a cluster of REIMPLEMENTs into LIFT-WITH-GATINGs.
2. **Transport seam (cheap, half-done).** Standard handlers/clients talk only to ArduinoMongoose
   *wrapper objects* (`MongooseHttpServer/Client`, `MongooseMqttClient`) — never raw `mg_*`. Those
   wrappers are a thin veneer over the SAME Mongoose 5.x core lite already links. A small
   header-compatible shim makes handlers/clients lift as **unmodified files**. The lite tree
   ALREADY has a `MongooseMqttClient` shim + near-verbatim `mqtt.cpp` (proves it). HTTP-server shim
   ≈ 1–1.5 days (request accessors + `mbuf`-backed `Print` response + `on()` dispatch + digest auth).
3. **Config (mirror, don't lift).** `ConfigJson` is welded to `EEPROM.h` (binary blob). Keep the
   hand-rolled `lite_config_store` (LittleFS JSON) but **mirror upstream key NAMES/types** so lifted
   modules find what they expect. Store the `*_enabled` flags as plain JSON booleans under the same
   long names and reimplement the `inline bool config_*_enabled()` accessors (drop the `flags`
   uint32 bitmask machinery).

## Verdicts by category

### LIFT-CLEAN (copy ~verbatim)
`input_filter` (EMA math) · `ha_oauth` (pure, host-tested) · `home_battery` · `vehicle_extras`
· `embedded_files` · `tsdb_sample` (int16 math) · `root_ca` (PEM data) · `MongooseSntpClient` (libdep).

### LIFT-WITH-GATING
- After **control seam**: `manual` (59), `limit` (357; gate out SoC/Range types — no vehicle
  telemetry), `temp_throttle` (115; cleanest — `getTemp()` exists), `divert` (306; +grid/solar feed
  via MQTT, drop emoncms), `current_shaper` (216; pairs with divert).
- After **transport shim**: `mqtt` (625; +control retarget — this is Slice 3), `emoncms` (110),
  `web_server_config` (100), `web_server_static` (89; trimmed asset table), `web_server.h`,
  `web_server_time` (131; +NTP).
- After **NTP/config**: `time_man` (282; strip RAPI setTime; good for Slice 4),
  `energy_logger` (891; **the energy-history answer** — pure LittleFS rollups; swap `EvseManager*`
  input for a backend-fed `{wh,power,temp,ts}` sample), `event_log` (193; libc time + LittleFS).

### REIMPLEMENT (logic worth porting; file too welded)
`scheduler` (891; reuse week-graph + claim concepts — Slice 4) · `LedManagerTask` (784; keep the
~80-line state→color priority FSM for Slice 2 RGB, write a tiny EFM32 GPIO/PWM driver, drop
WS2812/LEDC) · `web_server` core (1470; trim to status/config/scan/AP-off/restart) ·
`net_manager` (868; ESP-welded — `WiFi.begin` suffices; AP provisioning later/Slice 5) ·
`energy_meter` (475; feed Wh accrual from `getPower()`) · `ota` (56; EFM32 dual-bank, not ESP) ·
`app_config` (781; schema-mirror only) · `ohm` (90; needs backend sleep verb) ·
`input` (199; superseded by lite `build_status_json`) · `mongoose_rng` (already done, Task #86).

### SKIP (out of scope for JuiceBox)
`ocpp` (753; public charging) · `tesla_client` (468; Tesla owner-API dead) ·
`home_assistant` (478; OAuth-pull retired → use HA→push via home_battery/vehicle_extras) ·
`web_server_home_assistant` · `rfid`/`rfid_user`/`pn532`/`web_server_rfid` (no reader) ·
`certificates`/`web_server_certificates`/TLS (defer to Slice 3+) ·
`tsdb_energy_logger`/`web_server_energy_tsdb` (esp_tsdb = ESP-IDF/xtensa, not portable) ·
`app_config_v1` (legacy ESP EEPROM migration) · `lcd`/`lcd_tft` (no display) ·
`web_server_claims`/`web_server_events`/`web_server_update`/`http_update` (until the backing
subsystem/OTA exists) · `evse_monitor`/`evse_man`/`main` (replaced by JuiceBoxBackend/main_lite;
extract the *claim-arbitration concept* from evse_man if multiple controllers ever coexist).

## Slice-1 seed config keys (names/types/defaults verbatim from app_config.cpp)
Core: `limit_default_type` (String "") · `limit_default_value` (u32 0) · `max_current_soft` (long;
service-max, lite owns it, pushes to backend) · `voltage` (u32 centivolt, 0=backend default) ·
`hostname` (String "openevse-"+shortId) · `time_zone` (String) · `default_state` (bool, charge-on-boot).
Pre-seed so later lifts need no migration: MQTT (`mqtt_enabled`,`mqtt_server`,`mqtt_port`=1883,
`mqtt_topic`,`mqtt_user`,`mqtt_pass`,`mqtt_announce_topic`,`mqtt_grid_ie`,`mqtt_solar`,`mqtt_live_pwr`,
`mqtt_vrms`) · divert (`divert_enabled`,`divert_type`=-1,`divert_PV_ratio`=1.1,smoothing 20/600,
`divert_min_charge_time`=600) · shaper (`current_shaper_enabled`,`current_shaper_max_pwr`=0,
smoothing 60, pause 300, maxinterval 120) · temp (`temp_throttle_enabled`,`temp_throttle_setpoint`,
`over_temp_shutdown`=72) · sntp (`sntp_enabled`,`sntp_hostname`="pool.ntp.org",`scheduler_start_window`=600).

## Roadmap impact
Insert **Slice 1.5: grow the control seam into a claim/setpoint surface** — it's the prerequisite
that turns limit/temp_throttle/divert/shaper/scheduler/mqtt from rebuilds into gated lifts, and
Slice 1's charge-limit already needs its first method (`setChargeCurrent`). Transport-shim is a
named, bounded task before Slice 3 (MQTT). GUI: full SPA (514 KiB) still doesn't fit → trimmed UI
lifts `web_server_config`/`web_server_static` over the HTTP shim.
