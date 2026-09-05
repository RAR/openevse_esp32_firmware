#ifdef ENABLE_SD_CARD

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "sdlog_store.h"
#include "sdlog_ring.h"
#include "sd_card.h"
#include "debug.h"

static FILE    *_fp = nullptr;
static bool     _ready = false;
static uint32_t _next_seq = 0;
static uint32_t _oldest_seq = 0;   // lowest sequence still held
static uint32_t _newest_ts = 0;

static bool slot_read(void *ctx, uint32_t index, uint8_t out[SDLOG_RECORD_BYTES])
{
  (void)ctx;
  if(_fp == nullptr) {
    return false;
  }
  if(fseek(_fp, (long)index * SDLOG_RECORD_BYTES, SEEK_SET) != 0) {
    return false;
  }
  return fread(out, SDLOG_RECORD_BYTES, 1, _fp) == 1;
}

// Create the ring at full size. Written in blocks rather than record-by-record:
// a million 32-byte writes through the FAT layer would take minutes.
static bool preallocate()
{
  DBUGF("[sdlog] creating %lu-record ring (%lu MB), this takes a moment",
        (unsigned long)SDLOG_CAPACITY,
        (unsigned long)((uint64_t)SDLOG_CAPACITY * SDLOG_RECORD_BYTES / (1024 * 1024)));

  unsigned long t0 = millis();
  FILE *fp = fopen(SDLOG_PATH, "wb");
  if(fp == nullptr) {
    return false;
  }

  // 0xFF, matching erased flash and what sdlog_is_blank() treats as "never
  // written". Zeroes would work too, but 0xFF is what an unwritten card region
  // tends to look like and keeps the two indistinguishable on purpose.
  static const size_t CHUNK = 4096;
  uint8_t *blank = (uint8_t *)malloc(CHUNK);
  if(blank == nullptr) {
    fclose(fp);
    return false;
  }
  memset(blank, 0xFF, CHUNK);

  uint64_t total = (uint64_t)SDLOG_CAPACITY * SDLOG_RECORD_BYTES;
  bool ok = true;
  for(uint64_t written = 0; written < total; written += CHUNK)
  {
    size_t want = (size_t)((total - written) < CHUNK ? (total - written) : CHUNK);
    if(fwrite(blank, 1, want, fp) != want) {
      ok = false;
      break;
    }
  }

  free(blank);
  fclose(fp);

  if(!ok) {
    DBUGLN("[sdlog] ring creation failed (card full?)");
    remove(SDLOG_PATH);
  } else {
    // The one slow step in bring-up; SDLOG_CAPACITY is the knob if this is unreasonable.
    DBUGF("[sdlog] ring created in %lu ms", millis() - t0);
  }
  return ok;
}

bool sdlog_store_begin()
{
  if(!sd_card_mounted()) {
    return false;
  }

  _fp = fopen(SDLOG_PATH, "r+b");
  if(_fp == nullptr)
  {
    if(!preallocate()) {
      return false;
    }
    _fp = fopen(SDLOG_PATH, "r+b");
    if(_fp == nullptr) {
      DBUGLN("[sdlog] could not reopen the ring after creating it");
      return false;
    }
  }

  // Refuse a file that is not the expected size rather than indexing off the end
  // of it. A capacity change would break the seq % capacity invariant that all
  // the recovery logic rests on.
  if(fseek(_fp, 0, SEEK_END) != 0) {
    fclose(_fp); _fp = nullptr;
    return false;
  }
  long size = ftell(_fp);
  if(size != (long)((uint64_t)SDLOG_CAPACITY * SDLOG_RECORD_BYTES)) {
    DBUGF("[sdlog] ring is %ld bytes, expected %llu -- refusing to use it",
          size, (unsigned long long)SDLOG_CAPACITY * SDLOG_RECORD_BYTES);
    fclose(_fp); _fp = nullptr;
    return false;
  }

  SdlogRingScan scan;
  if(!sdlog_ring_scan(slot_read, nullptr, SDLOG_CAPACITY, scan)) {
    DBUGLN("[sdlog] head recovery failed");
    fclose(_fp); _fp = nullptr;
    return false;
  }

  _next_seq = scan.next_seq;
  _oldest_seq = (scan.next_seq > SDLOG_CAPACITY) ? (scan.next_seq - SDLOG_CAPACITY) : 0;
  _ready = true;

  if(scan.any_records) {
    DBUGF("[sdlog] resuming at seq %lu (head %lu, %lu probes, %lu damaged slots)",
          (unsigned long)scan.next_seq, (unsigned long)scan.newest_seq,
          (unsigned long)scan.probes, (unsigned long)scan.corrupt_seen);
  } else {
    DBUGLN("[sdlog] empty ring, starting at seq 0");
  }
  return true;
}

bool sdlog_store_ready()
{
  return _ready;
}

