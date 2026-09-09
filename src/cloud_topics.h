#ifndef _OPENEVSE_CLOUD_TOPICS_H
#define _OPENEVSE_CLOUD_TOPICS_H

// -------------------------------------------------------------------
// Topic routing for the Overwatt cloud MQTT connection.
//
// The agent core publishes a bare suffix ("agent/status", "agent/ack",
// ...) and leaves the full topic to the host. This file is that
// mapping, and nothing else: no Arduino, no ESP-IDF, no firmware
// headers, so the whole allow-list is exercised by the native tests.
//
// It matters that this is exact. The AWS IoT device policy authorises
// publishing to the device root and to precisely two Basic Ingest
// topics; an unauthorised publish is not a per-message error, IoT Core
// closes the MQTT connection. A topic off by one segment therefore
// shows up as a reconnect loop with no logged cause, so an unknown
// suffix is refused here rather than sent and guessed at.
// -------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

// The device root's first segment: every topic is d/<thing>/...
#ifndef CLOUD_TOPIC_ROOT
#define CLOUD_TOPIC_ROOT "d"
#endif

// The two IoT rule names that appear inside a Basic Ingest topic. They
// are part of the policy's resource list, so they are literals, not a
// convention - see docs/user/cloud.md.
#ifndef CLOUD_RULE_STATUS
#define CLOUD_RULE_STATUS "agent_status"
#endif
#ifndef CLOUD_RULE_SESSION
#define CLOUD_RULE_SESSION "agent_session"
#endif

// "evse-" plus 12 hex digits, plus the NUL
#define CLOUD_THING_LEN 18

// Longest topic this file can build:
//   "$aws/rules/agent_session/d/<thing>/agent/session"
// = 24 + 2 + 17 + 14 + NUL. 96 leaves room for a renamed rule.
#define CLOUD_TOPIC_BUF 96

// True when thing is exactly "evse-" followed by 12 lowercase hex
// digits. The IoT policy resolves through the thing name, so a name
// that fails this will not merely lose a topic, it will fail to
// connect at all - better to refuse to publish and say why.
bool cloud_thing_valid(const char *thing);

// Derive the canonical thing name from a MAC address in any of the
// usual spellings ("30:76:F5:EC:27:60", "30-76-f5-ec-27-60",
// "3076f5ec2760"). Writes "evse-3076f5ec2760" and returns true, or
// returns false and leaves buf empty when the MAC does not hold
// exactly 12 hex digits or buf is shorter than CLOUD_THING_LEN.
bool cloud_thing_from_mac(char *buf, size_t len, const char *mac);

// Build the full topic for one of the core's suffixes.
//
// connect_status selects the connect-time route for "agent/status":
// true gives the ordinary retained d/<thing>/agent/status topic (the
// one retained snapshot the broker holds), false gives Basic Ingest.
// It is ignored for every other suffix.
//
// Returns the length written, or 0 - leaving buf empty - when the
// suffix is not one the policy authorises, the thing name is invalid,
// or buf is too small. A 0 return must be treated as "do not publish".
size_t cloud_topic_for_suffix(char *buf, size_t len, const char *thing,
                              const char *suffix, bool connect_status);

// The two topics subscribed at connect, by the same rules. Convenience
// wrappers over cloud_topic_for_suffix() so callers cannot misspell a
// suffix that only fails at runtime.
size_t cloud_topic_cmd(char *buf, size_t len, const char *thing);
size_t cloud_topic_lease(char *buf, size_t len, const char *thing);

// The presence topic, which is also the connection's last will.
size_t cloud_topic_presence(char *buf, size_t len, const char *thing);

#endif // _OPENEVSE_CLOUD_TOPICS_H
