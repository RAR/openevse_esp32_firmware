#include "heap_trap.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <string.h>

// Poison layout from components/heap/multi_heap_poisoning.c (comprehensive):
//   used block : [head canary 0xABBA1234][alloc_size] data... [tail canary 0xBAAD5678]
//   free block : first 8 bytes are the TLSF free-list links, the rest is 0xFE fill
//
// poison_head_t is {uint32_t head_canary; size_t alloc_size;} - EIGHT bytes on
// a 32-bit target, with no owner field. This originally assumed twelve, read
// alloc_size out of the first word of user data, and looked for the tail four
// bytes past the real one. On 2026-09-09 that reported a perfectly healthy
// 180-byte block as corrupt, because its first data word happened to be
// 0x00001004, and stopped the walk there - so a genuine corruption that IDF
// had just detected went unlocated.
#define HEAD_CANARY  0xABBA1234u
#define TAIL_CANARY  0xBAAD5678u
#define FREE_FILL    0xFEu
// "HTRP" plus a layout revision. The record lives in RTC_NOINIT memory and
// survives an OTA, so a build whose HeapTrapRecord layout differs from the
// one that wrote it will decode the old bytes at the wrong offsets and
// report convincing nonsense - which is exactly what happened on
// 2026-09-09, when adding total_blocks/located shifted everything after
// blocks_seen and a stale record came back with bad.addr 0x00000101.
// BUMP THIS whenever HeapTrapRecord or BlockRec changes.
#define TRAP_MAGIC   0x48545202u   // "HTR" + layout rev 2

// sizeof(poison_head_t) and sizeof(poison_head_t) + sizeof(poison_tail_t)
#define POISON_HEAD      8u
#define POISON_OVERHEAD  12u

struct BlockRec {
  uint32_t addr;
  uint32_t size;
  uint8_t  used;
  uint8_t  reason;     // 0 ok, 1 head canary, 2 tail canary, 3 free fill
  uint32_t words[16];  // first 16 words of the block
};

struct HeapTrapRecord {
  uint32_t magic;
  uint32_t uptime_s;
  uint32_t heap_start;
  uint32_t heap_size;
  uint32_t blocks_seen;
  uint32_t total_blocks;   // blocks walked in the whole heap
  uint8_t  located;        // the walk found a failing block of its own
  BlockRec prev;       // block immediately before the bad one (overflow suspect)
  BlockRec bad;
  BlockRec next;
  uint32_t prev_tail[8];
};

static RTC_NOINIT_ATTR HeapTrapRecord rec;

struct WalkCtx {
  bool have_prev;
  walker_block_info_t prev;
  bool found;
  bool want_next;
  uint32_t seen;
  uint32_t total;
};

static void fill(BlockRec &r, const walker_block_info_t &b, uint8_t reason)
{
  r.addr = (uint32_t)(uintptr_t)b.ptr;
  r.size = b.size;
  r.used = b.used ? 1 : 0;
  r.reason = reason;
  size_t n = b.size < sizeof(r.words) ? b.size / 4 : 16;
  memset(r.words, 0, sizeof(r.words));
  memcpy(r.words, b.ptr, n * 4);
}

static uint8_t check_block(const walker_block_info_t &b)
{
  const uint32_t *w = (const uint32_t *)b.ptr;
  if(b.used) {
    if(b.size < POISON_OVERHEAD) return 0;
    if(w[0] != HEAD_CANARY) return 1;
    uint32_t alloc = w[1];
    if(alloc + POISON_OVERHEAD > b.size) return 4;
    uint32_t tail;
    memcpy(&tail, (const uint8_t *)b.ptr + POISON_HEAD + alloc, 4);
    if(tail != TAIL_CANARY) return 2;
    return 0;
  }
  const uint8_t *p = (const uint8_t *)b.ptr;
  for(size_t i = 8; i < b.size; i++) {
    if(p[i] != FREE_FILL) return 3;
  }
  return 0;
}

