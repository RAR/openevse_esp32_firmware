#pragma once
#include <stdint.h>

// Backend-agnostic EVSE state. Each backend maps its native codes onto this.
enum class LiteEvseState : uint8_t {
  Unknown = 0,
  NotConnected,   // no vehicle on the pilot
  Connected,      // vehicle present, not charging
  Charging,
  Error,          // fault / GFI / lockout
};
