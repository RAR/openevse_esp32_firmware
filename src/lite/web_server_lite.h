#pragma once
#ifdef OPENEVSE_LITE
class LiteEvseManager;
void web_server_lite_begin(LiteEvseManager &mgr);
void web_server_lite_loop();
#endif
