#if defined(ENABLE_DEBUG) && !defined(ENABLE_DEBUG_CLOUD)
#undef ENABLE_DEBUG
#endif

#include "cloud_client.h"

#ifdef ENABLE_CLOUD_CLIENT

#include <WiFi.h>

#include "app_config.h"
#include "certificates.h"
#include "current_shaper.h"
#include "debug.h"
#include "divert.h"
#include "emonesp.h"
#include "espal.h"
#include "evse_monitor.h"
#include "limit.h"
#include "manual.h"
#include "net_manager.h"
#include "scheduler.h"

// One image per chip: a classic ESP32 (WROOM) has heap for exactly one
// MQTT connection, the S3 and P4 for two. Which connection actually
// runs on a one-connection chip is a runtime decision taken from
// config below, never a build flag.
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32P4)
#define CLOUD_CHIP_CLASS EvseCloudAgentChip_TwoConnections
#else
#define CLOUD_CHIP_CLASS EvseCloudAgentChip_OneConnection
#endif

// The heap rule's three numbers are bench measurements, not guesses,
// and a zero floor switches the rule off entirely. They stay at zero
// until the WROOM and S3 soaks in docs/user/cloud.md supply them; a
// build flag can set them meanwhile.
#ifndef CLOUD_HEAP_FLOOR_BYTES
#define CLOUD_HEAP_FLOOR_BYTES 0
#endif
#ifndef CLOUD_HEAP_MARGIN_BYTES
#define CLOUD_HEAP_MARGIN_BYTES 0
#endif
#ifndef CLOUD_HEAP_SUSTAIN_MS
#define CLOUD_HEAP_SUSTAIN_MS 0
#endif

// Before this the clock has never been synchronised, and the core must
// publish no timestamp rather than one from 1970.
#define CLOUD_CLOCK_VALID_FROM 1609459200UL   // 2021-01-01T00:00:00Z

CloudClient cloudClient(evse);

CloudClient::CloudClient(EvseManager &evseManager) :
  MicroTasks::Task(),
  _core(*this),
  _evse(&evseManager),
  _identityValid(false),
  _connecting(false),
  _nextReconnectAttempt(0),
  _connectStartTime(0),
  _pendingConnected(false),
  _connectStatusPublish(false),
  _queueHead(0),
  _queueTail(0),
  _queueDropped(0),
  _claimsVersion(0),
  _limitVersion(0),
  _scheduleVersion(0),
  _configVersion(0),
  _lastEvseState(0),
  _lastVehicle(false),
  _lastFlags(0),
  _stateChangeListener(this),
  _localRun(true),
  _localStopReason(EvseCloudAgentLocalStop_None),
  _restartPending(false),
  _monotonicHigh(0),
  _monotonicLast(0)
{
  _thing[0] = '\0';
  _topic[0] = '\0';
}

CloudClient::~CloudClient()
{
  if(_client.connected()) {
    _client.disconnect();
  }
}

void CloudClient::begin()
{
  MicroTask.startTask(this);
}

// -------------------------------------------------------------------
// Identity
// -------------------------------------------------------------------

bool CloudClient::identityFromConfig()
{
  _thing[0] = '\0';

  if(cloud_thing.length() > 0)
  {
    if(cloud_thing.length() + 1 > sizeof(_thing) ||
       !cloud_thing_valid(cloud_thing.c_str()))
    {
      DBUGF("cloud_thing '%s' is not evse-<12 lowercase hex>", cloud_thing.c_str());
      return false;
    }
    strncpy(_thing, cloud_thing.c_str(), sizeof(_thing) - 1);
    _thing[sizeof(_thing) - 1] = '\0';
    return true;
  }

  // Not claimed yet, or claimed without a thing name: fall back on the
  // canonical derivation so the identity is never a surprise.
  String mac = net.getMac();
  if(!cloud_thing_from_mac(_thing, sizeof(_thing), mac.c_str())) {
    DBUGF("Cannot derive a thing name from MAC '%s'", mac.c_str());
    return false;
  }

  return true;
}

