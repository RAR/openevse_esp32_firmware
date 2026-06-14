#pragma once
#ifdef OPENEVSE_LITE
class LiteEvseManager;
class LiteClock;
struct LiteEnergyTotals;
void web_server_lite_begin(LiteEvseManager &mgr, LiteClock &clock, LiteEnergyTotals &totals);
void web_server_lite_loop();
#endif
