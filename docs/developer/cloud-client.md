# Cloud client — implementation notes

How the Overwatt cloud client is wired into this firmware, the answers to the
open questions in the cloud team's integration brief, and the places where this
implementation deliberately differs from it.

User-facing behaviour is in [cloud.md](../user/cloud.md). The protocol itself is
the `evse-cloud-agent` library, pinned in `platformio.ini`; nothing in this
firmware reimplements it.

## Shape

`CloudClient` (`src/cloud_client.{h,cpp}`) is one `MicroTasks::Task` that
inherits `EvseCloudAgentHost` and owns:

- its own `MongooseMqttClient`, entirely separate from the one in `mqtt.cpp`
- an `EvseCloudAgentCore`, driven from that task's `loop()`
- an `EvseCloudAgentHeapRule`

`src/cloud_topics.{h,cpp}` is the topic routing, deliberately kept free of every
firmware header so it can be exercised on the host. `CloudClient::publish()` is
a thin wrapper over `cloud_publish_route()`, which decides the topic *and* which
retain flag survives, so the tests drive the same code the device does. It is the highest-risk part of the
integration, because the broker answers an unauthorised publish by closing the
connection rather than rejecting the message: a topic wrong by one segment looks
exactly like a network fault.

## Threading

The core has no internal locking and every entry point must run on one task.
Two places would otherwise break that:

- **Inbound payloads** arrive on Mongoose's event handler.
  `handleMessage()` matches the topic and copies the payload into a three-slot
  ring; `drainQueue()` dispatches them from `loop()`. Nothing calls into the
  core from the callback.
- **`onConnected()`** publishes four documents synchronously, and
  `publishControl()` alone peaks around 2.7 kB of stack. The connect callback
  only subscribes and sets `_pendingConnected`; the core is told from `loop()`.

Neither path calls `MicroTask.wakeTask()`. That is not an oversight:
`MicroTasksClass::wakeTask()` runs the target's `loop()` **synchronously**, so
waking from a Mongoose callback would put `onConnected()` and `onCommand()`
straight back on the stack the deferral exists to get off. The task polls at one
second instead, which is also the core's own debounce, so nothing is lost.
`onDisconnected()` is the one core call made from a callback, because it only
clears a flag and publishes nothing.

Two host suites cover it. `test/test_cloud_topics/` asserts the mapping in
isolation, character for character. `test/test_cloud_publish_route/` wires the
**real** `EvseCloudAgentCore` to a fake transport that routes exactly as
`CloudClient::publish()` does, and asserts the behaviour that reading the
mapping cannot settle: `onConnected()` puts exactly one retained status document
on `d/<thing>/agent/status`, every later status goes to Basic Ingest unretained
and never touches the retained topic again, a held session record replayed at
connect still takes Basic Ingest, and nothing the core publishes across a whole
connect/charge/command/heartbeat cycle lands outside the two grants. That is
bench item four, answered without hardware.

## Change detection

`onStateChanged()` comes from the `EvseManager` state-change listener plus the
heap rule when its decision flips. `onControlChanged()` is driven by polling
four version counters, the way `Mqtt::checkAndPublishUpdates()` does.

The override is watched through **`EvseManager::getClaimsVersion()`**, not
`manual.getVersion()`. `releaseAutoReleaseClaims()` drops the manual claim at
the end of a session by calling `Claim::release()` directly, bypassing
`EvseManager::release()` — so before this change neither version moved and no
event was sent, and every consumer went on showing an override that no longer
existed. That is fixed in `EvseManager::releaseAutoReleaseClaims()`, which now
bumps both versions and sends the same event `release()` does; the claims
version is watched here because it is the one that covers every path.

## Answers to the brief's open questions

**1. Which task owns the MQTT client, and is it the same one that drives
`core.loop()`?** Yes, both are `CloudClient` — see *Threading* above. The
`MicroTasks::Task` shape from the earlier draft survives.

This was built on **master**, not `feature/arduino-idf-component`. Nothing in
the adapter is IDF-specific, so it should move across unchanged; the chip class
is the only compile-time decision and it keys off `CONFIG_IDF_TARGET_*`.

**2. Where `readState()` and `readControl()` get their data.**

| Field | Source |
| --- | --- |
| state, vehicle, session_wh, amp, volt, pilot | `EvseManager` |
| temp_c | `EvseManager::getTemperature(EVSE_MONITOR_TEMP_MONITOR)`, gated on `isTemperatureValid()` |
| wifi_rssi | `WiFi.RSSI()`, gated on `net.isWifiClientConnected()` |
| free_heap | `ESPAL.getFreeHeap()` |
| flags | `manual.isActive()`, `divert.isActive()`, `limit.hasLimit()`, the heap rule |
| override | `manual` + `EvseManager::getClaimProperties(EvseClient_OpenEVSE_Manual)` |
| limit | `limit.get()` |
| schedule | `Scheduler::eventAt()`, a new accessor |
| config | `EvseManager` currents, the config flag accessors, `currentfirmware`, `esp_hostname`, `time_zone` |

`Scheduler::eventAt(index)` was added so the control document can be built from
a fixed-size walk rather than serialising the whole schedule into a
`DynamicJsonDocument` on a heap-tight board.

Borrowed strings all point at storage that outlives the call: globals, or the
`_ipAddress` / `_chargeMode` members, because `net.getIp()` returns a temporary.

**3. How the local publisher and the cloud client share the MQTT client on S3.**
They do not share anything. Two independent `MongooseMqttClient` instances, as
the design assumed. The only coupling is one-way: `Mqtt::loop()` and
`Mqtt::attemptConnection()` consult `cloudClient.localPublisherAllowed()`, and a
stub with the same surface keeps those call sites free of `#ifdef` when the
client is not compiled in.

