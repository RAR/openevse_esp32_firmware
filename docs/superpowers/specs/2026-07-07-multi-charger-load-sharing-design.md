# Multi-Charger Household Load Sharing — Design

**Date:** 2026-07-07
**Status:** Approved design, pre-implementation
**Scope:** Lease-based load sharing for households with 2+ OpenEVSE chargers, coordinated
by a dedicated in-home "Load Manager" device (ESP32-P4 + touchscreen), with the protocol
designed primary-agnostic so the coordinator could later be hosted elsewhere (a charger,
HA, etc.) without protocol changes.

---

## 1. Problem and goals

Households with two or more EVSEs share one panel/feeder limit. Today each OpenEVSE can
shape its own current (current shaper, solar divert), but two independent controllers
reacting to the same CT race and oscillate, and nothing enforces a *combined* ceiling.
Commercial systems solve this with leader/follower groups (Tesla Wall Connector, Wallbox
Power Sharing, go-e); it is a frequently requested OpenEVSE feature.

**Goals**

- Hard guarantee: combined EV draw never exceeds the household EV budget, under every
  failure mode (reboots, WiFi partition, coordinator death), without a consensus protocol.
- Demand shifting: an idle/full charger's share flows to the active one; a tight budget
  time-slices fairly rather than starving one car.
- Solar awareness: eco-mode chargers draw from exportable surplus; fast-mode chargers
  draw from the grid budget.
- Chargers remain fully functional standalone (no Load Manager present) — no removed
  features, no fork from upstream behavior.
- Charger-side changes structured for upstream PRs from day one.

**Non-goals (v1)**

- SoC-aware allocation (layers on later as a priority tiebreaker; the box already sees
  `battery_level` in charger status).
- Charger-hosted or HA-hosted primary implementations (protocol permits them; not built).
- Porting the box to cheap ESP32-S3 panel hardware — v1 ships on the ESP32-P4 (§7); the
  box firmware stays display-abstracted so an S3 port is possible later.
- Per-phase / 3-phase budget modelling. Budget and leases are single-value amps.

## 2. Decisions (with alternatives considered)

| Decision | Chosen | Rejected alternatives |
|---|---|---|
| Primary form | Protocol-first: primary-agnostic lease protocol; dedicated box is the reference (v1: only) primary | Box-only protocol; charger-hosted + box both in v1 |
| Data ingestion | Box is the *preferred* source, distributing house-level data to chargers; chargers keep all standalone capability | Stripping MQTT/HA ingestion from chargers (breaks standalone + upstream) |
| Budget intelligence | Full dynamic: static budget + CT headroom + solar surplus, each layer degrading gracefully | Static-only; static+CT-only |
| Screen | LVGL touchscreen from day one | Headless v1 |
| Mode control (eco/fast) | Settable on charger or box; box arbitrates (last-writer-wins, box wins ties) | Charger-authoritative; box-authoritative |
| Transport | WebSocket, charger→box | HTTP polling (box-initiated); MQTT via broker |
| Scarcity policy | Per-charger priority + rotation among equal-priority starving chargers | Static priority only; SoC-aware |
| Box hardware | Guition JC4880P443C (ESP32-P4 + 4.3" ST7701 touch, already ported) | New S3 panel bring-up first |
| Box firmware home | This repo, new PlatformIO env (FakeEVSE-style personality) | Separate repo |
| Upstream posture | Upstream-first: discussion issue, then small PRs against upstream master for charger-side pieces; box env stays fork-side | Fork-only; modernization-branch base |
| Fleet size | Design for N secondaries (charger list), test with 2 | Hard-coded pair |

## 3. Architecture

Three pieces, one protocol:

1. **Managed-mode module** (charger side, upstream target) — gated firmware module any
   OpenEVSE charger can enable. Owns a WebSocket client to the box and a Safety-class
   claim on the EVSE manager that enforces the current lease — or the configured
   islanded current when no lease is live.
2. **Load Manager box** (fork reference primary) — new env in this repo reusing WiFi
   provisioning, web server, config store, MQTT/HA clients, LVGL stack, and OTA. Runs the
   WS server, the allocator, and the status screen. No RAPI/EVSE monitor/OCPP.
3. **Lease protocol** — charger-initiated WebSocket carrying JSON messages (§5). This is
   the part that gets specified publicly and PR'd upstream.

### The core invariant

> A charger with managed mode enabled and no live lease charges at its configured
> **islanded current** (default **0 A** = paused), period.

This one rule collapses every failure mode — box death, box reboot with amnesia, charger
reboot, WiFi partition, stale grants — into a single safe state. The box never needs to
persist grants.

**Safety rule (validated on the box at config-write time):**
Σ(islanded currents of all registered chargers) ≤ static budget. The box refuses a
configuration that could overload the panel even with every charger islanded.

**Allocation-time rule:** Σ(active leases) + Σ(islanded currents of currently
*disconnected* registered chargers) ≤ live budget. The box always reserves the islanded
current of any charger it cannot see, because that charger may legitimately be drawing it.

An islanded current > 0 (e.g. 6 A) is an explicit availability-vs-fail-dark trade the
user can opt into per charger (garage WiFi flakiness should not mean the car is empty at
7am); the safety rule bounds the worst case.

### Composition with the Charge Manager (upstream PR #1112, open)

PR #1112 gives scheduler timer windows per-event **features** (Divert, Shaper, RFID,
Current = amps, OCPP placeholder) and **session limits** (Time / Energy / SoC / Cost),
activating timer-controlled divert/shaper claims at Limit-class priority **1100** (the
standalone shaper keeps Safety 5000). The lease is designed to sit cleanly on top:

- The lease claims **`max_current`** (plus `state` for pause) at a priority strictly
  above Safety 5000 — above both the standalone shaper and every #1112 window claim.
  Because `max_current` composes as a *ceiling* on whatever charge current wins below
  it, schedules keep deciding **when** and **intent** while the lease bounds **how
  much**: a scheduled 32 A "Current" window under a 16 A lease charges at 16 A; an eco
  window under that lease tracks surplus below 16 A.
- While managed, a window whose feature is **Shaper** is inert (redundant — the lease is
  the shaped limit), and a **Divert** window maps to `mode_request(eco)` for the window's
  duration (house-level eco pool replaces local divert), reverting after. Existing
  schedules keep their meaning without running a second controller.
- **Session limits** (Time/Energy/SoC) and **RFID-required** windows remain fully local
  and orthogonal: a lease caps amps; a limit ends a session.
- #1112's `setSolar()/setGridIe()` injection points on divert confirm the upstream
  data-push surface the `data` message reuses.
- Practically: PR B (§9) is developed to apply on top of #1112 (it likely merges first),
  and #1112's pytest integration harness (`tests/integration/`) is the natural home for
  the protocol soak tests driving FakeEVSE instances.

## 4. Managed-mode module (charger side)

**Config** (config store keys, all persisted):

- `managed_mode_enabled` (bool, default false)
- `managed_mode_url` (string — `ws://<box-host>/managed`; charger initiates, so the box
  never needs charger addresses)
- `managed_mode_islanded_current` (amps, default 0)

**Behavior:**