// -------------------------------------------------------------------
// Task
// -------------------------------------------------------------------

void CloudClient::setup()
{
  DBUGLN("CloudClient::setup");

  EvseCloudAgentHeapLimits limits;
  limits.floor_bytes  = CLOUD_HEAP_FLOOR_BYTES;
  limits.margin_bytes = CLOUD_HEAP_MARGIN_BYTES;
  limits.sustain_ms   = CLOUD_HEAP_SUSTAIN_MS;
  _heapRule.setLimits(limits);

  _core.setInterval(cloud_agent_interval);

  _client.onMessage([this](MongooseString topic, MongooseString payload) {
    this->handleMessage(topic, payload);
  });

  _client.onError([this](int err) {
    DBUGF("Cloud MQTT error %d", err);
    this->onCloudDisconnect(err, "ERROR");
  });

  _client.onClose([this]() {
    DBUGLN("Cloud MQTT connection closed");
    this->onCloudDisconnect(-1, "CLOSED");
  });

  // The EVSE state, vehicle presence and the flags all move the status
  // document; the core debounces the resulting publish.
  _evse->onStateChange(&_stateChangeListener);

  applyHeapRule();
}

unsigned long CloudClient::loop(MicroTasks::WakeReason reason)
{
  applyHeapRule();

  bool enabled = config_cloud_enabled() && cloud_server.length() > 0 &&
                 cloud_agent_interval > 0;

  if(!enabled)
  {
    if(_client.connected() || _connecting) {
      DBUGLN("Cloud client disabled, disconnecting");
      _client.disconnect();
      _connecting = false;
    }
    return CLOUD_CLIENT_LOOP_INTERVAL;
  }

  // A connection attempt that never called back
  if(_connecting && (millis() - _connectStartTime) > (CLOUD_CLIENT_CONNECT_TIMEOUT * 2)) {
    DBUGLN("Cloud MQTT connection attempt timed out, will retry");
    _connecting = false;
    _nextReconnectAttempt = millis() + CLOUD_CLIENT_CONNECT_TIMEOUT;
  }

  if(net.isConnected() && !_client.connected() && !_connecting)
  {
    long now = millis();
    if(now > _nextReconnectAttempt) {
      _nextReconnectAttempt = now + CLOUD_CLIENT_CONNECT_TIMEOUT;
      attemptConnection();
    }
    return CLOUD_CLIENT_LOOP_INTERVAL;
  }

  if(!_client.connected()) {
    return CLOUD_CLIENT_LOOP_INTERVAL;
  }

  // The transport came up in a Mongoose callback; tell the core here,
  // on this task, because onConnected() publishes four documents and
  // publishControl() alone peaks around 2.7 kB of stack.
  if(_pendingConnected)
  {
    _pendingConnected = false;

    // Seed the change detectors so the connect-time control document
    // is not immediately followed by a spurious change publish.
    _claimsVersion   = _evse->getClaimsVersion();
    _limitVersion    = limit.getVersion();
    _scheduleVersion = scheduler.getVersion();
    _configVersion   = config_version();

    _connectStatusPublish = true;
    _core.onConnected();
    _connectStatusPublish = false;
  }

  drainQueue();
  checkControlChanged();

  if(_stateChangeListener.IsTriggered()) {
    _core.onStateChanged();
  }

  unsigned long wait = _core.loop();

  // The core sleeps until its next publish; the queue and the change
  // detectors need a look in more often than that.
  if(wait > CLOUD_CLIENT_LOOP_INTERVAL) {
    wait = CLOUD_CLIENT_LOOP_INTERVAL;
  }

  return wait;
}

// -------------------------------------------------------------------
// Connection
// -------------------------------------------------------------------

