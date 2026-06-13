#pragma once
#if defined(OPENEVSE_LITE) && defined(LITE_EVSE_BACKEND_JUICEBOX)
#include <Arduino.h>
#include "lite_evse_backend.h"
#include "juicebox_proto.h"

class JuiceBoxBackend : public LiteEvseBackend {
public:
  explicit JuiceBoxBackend(Stream &port) : _port(port) {}

  void begin() override;
  void loop()  override;

  bool          isOnline() const override;
  // A non-zero $ES F (fault) field takes precedence over the S state code.
  LiteEvseState getState() const override {
    return _status.fault != 0 ? LiteEvseState::Error : juicebox_map_state(_status.state);
  }
  int  getAmps()  const override { return _status.amps; }
  int  getPower() const override { return _status.power; }
  int  getTemp()  const override { return _status.temp; }
  int  getFault() const override { return _status.fault; }
  void addStatusFields(JsonDocument &doc) const override;

private:
  void handleFrame(const JuiceBoxFrame &f);
  void sendKeepalive();

  Stream        &_port;
  JuiceBoxParser _parser;
  JuiceBoxStatus _status = {};
  unsigned long  _lastRxMillis   = 0;
  unsigned long  _lastBeatMillis = 0;
  bool           _everRx         = false;
  char _hw[24] = {0};
  char _fw[16] = {0};
  char _pv[8]  = {0};
  char _md[48] = {0};
  char _wc[24] = {0};   // last $WC payload (the handshake nonce — live-capture aid)
};
#endif
