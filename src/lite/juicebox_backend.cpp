#if defined(OPENEVSE_LITE) && defined(LITE_EVSE_BACKEND_JUICEBOX)
#include "juicebox_backend.h"
#include <string.h>

// Comm-watchdog reload is ~3000 ticks (~3 s, LIKELY per the Task 1 RE note);
// keepalive comfortably under it. Adjust once the timeout is HW-confirmed.
static const unsigned long JB_KEEPALIVE_INTERVAL_MS = 1000;
// Offline timeout must EXCEED the Atmel's slowest liveness frame. HW-observed
// 2026-06-13: in steady state the ONLY inbound frame is a $TP ping every ~63 s
// (metronomic; $ES/$MD only at boot/on-change). A 5 s timeout made isOnline()
// flap (true for 5 s after each ping, false for the other ~58 s). 90 s clears one
// ping interval with margin without flapping; a real dead controller still trips
// offline within ~90 s. Bump toward ~130 s if we ever need to ride out a missed ping.
static const unsigned long JB_OFFLINE_TIMEOUT_MS    = 90000;

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
  // Keepalive holds the comm watchdog. IMPORTANT (RE-confirmed): $SL is the only $S
  // keepalive and it ALWAYS sets the J1772 pilot current — there is NO current-neutral
  // heartbeat — so we advertise a deliberate safe limit (_chargeLimit, default 6 A floor),
  // never the MCU's reported max. Gated on _everRx so we don't transmit into a silent line.
  if (_everRx && (now - _lastBeatMillis) >= JB_KEEPALIVE_INTERVAL_MS) {
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
#ifdef JB_DEBUG
  // Bench-debug echo of every received Atmel frame to VCOM (raw payload incl. any
  // foreign :tag:). Protocol-safe: the Atmel ignores non-$ log noise. Built only in
  // [env:openevse_lite_debug]; compiled out of the production env.
  LT_I("JBRX [%s] len=%u: %s", f.type, (unsigned)f.len, f.payload);
#endif
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
  // Advertise the deliberate safe limit, not the MCU's reported max. juicebox_build_amps_set
  // clamps to [0,79]; the MCU further clamps <6 up to the 6 A J1772 floor.
  char buf[32];
  int amps = _enabled ? _chargeLimit : 6; // Disabled => 6 A floor (see setState TODO)
  size_t n = juicebox_build_amps_set(amps, buf, sizeof(buf));
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