void CloudClient::attemptConnection()
{
  if(_connecting || _client.connected()) {
    return;
  }

  _identityValid = identityFromConfig();
  if(!_identityValid) {
    // Connecting with a client id that is not the thing name is
    // refused by the IoT policy, so do not even try.
    DBUGLN("Cloud client has no valid thing name, not connecting");
    _nextReconnectAttempt = millis() + (60 * 1000);
    return;
  }

  _connecting = true;
  _connectStartTime = millis();

  String host = cloud_server + ":" + String(cloud_port);
  DBUGF("Cloud MQTT connecting to %s as %s", host.c_str(), _thing);

  // The last will must be installed before the connect, and it is the
  // presence topic - this connection has no announce topic.
  char will_payload[EVSE_CLOUD_AGENT_LWT_BUF];
  char will_topic[CLOUD_TOPIC_BUF];
  if(0 == evse_cloud_agent_lwt_payload(will_payload, sizeof(will_payload)) ||
     0 == cloud_topic_presence(will_topic, sizeof(will_topic), _thing))
  {
    DBUGLN("Cannot build the cloud last will, not connecting");
    _connecting = false;
    return;
  }
  _client.setLastWillAndTestimment(will_topic, will_payload, true);

  _client.setRejectUnauthorized(config_mqtt_reject_unauthorized());

  if(cloud_certificate_id != "")
  {
    uint64_t cert_id = 0;
    if(certificate_id_from_string(cloud_certificate_id.c_str(), cert_id)) {
      const char *cert = certs.getCertificate(cert_id);
      const char *key  = certs.getKey(cert_id);
      if(NULL != cert && NULL != key) {
        _client.setCertificate(cert, key);
      } else {
        DBUGF("Cloud certificate %s not found", cloud_certificate_id.c_str());
      }
    } else {
      DBUGF("Ignoring malformed cloud_certificate_id '%s'", cloud_certificate_id.c_str());
    }
  }

  // The client id MUST equal the thing name: the device policy resolves
  // through ${iot:Connection.Thing.ThingName}, so anything else is
  // refused at connect rather than merely losing a topic. It is derived
  // here rather than read from config so it cannot drift.
  _connecting = _client.connect(MQTT_MQTTS, host.c_str(), _thing, [this]() {
    this->onCloudConnect();
  });

  if(!_connecting) {
    DBUGLN("Cloud MQTT immediate connection attempt failed");
    onCloudDisconnect(-100, "Initial connection failed");
  }
}

void CloudClient::onCloudConnect()
{
  DBUGLN("Cloud MQTT connected");
  _connecting = false;
  _nextReconnectAttempt = 0;

  // Subscribe here (cheap, no publishing) but leave the core's own
  // connect work to loop(): onConnected() publishes synchronously and
  // must not run on the Mongoose event handler.
  char topic[CLOUD_TOPIC_BUF];

  // QoS 1: IoT Core delivers at min(publisher, subscriber) QoS, so a
  // QoS 0 subscription would turn every acknowledged command into a
  // fire-and-forget one. The level is a plain 0..2 here, NOT the
  // MG_MQTT_QOS() flags encoding publish() takes.
  if(cloud_topic_cmd(topic, sizeof(topic), _thing)) {
    _client.subscribe(topic, 1);
  }
  if(cloud_topic_lease(topic, sizeof(topic), _thing)) {
    _client.subscribe(topic, 1);
  }

  // Deliberately NOT MicroTask.wakeTask(this): wakeTask() runs loop()
  // synchronously, which would put onConnected() straight back on this
  // Mongoose callback's stack - the thing the deferral exists to avoid.
  // The task's own one-second tick picks it up.
  _pendingConnected = true;
}

void CloudClient::onCloudDisconnect(int err, const char *reason)
{
  DBUGF("Cloud MQTT disconnected (%d, %s)", err, reason);
  _connecting = false;
  _pendingConnected = false;

  // onDisconnected() publishes nothing, so it is safe here, and telling
  // the core promptly stops it trying to publish into a dead socket.
  _core.onDisconnected();
}

// -------------------------------------------------------------------
// Inbound
// -------------------------------------------------------------------

