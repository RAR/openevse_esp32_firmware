#ifdef OPENEVSE_LITE
#include "lite_config_store.h"

#include <LittleFS.h>
#include <ArduinoJson.h>

// Slice-1 minimum config store: just the WiFi creds needed to re-associate
// across reboot. The full app_config/ConfigJson surface is not brought up this
// slice. Stored as a tiny JSON doc on LibreTiny LittleFS (mirrors the ESP32
// Arduino fs::FS API used elsewhere in this repo, e.g. tsdb_energy_logger.cpp).

static const char *LITE_WIFI_PATH = "/lite_wifi.json";

bool lite_config_begin()
{
  // format-on-fail: a blank/corrupt FS is reformatted so creds can be stored.
  return LittleFS.begin(true);
}

bool lite_config_load_wifi(LiteWifiConfig &out)
{
  if (!LittleFS.exists(LITE_WIFI_PATH)) {
    return false; // no creds stored yet
  }

  File f = LittleFS.open(LITE_WIFI_PATH, "r");
  if (!f) {
    return false;
  }

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    return false; // unparseable -> treat as "not stored"
  }

  const char *ssid = doc["ssid"] | "";
  if (ssid[0] == '\0') {
    return false; // no/blank ssid -> fall back to build-flag defaults
  }

  out.ssid = ssid;
  out.pass = doc["pass"] | "";
  return true;
}

bool lite_config_save_wifi(const LiteWifiConfig &in)
{
  StaticJsonDocument<256> doc;
  doc["ssid"] = in.ssid;
  doc["pass"] = in.pass;

  File f = LittleFS.open(LITE_WIFI_PATH, "w");
  if (!f) {
    return false;
  }

  bool ok = (serializeJson(doc, f) != 0);
  f.close();
  return ok;
}

void lite_config_erase()
{
  // no-op-safe: only remove if present (this replaces the ESP-only
  // esp_partition erase for the lite build).
  if (LittleFS.exists(LITE_WIFI_PATH)) {
    LittleFS.remove(LITE_WIFI_PATH);
  }
}
#endif
