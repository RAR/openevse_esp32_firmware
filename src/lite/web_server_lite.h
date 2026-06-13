#pragma once
#ifdef OPENEVSE_LITE
class LiteEvseBackend;
void web_server_lite_begin(LiteEvseBackend &backend);
void web_server_lite_loop();
#endif