void CloudClient::handleMessage(MongooseString topic, MongooseString payload)
{
  // Runs on the Mongoose event handler. The core has no locking and
  // publishes from inside onCommand(), so nothing is called through
  // from here - the payload is copied and dispatched from loop().
  char expected[CLOUD_TOPIC_BUF];
  String topic_string = topic.toString();

  if(cloud_topic_cmd(expected, sizeof(expected), _thing) &&
     topic_string == expected)
  {
    enqueue(InboundKind_Command, payload.c_str(), payload.length());
  }
  else if(cloud_topic_lease(expected, sizeof(expected), _thing) &&
          topic_string == expected)
  {
    enqueue(InboundKind_Lease, payload.c_str(), payload.length());
  }
  else
  {
    DBUGF("Unexpected cloud topic '%s'", topic_string.c_str());
  }

  // No wakeTask() here either: it would run drainQueue() - and so
  // onCommand(), which publishes - on this callback's stack.
}

void CloudClient::enqueue(uint8_t kind, const char *payload, size_t length)
{
  if(length >= CLOUD_CLIENT_QUEUE_SLOT_SIZE) {
    // The core drops an oversized payload without acknowledging it
    // (there is no id to answer with), so dropping it here is the same
    // outcome one step earlier.
    _queueDropped++;
    DBUGF("Cloud payload of %u bytes is too large, dropped", (unsigned)length);
    return;
  }

  uint8_t next = (_queueHead + 1) % CLOUD_CLIENT_QUEUE_SLOTS;
  if(next == _queueTail) {
    _queueDropped++;
    DBUGLN("Cloud inbound queue full, dropped");
    return;
  }

  Inbound &slot = _queue[_queueHead];
  slot.kind = kind;
  slot.length = length;
  memcpy(slot.payload, payload, length);
  slot.payload[length] = '\0';

  _queueHead = next;
}

void CloudClient::drainQueue()
{
  while(_queueTail != _queueHead)
  {
    Inbound &slot = _queue[_queueTail];

    if(InboundKind_Command == slot.kind)
    {
      _core.onCommand(slot.payload, slot.length);
    }
    else if(InboundKind_Lease == slot.kind)
    {
      // onLease() publishes nothing itself; loop() below is what turns
      // the new lease into a new wait.
      _core.onLease(slot.payload, slot.length);
    }

    _queueTail = (_queueTail + 1) % CLOUD_CLIENT_QUEUE_SLOTS;
  }
}

// -------------------------------------------------------------------
// Change detection
// -------------------------------------------------------------------

void CloudClient::checkControlChanged()
{
  bool changed = false;

  // The claims version covers the manual override, including the
  // auto-release that drops it at the end of a session - which is why
  // this watches claims rather than manual.getVersion().
  if(_claimsVersion != _evse->getClaimsVersion()) {
    _claimsVersion = _evse->getClaimsVersion();
    changed = true;
  }

  if(_limitVersion != limit.getVersion()) {
    _limitVersion = limit.getVersion();
    changed = true;
  }

  if(_scheduleVersion != scheduler.getVersion()) {
    _scheduleVersion = scheduler.getVersion();
    changed = true;
  }

  if(_configVersion != config_version()) {
    _configVersion = config_version();
    changed = true;
  }

  if(changed) {
    _core.onControlChanged();
  }
}

// -------------------------------------------------------------------
// Heap rule and the chip policy
// -------------------------------------------------------------------

void CloudClient::applyHeapRule()
{
  EvseCloudAgentConnections connections;
  connections.cloud_configured = config_cloud_enabled() && cloud_server.length() > 0;
  connections.local_configured = config_mqtt_enabled() && mqtt_server.length() > 0;

  EvseCloudAgentHeapDecision decision =
    _heapRule.evaluate(connections, CLOUD_CHIP_CLASS,
                       ESPAL.getFreeHeap(), monotonicMs());

  if(decision.local_run != _localRun ||
     decision.local_reason != _localStopReason)
  {
    DBUGF("Local publisher %s (%s)", decision.local_run ? "may run" : "stopped",
          evse_cloud_agent_local_stop_to_string(decision.local_reason));

    _localRun = decision.local_run;
    _localStopReason = decision.local_reason;

    // The cloud only learns the local publisher stopped if the flag
    // change is announced - readState() alone is not enough.
    _core.onStateChanged();
  }
}

