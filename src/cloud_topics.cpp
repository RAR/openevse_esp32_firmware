#include "cloud_topics.h"

#include <string.h>

// Suffixes the core can hand us, and where each one goes. Anything not
// in this table is refused: the device policy grants the device root
// and two Basic Ingest topics, and an unauthorised publish costs the
// whole MQTT connection.
#define SUFFIX_STATUS   "agent/status"
#define SUFFIX_SESSION  "agent/session"
#define SUFFIX_CONTROL  "agent/control"
#define SUFFIX_PRESENCE "agent/presence"
#define SUFFIX_ACK      "agent/ack"
#define SUFFIX_CMD      "agent/cmd"
#define SUFFIX_LEASE    "lease/set"

static bool is_lower_hex(char c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

static int hex_value(char c)
{
  if(c >= '0' && c <= '9') { return c - '0'; }
  if(c >= 'a' && c <= 'f') { return 10 + (c - 'a'); }
  if(c >= 'A' && c <= 'F') { return 10 + (c - 'A'); }
  return -1;
}

bool cloud_thing_valid(const char *thing)
{
  if(NULL == thing) {
    return false;
  }

  if(0 != strncmp(thing, "evse-", 5)) {
    return false;
  }

  const char *hex = thing + 5;
  for(int i = 0; i < 12; i++)
  {
    if(!is_lower_hex(hex[i])) {
      return false;
    }
  }

  // Exactly 12, no trailing anything
  return '\0' == hex[12];
}

bool cloud_thing_from_mac(char *buf, size_t len, const char *mac)
{
  if(NULL == buf || len < CLOUD_THING_LEN) {
    if(NULL != buf && len > 0) { buf[0] = '\0'; }
    return false;
  }

  buf[0] = '\0';

  if(NULL == mac) {
    return false;
  }

  char hex[13];
  int count = 0;

  for(const char *p = mac; '\0' != *p; p++)
  {
    if(':' == *p || '-' == *p || '.' == *p || ' ' == *p) {
      continue;
    }

    int value = hex_value(*p);
    if(value < 0) {
      return false;      // not a MAC at all
    }

    if(count >= 12) {
      return false;      // too many digits
    }

    hex[count++] = "0123456789abcdef"[value];
  }

  if(12 != count) {
    return false;
  }
  hex[12] = '\0';

  memcpy(buf, "evse-", 5);
  memcpy(buf + 5, hex, 13);
  return true;
}

// Join the parts into buf, or leave it empty and return 0 if they do
// not fit. Every caller below is all-or-nothing for the same reason:
// a truncated topic is an unauthorised topic.
static size_t join(char *buf, size_t len, const char *a, const char *b,
                   const char *c, const char *d)
{
  size_t total = strlen(a) + strlen(b) + strlen(c) + strlen(d);
  if(total + 1 > len) {
    if(len > 0) { buf[0] = '\0'; }
    return 0;
  }

  const char *parts[4] = { a, b, c, d };
  char *p = buf;
  for(int i = 0; i < 4; i++)
  {
    size_t part_len = strlen(parts[i]);
    memcpy(p, parts[i], part_len);
    p += part_len;
  }
  *p = '\0';

  return total;
}

size_t cloud_topic_for_suffix(char *buf, size_t len, const char *thing,
                              const char *suffix, bool connect_status)
{
  if(NULL == buf || 0 == len) {
    return 0;
  }
  buf[0] = '\0';

  if(NULL == suffix || !cloud_thing_valid(thing)) {
    return 0;
  }

  // Basic Ingest, the two literal grants. Built from the rule name and
  // the thing name so the string is never assembled by hand elsewhere.
  if(0 == strcmp(suffix, SUFFIX_STATUS) && !connect_status) {
    return join(buf, len, "$aws/rules/" CLOUD_RULE_STATUS "/" CLOUD_TOPIC_ROOT "/",
                thing, "/", SUFFIX_STATUS);
  }

  if(0 == strcmp(suffix, SUFFIX_SESSION)) {
    return join(buf, len, "$aws/rules/" CLOUD_RULE_SESSION "/" CLOUD_TOPIC_ROOT "/",
                thing, "/", SUFFIX_SESSION);
  }

  // The device root: the connect-time status snapshot and everything
  // that is not a Basic Ingest publish.
  if(0 == strcmp(suffix, SUFFIX_STATUS)   ||
     0 == strcmp(suffix, SUFFIX_CONTROL)  ||
     0 == strcmp(suffix, SUFFIX_PRESENCE) ||
     0 == strcmp(suffix, SUFFIX_ACK)      ||
     0 == strcmp(suffix, SUFFIX_CMD)      ||
     0 == strcmp(suffix, SUFFIX_LEASE))
  {
    return join(buf, len, CLOUD_TOPIC_ROOT "/", thing, "/", suffix);
  }

  // Not in the allow-list. Refusing costs one dropped publish; sending
  // it would cost the connection.
  return 0;
}

size_t cloud_publish_route(char *buf, size_t len, const char *thing,
                           const char *suffix, bool retain_requested,
                           bool in_connect, bool *retain_out)
{
  if(NULL != retain_out) {
    *retain_out = false;
  }

  // Only the status document has a connect-time route. onConnected() also
  // publishes presence, control and possibly a held session record, and
  // none of those change route because of where they were published from.
  bool connect_status = in_connect && NULL != suffix &&
                        0 == strcmp(suffix, SUFFIX_STATUS);

  size_t length = cloud_topic_for_suffix(buf, len, thing, suffix, connect_status);
  if(0 == length) {
    return 0;
  }

  if(NULL != retain_out) {
    *retain_out = retain_requested && '$' != buf[0];
  }

  return length;
}

size_t cloud_topic_cmd(char *buf, size_t len, const char *thing)
{
  return cloud_topic_for_suffix(buf, len, thing, SUFFIX_CMD, false);
}

size_t cloud_topic_lease(char *buf, size_t len, const char *thing)
{
  return cloud_topic_for_suffix(buf, len, thing, SUFFIX_LEASE, false);
}

size_t cloud_topic_presence(char *buf, size_t len, const char *thing)
{
  return cloud_topic_for_suffix(buf, len, thing, SUFFIX_PRESENCE, false);
}
