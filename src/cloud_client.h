#ifndef _OPENEVSE_CLOUD_CLIENT_H
#define _OPENEVSE_CLOUD_CLIENT_H

// -------------------------------------------------------------------
// The Overwatt cloud client.
//
// A second, independent MQTT connection that carries the
// evse-cloud-agent "Device agent" contract and nothing else. The
// legacy publisher in mqtt.cpp is untouched: it keeps its own socket,
// its own format and its own broker, and neither ever publishes on the
// other's connection.
//
// This class is the host half of the contract. The protocol itself
// lives in the evse-cloud-agent library (EvseCloudAgentCore); every
// platform detail arrives through the sixteen EvseCloudAgentHost
// methods implemented below.
//
// Threading: the core has no internal locking, so every entry point
// must run on this task. Inbound MQTT payloads arrive on Mongoose's
// callback, so they are copied into a small queue here and dispatched
// from loop() rather than called straight through.
// -------------------------------------------------------------------

// Build inclusion. The 4MB openevse_wifi_v1 image is already at ~98%
// of its app partition, so the client is compiled in only where there
// is room for it. This is NOT the runtime switch - that is the
// cloud_enabled config key, which decides whether a compiled-in client
// actually connects.
#ifdef ENABLE_CLOUD_CLIENT

#include <Arduino.h>
#include <MongooseMqttClient.h>
#include <MicroTasks.h>
#include <MicroTasksEventListener.h>

#include <evse_cloud_agent_core.h>

#include "cloud_topics.h"
#include "evse_man.h"

#ifndef CLOUD_CLIENT_LOOP_INTERVAL
#define CLOUD_CLIENT_LOOP_INTERVAL 1000
#endif

#ifndef CLOUD_CLIENT_CONNECT_TIMEOUT
#define CLOUD_CLIENT_CONNECT_TIMEOUT (5 * 1000)
#endif

// Inbound payloads copied out of the Mongoose callback. Commands are
// small - the biggest realistic one, an eleven-key config.set, is
// about 330 bytes of text - so three 512-byte slots absorb a burst
// without making the queue a heap problem of its own.
#ifndef CLOUD_CLIENT_QUEUE_SLOTS
#define CLOUD_CLIENT_QUEUE_SLOTS 3
#endif
#ifndef CLOUD_CLIENT_QUEUE_SLOT_SIZE
#define CLOUD_CLIENT_QUEUE_SLOT_SIZE 512
#endif

// How many schedule events the control document can carry. Kept in
// step with the library's own default; raising it means raising
// EVSE_CLOUD_AGENT_CONTROL_BUF and the task stack too.
#define CLOUD_CLIENT_MAX_SCHEDULE EVSE_CLOUD_AGENT_MAX_SCHEDULE

class CloudClient : public MicroTasks::Task, public EvseCloudAgentHost
{
  private:
    enum InboundKind : uint8_t
    {
      InboundKind_None = 0,
      InboundKind_Command,
      InboundKind_Lease
    };

    struct Inbound
    {
      uint8_t kind;
      size_t  length;
      char    payload[CLOUD_CLIENT_QUEUE_SLOT_SIZE];
    };

    MongooseMqttClient _client;
    EvseCloudAgentCore _core;
    EvseManager *_evse;
    EvseCloudAgentHeapRule _heapRule;

    // Identity and routing
    char _thing[CLOUD_THING_LEN];
    char _topic[CLOUD_TOPIC_BUF];      // scratch, publish() only

    // Connection state
    bool _connecting;
    long _nextReconnectAttempt;
    unsigned long _connectStartTime;
    bool _pendingConnected;            // transport up, core not told yet
    bool _connectStatusPublish;        // route agent/status to the retained topic

    // Inbound queue, written from the Mongoose callback and drained here
    Inbound _queue[CLOUD_CLIENT_QUEUE_SLOTS];
    volatile uint8_t _queueHead;
    volatile uint8_t _queueTail;
    uint32_t _queueDropped;

