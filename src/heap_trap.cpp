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
#define TRAP_MAGIC   0x48545203u   // "HTR" + layout rev 3

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

  // Which code was running. last_good is the most recent checkpoint that saw a
  // clean heap; fail is the checkpoint that did not. The corrupter ran between
  // them. iters_between counts loop() iterations across that span: 0 means both
  // checkpoints were in the same iteration, which is the precise case. Anything
  // larger means the span covers uninstrumented iterations, so the stage pair
  // brackets the write only loosely -- shorten the sweep interval and wait for
  // the next trip.
  uint8_t  last_good_stage;
  uint32_t last_good_ctx;      // task object, when last_good_stage is TASK
  uint32_t last_good_vtable;
  uint8_t  fail_stage;
  uint32_t fail_ctx;
  uint32_t fail_vtable;
  uint32_t iters_between;
  uint32_t sweep_us_last;
  uint32_t sweep_us_max;
  uint32_t sweeps;
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

// ---------------------------------------------------------------------------
// Checkpoints
//
// Running state lives in plain statics, not in the RTC record: heap_trap_capture()
// memsets the record, so the stage that was last clean has to survive somewhere
// else and be copied in afterwards.
// ---------------------------------------------------------------------------

// Coverage, not frequency, is what decides whether a trip tells us anything.
// A checkpoint only localises the write if the write happened between two
// checkpoints of the SAME pass, so the useful fraction of trips is roughly the
// fraction of wall-clock time spent inside an instrumented pass. Sweeping every
// 200 ms with ~6 ms of walking per pass covers 3% of the time - at an 8 h to
// 2 d MTBF that is years of waiting for one informative trip.
//
// So the default is to check on EVERY iteration (sweep_ms 0) and to cut the
// cost by checking only a few coarse stages instead of all of them. Coverage
// goes to ~100%, resolution drops to "one of four segments", and the mask can
// be widened to subdivide the guilty segment once it is known - including the
// per-task layer, which is off until it is worth its cost.
#define CP_BIT(stage) (1u << (stage))
#define CP_MASK_DEFAULT (CP_BIT(HEAP_TRAP_LOOP_TOP) | CP_BIT(HEAP_TRAP_MONGOOSE) | \
                         CP_BIT(HEAP_TRAP_WEB_SERVER) | CP_BIT(HEAP_TRAP_RAPI) | \
                         CP_BIT(HEAP_TRAP_MICROTASK))

static bool     cp_armed = false;
static uint32_t cp_sweep_ms = 0;       // 0 = every iteration
static uint32_t cp_mask = CP_MASK_DEFAULT;
static uint32_t cp_last_sweep = 0;
static uint32_t cp_iters = 0;

static uint8_t  cp_good_stage = HEAP_TRAP_NONE;
static uint32_t cp_good_ctx = 0;
static uint32_t cp_good_vtable = 0;
static uint32_t cp_good_iter = 0;

static uint32_t cp_sweeps = 0;
static uint32_t cp_us_last = 0;
static uint32_t cp_us_max = 0;

void heap_trap_set_sweep_ms(uint32_t ms)
{
  cp_sweep_ms = ms;
}

void heap_trap_set_mask(uint32_t mask)
{
  // A zero mask disables the checkpoint layer and leaves the 10 s backstop.
  cp_mask = mask;
}

void heap_trap_loop_begin()
{
  cp_iters++;
  if(0 == cp_sweep_ms) {
    cp_armed = true;    // every iteration: full coverage
    return;
  }
  // Arm for a whole iteration at a time, so every stage of one pass is checked
  // and the pair of stages either side of a failure is meaningful.
  cp_armed = (millis() - cp_last_sweep) >= cp_sweep_ms;
  if(cp_armed) {
    cp_last_sweep = millis();
  }
}

static void cp_check(uint8_t stage, uint32_t ctx, uint32_t vtable)
{
  if(!cp_armed || 0 == (cp_mask & CP_BIT(stage))) {
    return;
  }

  uint32_t t0 = micros();
  bool ok = heap_caps_check_integrity_all(false);
  uint32_t dt = micros() - t0;

  cp_sweeps++;
  cp_us_last = dt;
  if(dt > cp_us_max) {
    cp_us_max = dt;
  }

  if(ok) {
    cp_good_stage = stage;
    cp_good_ctx = ctx;
    cp_good_vtable = vtable;
    cp_good_iter = cp_iters;
    return;
  }

  heap_trap_capture();
  rec.last_good_stage = cp_good_stage;
  rec.last_good_ctx = cp_good_ctx;
  rec.last_good_vtable = cp_good_vtable;
  rec.fail_stage = stage;
  rec.fail_ctx = ctx;
  rec.fail_vtable = vtable;
  rec.iters_between = cp_iters - cp_good_iter;
  rec.sweep_us_last = cp_us_last;
  rec.sweep_us_max = cp_us_max;
  rec.sweeps = cp_sweeps;

  heap_caps_check_integrity_all(true);   // UART, for anyone listening
  abort();
}

