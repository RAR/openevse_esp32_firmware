#ifdef ENABLE_SD_CARD

#include <Arduino.h>
#include <SD_MMC.h>
#include <driver/gpio.h>

#include "sd_card.h"
#include "debug.h"

#ifndef SD_PIN_CLK
#define SD_PIN_CLK  38
#endif
#ifndef SD_PIN_CMD
#define SD_PIN_CMD  39
#endif
#ifndef SD_PIN_D0
#define SD_PIN_D0   40
#endif
// Card-detect. -1 disables the check and assumes a card is present.
#ifndef SD_PIN_CD
#define SD_PIN_CD   41
#endif

// 1-bit SDMMC tops out well below the 4-bit ceiling, and this is an append-mostly
// workload at one 32-byte record a minute, so there is nothing to gain from
// pushing the clock on unproven wiring.
#ifndef SD_MMC_FREQ_KHZ
#define SD_MMC_FREQ_KHZ 20000
#endif

static bool _mounted = false;
// Set whenever card-detect has read "absent" since the last mount. A mount is
// only attempted on the rising edge of detect, so a card that is present but
// unmountable is tried once, not once a minute.
static bool _seen_absent = false;
static uint32_t _generation = 0;
static const char *_status = "not probed";
static uint64_t _size = 0, _used = 0;
static unsigned long _usage_at = 0;
#define SD_USAGE_REFRESH_MS (5UL * 60UL * 1000UL)

static void refresh_usage()
{
  _size = SD_MMC.cardSize();
  _used = SD_MMC.usedBytes();
  _usage_at = millis();
}

uint64_t sd_card_size() { return _mounted ? _size : 0; }
uint64_t sd_card_used() { return _mounted ? _used : 0; }

bool sd_card_detected()
{
#if SD_PIN_CD >= 0
  return digitalRead(SD_PIN_CD) == LOW;   // switch shorts to GND on insertion
#else
  return true;
#endif
}

bool sd_card_begin()
{
#if SD_PIN_CD >= 0
  // Internal pull-up is load-bearing on v1.2: the detect switch only pulls down,
  // and R33's pull-up is on DAT3, a different net. Harmless on v1.3, which fits
  // R42 externally. Safe to use pinMode here -- unlike the RAPI RX line, this pin
  // is not attached to a peripheral, so the core-3 peripheral manager has nothing
  // to tear down.
  pinMode(SD_PIN_CD, INPUT_PULLUP);
#endif

  if(!sd_card_detected()) {
    _seen_absent = true;
    _status = "no card";
    DBUGLN("[sd] no card detected");
    return false;
  }

  if(!SD_MMC.setPins(SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0)) {
    _status = "pin config failed";
    DBUGLN("[sd] setPins failed");
    return false;
  }

  // mode1bit = true: DAT1/DAT2 reach no GPIO on this board.
  // format_if_mount_failed = false, deliberately. An unreadable card is a
  // diagnosis, not an invitation to erase whatever the user had on it.
  if(!SD_MMC.begin(SD_MOUNT_POINT, true, false, SD_MMC_FREQ_KHZ)) {
    _status = "mount failed";
    DBUGLN("[sd] mount failed (card present). If this card works elsewhere, check "
           "R33 / DAT3 -- a DAT3 that floats low latches the card into SPI mode");
    return false;
  }

  if(SD_MMC.cardType() == CARD_NONE) {
    SD_MMC.end();
    _status = "no card";
    DBUGLN("[sd] mounted but no card type");
    return false;
  }

  _mounted = true;
  _seen_absent = false;
  _generation++;
  _status = "mounted";
  refresh_usage();
  DBUGF("[sd] mounted, %llu MB, %llu MB used", _size / (1024ULL * 1024ULL), _used / (1024ULL * 1024ULL));
  return true;
}

bool sd_card_mounted()
{
  return _mounted;
}

uint32_t sd_card_generation()
{
  return _generation;
}

bool sd_card_poll()
{
  if(!_mounted) {
    if(!sd_card_detected()) {
      _seen_absent = true;
      return false;
    }
    if(!_seen_absent) {
      // Present but not mounted, and detect never dropped: the earlier mount
      // attempt failed and nothing has changed. Wait for a fresh insertion.
      return false;
    }
    DBUGLN("[sd] card inserted, mounting");
    return sd_card_begin();
  }

  if(!sd_card_detected())
  {
    // Pulled while live. Drop the mount so the logger falls back to internal
    // flash rather than writing into a vanished filesystem.
    DBUGLN("[sd] card removed, unmounting and falling back to internal flash");
    SD_MMC.end();
    _mounted = false;
    _status = "removed";
    return false;
  }
  if(millis() - _usage_at > SD_USAGE_REFRESH_MS) {
    refresh_usage();
  }

  return true;
}

const char *sd_card_status()
{
  return _status;
}

#endif // ENABLE_SD_CARD