static bool walker(struct walker_heap_info heap, walker_block_info_t block, void *user)
{
  WalkCtx *c = (WalkCtx *)user;
  c->total++;
  if(c->found) {
    if(c->want_next) {
      fill(rec.next, block, check_block(block));
      c->want_next = false;
    }
    // Keep walking to count the heap, so "located nothing" can be told
    // apart from "stopped at the first block".
    return true;
  }
  c->seen++;
  uint8_t reason = check_block(block);
  if(reason) {
    rec.heap_start = (uint32_t)(uintptr_t)heap.start;
    rec.heap_size = (uint32_t)(heap.end - heap.start);
    rec.blocks_seen = c->seen;
    fill(rec.bad, block, reason);
    if(c->have_prev) {
      fill(rec.prev, c->prev, 0);
      size_t n = c->prev.size / 4;
      size_t take = n < 8 ? n : 8;
      memset(rec.prev_tail, 0, sizeof(rec.prev_tail));
      memcpy(rec.prev_tail, (const uint32_t *)c->prev.ptr + (n - take), take * 4);
    } else {
      memset(&rec.prev, 0, sizeof(rec.prev));
    }
    memset(&rec.next, 0, sizeof(rec.next));
    c->found = true;
    c->want_next = true;
    rec.located = 1;
    return true;
  }
  c->prev = block;
  c->have_prev = true;
  return true;
}

void heap_trap_capture()
{
  WalkCtx ctx = {};
  memset(&rec, 0, sizeof(rec));
  rec.uptime_s = millis() / 1000;
  heap_caps_walk_all(walker, &ctx);
  rec.total_blocks = ctx.total;
  rec.magic = TRAP_MAGIC;
}

void heap_trap_tick()
{
  static uint32_t last = 0;
  if(millis() - last < 10000) return;
  last = millis();
  if(heap_caps_check_integrity_all(false)) return;
  heap_trap_capture();
  heap_caps_check_integrity_all(true);   // UART, for anyone listening
  abort();
}

bool heap_trap_present() { return rec.magic == TRAP_MAGIC; }
void heap_trap_clear() { rec.magic = 0; }

static void block_json(JsonObject o, const BlockRec &b)
{
  static const char *reasons[] = {"ok", "head_canary", "tail_canary", "free_fill",
                                  "alloc_size"};
  char buf[12];
  snprintf(buf, sizeof(buf), "0x%08x", (unsigned)b.addr); o["addr"] = buf;
  o["size"] = b.size;
  o["used"] = b.used;
  o["reason"] = reasons[b.reason < 5 ? b.reason : 0];
  JsonArray w = o.createNestedArray("words");
  for(int i = 0; i < 16; i++) { snprintf(buf, sizeof(buf), "%08x", (unsigned)b.words[i]); w.add(buf); }
}

void heap_trap_json(JsonDocument &doc)
{
  doc["present"] = heap_trap_present();
  if(!heap_trap_present()) return;
  char buf[12];
  doc["uptime_s"] = rec.uptime_s;
  snprintf(buf, sizeof(buf), "0x%08x", (unsigned)rec.heap_start); doc["heap_start"] = buf;
  doc["heap_size"] = rec.heap_size;
  doc["blocks_seen"] = rec.blocks_seen;
  doc["total_blocks"] = rec.total_blocks;
  // IDF's own integrity check is what fires the trap. When this is false
  // the walk below did NOT find the block IDF objected to, so "bad" is
  // meaningless and the record says so rather than looking like a finding.
  doc["located"] = rec.located ? true : false;
  block_json(doc.createNestedObject("prev"), rec.prev);
  JsonArray t = doc.createNestedArray("prev_tail");
  for(int i = 0; i < 8; i++) { snprintf(buf, sizeof(buf), "%08x", (unsigned)rec.prev_tail[i]); t.add(buf); }
  block_json(doc.createNestedObject("bad"), rec.bad);
  block_json(doc.createNestedObject("next"), rec.next);
}
