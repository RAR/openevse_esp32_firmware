#include "juicebox_proto.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static bool is_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

bool juicebox_parse_frame_body(const char *raw, size_t n, JuiceBoxFrame &out) {
  memset(&out, 0, sizeof(out));
  if (n < 3) return false;                  // need 2 type chars + at least ':'
  out.type[0] = raw[0];
  out.type[1] = raw[1];
  out.type[2] = '\0';

  const char *payload = nullptr;
  if (n >= 6 && is_hex(raw[2]) && is_hex(raw[3]) && is_hex(raw[4]) && raw[5] == ':') {
    payload = raw + 6;
    out.len = (uint16_t)strtol(raw + 2, nullptr, 16);   // strtol reads the 3 hex up to ':'
  } else if (raw[2] == ':') {
    payload = raw + 3;
  } else {
    const char *colon = strchr(raw + 2, ':');
    if (!colon) return false;
    payload = colon + 1;
  }

  size_t plen = strlen(payload);
  if (plen > JB_MAX_PAYLOAD) plen = JB_MAX_PAYLOAD;
  memcpy(out.payload, payload, plen);
  out.payload[plen] = '\0';
  if (out.len == 0) out.len = (uint16_t)plen;
  return true;
}

bool JuiceBoxParser::flush(JuiceBoxFrame &out) {
  if (!_started || _n == 0) { _started = false; _n = 0; return false; }
  _raw[_n] = '\0';
  bool ok = juicebox_parse_frame_body(_raw, _n, out);
  _started = false; _n = 0;
  return ok;
}

bool JuiceBoxParser::feed(uint8_t b, JuiceBoxFrame &out) {
  if (b == '$') {
    bool ready = flush(out);   // close any in-progress frame
    _started = true; _n = 0;
    return ready;
  }
  if (b == '\r' || b == '\n') {
    return flush(out);
  }
  if (_started) {
    if (_n < sizeof(_raw) - 1) _raw[_n++] = (char)b;
    else { _started = false; _n = 0; }   // runaway → drop, resync on next '$'
  }
  return false;
}