    // Change detection for the control document. The manual override is
    // watched through the claims version rather than the override
    // version because an auto-release moves the former and, before the
    // fix in EvseManager::releaseAutoReleaseClaims(), moved neither.
    uint8_t  _claimsVersion;
    uint8_t  _limitVersion;
    uint32_t _scheduleVersion;
    uint32_t _configVersion;

    MicroTasks::EventListener _stateChangeListener;

    // Borrowed-string storage: everything handed to the core must
    // outlive the call that handed it over, and net.getIp() returns a
    // temporary.
    String _ipAddress;
    String _chargeMode;

    // Heap rule outcome, surfaced on GET /status and in the status flags
    bool    _localRun;
    uint8_t _localStopReason;

    // millis() wraps every 49 days; monotonicMs() must not.
    uint64_t _monotonicHigh;
    uint32_t _monotonicLast;

    void attemptConnection();
    void onCloudConnect();
    void onCloudDisconnect(int err, const char *reason);
    void handleMessage(MongooseString topic, MongooseString payload);
    void enqueue(uint8_t kind, const char *payload, size_t length);
    void drainQueue();
    void checkControlChanged();
    void applyHeapRule();
    bool identityFromConfig();

  protected:
    void setup() override;
    unsigned long loop(MicroTasks::WakeReason reason) override;

  public:
    CloudClient(EvseManager &evse);
    ~CloudClient();

    void begin();

    bool isConnected() { return _client.connected(); }
    const char *getThing() { return _thing; }

    // Re-read cloud_* config: identity, interval, and whether the
    // client (and the local publisher) should be running at all.
    void notifyConfigChanged();

    // True when the local MQTT publisher may run. The chip policy and
    // the heap rule both feed this; mqtt.cpp consults it before
    // connecting.
    bool localPublisherAllowed() { return _localRun; }

    // "", "not_configured", "one_connection" or "low_heap"
    const char *localStopReason();

    // Inbound payloads dropped for being too large or arriving faster
    // than they were drained. A command dropped here is never
    // acknowledged, so this is the only visible trace of one.
    uint32_t getDropped() { return _queueDropped; }

    // ---- EvseCloudAgentHost ----
    bool publish(const char *topic_suffix, const char *payload, bool retain) override;
    uint64_t monotonicMs() override;
    uint32_t epochSeconds() override;
    void readState(EvseCloudAgentState &state) override;
    void readControl(EvseCloudAgentControl &control) override;
    const char *firmwareVersion() override;
    const char *ipAddress() override;
    EvseCloudAgentResult setOverride(bool active, uint32_t charge_current,
                                     bool has_charge_current) override;
    EvseCloudAgentResult clearOverride() override;
    EvseCloudAgentResult setLimit(const char *type, int32_t value,
                                  bool auto_release) override;
    EvseCloudAgentResult clearLimit() override;
    EvseCloudAgentResult setSchedule(const EvseCloudAgentScheduleEvent &event) override;
    EvseCloudAgentResult clearSchedule(int32_t id) override;
    EvseCloudAgentResult setDivert(int32_t mode) override;
    EvseCloudAgentResult setConfig(const char *json) override;
    EvseCloudAgentResult restart() override;
};

extern CloudClient cloudClient;

#else // !ENABLE_CLOUD_CLIENT

// The stub pulls in no firmware headers, so it has to declare its own
// fixed-width types - the real path gets them from Arduino.h.
#include <stdint.h>

// A stub with the same surface, so the call sites in mqtt.cpp,
// app_config.cpp and web_server.cpp stay free of #ifdef. Every method
// is an inline constant, so the whole thing compiles away.
class CloudClient
{
  public:
    void begin() { }
    bool isConnected() { return false; }
    const char *getThing() { return ""; }
    void notifyConfigChanged() { }

    // Nothing gates the local publisher when there is no cloud client.
    bool localPublisherAllowed() { return true; }
    const char *localStopReason() { return ""; }
    uint32_t getDropped() { return 0; }
};

extern CloudClient cloudClient;

#endif // ENABLE_CLOUD_CLIENT

#endif // _OPENEVSE_CLOUD_CLIENT_H
