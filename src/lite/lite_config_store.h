#pragma once
#ifdef OPENEVSE_LITE
#include <Arduino.h>
#include "lite_energy_totals.h"
#include "lite_schedule.h"

struct LiteWifiConfig { String ssid; String pass; };

// Typed EVSE config. Key names mirror upstream app_config so later module lifts
// find what they expect. Each field persists as its own FlashDB KV blob.
struct LiteEvseConfig {
  int max_current_soft; // active charge-current setpoint (A) the keepalive advertises
  int max_current_hard; // service-max ceiling (A) — install rating; soft is clamped to this
};

// Contract: call lite_config_begin() once at boot before any load/save/erase.
// Backed by a FlashDB KVDB on the `kvs` FAL partition (0x1F0000+0x8000).
bool lite_config_begin();                         // fdb_kvdb_init on the kvs partition; true on success

bool lite_config_load_wifi(LiteWifiConfig &out);  // false if no ssid stored yet
bool lite_config_save_wifi(const LiteWifiConfig &in);

bool lite_config_load_evse(LiteEvseConfig &out);  // false if max_current_hard key absent (use defaults)
bool lite_config_save_evse(const LiteEvseConfig &in);

void lite_config_erase();                         // wipe WiFi creds (eraseConfig)

// Clock config (mirrors upstream keys: sntp_hostname/"sh", time_zone offset).
struct LiteClockConfig { String sntp_hostname; int tz_offset_min; };

bool lite_config_load_totals(LiteEnergyTotals &out);   // false if key absent (caller inits)
bool lite_config_save_totals(const LiteEnergyTotals &in);

bool lite_config_load_schedule(LiteSchedule &out);     // false if key absent (caller zero-inits)
bool lite_config_save_schedule(const LiteSchedule &in);

bool lite_config_load_clock(LiteClockConfig &out);     // fills defaults if keys absent
bool lite_config_save_clock(const LiteClockConfig &in);
#endif
