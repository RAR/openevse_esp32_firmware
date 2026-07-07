# Multi-Charger Household Load Sharing — Design

**Date:** 2026-07-07 (reworked same day after discovering upstream's in-progress work)
**Status:** Approved design, pre-implementation
**Scope:** Load sharing for households with 2+ OpenEVSE chargers. **Strategy: adopt and
extend upstream's in-progress load-sharing implementation** (issue #940, branch
`jeremypoulter/issue940`) rather than propose a competing protocol. Our contributions:
a dedicated in-home "Load Manager" box (ESP32-P4 + touchscreen) running the controller
role without an attached EVSE, priority + rotation allocation, solar/eco awareness, and
claim-priority hardening.

---

## 1. Problem and context

Households with two or more EVSEs share one panel/feeder limit. Today each OpenEVSE can
shape its own current, but two independent controllers reacting to the same CT race and
oscillate, and nothing enforces a *combined* ceiling. Commercial systems solve this with
leader/follower groups (Tesla Wall Connector, Wallbox Power Sharing, go-e).

**Upstream is already building this.** Issue #940 "Local power sharing group" (an
expansion of #592, motivated by the Enel X JuiceBox shutdown) has an active
implementation branch by Jeremy Poulter: `jeremypoulter/issue940` (~4,100 lines:
`loadsharing_types/algorithm/discovery_task/peer_poller`, web API, GUI branch, plus the
integration-test and divert_sim groundwork split out as PRs #1124/#1125). We
independently converged with it on the fundamentals — WebSocket status transport,
member-side failsafe current on lost comms, offline-member budget reservation,
claims-based enforcement — which validates both designs. This spec adopts #940's
protocol and modules as the foundation and defines our extensions on top.

### What #940 provides (adopted as-is)

- **Roles:** `loadsharing_role` = controller | member, configured per charger. Members
  know the controller via `loadsharing_controller_host`; the controller finds members
  via mDNS discovery (`loadsharing_discovery_task`) and/or a configured peer list.
- **Transport:** the controller dials out to each member's *existing* `/ws` GUI
  WebSocket for real-time status ingestion (HTTP bootstrap first), pushes group config
  to members via `POST /config` (kept in sync via `loadsharing_config_version` +
  config hash), and delivers per-member `target_current` allocations. Poller task wakes
  every 500 ms; WS staleness and heartbeat timeouts mark peers offline.
- **Member failsafe (= our "islanded current"):** a member that loses contact with its
  controller past `loadsharing_heartbeat_timeout` (default 30 s) falls back to
  `loadsharing_failsafe_safe_current` (default 6 A); `loadsharing_failsafe_mode` can be
  `safe_current` or `disable` (0 A). Timeouts are relative to local millis — no
  wall-clock/NTP dependency.
- **Offline reserve:** the controller subtracts `failsafe_peer_assumed_current` per
  offline member from the group budget before allocating — the conservative accounting
  our design also required (Σ of what unreachable chargers may legitimately draw).
- **Allocation:** `computeAllocations()` is a pure function — "equal share with
  minimums": budget × safety factor − offline reserve, minimums first, remainder split
  equally with iterative max-capping; when minimums don't fit, a deterministic subset
  (sorted by member id) charges and the rest get 0.
- **Enforcement:** allocations applied via EvseManager claims
  (`EvseClient_OpenEVSE_LoadSharing`).

## 2. Our extensions (the actual work)

| # | Extension | Why |
|---|---|---|
| E1 | **Load Manager box** — `openevse_lm` PlatformIO env: the #940 controller role on a Guition ESP32-P4 + 4.3" touchscreen, with no attached EVSE | A charger-free primary that lives in the house; screen shows household state; symmetric charger fleet (all members) |
| E2 | **Priority + rotation allocator** | #940's scarcity behavior starves deterministically (same id wins every time). Use the existing-but-unused `loadsharing_priority` config key, and rotate equal-priority starving members on an interval so both cars charge overnight |
| E3 | **Solar/eco pools** | Fast pool shares the grid budget; eco pool shares exportable solar surplus. House-level divert replaces N independent divert controllers |
| E4 | **Member failsafe claim** (bench-corrected 2026-07-07) | Connected members already claim at Limit 1100, which outbids a Manual override (1000) — but the failsafe path places NO claim (`checkMemberFailsafe()` only flips a flag), so an islanded member never throttles and an override runs uncapped. Fix = apply the failsafe claim member-side; priorities stay as-is. See `docs/superpowers/2026-07-940-bench-findings.md` |
| E5 | **Mode arbitration** (eco/fast per charger, settable on charger or box, box wins) | Needed by E3; uses members' existing mode/config APIs |

E1 is fork-side (upstream has no P4 target). E2 and E4 are upstream contributions onto
the #940 branch. E3 + E5 prove out fork-side first, then offered upstream.

## 3. Decisions (with alternatives considered)

| Decision | Chosen | Rejected alternatives |
|---|---|---|
| Relationship to #940 | Adopt protocol + modules, extend | Competing protocol (our original draft); fork-only divergence |
| Primary form | #940 controller role, hostable on a charger *or* the box (protocol already primary-agnostic in this sense) | Box-only; new dedicated protocol |
| Transport | #940's: controller dials member `/ws`, mDNS discovery, config-push sync | Member-dials-in WS (our original draft); MQTT; HTTP-poll-only |
| Data ingestion | Box is the *preferred* source of house-level data (solar/grid/battery), distributing to members via the existing `POST /status` push shapes; chargers keep standalone capability | Stripping MQTT/HA ingestion from chargers |
| Budget intelligence | Static group budget + CT headroom + solar surplus, each layer degrading downward | Static-only; static+CT-only |
| Screen | LVGL touchscreen on the box from day one | Headless v1 |
| Scarcity policy | Priority (existing `loadsharing_priority` key) + rotation with min-hold | #940's deterministic id-sorted subset; SoC-aware |
| Box hardware | Guition JC4880P443C (ESP32-P4 + 4.3" ST7701 touch, already ported) | New S3 panel bring-up first |
| Box firmware home | This repo, new env (FakeEVSE-style personality) | Separate repo |
| Upstream posture | Engage on #940: bench-test Jeremy's branch, review feedback, contribution PRs | Discussion issue proposing a new protocol (obsoleted by #940) |
| Fleet size | N members (#940 already models a peer list) | Hard-coded pair |

## 4. The core invariant (unchanged, now in #940 vocabulary)

> A charger in a load-sharing group without live contact with its controller charges at
> its **failsafe current** (`loadsharing_failsafe_safe_current`, or 0 in `disable`
> mode), period.

This collapses every failure mode — controller death, controller reboot with amnesia,
member reboot, WiFi partition, stale allocations — into one safe state. #940 already
implements the member half (`checkMemberFailsafe()`) and the controller half (offline
reserve). Our additions preserve it:

- **Config safety rule** (to contribute if #940 lacks it): Σ(failsafe currents of all
  group members) ≤ group max current — the group must be safe even with every member
  islanded. Validated when group config is written/pushed.
- **Failsafe claim (E4, bench-corrected):** the failsafe path must place the same
  Limit-priority claim the allocation path already uses (web_server.cpp:1234-1250) —
  max_current = failsafe current, or state = disabled in `disable` mode. Without it the
  invariant only holds while connected: a member that loses its controller reports
  failsafe but charges uncapped, and a manual override runs uncontested (bench Drills
  A/B). The original "must outrank Safety 5000" framing was wrong — Limit 1100 already
  outbids Manual 1000; no priority change is needed.

The failsafe default (6 A vs 0) is an availability-vs-fail-dark trade per charger;
`disable` mode is the conservative choice, 6 A keeps a car trickling through a WiFi
outage. The Σ rule bounds the worst case either way.

## 5. Load Manager box (E1)

- **Env:** `openevse_lm` (P4 board variant first). Compiles in the #940 load-sharing
  modules with `loadsharing_role=controller`, the allocator extensions (§6), and
  `lm_screen` LVGL screens; compiles out `evse_man` consumers tied to a physical EVSE
  (RAPI, EVSE monitor, OCPP). Inherits config store, WiFi provisioning (incl. the QR
  AP-mode screen), web server, MQTT/HA clients, OTA.
- **Refactor needed (upstream-friendly):** #940's controller assumes it is *also* a
  charging station (it claims its own EvseManager alongside sending peer allocations).
  Make the "self" member optional so the controller role runs EVSE-less — a small,
  clearly motivated change to propose on the branch ("controller without a local
  EVSE"), which also benefits anyone wanting a dedicated coordinator.
- **Data hub:** the box subscribes once to house-level feeds (MQTT topics or HA
  entities — the same sources shaper/divert read today) and redistributes to members
  via the existing `POST /status` push shapes. Members need zero new ingestion code and
  keep full standalone capability when unmanaged. Under management, members' local
  shaper/divert stand down (the allocation is the shaped limit; house-level eco replaces
  local divert) — enforced naturally by the allocation claim at Limit 1100 (plus the E4 failsafe claim when islanded), which outranks
  them.
- **Screen (LVGL, nightshift + light themes reused):** home screen = household headroom
  bar (budget / EV draw / house draw / solar) + one row per member (name, state color,
  mode, allocated vs drawn amps, connection health). Touch a row → detail +
  priority/mode controls. Offline member rows amber: "failsafe @ N A".
- **Web UI:** gui-nightshift stack with the load-share pages from Jeremy's GUI branch,
  plus box-specific pages (feeds config, priorities). v1 = functional config pages.
- **Hardware:** Guition JC4880P443C; display layer stays behind the existing panel
  abstraction so a cheap ESP32-S3 panel is a later port, not a rewrite.

## 6. Allocator extensions (E2 + E3)

`computeAllocations()` stays a pure function (inputs → allocations); we extend its
inputs and policy, keeping #940's shape so it remains native-testable and upstreamable:

**Budget pipeline** — three layers, each degrading *downward only*:

1. **Static group budget** (`loadsharing_group_max_current` × safety factor — #940 as-is).
2. **CT headroom** (optional): with a house-load feed, budget = min(static,
   panel_limit − live non-EV draw), with the shaper's smoothing and hysteresis relocated
   here.
3. **Solar surplus** (optional): computed divert-style (EMA smoothing, thresholds);
   feeds the eco pool, never raises the grid budget.

Feed staleness drops that layer (solar stale → eco pool empties politely; CT stale →
static budget). Never adjusts *up* on missing data.

**Two pools by arbitrated mode (E3/E5):** fast-mode members share the grid budget
(allocated first); eco-mode members share min(solar surplus, remaining budget) plus any
per-member configured grid allowance (mirroring divert's minimum-charge behavior). Mode
is settable on the member (its UI/HA, forwarded as a request) or on the box; the
controller arbitrates, last-writer-wins, controller wins ties, and confirms via the
existing member mode APIs.

**Scarcity — priority + rotation (E2):** within a pool, sort by `loadsharing_priority`
(key already exists in #940's config, currently unused by the algorithm), then apply
#940's minimums logic; when equal-priority members are starving (allocated 0 while
demanding), rotate the subset front on a configurable interval (default 30 min) with a
minimum-hold time so rotation never flaps. Replaces the deterministic id-sort that
starves the same car every time. The 6 A J1772 floor is #940's `min_current` — pausing
one car beats starving both.

**Stability:** allocations shrink immediately but grow through hysteresis, so a kettle
or passing cloud doesn't whipsaw the group.

## 7. Failure modes

| Failure | Behavior (mechanism) |
|---|---|
| Controller (box) dies / reboots | Members hit heartbeat timeout → failsafe current (#940 `checkMemberFailsafe`). Controller returns with no allocation memory; re-bootstraps peers, reallocates. Nothing persisted. |
| Member reboots | Boots unallocated → failsafe current → controller re-discovers/reconnects → allocation resumes. |
| WiFi partition (one member) | Member → failsafe; controller reserves `failsafe_peer_assumed_current` for it (#940 offline reserve) and reallocates the rest. |
| Feed (CT/solar/HA) stale | That budget layer drops out downward (§6). |
| Group config error | Σ(failsafe) ≤ group budget validated at config write/push (§4). |
| Clock skew / NTP loss | Irrelevant: timeouts are relative millis (#940 as-is). |
| Manual override on a member | Connected: outbid by the allocation claim (Limit 1100 > Manual 1000), verified on bench. Islanded: currently WINS because the failsafe path places no claim — fixed by E4. |

## 8. Interplay with the Charge Manager (PR #1112)

#1112 (open) gives scheduler windows per-event features (Divert, Shaper, RFID,
Current = amps) and session limits, with timer-window claims at Limit 1100 — the *same*
priority #940 currently uses for load sharing, which makes their interaction
order-dependent. Whether load-sharing claims should be raised above the window claims is
a layering question to settle on the two reviews together; the required outcome is that
schedules keep deciding **when** and **intent** while the allocation
bounds **how much** (a scheduled 32 A window under a 16 A allocation charges at 16 A).
Under management, a Shaper window is inert (redundant) and a Divert window maps to an
eco mode request (house-level eco replaces local divert). Session limits and
RFID-required windows stay fully local and orthogonal. Both PRs will likely merge; the
priority-layering conversation belongs on the #940 review with #1112 in view.

## 9. Upstream engagement plan

1. **Bench #940 as-is** — two FakeEVSE charger builds running Jeremy's branch + our
   pytest harness (his #1125 integration-test scaffolding): protocol soak,
   kill-the-controller, kill-the-WiFi, config-push drills. Report results on the
   issue/PR. This is review feedback backed by a working bench, the most credible way
   to show up.
2. **Review feedback on #940:** member-failsafe-claim gap (E4, §7 last row), Σ(failsafe) ≤
   budget config validation, scarcity starvation (motivates E2).
3. **Contribution PR: E2 (priority + rotation)** onto the #940 branch — pure-function
   change + native tests, uses the existing `loadsharing_priority` key.
4. **Contribution PR/patch: controller-without-local-EVSE** (§5 refactor) — enables any
   dedicated-coordinator use case, not just our box.
5. **Fork-side in parallel:** `openevse_lm` box env + LVGL screens + data-hub feeds
   (E1), then E3/E5 (pools + mode arbitration) proven on the fork and offered upstream
   once #940 lands.

## 10. Testing

- **Native units** (doctest, per existing `test_*` pattern): extended
  `computeAllocations()` table-driven — pools, priorities, rotation + min-hold,
  minimums/capping (regression vs #940's cases), offline reserve, Σ-invariant,
  hysteresis. Mode-arbitration state machine as a pure unit.
- **Zero-hardware bench:** FakeEVSE members + box controller build; full failure-mode
  drill list from §7; also used to validate stock #940 (step 1 above).
- **HW validation:** two real chargers + the P4 box; overnight scarcity-rotation test
  with both cars; solar-day eco-pool test.

## 11. Open items (deliberately deferred)

- Whether load-sharing claims should be raised above #1112's 1100-class window claims once both merge — settled on the two reviews (E4 itself needs no priority change)
  (must consider #1112's 1100-class windows).
- How #940 delivers allocations to members (WS message vs HTTP) is treated as its
  internal detail; we adopt whatever the branch settles on.
- Rotation fairness refinement (energy-weighted) and SoC-aware tiebreaking — v2; the
  data is already in member status.
- Box-side auth/TLS toward members (LAN-trust in v1, matches existing charger APIs).
- Porting the box to cheap ESP32-S3 panel hardware — v1 ships on the ESP32-P4 (§5);
  the box firmware stays display-abstracted so an S3 port is possible later.
- Whether upstream wants E3 (solar pools) in-tree or as a follow-on — decided after E2
  lands and #940 merges.