- On enable/boot: claim `max_current = islanded_current` immediately, at a priority
  strictly above Safety 5000 (and therefore above #1112's timer-window claims at 1100),
  so no schedule, shaper, divert, or manual override can outbid it; then connect and
  reconcile.
- Lease handling: apply granted amps to the claim; a lease of 0 A presents as a distinct
  **"waiting for load manager"** paused state on the web UI and LCD (LVGL charge/standby
  screens gain a status line) — visibly different from a fault or manual stop.
- TTL: leases carry a TTL in **relative milliseconds**, measured against the charger's
  own `millis()`. No wall-clock, no NTP dependency, no skew problem. Expiry or socket
  drop → revert claim to islanded current. Socket drop is the fast path; TTL is the
  backstop for half-open sockets.
- Reconnect: exponential backoff (1 s → 60 s cap) with jitter.
- Local shaper stand-down: while managed mode is enabled, the charger's own current
  shaper is suppressed (the lease *is* the shaped limit; two controllers on one CT
  double-subtract and oscillate). Divert/eco decisioning likewise moves to the box; the
  charger's eco/fast toggle becomes a `mode_request` (§5) rather than local divert control.
- Data intake: `data` messages from the box are fed through the exact same handler as
  `POST /status` push — zero new ingestion code paths.
- Mode changes made locally (LCD/web/HA) are sent as `mode_request` and only become truth
  when the box echoes `mode_set` (normally within one heartbeat, so UX-invisible).
- Standalone compatibility: with `managed_mode_enabled=false` nothing changes anywhere.

**Flash budget:** wifi_v1 (4 MB) sits at ~97.8%; the module is compile-gated
(`ENABLE_MANAGED_MODE`) with the opt-in/opt-out default per target decided with upstream
during the discussion-issue phase (precedent: `DISABLE_OCPP` on esp32-c3).

### Claim-TTL primitive (precursor)

A small standalone extension to the claims API: a claim may carry an expiry (relative ms,
auto-release if not renewed). Useful independently — today any external controller that
sets a claim and dies wedges it forever. Managed mode's lease enforcement is built on it.
This ships as its own upstream PR first (§9).

## 5. Lease protocol (WebSocket, JSON)

Charger connects to `ws://<box>/managed`. All messages are JSON objects with a `"msg"`
type field. Protocol is versioned via `hello`.

**Charger → box:**

| msg | Fields | Cadence |
|---|---|---|
| `hello` | `protocol` (int, 1), `id` (charger hostname), `firmware`, `max_current` (hardware ceiling), `islanded_current`, current `mode` | On connect |
| `state` | `/status`-shaped snapshot subset: EVSE state, pilot amps, measured amps, session energy, vehicle SoC/range if known, `mode`, active lease seq | Every heartbeat (default **5 s**) |
| `mode_request` | `mode` (`eco` \| `fast`) | On local user change |

**Box → charger:**

| msg | Fields | Cadence |
|---|---|---|
| `lease` | `amps` (0 = pause), `ttl_ms` (default **30 000**), `seq` (monotonic per box boot-epoch) | On every received `state` (renewal) + immediately on allocation change |
| `mode_set` | `mode` — box-arbitrated truth (adopts `mode_request` unless it conflicts with a box-side change in the same window; box wins ties) | On change / in reply to `mode_request` |
| `data` | `POST /status`-push-shaped house data: `solar`, `grid_ie`, `battery_level`, etc. | On feed update, throttled |

**Rules:**

- A charger ignores any `lease` whose `seq` is lower than the last applied one from the
  same connection (guards against reordering); a new connection resets the epoch.
- Renewal rides the heartbeat: `state` up → `lease` down. One round trip per cycle; a
  missed cycle is tolerated: at 30 s TTL / 5 s heartbeat, ~6 renewals fit in one
  TTL before a charger islands.
- Defaults (`heartbeat_s=5`, `ttl_ms=30000`) are box-side config, sent to chargers via
  the lease itself (`ttl_ms`) — chargers never assume a TTL.

## 6. Allocator (box side)

**Budget pipeline** — three layers, each degrading *downward only*:

1. **Static budget** (mandatory config): amps available to EV circuits.
2. **CT headroom** (optional): with a house-load feed (MQTT topic or HA entity — the same
   sources `current_shaper` reads today), budget = min(static, panel_limit − live non-EV
   draw), with the shaper's smoothing window, hysteresis, and safety margin relocated here.
3. **Solar surplus** (optional): with a solar/grid_ie feed, compute exportable surplus
   using `divert.cpp`'s established math (EMA smoothing, thresholds). Surplus feeds the
   eco pool; it does not raise the grid budget.

Feed staleness (divert-style timeout) drops that layer: solar stale → eco pool empties
politely; CT stale → budget falls back to static. Never adjusts *up* on missing data.

**Two pools by box-arbitrated mode:**

- **Fast pool** — shares the grid budget. Allocated first.
- **Eco pool** — shares `min(solar surplus, remaining budget)`, plus any per-charger
  configured grid allowance (mirroring divert's minimum-charge-current behavior).

**Within a pool — priority + rotation:**

- Chargers sorted by per-charger priority (box config). Walk the list granting each its
  request (clamped to its `max_current`) until the pool is exhausted.
- **6 A floor (J1772):** any grant that would fall below 6 A becomes 0; the remainder
  flows to the next charger in line. Pausing one car beats starving both.
- **Rotation:** when equal-priority chargers are starving (received 0 while active), the
  front of the line rotates on a configurable interval (default **30 min**) with a
  minimum-hold time so rotation never flaps mid-oscillation.

**Stability:** grants shrink immediately but grow through the shaper's hysteresis
(rate-limited), so a kettle or a passing cloud doesn't whipsaw allocations. All grants are
re-derived every heartbeat from current inputs — the allocator is a pure function
(inputs → grants) with a small rotation-state memory.

## 7. Load Manager box firmware

- **Env:** `openevse_lm` (P4 board variant first). Compiles out `evse_man`, RAPI, OCPP,
  charger-side divert/shaper consumers; compiles in `load_manager` (WS server + charger
  registry + allocator) and `lm_screen` (LVGL screens). Inherits config store, WiFi
  provisioning (incl. the QR AP-mode screen), web server, MQTT/HA clients, OTA.
- **Charger registry:** chargers appear on first `hello` and are persisted (name,
  islanded current as reported, priority, mode) so the box can reserve islanded amps for
  registered-but-disconnected chargers and render their rows. Registry edits (priority,
  remove) via box UI.
- **Screen (LVGL, nightshift + light themes reused):** home screen = household headroom
  bar (budget / EV draw / house draw / solar) + one row per charger (name, state color,
  mode, allocated vs drawn amps, lease health). Touch a row → detail + priority/mode
  controls. Disconnected charger rows amber: "islanded @ N A".
- **Web UI:** the box serves the gui-nightshift stack with a load-manager page set
  (charger list, budget config, feed config, priorities). v1 = functional config pages.
- **Hardware:** Guition JC4880P443C. Display layer stays behind the existing panel
  abstraction so an ESP32-S3 panel board is a port, not a rewrite.

## 8. Failure modes

| Failure | Behavior |
|---|---|
| Box dies / reboots | Sockets drop → chargers island immediately (TTL backstops half-open sockets). Box returns with no grant memory; chargers reconnect, `hello`, get fresh leases. Nothing persisted, nothing stale. |
| Charger reboots | Boots to islanded current → connects → reconciles. |
| WiFi partition (one charger) | That charger islands; box reserves its islanded amps and reallocates the rest. |
| Feed (CT/solar/HA) stale | That budget layer drops out downward (§6). |
| Box config error | Σ(islanded) ≤ static budget validated at config write; invalid configs refused. |
| Clock skew / NTP loss | Irrelevant: TTLs are relative durations on the charger's own millis. |
| Box reboot mid-flight leases | New connection epoch + `seq` reset; chargers ignore stale-seq leases; worst case one TTL of islanded behavior. |

## 9. Upstream sequencing

1. **Discussion issue** on OpenEVSE_ESP32_Firmware: the protocol section (§3–§5) as a
   sketch, framed as the leader/follower feature (Tesla/Wallbox/go-e precedent), stating
   the primary-agnostic design. Get maintainer buy-in on protocol shape + flash gating
   before significant code.
2. **PR A — claim-TTL primitive** (§4): tiny, standalone, independently useful.
3. **PR B — managed mode + WS client + lease protocol**: gated `ENABLE_MANAGED_MODE`,
   built against upstream master and structured to apply cleanly on top of PR #1112
   (charge manager) — see §3's composition rules. If #1112 merges first, PR B rebases
   onto it and extends its `tests/integration/` harness.
4. **Fork-side in parallel:** `openevse_lm` box env, allocator, screens — the allocator
   module written portable so a charger-hosted primary is possible later.

PRs A/B live on branches rebased on upstream master, kept submittable at all times;
development/bench happens on the fork.

## 10. Testing

- **Native units** (doctest, per `test_ha_oauth`/`test_fake_evse` pattern):
  - Allocator as a pure function: (budget, surplus, charger list with modes/priorities/
    requests/max) → grants. Table-driven cases for pools, 6 A floor, rotation +
    min-hold, hysteresis, disconnected-charger reservation, Σ-invariants.
  - Charger-side lease state machine as a pure unit: events (lease, tick, socket-drop,
    ttl-expiry, enable/disable) → claimed amps + UI state.
  - Protocol codec: message parse/serialize round-trips, stale-seq rejection.
- **Zero-hardware bench:** two FakeEVSE charger builds + one box build; soak the full
  protocol including kill-the-box, kill-the-WiFi, config-change-under-load drills.
- **HW validation:** two real chargers + the P4 box; overnight scarcity-rotation test
  with both cars; solar-day eco-pool test.

## 11. Open items (deliberately deferred)

- Gating default (opt-in vs opt-out per target) — decided with upstream in the
  discussion issue, driven by the 4 MB flash budget.
- Rotation fairness refinement (energy-delivered-weighted rather than time-sliced) and
  SoC-aware tiebreaking — v2, data is already in `state`.
- Box-side TLS/auth for the WS endpoint (LAN-trust in v1, matches existing charger APIs).
- S3 panel port of the box firmware.
- Charger-hosted primary build of the allocator module.