const char *CloudClient::localStopReason()
{
  return evse_cloud_agent_local_stop_to_string(_localStopReason);
}

void CloudClient::notifyConfigChanged()
{
  _core.setInterval(cloud_agent_interval);

  // A config change can move the identity, the broker or which
  // connections are configured, so the hysteresis state is stale.
  _heapRule.reset();
  applyHeapRule();

  bool enabled = config_cloud_enabled() && cloud_server.length() > 0;

  if(!enabled)
  {
    if(_client.connected() || _connecting) {
      _client.disconnect();
      _connecting = false;
    }
  }
  else if(!_client.connected() && !_connecting)
  {
    _nextReconnectAttempt = 0;      // try again straight away
  }
  else if(_client.connected())
  {
    // Identity or broker may have moved under a live connection.
    char thing[CLOUD_THING_LEN];
    strncpy(thing, _thing, sizeof(thing));
    if(identityFromConfig() && 0 != strcmp(thing, _thing)) {
      DBUGLN("Cloud identity changed, reconnecting");
      _client.disconnect();
      _connecting = false;
    }
  }

  // Config changes can arrive from a web request, which is also a
  // Mongoose callback, so this does not wake the task either.
}

// -------------------------------------------------------------------
// EvseCloudAgentHost
// -------------------------------------------------------------------

bool CloudClient::publish(const char *topic_suffix, const char *payload, bool retain)
{
  if(!_client.connected()) {
    return false;
  }

  bool connect_status = _connectStatusPublish &&
                        0 == strcmp(topic_suffix, EVSE_CLOUD_AGENT_TOPIC_STATUS);

  size_t length = cloud_topic_for_suffix(_topic, sizeof(_topic), _thing,
                                         topic_suffix, connect_status);
  if(0 == length) {
    // Not in the policy's allow-list. Publishing it anyway would not
    // fail the message, it would close the connection.
    DBUGF("Refusing to publish unroutable suffix '%s'", topic_suffix);
    return false;
  }

  // Basic Ingest never reaches the broker, so it cannot retain
  // anything; the core's retain flag is advisory there.
  bool retain_flag = retain && ('$' != _topic[0]);

  return _client.publish(_topic, payload, retain_flag, MG_MQTT_QOS(1));
}

uint64_t CloudClient::monotonicMs()
{
  // millis() wraps every 49 days and the core's lease cap and
  // debounces are absolute, so the wrap is carried here.
  uint32_t now = millis();
  if(now < _monotonicLast) {
    _monotonicHigh += 0x100000000ULL;
  }
  _monotonicLast = now;

  return _monotonicHigh + now;
}

uint32_t CloudClient::epochSeconds()
{
  time_t now = time(NULL);

  // Never synchronised: the core omits the timestamp rather than
  // publishing one from 1970, and refuses a command carrying exp_ts.
  if(now < (time_t)CLOUD_CLOCK_VALID_FROM) {
    return 0;
  }

  return (uint32_t)now;
}

void CloudClient::readState(EvseCloudAgentState &state)
{
  state.state      = _evse->getEvseState();
  state.vehicle    = _evse->isVehicleConnected();
  state.session_wh = _evse->getSessionEnergy();

  state.amp       = _evse->getAmps();
  state.amp_valid = true;

  state.volt       = _evse->getVoltage();
  state.volt_valid = true;

  state.pilot_a     = _evse->getChargeCurrent();
  state.pilot_valid = true;

  if(_evse->isTemperatureValid(EVSE_MONITOR_TEMP_MONITOR)) {
    state.temp_c     = _evse->getTemperature(EVSE_MONITOR_TEMP_MONITOR);
    state.temp_valid = true;
  }

  if(net.isWifiClientConnected()) {
    state.wifi_rssi  = WiFi.RSSI();
    state.rssi_valid = true;
  }

  state.free_heap  = ESPAL.getFreeHeap();
  state.heap_valid = true;

  uint32_t flags = 0;
  if(manual.isActive())  { flags |= EVSE_CLOUD_AGENT_FLAG_MANUAL_OVERRIDE; }
  if(divert.isActive())  { flags |= EVSE_CLOUD_AGENT_FLAG_DIVERT_ACTIVE; }
  if(limit.hasLimit())   { flags |= EVSE_CLOUD_AGENT_FLAG_LIMIT_ACTIVE; }

  // Only when a configured local publisher was actively stopped - a
  // charger that never had a local broker is not "disabled".
  if(EvseCloudAgentLocalStop_OneConnection == _localStopReason ||
     EvseCloudAgentLocalStop_LowHeap == _localStopReason)
  {
    flags |= EVSE_CLOUD_AGENT_FLAG_LOCAL_MQTT_DISABLED;
  }

  state.flags = flags;
}

