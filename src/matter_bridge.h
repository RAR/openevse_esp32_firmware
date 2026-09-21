#ifndef MATTER_BRIDGE_H
#define MATTER_BRIDGE_H

// SPIKE: Matter footprint probe. One On/Off Plugin Unit endpoint whose
// on/off drives an EvseManager claim, commissioned on-network (no BLE on
// the classic ESP32). Throwaway: measures flash/RAM and mDNS coexistence.

#ifdef ENABLE_MATTER

#include <MicroTasks.h>
#include "evse_man.h"

class MatterBridge : public MicroTasks::Task
{
  private:
    EvseManager *_evse;
    bool _started;
    bool _reportedCommissioned;

  protected:
    void setup();
    unsigned long loop(MicroTasks::WakeReason reason);

  public:
    MatterBridge();
    void begin(EvseManager &evse);
    bool onPlugChange(bool on);
};

extern MatterBridge matterBridge;

#endif // ENABLE_MATTER
#endif // MATTER_BRIDGE_H
