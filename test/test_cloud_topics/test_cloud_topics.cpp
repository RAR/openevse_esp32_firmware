// Host-side tests for the cloud topic routing (cloud_topics.cpp).
//
// This is the one place where being wrong is invisible. The AWS IoT device
// policy authorises the device root and precisely two Basic Ingest topics; an
// unauthorised publish is not a rejected message, IoT Core closes the MQTT
// connection, with no error payload to log. A topic off by one segment
// therefore shows up on the bench as a reconnect loop with no cause. So the
// allow-list is asserted here character for character, and an unroutable
// suffix must refuse rather than produce something plausible.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "cloud_topics.h"

#include <string.h>

static const char *THING = "evse-3076f5ec2760";

TEST_CASE("a thing name is evse- plus twelve lowercase hex") {
  CHECK(cloud_thing_valid("evse-3076f5ec2760"));
  CHECK(cloud_thing_valid("evse-000000000000"));
  CHECK(cloud_thing_valid("evse-ffffffffffff"));

  CHECK_FALSE(cloud_thing_valid(NULL));
  CHECK_FALSE(cloud_thing_valid(""));
  CHECK_FALSE(cloud_thing_valid("evse-3076F5EC2760"));   // upper case
  CHECK_FALSE(cloud_thing_valid("evse-3076f5ec276"));    // 11 digits
  CHECK_FALSE(cloud_thing_valid("evse-3076f5ec27600"));  // 13 digits
  CHECK_FALSE(cloud_thing_valid("evse-3076f5ec276g"));   // not hex
  CHECK_FALSE(cloud_thing_valid("openevse-3076f5ec2760"));
  CHECK_FALSE(cloud_thing_valid("3076f5ec2760"));
  CHECK_FALSE(cloud_thing_valid("evse-3076f5ec2760 "));  // trailing space
}

TEST_CASE("the thing name derives from a MAC in any spelling") {
  char buf[CLOUD_THING_LEN];

  CHECK(cloud_thing_from_mac(buf, sizeof(buf), "30:76:F5:EC:27:60"));
  CHECK(0 == strcmp(buf, "evse-3076f5ec2760"));

  CHECK(cloud_thing_from_mac(buf, sizeof(buf), "30-76-f5-ec-27-60"));
  CHECK(0 == strcmp(buf, "evse-3076f5ec2760"));

  CHECK(cloud_thing_from_mac(buf, sizeof(buf), "3076f5ec2760"));
  CHECK(0 == strcmp(buf, "evse-3076f5ec2760"));

  // Whatever it produces must satisfy the validator, or the device
  // would connect with a client id the IoT policy refuses.
  CHECK(cloud_thing_valid(buf));
}

TEST_CASE("a MAC that is not twelve hex digits is refused, not truncated") {
  char buf[CLOUD_THING_LEN];

  CHECK_FALSE(cloud_thing_from_mac(buf, sizeof(buf), "30:76:F5:EC:27"));
  CHECK(0 == buf[0]);

  CHECK_FALSE(cloud_thing_from_mac(buf, sizeof(buf), "30:76:F5:EC:27:60:11"));
  CHECK(0 == buf[0]);

  CHECK_FALSE(cloud_thing_from_mac(buf, sizeof(buf), "not-a-mac"));
  CHECK(0 == buf[0]);

  CHECK_FALSE(cloud_thing_from_mac(buf, sizeof(buf), NULL));
  CHECK(0 == buf[0]);

  // A buffer one short of the answer must fail rather than half-fill
  char small[CLOUD_THING_LEN - 1];
  CHECK_FALSE(cloud_thing_from_mac(small, sizeof(small), "3076f5ec2760"));
}

TEST_CASE("the connect-time status is the ordinary retained topic") {
  char buf[CLOUD_TOPIC_BUF];

  REQUIRE(cloud_topic_for_suffix(buf, sizeof(buf), THING, "agent/status", true));
  CHECK(0 == strcmp(buf, "d/evse-3076f5ec2760/agent/status"));
}

TEST_CASE("every later status goes through Basic Ingest, spelled exactly") {
  char buf[CLOUD_TOPIC_BUF];

  REQUIRE(cloud_topic_for_suffix(buf, sizeof(buf), THING, "agent/status", false));
  CHECK(0 == strcmp(buf,
    "$aws/rules/agent_status/d/evse-3076f5ec2760/agent/status"));
}

TEST_CASE("a session record always goes through Basic Ingest") {
  char buf[CLOUD_TOPIC_BUF];

  // Including the replay onConnected() may make for a run that ended
  // while the link was down, which takes the same route as any other.
  for(int connect_status = 0; connect_status <= 1; connect_status++)
  {
    REQUIRE(cloud_topic_for_suffix(buf, sizeof(buf), THING, "agent/session",
                                   0 != connect_status));
    CHECK(0 == strcmp(buf,
      "$aws/rules/agent_session/d/evse-3076f5ec2760/agent/session"));
  }
}

TEST_CASE("control, presence and ack are always the ordinary topic") {
  char buf[CLOUD_TOPIC_BUF];

  REQUIRE(cloud_topic_for_suffix(buf, sizeof(buf), THING, "agent/control", true));
  CHECK(0 == strcmp(buf, "d/evse-3076f5ec2760/agent/control"));

  REQUIRE(cloud_topic_for_suffix(buf, sizeof(buf), THING, "agent/control", false));
  CHECK(0 == strcmp(buf, "d/evse-3076f5ec2760/agent/control"));

  REQUIRE(cloud_topic_presence(buf, sizeof(buf), THING));
  CHECK(0 == strcmp(buf, "d/evse-3076f5ec2760/agent/presence"));

  REQUIRE(cloud_topic_for_suffix(buf, sizeof(buf), THING, "agent/ack", false));
  CHECK(0 == strcmp(buf, "d/evse-3076f5ec2760/agent/ack"));
}