bool sdlog_store_append(uint32_t timestamp, const int16_t cols[SDLOG_RECORD_COLS])
{
  if(!_ready || _fp == nullptr) {
    return false;
  }

  SdlogRecord rec;
  rec.seq = _next_seq;
  rec.timestamp = timestamp;
  memcpy(rec.cols, cols, sizeof(rec.cols));

  uint8_t raw[SDLOG_RECORD_BYTES];
  sdlog_encode(rec, raw);

  uint32_t index = _next_seq % SDLOG_CAPACITY;
  if(fseek(_fp, (long)index * SDLOG_RECORD_BYTES, SEEK_SET) != 0 ||
     fwrite(raw, SDLOG_RECORD_BYTES, 1, _fp) != 1 ||
     fflush(_fp) != 0)
  {
    // Do not keep trying into a broken card: drop to not-ready so the caller
    // falls back to internal flash on this and every later sample.
    DBUGLN("[sdlog] append failed, falling back to internal flash");
    _ready = false;
    return false;
  }

  _next_seq++;
  _newest_ts = timestamp;
  if(_next_seq > SDLOG_CAPACITY) {
    _oldest_seq = _next_seq - SDLOG_CAPACITY;
  }
  return true;
}

void sdlog_store_end()
{
  if(_fp != nullptr) {
    fclose(_fp);
    _fp = nullptr;
  }
  _ready = false;
}

// Read one record by sequence, verifying it belongs where it was found.
static bool read_seq(uint32_t seq, SdlogRecord &rec)
{
  uint32_t index = seq % SDLOG_CAPACITY;
  uint8_t raw[SDLOG_RECORD_BYTES];
  if(!slot_read(nullptr, index, raw)) {
    return false;
  }
  if(!sdlog_decode(raw, rec)) {
    return false;
  }
  // The slot holds SOME intact record; make sure it is the lap we asked for and
  // not the previous one still sitting there.
  return rec.seq == seq;
}

bool sdlog_store_range(uint32_t &oldest, uint32_t &newest)
{
  if(!_ready || _next_seq == 0) {
    return false;
  }

  SdlogRecord rec;
  oldest = 0;
  newest = _newest_ts;

  // Walk forward from the nominal oldest until an intact record turns up; a
  // damaged tail should not present as "no history".
  for(uint32_t seq = _oldest_seq; seq < _next_seq && seq < _oldest_seq + SDLOG_RECS_PER_SECTOR * 4; seq++)
  {
    if(read_seq(seq, rec)) {
      oldest = rec.timestamp;
      break;
    }
  }

  if(newest == 0)
  {
    for(uint32_t back = 1; back <= SDLOG_RECS_PER_SECTOR * 4 && back <= _next_seq; back++)
    {
      if(read_seq(_next_seq - back, rec)) {
        newest = rec.timestamp;
        break;
      }
    }
  }

  return oldest != 0 || newest != 0;
}

// Find the lowest sequence whose timestamp is >= `ts`. Timestamps ascend with
// sequence, so this is a binary search -- the same reason the head scan is one.
static uint32_t seek_seq_for_ts(uint32_t ts)
{
  uint32_t lo = _oldest_seq;
  uint32_t hi = _next_seq;

  while(lo < hi)
  {
    uint32_t mid = lo + (hi - lo) / 2;

    SdlogRecord rec;
    uint32_t probe = mid;
    bool got = false;

    // Step forward over damage; the window only ever shrinks because the
    // narrowing below uses mid, never probe.
    for(uint32_t step = 0; step < SDLOG_RECS_PER_SECTOR && probe + step < hi; step++)
    {
      if(read_seq(probe + step, rec)) {
        got = true;
        break;
      }
    }

    if(got && rec.timestamp < ts) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }

  return lo;
}

bool sdlog_query_init(SdlogQuery &q, uint32_t start_ts, uint32_t end_ts)
{
  q.open = false;
  if(!_ready) {
    return false;
  }

  q.start_ts = start_ts;
  q.end_ts   = end_ts;
  q.next_seq = seek_seq_for_ts(start_ts);
  q.end_seq  = _next_seq;
  q.open     = true;
  return true;
}

bool sdlog_query_next(SdlogQuery &q, uint32_t &ts, int16_t cols[SDLOG_RECORD_COLS])
{
  if(!q.open) {
    return false;
  }

  while(q.next_seq < q.end_seq)
  {
    SdlogRecord rec;
    uint32_t seq = q.next_seq++;

    if(!read_seq(seq, rec)) {
      continue;   // torn or overwritten mid-read; skip rather than emit zeroes
    }
    if(rec.timestamp < q.start_ts) {
      continue;
    }
    if(rec.timestamp > q.end_ts) {
      q.open = false;
      return false;
    }

    ts = rec.timestamp;
    memcpy(cols, rec.cols, sizeof(rec.cols));
    return true;
  }

  q.open = false;
  return false;
}

void sdlog_query_close(SdlogQuery &q)
{
  q.open = false;
}

bool sdlog_query_count(uint32_t start_ts, uint32_t end_ts, uint32_t &count)
{
  SdlogQuery q;
  if(!sdlog_query_init(q, start_ts, end_ts)) {
    return false;
  }

  count = 0;
  uint32_t ts;
  int16_t cols[SDLOG_RECORD_COLS];
  while(sdlog_query_next(q, ts, cols)) {
    count++;
  }
  sdlog_query_close(q);
  return true;
}

#endif // ENABLE_SD_CARD