**4. Is `mqtt_client_id` shared with the local-broker connection?** No, and the
claim should stop writing it.

The cloud connection's client id is **derived from `cloud_thing` in the
firmware** and is not configurable. Since the IoT policy resolves through
`${iot:Connection.Thing.ThingName}`, a client id that is not the thing name
fails to connect at all rather than losing a topic — so making it a separate
writable key only creates a way to break the device. Deriving it means the two
cannot drift.

`mqtt_client_id` is a **new** key that names the **local** connection only. It
defaults to `esp_hostname`, which is what the firmware used before it was
configurable, so existing installs are unchanged. The cloud may write it
harmlessly, but nothing needs it to.

**5. Config key names and encodings.** All accepted as proposed.

| Key | Type | Short name | Default |
| --- | --- | --- | --- |
| `cloud_enabled` | JSON boolean | `ce` | false |
| `cloud_server` | string | `cs` | "" |
| `cloud_port` | number | `cpt` | 8883 |
| `cloud_thing` | string | `cth` | "" (derived from the MAC when empty) |
| `cloud_certificate_id` | string | `cci` | "" |
| `cloud_agent_interval` | number | `cai` | 60 |
| `mqtt_client_id` | string | `mcid` | hostname |

`cloud_enabled` is a real JSON boolean: it is a bit in the `flags` word exposed
through `ConfigOptVirtualMaskedBool`, which serialises as `true`/`false`, the
same as `mqtt_enabled` and `divert_enabled`.

The GUI reads the local-publisher stop reason from
**`local_mqtt_disabled_reason`** on `GET /status`, carrying the library's own
strings verbatim (`""`, `not_configured`, `one_connection`, `low_heap`).
`GET /status` also gained `cloud_connected` and `cloud_thing`.

The status flag keeps the spelling `local_mqtt_disabled`, and is set **only**
for `one_connection` and `low_heap` — the cases where a configured publisher was
actively refused. A charger that never had a local broker reports
`not_configured` on `/status` but does not raise the flag, since nothing was
disabled.

## Deviations from the brief

**A build-inclusion gate, `ENABLE_CLOUD_CLIENT`.** The 4MB `openevse_wifi_v1`
image has under 3 kB of app partition to spare before any of this, so the client
is compiled in on the 16MB environments only. This is not the runtime switch the
brief replaced with `cloud_enabled` — that remains the runtime switch — it is
the same kind of flash-budget gate as `DISABLE_OCPP`.

The `cloud_*` config keys are gated with it, because six options the board can
never act on cost about 1.2 kB of a 2.6 kB margin. `mqtt_client_id` is not
gated: it names the local connection and is useful everywhere.

**ArduinoMongoose needed a change.** `MongooseMqttClient::subscribe()` had no
QoS parameter and always sent 0, and IoT Core delivers at min(publisher,
subscriber) QoS — so `agent/cmd` would have been fire-and-forget however the
cloud published it. `platformio.ini` therefore pins `RAR/ArduinoMongoose` at
`ca0a817` plus one commit adding an optional level. Note the level is a plain
0..2, not the `MG_MQTT_QOS()` flags encoding `publish()` takes; the two sit one
argument apart and mixing them silently requests QoS 2. Revert the pin to
`jeremypoulter` once that is merged upstream.

## Footprint

Measured on `openevse_wifi_tft_v1`, with and without `ENABLE_CLOUD_CLIENT`,
everything else identical.

| `openevse_wifi_tft_v1` | Baseline | With the client | Delta |
| --- | --- | --- | --- |
| Flash | 2,600,095 | 2,617,383 | +17,288 |
| Static RAM | 88,444 | 91,284 | +2,840 |

The RAM figure is mostly the 1,536-byte inbound queue plus the core's own
`.bss`. Both are comfortably inside the 16MB budget.

The 4MB board is why the gate exists. `openevse_wifi_v1` was already at 99.8%
of its 1,966,080-byte app partition on fork master, with 3,279 bytes to spare:

| `openevse_wifi_v1` | Bytes | Free |
| --- | --- | --- |
| Fork master (a4e387f4) | 1,962,801 | 3,279 |
| This branch | 1,963,421 | 2,659 |

The 620-byte difference is `mqtt_client_id` and the routing helper. Gating the
`cloud_*` options as well as the client is what keeps it to that rather than
1,216 bytes.

## Still to do on the bench

1. Confirm on the AWS console that the three agent rules carry the device-root
   SQL, and that the legacy `ev_*` rules still match what they matched before.
2. **Measure the heap rule's three numbers** by soaking a WROOM and an S3 with
   TLS up and both connections configured. They are compile-time constants
   (`CLOUD_HEAP_FLOOR_BYTES`, `CLOUD_HEAP_MARGIN_BYTES`,
   `CLOUD_HEAP_SUSTAIN_MS`) and stay at zero — the rule switched off — until
   the soak supplies them. `one_connection` does not depend on them.
3. Verify the last will flips presence on a power cut.
4. Verify exactly one retained status document per connect, and that later
   status publishes do not appear on the retained topic. Covered on the host by
   `test/test_cloud_publish_route/`; the bench check is now only that the
   broker agrees.
5. Verify both Basic Ingest topics are authorised, spelled exactly. Watch for a
   reconnect loop that starts on the first heartbeat rather than at connect —
   that is the signature of an unauthorised publish.
6. After a day, read CloudWatch `PublishIn`: under 5k/day/charger.
7. Roll out bench S3 first, then the classic ESP32. Unclaim under the old name
   before claiming fresh as a cloud client.