TEST_CASE("the two subscribed topics sit under the device root") {
  char buf[CLOUD_TOPIC_BUF];

  REQUIRE(cloud_topic_cmd(buf, sizeof(buf), THING));
  CHECK(0 == strcmp(buf, "d/evse-3076f5ec2760/agent/cmd"));

  REQUIRE(cloud_topic_lease(buf, sizeof(buf), THING));
  CHECK(0 == strcmp(buf, "d/evse-3076f5ec2760/lease/set"));
}

TEST_CASE("only two topics may ever appear under $aws/rules/") {
  char buf[CLOUD_TOPIC_BUF];

  // The wildcard grant was removed because a * in an IoT policy resource
  // spans /, which let one charger write another's topics. So nothing
  // but these two may be built with the prefix.
  static const char *SUFFIXES[] = {
    "agent/status", "agent/session", "agent/control", "agent/presence",
    "agent/ack", "agent/cmd", "lease/set"
  };

  int ingest = 0;
  for(size_t i = 0; i < sizeof(SUFFIXES) / sizeof(SUFFIXES[0]); i++)
  {
    for(int connect_status = 0; connect_status <= 1; connect_status++)
    {
      if(cloud_topic_for_suffix(buf, sizeof(buf), THING, SUFFIXES[i],
                                0 != connect_status))
      {
        if(0 == strncmp(buf, "$aws/rules/", 11)) {
          CHECK((0 == strcmp(buf, "$aws/rules/agent_status/d/evse-3076f5ec2760/agent/status") ||
                 0 == strcmp(buf, "$aws/rules/agent_session/d/evse-3076f5ec2760/agent/session")));
          ingest++;
        }
      }
    }
  }

  // status (event route only) once, session on both routes
  CHECK(3 == ingest);
}

TEST_CASE("an unroutable suffix refuses rather than inventing a topic") {
  char buf[CLOUD_TOPIC_BUF];

  static const char *BAD[] = {
    "agent/statuses", "agent", "agent/", "/agent/status", "agent/status/",
    "lease", "lease/get", "agent/config", "#", "+", "", "rapi/in"
  };

  for(size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++)
  {
    CHECK(0 == cloud_topic_for_suffix(buf, sizeof(buf), THING, BAD[i], false));
    CHECK(0 == buf[0]);
    CHECK(0 == cloud_topic_for_suffix(buf, sizeof(buf), THING, BAD[i], true));
    CHECK(0 == buf[0]);
  }

  CHECK(0 == cloud_topic_for_suffix(buf, sizeof(buf), THING, NULL, false));
}

TEST_CASE("an invalid thing name publishes nothing at all") {
  char buf[CLOUD_TOPIC_BUF];

  static const char *BAD_THINGS[] = {
    NULL, "", "evse-", "evse-3076f5ec276", "openevse-3076f5ec2760",
    "evse-3076F5EC2760", "+", "#"
  };

  for(size_t i = 0; i < sizeof(BAD_THINGS) / sizeof(BAD_THINGS[0]); i++)
  {
    CHECK(0 == cloud_topic_for_suffix(buf, sizeof(buf), BAD_THINGS[i],
                                      "agent/status", true));
    CHECK(0 == buf[0]);
    CHECK(0 == cloud_topic_cmd(buf, sizeof(buf), BAD_THINGS[i]));
    CHECK(0 == buf[0]);
  }
}

TEST_CASE("a short buffer produces nothing, never a truncated topic") {
  // A truncated topic is an unauthorised topic, which costs the whole
  // connection - so partial output is never acceptable.
  const char *longest =
    "$aws/rules/agent_session/d/evse-3076f5ec2760/agent/session";
  size_t needed = strlen(longest);

  char exact[64];
  REQUIRE(needed + 1 <= sizeof(exact));
  CHECK(needed == cloud_topic_for_suffix(exact, needed + 1, THING,
                                         "agent/session", false));
  CHECK(0 == strcmp(exact, longest));

  CHECK(0 == cloud_topic_for_suffix(exact, needed, THING, "agent/session", false));
  CHECK(0 == exact[0]);

  char tiny[4];
  CHECK(0 == cloud_topic_cmd(tiny, sizeof(tiny), THING));
  CHECK(0 == tiny[0]);
}

TEST_CASE("CLOUD_TOPIC_BUF holds the longest topic this file can build") {
  char buf[CLOUD_TOPIC_BUF];
  size_t longest = 0;

  static const char *SUFFIXES[] = {
    "agent/status", "agent/session", "agent/control", "agent/presence",
    "agent/ack", "agent/cmd", "lease/set"
  };

  for(size_t i = 0; i < sizeof(SUFFIXES) / sizeof(SUFFIXES[0]); i++)
  {
    for(int connect_status = 0; connect_status <= 1; connect_status++)
    {
      size_t length = cloud_topic_for_suffix(buf, sizeof(buf), THING,
                                             SUFFIXES[i], 0 != connect_status);
      CHECK(length > 0);
      if(length > longest) { longest = length; }
    }
  }

  CHECK(longest + 1 <= CLOUD_TOPIC_BUF);
}
