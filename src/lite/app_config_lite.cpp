#ifdef OPENEVSE_LITE
// Minimal app_config shim for the lite build.
//
// The EVSE core (evse_man.cpp / evse_monitor.cpp) reaches into app_config.h for
// exactly two helpers — config_default_state() and config_threephase_enabled().
// Both are `inline` in app_config.h and resolve against the global `flags`
// bitmask, so the only symbol the linker actually needs from the config
// subsystem is `flags` itself. We provide it here rather than compiling the
// full src/app_config.cpp (which drags in the entire ConfigJson/persistence
// surface that is deliberately deferred this slice).
//
// Value: CONFIG_DEFAULT_STATE (bit 26) set, everything else clear. That gives:
//   - config_default_state()      -> EvseState::Active   (the firmware default)
//   - config_threephase_enabled() -> false               (CONFIG_THREEPHASE clear)
#include "app_config.h"

uint32_t flags = CONFIG_DEFAULT_STATE;

#endif // OPENEVSE_LITE