void CloudClient::readControl(EvseCloudAgentControl &control)
{
  // Filled from live state on every call, never a delta: the cloud
  // reads a missing key as "none set" and wipes that slice of its
  // mirror, so a partial document silently deletes schedule events.

  if(manual.isActive())
  {
    EvseProperties &props = _evse->getClaimProperties(EvseClient_OpenEVSE_Manual);
    control.override_set    = true;
    control.override_active = (EvseState::Active == props.getState());

    uint32_t current = props.getChargeCurrent();
    if(current > 0) {
      control.override_charge_current = (int32_t)current;
      control.override_charge_current_valid = true;
    }

    if(props.hasAutoRelease()) {
      control.override_auto_release = props.isAutoRelease();
      control.override_auto_release_valid = true;
    }
  }

  if(limit.hasLimit())
  {
    LimitProperties props = limit.get();
    control.limit_set          = true;
    control.limit_type         = props.getType().toString();
    control.limit_value        = (int32_t)props.getValue();
    control.limit_auto_release = props.getAutoRelease();
  }

  control.schedule_count = 0;
  for(size_t i = 0; i < SCHEDULER_MAX_EVENTS &&
                    control.schedule_count < CLOUD_CLIENT_MAX_SCHEDULE; i++)
  {
    Scheduler::Event *event = scheduler.eventAt(i);
    if(NULL == event) {
      continue;
    }

    EvseCloudAgentScheduleEvent &out = control.schedule[control.schedule_count];
    out.id     = (int32_t)event->getId();
    out.active = (EvseState::Active == event->getState());
    out.days   = event->getDays() & 0x7f;      // strip SCHEDULER_REPEAT
    snprintf(out.time, sizeof(out.time), "%02d:%02d:%02d",
             (int)event->getHours(), (int)event->getMinutes(),
             (int)event->getSeconds());

    control.schedule_count++;
  }

  // Strings below are borrowed by the core for the length of this
  // call, so every one of them points at storage that outlives it -
  // a global String or a member, never a temporary.
  _chargeMode = (0 == config_charge_mode()) ? "fast" : "eco";

  control.config.max_current_soft       = (int32_t)_evse->getMaxConfiguredCurrent();
  control.config.max_current_soft_valid = true;
  control.config.min_current_hard       = (int32_t)_evse->getMinCurrent();
  control.config.min_current_hard_valid = true;
  control.config.max_current_hard       = (int32_t)_evse->getMaxHardwareCurrent();
  control.config.max_current_hard_valid = true;

  control.config.divert_enabled               = config_divert_enabled();
  control.config.divert_enabled_valid         = true;
  control.config.current_shaper_enabled       = config_current_shaper_enabled();
  control.config.current_shaper_enabled_valid = true;
  control.config.pause_uses_disabled          = config_pause_uses_disabled();
  control.config.pause_uses_disabled_valid    = true;

  control.config.charge_mode = _chargeMode.c_str();
  control.config.version     = currentfirmware.c_str();
  control.config.firmware    = _evse->getFirmwareVersion();
  control.config.hostname    = esp_hostname.c_str();
  control.config.time_zone   = time_zone.c_str();
}

const char *CloudClient::firmwareVersion()
{
  return currentfirmware.c_str();
}

const char *CloudClient::ipAddress()
{
  // net.getIp() returns a temporary; the core borrows what it is given.
  _ipAddress = net.getIp();
  return _ipAddress.c_str();
}