void heap_trap_checkpoint(uint8_t stage)
{
  cp_check(stage, 0, 0);
}

extern "C" void heap_trap_task_checkpoint(const void *task)
{
  // The vtable pointer is the first word of a polymorphic object; resolved
  // against the ELF it names the class, which is what we actually want to know.
  uint32_t vtable = 0;
  if(task) {
    memcpy(&vtable, task, sizeof(vtable));
  }
  cp_check(HEAP_TRAP_TASK, (uint32_t)(uintptr_t)task, vtable);
}

void heap_trap_tick()
{
  static uint32_t last = 0;
  if(millis() - last < 10000) return;
  last = millis();
  if(heap_caps_check_integrity_all(false)) return;
  heap_trap_capture();
  // The backstop catches corruption from outside loop() (tiT, the wifi task)
  // and from uninstrumented iterations, so there is no meaningful failing
  // stage - only the last stage that was known clean.
  rec.last_good_stage = cp_good_stage;
  rec.last_good_ctx = cp_good_ctx;
  rec.last_good_vtable = cp_good_vtable;
  rec.fail_stage = HEAP_TRAP_NONE;
  rec.iters_between = cp_iters - cp_good_iter;
  rec.sweep_us_last = cp_us_last;
  rec.sweep_us_max = cp_us_max;
  rec.sweeps = cp_sweeps;
  heap_caps_check_integrity_all(true);   // UART, for anyone listening
  abort();
}

bool heap_trap_present() { return rec.magic == TRAP_MAGIC; }
void heap_trap_clear() { rec.magic = 0; }

static const char *stage_name(uint8_t stage)
{
  static const char *names[HEAP_TRAP_STAGE_MAX] = {
    "none", "loop_top", "mongoose", "http_update", "web_server", "diagnostics",
    "flash_migrate", "ota", "rapi", "microtask", "tesla", "emoncms", "serial",
    "task"
  };
  return stage < HEAP_TRAP_STAGE_MAX ? names[stage] : "?";
}

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

  // Live, so the cost of a sweep can be read off a running unit and the
  // interval retuned (?ms=N) without a reflash.
  JsonObject live = doc.createNestedObject("live");
  live["sweep_ms"] = cp_sweep_ms;      // 0 = every iteration
  char mbuf[12];
  snprintf(mbuf, sizeof(mbuf), "0x%04x", (unsigned)cp_mask);
  live["mask"] = mbuf;
  live["sweeps"] = cp_sweeps;
  live["sweep_us_last"] = cp_us_last;
  live["sweep_us_max"] = cp_us_max;
  live["iters"] = cp_iters;
  live["last_good_stage"] = stage_name(cp_good_stage);

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

  // Who was running. fail_stage "none" means the 10 s backstop fired rather
  // than a checkpoint, so only last_good is meaningful.
  JsonObject w = doc.createNestedObject("where");
  w["last_good_stage"] = stage_name(rec.last_good_stage);
  w["fail_stage"] = stage_name(rec.fail_stage);
  w["iters_between"] = rec.iters_between;
  if(rec.last_good_ctx) {
    snprintf(buf, sizeof(buf), "0x%08x", (unsigned)rec.last_good_ctx);
    w["last_good_task"] = buf;
    snprintf(buf, sizeof(buf), "0x%08x", (unsigned)rec.last_good_vtable);
    w["last_good_vtable"] = buf;
  }
  if(rec.fail_ctx) {
    snprintf(buf, sizeof(buf), "0x%08x", (unsigned)rec.fail_ctx);
    w["fail_task"] = buf;
    snprintf(buf, sizeof(buf), "0x%08x", (unsigned)rec.fail_vtable);
    w["fail_vtable"] = buf;
  }
  w["sweeps"] = rec.sweeps;
  w["sweep_us_last"] = rec.sweep_us_last;
  w["sweep_us_max"] = rec.sweep_us_max;
}
