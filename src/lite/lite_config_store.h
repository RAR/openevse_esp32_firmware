#pragma once
#ifdef OPENEVSE_LITE
#include <Arduino.h>

struct LiteWifiConfig { String ssid; String pass; };

// Contract: call lite_config_begin() once at boot before any load/save/erase;
// those calls assume LittleFS is already mounted.
bool lite_config_begin();                        // mount LittleFS; format if absent
bool lite_config_load_wifi(LiteWifiConfig &out); // false if no creds stored yet
bool lite_config_save_wifi(const LiteWifiConfig &in);
void lite_config_erase();                         // wipe the config file (eraseConfig)
#endif