EvseCloudAgentResult CloudClient::setOverride(bool active, uint32_t charge_current,
                                              bool has_charge_current)
{
  EvseProperties props(active ? EvseState::Active : EvseState::Disabled);
  if(has_charge_current) {
    props.setChargeCurrent(charge_current);
  }

  // Same path as the legacy override/set topic: a Manual claim through
  // ManualOverride, which fills in auto_release when it is not set.
  return manual.claim(props) ? EvseCloudAgentResult_Ok : EvseCloudAgentResult_Failed;
}

EvseCloudAgentResult CloudClient::clearOverride()
{
  if(!manual.isActive()) {
    return EvseCloudAgentResult_Ok;      // idempotent
  }

  return manual.release() ? EvseCloudAgentResult_Ok : EvseCloudAgentResult_Failed;
}

EvseCloudAgentResult CloudClient::setLimit(const char *type, int32_t value,
                                           bool auto_release)
{
  if(NULL == type || value < 0) {
    return EvseCloudAgentResult_BadArgs;
  }

  LimitType limit_type;
  limit_type = (LimitType::Value)limit_type.fromString(type);
  if(LimitType::None == limit_type) {
    return EvseCloudAgentResult_BadArgs;
  }

  LimitProperties props;
  props.init();
  props.setType(limit_type);
  props.setValue((uint32_t)value);
  props.setAutoRelease(auto_release);

  return limit.set(props) ? EvseCloudAgentResult_Ok : EvseCloudAgentResult_Failed;
}

EvseCloudAgentResult CloudClient::clearLimit()
{
  if(!limit.hasLimit()) {
    return EvseCloudAgentResult_Ok;      // idempotent
  }

  return limit.clear() ? EvseCloudAgentResult_Ok : EvseCloudAgentResult_Failed;
}

EvseCloudAgentResult CloudClient::setSchedule(const EvseCloudAgentScheduleEvent &event)
{
  if(event.id <= 0 || '\0' == event.time[0]) {
    return EvseCloudAgentResult_BadArgs;
  }

  // The firmware's day bitmask has the same bit order as the agent's
  // (bit 0 sunday), plus SCHEDULER_REPEAT, which every event carries.
  uint8_t days = (event.days & 0x7f) | SCHEDULER_REPEAT;

  return scheduler.addEvent((uint32_t)event.id, event.time, days,
                            event.active ? "active" : "disabled")
    ? EvseCloudAgentResult_Ok : EvseCloudAgentResult_Failed;
}

EvseCloudAgentResult CloudClient::clearSchedule(int32_t id)
{
  if(id <= 0) {
    return EvseCloudAgentResult_BadArgs;
  }

  // Idempotent: removing an event that is not there is not a failure.
  scheduler.removeEvent((uint32_t)id);
  return EvseCloudAgentResult_Ok;
}

EvseCloudAgentResult CloudClient::setDivert(int32_t mode)
{
  if(1 != mode && 2 != mode) {
    return EvseCloudAgentResult_BadArgs;
  }

  divert.setMode((DivertMode)mode);
  return EvseCloudAgentResult_Ok;
}

EvseCloudAgentResult CloudClient::setConfig(const char *json)
{
  if(NULL == json) {
    return EvseCloudAgentResult_BadArgs;
  }

  // The same path POST /config drives. The core has already capped the
  // args at EVSE_CLOUD_AGENT_CONFIG_BUF, so this cannot be large.
  if(!config_deserialize(json)) {
    return EvseCloudAgentResult_BadArgs;
  }

  config_commit(false);
  return EvseCloudAgentResult_Ok;
}

EvseCloudAgentResult CloudClient::restart()
{
  // SCHEDULE the reboot and return: the core publishes the ack after
  // this returns, so rebooting inline would lose it. restart_system()
  // arms a one-second alarm, which is the window the ack goes out in.
  _restartPending = true;
  restart_system();
  return EvseCloudAgentResult_Ok;
}

#else // !ENABLE_CLOUD_CLIENT

CloudClient cloudClient;

#endif // ENABLE_CLOUD_CLIENT
