#pragma once
#ifdef OPENEVSE_LITE

#include "evse_man.h"

// Slice-1 minimal web server for the JuiceBox-40 lite build. Stands up the
// vendored MongooseLite C core on :80 and serves a live /status sourced from
// the real EvseManager (RAPI-driven). No ArduinoMongoose / web_server.cpp.
void web_server_lite_begin(EvseManager &evse);
void web_server_lite_loop();

#endif
