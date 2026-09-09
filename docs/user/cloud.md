# Cloud client

The cloud client is a second MQTT connection, separate from the one on the
[MQTT settings page](integrations.md). It carries one consolidated telemetry
contract to a managed broker and nothing else, and it is what a phone app talks
to when it watches or controls the charger from outside the house.

It does not replace, change or interfere with the local MQTT publisher. That
connection keeps its own broker, its own topics and its own format. Neither
ever publishes on the other's socket.

## What it sends

Four documents, each a single JSON object rather than one topic per value, so a
reader never has to guess which snapshot it is looking at:

- **status** — charge state, vehicle presence, session energy, and whichever of
  current, voltage, pilot, temperature, signal strength and free heap are
  available.
- **control** — the override, the session limit, the schedule and a slice of the
  configuration. Always complete, so one document replaces a reader's whole
  picture.
- **presence** — online or offline. The offline half is the connection's last
  will, so the cloud learns about a power cut without waiting for a timeout.
- **session** — one record when a charging run ends.

A status document goes out when something changes (coalesced over about a
second), once a minute otherwise, and faster for a short while when the cloud
asks for it — which is what makes a phone screen feel live without the charger
publishing at that rate all day.

## What it accepts

Commands, each acknowledged: set or clear the override, set or clear the
session limit, add or remove a schedule event, set the solar divert mode,
change a configuration value, and restart the gateway. Every command carries an
identifier, so a command redelivered after a dropped connection is
acknowledged again rather than executed twice.

## Settings

| Option | Meaning |
| --- | --- |
| `cloud_enabled` | Whether the client connects at all. |
| `cloud_server` | Broker hostname. |
| `cloud_port` | Broker port, 8883 by default. TLS always. |
| `cloud_thing` | This charger's cloud identity, `evse-` followed by the twelve hex digits of its WiFi MAC address. Left empty, the firmware derives it. |
| `cloud_certificate_id` | Which stored client certificate to authenticate with. See the certificates page. |
| `cloud_agent_interval` | Seconds between status documents, 60 by default. Zero switches the client off. |

These are normally written for you when the charger is claimed by an account,
not typed in by hand.

The identity is fixed and is not the hostname. The charger can be renamed
whenever you like without affecting the cloud connection, and `mqtt_client_id`
now names the **local** broker connection only.

## One connection or two

A classic ESP32 gateway does not have the memory to run two TLS-capable MQTT
connections at once. On those boards, if both are configured the cloud
connection wins and the local publisher stays down. The MQTT settings page says
so rather than showing a connection that silently never comes up: `GET /status`
carries `local_mqtt_disabled_reason`, which is one of

| Value | Meaning |
| --- | --- |
| *(empty)* | The local publisher is free to run. |
| `not_configured` | No local broker is configured. |
| `one_connection` | This board runs one connection and the cloud has it. |
| `low_heap` | Memory fell below the floor and the local publisher was stopped to protect the cloud connection. |

Boards with more memory, such as the ESP32-S3, run both.

The `low_heap` rule needs three measured numbers — a floor, a restart margin
and how long memory must stay healthy before restarting — and until a soak on
real hardware supplies them the floor is zero, which switches that rule off
entirely. `one_connection` does not depend on them and is always in force.

The client is compiled in only on gateways with room for it. The 4MB
`openevse_wifi_v1` image is close to its partition limit, so it is built
without the cloud client; the 16MB images include it.

## Troubleshooting

**The charger reconnects in a loop, with nothing in the log.** The managed
broker closes the whole connection on an unauthorised publish rather than
rejecting the one message, so a topic that is wrong by a single segment looks
exactly like a network fault. Check that `cloud_thing` matches the identity the
account issued.

**It never connects at all.** The connection's client identifier must equal the
thing name; the firmware derives it from `cloud_thing` for exactly this reason,
so a mismatch means `cloud_thing` itself is wrong. A name that is not `evse-`
plus twelve lowercase hex digits is refused before a connection is attempted.

**Commands do nothing.** Check the clock. A command carries an expiry, and a
charger whose clock has never been set refuses it rather than acting on what
might be a stale replay.
