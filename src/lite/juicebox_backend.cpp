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

// Copy a frame payload into a fixed display buffer, bounded by the frame's declared
// length so the trailing :tag: (added by a foreign layer, beyond the $-protocol's
// length-delimited payload) is stripped. Never reads past the parsed (NUL-terminated)
// payload. Always NUL-terminates.
static void copy_bounded(char *dst, size_t cap, const char *src, uint16_t len) {
  size_t n = strlen(src);
  if (n > len)      n = len;       // strip anything past the declared length (the :tag:)
  if (n > cap - 1)  n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

void JuiceBoxBackend::handleFrame(const JuiceBoxFrame &f) {
  if      (!strcmp(f.type, "ES")) { juicebox_parse_es(f.payload, f.len, _status); }
  else if (!strcmp(f.type, "HW")) { copy_bounded(_hw, sizeof(_hw), f.payload, f.len); }
  else if (!strcmp(f.type, "FW")) { copy_bounded(_fw, sizeof(_fw), f.payload, f.len); }
  else if (!strcmp(f.type, "PV")) { copy_bounded(_pv, sizeof(_pv), f.payload, f.len); }
  else if (!strcmp(f.type, "MD")) { copy_bounded(_md, sizeof(_md), f.payload, f.len); }
  else if (!strcmp(f.type, "WC")) { copy_bounded(_wc, sizeof(_wc), f.payload, f.len); }
  else if (!strcmp(f.type, "WR")) { copy_bounded(_wr, sizeof(_wr), f.payload, f.len); }
  // other types ignored this slice
}

void JuiceBoxBackend::sendKeepalive() {
  char buf[32];
  size_t n = juicebox_build_amps_set(_status.amps, buf, sizeof(buf));
  if (n) _port.write((const uint8_t *)buf, n);
}

void JuiceBoxBackend::addStatusFields(JsonDocument &doc) const {
  doc["state_str"] = lite_evse_state_name(getState());
  if (_hw[0]) doc["hw"]       = _hw;
  if (_fw[0]) doc["fw"]       = _fw;
  if (_pv[0]) doc["protocol"] = _pv;
  if (_md[0]) doc["md"]       = _md;
  if (_wc[0]) doc["wc"]       = _wc;
  if (_wr[0]) doc["wr"]       = _wr;
  doc["line"] = _status.line;          // raw JB L field (semantics unknown per RE)
}
#endif
