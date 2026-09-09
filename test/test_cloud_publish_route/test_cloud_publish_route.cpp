// Host-side test of the routing the REAL agent core actually drives.
//
// test_cloud_topics/ asserts the mapping in isolation. This one wires the
// genuine EvseCloudAgentCore to a fake transport that routes exactly as
// CloudClient::publish() does — same cloud_publish_route() call, same
// one-shot in-connect flag — and asserts what comes out the other end.
//
// The question it settles is the one that cannot be answered by reading the
// mapping alone: does the single connect-time status document land on the
// retained device-root topic, and does every later one leave it alone? If it
// does not, a reconnecting consumer finds no retained snapshot and the cloud
// has no baseline to read, which on hardware looks like nothing at all going
// wrong.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "cloud_topics.h"

#include <evse_cloud_agent_core.h>

#include <string.h>
#include <string>
#include <vector>

static const char *THING = "evse-3076f5ec2760";

static const char *STATUS_RETAINED = "d/evse-3076f5ec2760/agent/status";
static const char *STATUS_INGEST =
  "$aws/rules/agent_status/d/evse-3076f5ec2760/agent/status";
static const char *SESSION_INGEST =
  "$aws/rules/agent_session/d/evse-3076f5ec2760/agent/session";
static const char *CONTROL   = "d/evse-3076f5ec2760/agent/control";
static const char *PRESENCE  = "d/evse-3076f5ec2760/agent/presence";

struct Sent
{
  std::string topic;
  std::string payload;
  bool retain;
};

// Stands in for CloudClient: the same routing call, the same one-shot flag,
// and a record of what reached the wire.
class FakeCloudHost : public EvseCloudAgentHost
{
  public:
    std::vector<Sent> sent;
    bool in_connect = false;
    bool connected = true;

    uint64_t now_ms = 1000;
    uint32_t epoch = 1788000000;

    uint8_t state = EVSE_CLOUD_AGENT_STATE_NOT_CONNECTED;
    bool vehicle = false;
    double session_wh = 0;

    // This is CloudClient::publish(), minus Mongoose.
    bool publish(const char *suffix, const char *payload, bool retain) override
    {
      if(!connected) {
        return false;
      }

      char topic[CLOUD_TOPIC_BUF];
      bool retain_flag = false;
      size_t length = cloud_publish_route(topic, sizeof(topic), THING, suffix,
                                          retain, in_connect, &retain_flag);
      if(0 == length) {
        return false;
      }

      sent.push_back({ std::string(topic), std::string(payload), retain_flag });
      return true;
    }

    uint64_t monotonicMs() override { return now_ms; }
    uint32_t epochSeconds() override { return epoch; }

    void readState(EvseCloudAgentState &s) override
    {
      s.state = state;
      s.vehicle = vehicle;
      s.session_wh = session_wh;
    }

    void readControl(EvseCloudAgentControl &c) override
    {
      c.config.hostname = "openevse-test";
    }

    const char *firmwareVersion() override { return "test"; }
    const char *ipAddress() override { return "10.0.0.1"; }

    EvseCloudAgentResult setOverride(bool, uint32_t, bool) override { return EvseCloudAgentResult_Ok; }
    EvseCloudAgentResult clearOverride() override { return EvseCloudAgentResult_Ok; }
    EvseCloudAgentResult setLimit(const char *, int32_t, bool) override { return EvseCloudAgentResult_Ok; }
    EvseCloudAgentResult clearLimit() override { return EvseCloudAgentResult_Ok; }
    EvseCloudAgentResult setSchedule(const EvseCloudAgentScheduleEvent &) override { return EvseCloudAgentResult_Ok; }
    EvseCloudAgentResult clearSchedule(int32_t) override { return EvseCloudAgentResult_Ok; }
    EvseCloudAgentResult setDivert(int32_t) override { return EvseCloudAgentResult_Ok; }
    EvseCloudAgentResult setConfig(const char *) override { return EvseCloudAgentResult_Ok; }
    EvseCloudAgentResult restart() override { return EvseCloudAgentResult_Ok; }

    // What CloudClient::loop() does around core.onConnected()
    void connect(EvseCloudAgentCore &core)
    {
      in_connect = true;
      core.onConnected();
      in_connect = false;
    }

    size_t countTopic(const char *topic) const
    {
      size_t n = 0;
      for(const Sent &s : sent) {
        if(s.topic == topic) { n++; }
      }
      return n;
    }

    const Sent *first(const char *topic) const
    {
      for(const Sent &s : sent) {
        if(s.topic == topic) { return &s; }
      }
      return nullptr;
    }
};

TEST_CASE("connect publishes exactly one retained status, on the plain topic") {
  FakeCloudHost host;
  EvseCloudAgentCore core(host);
  core.setInterval(60);

  host.connect(core);

  CHECK(1 == host.countTopic(STATUS_RETAINED));
  CHECK(0 == host.countTopic(STATUS_INGEST));

  const Sent *status = host.first(STATUS_RETAINED);
  REQUIRE(nullptr != status);
  CHECK(status->retain);

  // Presence and control are retained on the device root too
  const Sent *presence = host.first(PRESENCE);
  REQUIRE(nullptr != presence);
  CHECK(presence->retain);

  const Sent *control = host.first(CONTROL);
  REQUIRE(nullptr != control);
  CHECK(control->retain);
}

