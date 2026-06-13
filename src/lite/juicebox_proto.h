#pragma once
#include <stdint.h>
#include <stddef.h>
#include "lite_evse_state.h"

static const size_t JB_TYPE_LEN    = 2;
static const size_t JB_MAX_PAYLOAD = 80;

// One decoded protocol frame: 2-char type + NUL-terminated payload.
struct JuiceBoxFrame {
  char     type[JB_TYPE_LEN + 1];
  char     payload[JB_MAX_PAYLOAD + 1];
  uint16_t len;                       // payload length
};

// Decoded $ES status fields (raw JB values, pre-normalization).
struct JuiceBoxStatus {
  bool valid;
  int  state, line, temp, h, amps, power, fault;
};

// Split a frame body (everything AFTER the leading '$') into type + payload.
// Handles "<TT><3hex>:<payload>" (e.g. $ES01C:...) and "<TT>:<payload>" (e.g. $MD:...).
bool juicebox_parse_frame_body(const char *raw, size_t n, JuiceBoxFrame &out);

// Incremental, framing-tolerant parser. A frame runs from a '$' to the next '$'
// or a CR/LF terminator (whichever comes first). Resyncs on '$'. feed() returns
// true and fills `out` exactly once per completed frame.
class JuiceBoxParser {
public:
  JuiceBoxParser() : _n(0), _started(false) {}
  bool feed(uint8_t b, JuiceBoxFrame &out);
  void reset() { _n = 0; _started = false; }
private:
  bool flush(JuiceBoxFrame &out);
  char   _raw[JB_MAX_PAYLOAD + 16];
  size_t _n;
  bool   _started;
};

// Decode a $ES payload (e.g. "S00,L00,T00,H00,A00,P000,F00") into JuiceBoxStatus.
bool juicebox_parse_es(const char *payload, uint16_t len, JuiceBoxStatus &out);

// Map a raw JB S-field code onto the canonical state (codes confirmed in Task 1).
LiteEvseState juicebox_map_state(int raw);

// Build "$<type><LLL hex>:<payload>" into buf. Returns bytes written, 0 on overflow.
size_t juicebox_build_frame(const char *type, const char *payload, char *buf, size_t buflen);

// Build the keep-alive heartbeat frame (exact bytes per Task 1). Returns bytes written.
size_t juicebox_build_heartbeat(char *buf, size_t buflen);
