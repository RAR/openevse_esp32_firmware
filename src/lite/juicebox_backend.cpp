#if defined(OPENEVSE_LITE) && defined(LITE_EVSE_BACKEND_JUICEBOX)
#include "juicebox_backend.h"
#include <string.h>

// Comm-watchdog reload is ~3000 ticks (~3 s, LIKELY per the Task 1 RE note);
// keepalive comfortably under it. Adjust once the timeout is HW-confirmed.
static const unsigned long JB_KEEPALIVE_INTERVAL_MS = 1000;
static const unsigned long JB_OFFLINE_TIMEOUT_MS    = 5000;

void JuiceBoxBackend::begin() {
  _lastBeatMillis = millis();
}

void JuiceBoxBackend::loop() {
  JuiceBoxFrame f;
  while (_port.available() > 0) {
    int b = _port.read();
    if (b < 0) break;
    if (_parser.feed((uint8_t)b, f)) {
      _lastRxMillis = millis();
      _everRx = true;
      handleFrame(f);
    }
  }

  unsigned long now = millis();
  // Keepalive = re-send the MCU's OWN reported amps (idempotent: holds the comm
  // watchdog without changing charge current). Gated on: (a) we've seen the MCU
  // (_everRx) so we don't transmit into a silent line, and (b) we have a valid
  // $ES (_status.valid) so we echo a REAL amps value — echoing 0 before we know
  // the state would command 0 A and pause charging.
  if (_everRx && _status.valid && (now - _lastBeatMillis) >= JB_KEEPALIVE_INTERVAL_MS) {
    sendKeepalive();
    _lastBeatMillis = now;
  }
}

bool JuiceBoxBackend::isOnline() const {
  return _everRx && (millis() - _lastRxMillis) < JB_OFFLINE_TIMEOUT_MS;
}

void JuiceBoxBackend::handleFrame(const JuiceBoxFrame &f) {
  if      (!strcmp(f.type, "ES")) { juicebox_parse_es(f.payload, f.len, _status); }
  else if (!strcmp(f.type, "HW")) { strncpy(_hw, f.payload, sizeof(_hw) - 1); }
  else if (!strcmp(f.type, "FW")) { strncpy(_fw, f.payload, sizeof(_fw) - 1); }
  else if (!strcmp(f.type, "PV")) { strncpy(_pv, f.payload, sizeof(_pv) - 1); }
  else if (!strcmp(f.type, "MD")) { strncpy(_md, f.payload, sizeof(_md) - 1); }
  else if (!strcmp(f.type, "WC")) { strncpy(_wc, f.payload, sizeof(_wc) - 1); }
  // other types ignored this slice
}

void JuiceBoxBackend::sendKeepalive() {
  char buf[32];
  size_t n = juicebox_build_amps_set(_status.amps, buf, sizeof(buf));
  if (n) _port.write((const uint8_t *)buf, n);
}

void JuiceBoxBackend::addStatusFields(JsonDocument &doc) const {
  if (_hw[0]) doc["hw"]       = _hw;
  if (_fw[0]) doc["fw"]       = _fw;
  if (_pv[0]) doc["protocol"] = _pv;
  if (_md[0]) doc["md"]       = _md;
  if (_wc[0]) doc["wc"]       = _wc;   // handshake nonce — live-capture aid
  doc["line"] = _status.line;          // raw JB L field (semantics unknown per RE)
}
#endif
