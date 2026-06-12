#ifdef OPENEVSE_LITE
#include "espal_lite.h"
#include "espal_lite_format.h"

// EFM32 DEVINFO unique id source: DEVINFO->UNIQUEL (bits 31:0) and DEVINFO->UNIQUEH (bits 63:32).
// LibreTiny exposes this via lt_cpu_get_uid64() — confirm exact symbol against the LibreTiny
// WGM160P port at integration time before relying on DEVINFO struct access directly.

void EspalLite::begin() {
  // Nothing to initialise on the EFM32 side for ESPAL itself; peripherals
  // are brought up by their own subsystems.
}

uint32_t EspalLite::getFreeHeap() {
  return (uint32_t)xPortGetFreeHeapSize();
}

String EspalLite::getShortId() {
  // lt_cpu_get_uid64() returns the 64-bit EFM32 DEVINFO unique id.
  uint64_t uid = lt_cpu_get_uid64();
  return String(lite_format_short_id(uid).c_str());
}

String EspalLite::getLongId() {
  uint64_t uid = lt_cpu_get_uid64();
  return String(lite_format_long_id(uid).c_str());
}

uint32_t EspalLite::getFlashChipSize() {
  // WGM160P has 2 MB internal flash.
  return lite_flash_size_bytes(0x200000);
}

String EspalLite::getChipInfo() {
  return String("EFM32GG11B820/WF200");
}

void EspalLite::reset() {
  NVIC_SystemReset();
}

void EspalLite::eraseConfig() {
  // TODO: delegate to Task-4 config store (LittleFS WiFi-cred store);
  // do not invent the store API here — wire up once Task 4 is merged.
}

EspalLite ESPAL;
#endif
