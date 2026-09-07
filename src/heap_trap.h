#ifndef HEAP_TRAP_H
#define HEAP_TRAP_H

// Heap-corruption trap (HEAP_DEBUG_INTEGRITY builds only). Every few seconds
// loop() runs heap_caps_check_integrity_all(); on failure heap_trap_capture()
// walks every heap, finds the first block whose poison canaries / free fill
// are wrong, saves that block and its neighbours into RTC memory (survives the
// abort and reboot), and the firmware aborts so a core dump lands too.
// GET /debug/heaptrap reads the record back; DELETE clears it.

#include <ArduinoJson.h>

void heap_trap_tick();
void heap_trap_capture();
bool heap_trap_present();
void heap_trap_json(JsonDocument &doc);
void heap_trap_clear();

#endif
