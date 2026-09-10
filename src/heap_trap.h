#ifndef HEAP_TRAP_H
#define HEAP_TRAP_H

// Heap-corruption trap (HEAP_DEBUG_INTEGRITY builds only).
//
// Two layers:
//
// 1. A 10 s backstop: loop() runs heap_caps_check_integrity_all(); on failure
//    heap_trap_capture() walks every heap, finds the first block whose poison
//    canaries / free fill are wrong, saves that block and its neighbours into
//    RTC memory (survives the abort and reboot), and the firmware aborts so a
//    core dump lands too.
//
// 2. Checkpoints, which are the point of the exercise. The backstop names the
//    VICTIM - a block someone else scribbled on - and three trips have now
//    named three different innocent bystanders. To name the WRITER you have to
//    know which code ran between the last clean heap and the dirty one, so
//    every stage of loop() and every MicroTask dispatch calls a checkpoint.
//    One full instrumented pass runs every heap_trap_sweep_ms (default 200);
//    the checkpoints in every other iteration cost one bool test. On failure
//    the record carries the stage that was last clean and the stage that
//    failed, and for a task, which task object.
//
// GET /debug/heaptrap reads the record back and the live sweep timings; a
// ?ms=N query re-tunes the sweep interval without a reflash. DELETE clears.

#include <ArduinoJson.h>

enum HeapTrapStage {
  HEAP_TRAP_NONE = 0,
  HEAP_TRAP_LOOP_TOP,
  HEAP_TRAP_MONGOOSE,
  HEAP_TRAP_HTTP_UPDATE,
  HEAP_TRAP_WEB_SERVER,
  HEAP_TRAP_DIAGNOSTICS,
  HEAP_TRAP_FLASH_MIGRATE,
  HEAP_TRAP_OTA,
  HEAP_TRAP_RAPI,
  HEAP_TRAP_MICROTASK,
  HEAP_TRAP_TESLA,
  HEAP_TRAP_EMONCMS,
  HEAP_TRAP_SERIAL,
  HEAP_TRAP_TASK,        // one MicroTask's loop(); ctx is the task object
  HEAP_TRAP_STAGE_MAX
};

// Call once at the top of loop(). Decides whether this iteration is an
// instrumented one.
void heap_trap_loop_begin();

// Call after each stage of loop(). Cheap (one bool test) unless this
// iteration is the instrumented one.
void heap_trap_checkpoint(uint8_t stage);

// Call after each MicroTask's loop(). `task` is recorded along with its
// vtable pointer, which names the class when resolved against the ELF.
//
// extern "C" and called through a weak declaration from inside the MicroTasks
// library, so that library needs no build flag and no knowledge of this header:
// where the trap is not compiled in, the symbol is absent and the call site's
// null check skips it.
extern "C" void heap_trap_task_checkpoint(const void *task);

void heap_trap_tick();
void heap_trap_capture();
bool heap_trap_present();
void heap_trap_json(JsonDocument &doc);
void heap_trap_clear();
void heap_trap_set_sweep_ms(uint32_t ms);   // 0 = check every iteration
void heap_trap_set_mask(uint32_t mask);     // bit per HeapTrapStage

#endif
