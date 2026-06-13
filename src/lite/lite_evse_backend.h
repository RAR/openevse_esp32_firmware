#pragma once
#ifdef OPENEVSE_LITE
#include <ArduinoJson.h>
#include "lite_evse_state.h"

// Backend-agnostic EVSE device seam. web_server_lite + main_lite depend ONLY on this.
class LiteEvseBackend {
public:
  virtual ~LiteEvseBackend() {}
  virtual void begin() = 0;
  virtual void loop()  = 0;

  virtual bool          isOnline() const = 0;
  virtual LiteEvseState getState() const = 0;
  virtual int           getAmps()  const = 0;
  virtual int           getPower() const = 0;
  virtual int           getTemp()  const = 0;
  virtual int           getFault() const = 0;

  // Backend-specific extras (identity strings, raw fields, ...).
  virtual void addStatusFields(JsonDocument &doc) const = 0;
};
#endif // OPENEVSE_LITE
