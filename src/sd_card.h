// microSD card presence and mount, for the ESP32-S3 LCD board.
//
// J2 and R33 are fitted (both revisions). The socket is wired for SDMMC 1-bit --
// CLK, CMD and D0 only -- because DAT1/DAT2 reach no GPIO on either version, so
// 1-bit is not a fallback here, it is the only mode.
//
// R33 holds DAT3 high and is functional, not decoration: a DAT3 that floats low
// during init latches the card into SPI mode, where it never answers SDMMC. That
// presents as a dead socket on good hardware, so it is the first thing to check
// if a mount fails on a card that works elsewhere.
//
// Card-detect (IO41) is active low and needs a pull-up -- internal on v1.2,
// external R42 on v1.3. Swapping a card means opening the enclosure (the socket
// mouth is 1.46 mm inboard and push-push needs a full card length of travel), so
// insertion is treated as a boot-time fact. Removal is still watched, because
// that is a failure mode rather than a workflow: pulling a live card must degrade
// to internal flash, not wedge the logger.
#ifndef __SD_CARD_H
#define __SD_CARD_H

#include <stdbool.h>
#include <stdint.h>

#ifndef SD_MOUNT_POINT
#define SD_MOUNT_POINT "/sdcard"
#endif

#ifdef ENABLE_SD_CARD

// Probe card-detect and, if a card is there, mount it. Safe to call when no card
// is fitted; returns false and leaves the system on internal flash.
bool sd_card_begin();

// True if the card is mounted and has not been pulled since.
bool sd_card_mounted();

// True if the card-detect switch currently reads "card present". Independent of
// whether the mount succeeded, so a card that is physically in but unreadable can
// be told apart from an empty slot in the logs.
bool sd_card_detected();

// Re-read card-detect and unmount if the card has gone. Call from a slow poll;
// returns true if the card is still usable.
bool sd_card_poll();

// Human-readable state for /status and the boot log.
const char *sd_card_status();

// Card capacity and FAT usage in bytes (0 when not mounted). usedBytes() walks
// the FAT, which on a 32 GB card is not free, so the figures are cached at
// mount and refreshed from sd_card_poll() at most every few minutes.
uint64_t sd_card_size();
uint64_t sd_card_used();

#else

static inline bool sd_card_begin() { return false; }
static inline bool sd_card_mounted() { return false; }
static inline bool sd_card_detected() { return false; }
static inline bool sd_card_poll() { return false; }
static inline const char *sd_card_status() { return "unsupported"; }
static inline uint64_t sd_card_size() { return 0; }
static inline uint64_t sd_card_used() { return 0; }

#endif // ENABLE_SD_CARD

#endif // __SD_CARD_H
