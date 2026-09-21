#ifdef ENABLE_MATTER

#if defined(ENABLE_DEBUG) && !defined(ENABLE_DEBUG_MATTER)
#undef ENABLE_DEBUG
#endif

#include "debug.h"
#include "matter_bridge.h"
#include "net_manager.h"

#include <Matter.h>

// Not registered in evse_man.h on purpose: this is a probe.
#define EvseClient_Matter EVC(0x0005, 0x0001)
#define EvseManager_Priority_Matter 500

static MatterOnOffPlugin plug;

MatterBridge matterBridge;

MatterBridge::MatterBridge() :
  MicroTasks::Task(),
  _evse(nullptr),
  _started(false),
  _reportedCommissioned(false)
{
}

void MatterBridge::begin(EvseManager &evse)
{
  _evse = &evse;
  MicroTask.startTask(this);
}

void MatterBridge::setup()
{
}

bool MatterBridge::onPlugChange(bool on)
{
  DBUGF("Matter: plug -> %s", on ? "ON" : "OFF");
  EvseProperties props(on ? EvseState::Active : EvseState::Disabled);
  _evse->claim(EvseClient_Matter, EvseManager_Priority_Matter, props);
  return true;
}

unsigned long MatterBridge::loop(MicroTasks::WakeReason reason)
{
  if(!_started) {
    // On-network commissioning needs the STA up first; CHIP then owns mDNS.
    if(!net.isConnected()) {
      return 1000;
    }
    plug.begin(false);
    plug.onChange([](bool on) { return matterBridge.onPlugChange(on); });
    Matter.begin();
    _started = true;
    DBUGF("Matter: started, heap %u largest %u",
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }

  if(!Matter.isDeviceCommissioned()) {
    DBUGF("Matter: not commissioned. Pairing code %s  QR %s",
          Matter.getManualPairingCode().c_str(),
          Matter.getOnboardingQRCodeUrl().c_str());
    return 30 * 1000;
  }

  if(!_reportedCommissioned) {
    _reportedCommissioned = true;
    plug.updateAccessory();
    DBUGF("Matter: commissioned, heap %u largest %u",
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
  return 10 * 1000;
}

#endif // ENABLE_MATTER