TEST_CASE("every status after connect goes to Basic Ingest, unretained") {
  FakeCloudHost host;
  EvseCloudAgentCore core(host);
  core.setInterval(60);

  host.connect(core);
  size_t retained_after_connect = host.countTopic(STATUS_RETAINED);

  // A state change, past the debounce
  host.state = EVSE_CLOUD_AGENT_STATE_CHARGING;
  host.vehicle = true;
  core.onStateChanged();
  host.now_ms += 2000;
  core.loop();

  // The heartbeat
  host.now_ms += 61000;
  core.loop();

  CHECK(host.countTopic(STATUS_INGEST) >= 2);

  // The retained snapshot must not have been touched again: it is the one
  // document a reconnecting consumer reads as current truth.
  CHECK(retained_after_connect == host.countTopic(STATUS_RETAINED));

  for(const Sent &s : host.sent) {
    if(s.topic == STATUS_INGEST) {
      CHECK_FALSE(s.retain);
    }
  }
}

TEST_CASE("a session record takes Basic Ingest even when replayed at connect") {
  FakeCloudHost host;
  EvseCloudAgentCore core(host);
  core.setInterval(60);

  host.connect(core);
  host.sent.clear();

  // Run a charge, then end it while the transport is down, so the record is
  // held and replayed by the next onConnected().
  host.state = EVSE_CLOUD_AGENT_STATE_CHARGING;
  host.vehicle = true;
  core.onStateChanged();
  host.now_ms += 2000;
  core.loop();

  host.session_wh = 1500;
  core.onDisconnected();
  host.connected = false;
  host.state = EVSE_CLOUD_AGENT_STATE_NOT_CONNECTED;
  host.vehicle = false;
  host.now_ms += 60000;
  core.onStateChanged();

  CHECK(0 == host.countTopic(SESSION_INGEST));

  host.connected = true;
  host.sent.clear();
  host.connect(core);

  // The replay rides the same route as any other session record, and the
  // in-connect flag must not have diverted it onto the device root.
  CHECK(1 == host.countTopic(SESSION_INGEST));
  CHECK(0 == host.countTopic("d/evse-3076f5ec2760/agent/session"));

  const Sent *session = host.first(SESSION_INGEST);
  REQUIRE(nullptr != session);
  CHECK_FALSE(session->retain);
}

TEST_CASE("nothing the core publishes ever lands outside the policy") {
  FakeCloudHost host;
  EvseCloudAgentCore core(host);
  core.setInterval(60);

  host.connect(core);

  host.state = EVSE_CLOUD_AGENT_STATE_CHARGING;
  host.vehicle = true;
  core.onStateChanged();
  core.onControlChanged();
  host.now_ms += 2000;
  core.loop();

  const char *cmd =
    "{\"v\":1,\"id\":\"01JQ0000000000000000000000\",\"op\":\"ping\",\"args\":{}}";
  core.onCommand(cmd, strlen(cmd));

  host.now_ms += 61000;
  core.loop();

  REQUIRE(host.sent.size() > 0);

  for(const Sent &s : host.sent)
  {
    bool device_root = (0 == strncmp(s.topic.c_str(), "d/evse-3076f5ec2760/", 20));
    bool ingest = (s.topic == STATUS_INGEST) || (s.topic == SESSION_INGEST);

    // The policy grants the device root and exactly these two Basic Ingest
    // topics. Anything else closes the connection rather than failing the
    // message, so anything else here is a bug that hardware cannot report.
    CHECK((device_root || ingest));

    if(0 == strncmp(s.topic.c_str(), "$aws/rules/", 11)) {
      CHECK(ingest);
      CHECK_FALSE(s.retain);
    }
  }
}

TEST_CASE("the in-connect flag only ever diverts the status document") {
  char topic[CLOUD_TOPIC_BUF];
  bool retain = false;

  // Session, control, presence and ack are unmoved by it
  REQUIRE(cloud_publish_route(topic, sizeof(topic), THING, "agent/session",
                              true, true, &retain));
  CHECK(0 == strcmp(topic, SESSION_INGEST));
  CHECK_FALSE(retain);

  REQUIRE(cloud_publish_route(topic, sizeof(topic), THING, "agent/control",
                              true, true, &retain));
  CHECK(0 == strcmp(topic, CONTROL));
  CHECK(retain);

  REQUIRE(cloud_publish_route(topic, sizeof(topic), THING, "agent/ack",
                              false, true, &retain));
  CHECK(0 == strcmp(topic, "d/evse-3076f5ec2760/agent/ack"));
  CHECK_FALSE(retain);

  // Status is the one that moves
  REQUIRE(cloud_publish_route(topic, sizeof(topic), THING, "agent/status",
                              true, true, &retain));
  CHECK(0 == strcmp(topic, STATUS_RETAINED));
  CHECK(retain);

  REQUIRE(cloud_publish_route(topic, sizeof(topic), THING, "agent/status",
                              true, false, &retain));
  CHECK(0 == strcmp(topic, STATUS_INGEST));
  CHECK_FALSE(retain);
}

TEST_CASE("an unroutable publish reports failure and sets no retain") {
  char topic[CLOUD_TOPIC_BUF];
  bool retain = true;

  CHECK(0 == cloud_publish_route(topic, sizeof(topic), THING, "agent/nonsense",
                                 true, false, &retain));
  CHECK(0 == topic[0]);
  CHECK_FALSE(retain);

  retain = true;
  CHECK(0 == cloud_publish_route(topic, sizeof(topic), "evse-bad", "agent/status",
                                 true, true, &retain));
  CHECK(0 == topic[0]);
  CHECK_FALSE(retain);
}
