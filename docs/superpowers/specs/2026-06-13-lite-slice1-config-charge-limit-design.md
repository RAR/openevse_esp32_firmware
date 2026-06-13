# Slice 1 — Config foundation + configurable charge limit + service-max (design)

**Status:** design for review
**Worktree:** `/home/rar/oevse/openevse-juicebox-lite` (branch `feature/juicebox-lite`)
**Context:** first slice of the JuiceBox-lite reuse roadmap. See the lift audit
`docs/superpowers/notes/2026-06-13-standard-fw-lift-audit.md`. This slice builds the **config
foundation** every later slice depends on, plus the **first method of the control-seam write
surface** (`setChargeCurrent`), and ships the user-facing **configurable charge limit + service
max**.

## Goal
Let the JuiceBox-lite charge-current limit be set at runtime over HTTP, clamped to a configurable
service-max ceiling, persisted to LittleFS, and restored on boot — replacing the hard-coded
`_chargeLimit = 6` the keepalive advertises today.

## Scope

**In:**
1. Re-enable real LittleFS in the lite build (drop the no-op shim; mount at boot) and prove it
   persists on the JuiceBox flash (HW spike).
2. Generalize `lite_config_store` from WiFi-only to a typed EVSE-config JSON file, **mirroring
   upstream `app_config` key names** so later module lifts find what they expect.
3. Add the first control-seam write method: `setChargeCurrent(int)` / `getChargeCurrent()` on
   `LiteEvseBackend`, implemented by `JuiceBoxBackend` (drives the existing keepalive `_chargeLimit`).
4. HTTP `GET`/`POST /config` for the charge limit + service max, with clamp + persist + apply.
5. Boot flow: load config → clamp → apply to backend.

**Out (explicitly deferred):**
- The **full claim/priority arbitration model** (multiple control clients) → **Slice 1.5**. This
  slice adds only the single-setpoint `setChargeCurrent` seam, no claim table.
- MQTT, NTP/scheduler, divert/shaper, RGB, web UI assets → later slices.
- Pre-seeding non–slice-1 config keys: unnecessary. The store is a name-keyed JSON file, so later
  slices add their own keys with defaults — no migration. We only define the keys this slice uses.

## Architecture (Approach B — separate config layer, thin backend setter, web orchestrates)

```
HTTP POST /config ──▶ web_server_lite ──┐
                                        │ clamp(soft,[6,hard]); clamp(hard,[6,JB_ABS_MAX])
                                        ▼
                         lite_config_store  ──(persist /config.json)──▶ LittleFS
                                        │
                                        ▼
                         LiteEvseBackend.setChargeCurrent(soft)  ──▶ keepalive advertises $SL00N:soft
boot: lite_config_begin → load_evse → clamp → setChargeCurrent
```

Three responsibilities, cleanly separated:
- **`lite_config_store`** — persistence only (LittleFS JSON ⇄ struct). No clamp policy, no device I/O.
- **`LiteEvseBackend` / `JuiceBoxBackend`** — device control only. `setChargeCurrent(amps)` sets the
  RAM `_chargeLimit` the keepalive already advertises. No persistence, no HTTP.
- **`web_server_lite`** — orchestration + validation. Owns the clamp policy; reads/writes the store;
  applies to the backend. The boot path does the same load→clamp→apply.

This is the only structure that stays clean when Slice 3 (MQTT) and Slice 4 (scheduler) *also* need
to read config and set the current — they go through the same store + `setChargeCurrent` seam.

## Components & changes

### 1. LittleFS re-enable (HW spike first)
- **Files:** `platformio.ini` `[env:openevse_lite]` (`build_src_filter`, `-I src/lite`),
  `src/lite/LittleFS.h` (no-op shim — remove), `src/lite/main_lite.cpp`.
- Drop the no-op `src/lite/LittleFS.h` shim and the `-I src/lite` that shadows the real LibreTiny
  `LittleFS.h`; re-add `+<lite/lite_config_store.cpp>` to `build_src_filter` (it is currently
  excluded). Call `lite_config_begin()` once in `setup()` before WiFi.
- **Spike (gates the rest):** write a known value, reboot, read it back over `/status` or a temp log.
  Confirm LittleFS mounts and persists on the JuiceBox's EFM32 flash region. **If the flash region
  isn't present/usable, stop and resolve the layout before building on it** — this is the one real
  risk in the slice (T4 built the store but slice-2 stubbed it; persistence is unproven on HW).

### 2. Config store: WiFi-only → typed EVSE config
- **Files:** `src/lite/lite_config_store.{h,cpp}` (extend), test `test/test_lite_config/`.
- New struct + accessors (WiFi creds API unchanged):
```cpp
struct LiteEvseConfig {
  int max_current_soft; // active charge-current setpoint (A) the keepalive advertises
  int max_current_hard; // service-max ceiling (A) — install rating; soft is clamped to this
};
bool lite_config_load_evse(LiteEvseConfig &out); // false if /config.json absent → caller uses defaults
bool lite_config_save_evse(const LiteEvseConfig &in);
```
- Persisted to `/config.json` (separate from `/lite_wifi.json`), keys `max_current_soft` /
  `max_current_hard` (upstream `app_config` names). Uses the same ArduinoJson + LittleFS pattern as
  the existing WiFi store. Missing/blank/corrupt file → returns false (caller applies defaults).

### 3. Control-seam write method
- **Files:** `src/lite/lite_evse_backend.h`, `src/lite/juicebox_backend.{h,cpp}`.
- Add to the interface:
```cpp
virtual void setChargeCurrent(int amps) = 0; // desired charge current (A); backend may clamp to its own floor
virtual int  getChargeCurrent() const = 0;   // current advertised setpoint
```
- `JuiceBoxBackend::setChargeCurrent(amps)` sets `_chargeLimit = amps` (the keepalive already sends
  `juicebox_build_amps_set(_chargeLimit, …)`, which the Atmel further clamps to its 6 A floor and
  <81 ceiling). `getChargeCurrent()` returns `_chargeLimit`. **Keep the `_chargeLimit = 6` member
  initializer** as a safety default (so the advertised current is 6 A even if the boot apply is ever
  skipped); the boot path overwrites it with the loaded/clamped value.
- Note `getChargeCurrent()` (our setpoint) is distinct from the existing `getAmps()` (the Atmel's
  `$ES` `A` field = reported max/rating) — they are different values, both retained.

### 4. Clamp policy (pure, host-tested)
- **Files:** `src/lite/lite_charge_policy.{h,cpp}` (new, pure), test `test/test_lite_charge_policy/`.
```cpp
static constexpr int JB_MIN_CURRENT = 6;   // J1772 floor
static constexpr int JB_ABS_MAX     = 48;  // absolute safety cap regardless of config
int lite_clamp_service_max(int hard);                 // clamp to [JB_MIN_CURRENT, JB_ABS_MAX]
int lite_clamp_charge_current(int soft, int hard);    // clamp to [JB_MIN_CURRENT, clamp_service_max(hard)]
```
- Pure integer functions, no Arduino deps → unit-tested in the native doctest env like
  `juicebox_proto`.

### 5. Web endpoint
- **Files:** `src/lite/web_server_lite.cpp`.
- `GET /config` → `{"max_current_soft":N,"max_current_hard":M}`.
- `POST /config?max_current_soft=N&max_current_hard=M` (either or both via query params, so it is a
  one-line `curl -X POST`). Handler: parse present params (`mg_get_http_var` on the query string),
  apply `lite_clamp_service_max` then `lite_clamp_charge_current`, persist via
  `lite_config_save_evse`, apply via `backend.setChargeCurrent`, and respond with the **clamped**
  result echoed as JSON (so the caller sees what actually took effect).
- Add `max_current_soft` / `max_current_hard` to `/status` for visibility.

### 6. Boot flow
- **Files:** `src/lite/main_lite.cpp`.
- During `setup()`, after `s_backend.begin()`: `lite_config_load_evse(cfg)`; if absent,
  `cfg = { max_current_soft: 32, max_current_hard: 32 }` (default — see Defaults); clamp;
  `s_backend.setChargeCurrent(cfg.max_current_soft)`.

## Defaults & safety
- **Default `max_current_hard` = 32 A** and **`max_current_soft` = 32 A**. 32 A is the smallest
  JuiceBox model ever sold, so it's the safe out-of-box assumption for the hardware's rated current:
  a factory/unconfigured unit charges at its rated 32 A; the installer lowers `max_current_hard` if
  the circuit is smaller, or lowers `max_current_soft` to cap a session. (The earlier keepalive
  concern, `dffbe94`, was advertising the Atmel's *reported max* of ~80 A — 32 A on a 32 A-rated unit
  is the correct rating, not an over-command.)
- `JB_ABS_MAX = 48 A` is a hard ceiling enforced even if config somehow holds a larger value.
- The Atmel independently clamps `<6 → 6` and rejects `≥81`; our clamp is the host-side policy layer.
- LittleFS mount failure → in-RAM defaults (6/6) + the unit still runs read + safe keepalive; the
  failure is logged (debug build) and `/config` POST returns an error status (503) since it can't
  persist.

## Data flow (set request)
`POST /config?max_current_soft=24` → parse 24 → `clamp_service_max(hard=loaded)` →
`clamp_charge_current(24, hard)` → e.g. hard=16 ⇒ soft=16 → save `{soft:16,hard:16}` →
`backend.setChargeCurrent(16)` → next keepalive sends `$SL002:16` → response
`{"max_current_soft":16,"max_current_hard":16}`.

## Error handling
- Missing/unparseable query param → ignore that field (partial update allowed); if neither present →
  400.
- Out-of-range values → clamped, not rejected (response shows the clamped value).
- `/config.json` absent on load → defaults, not an error.
- LittleFS save failure → 503 + keep the in-RAM applied value (best effort).

## Testing
- **Native doctest** (`test/test_lite_charge_policy/`, `test/test_lite_config/`): clamp edge cases
  (below 6, above hard, above JB_ABS_MAX, hard<6, partial updates); config struct ⇄ JSON round-trip;
  absent-file → false.
- **On-device HW:**
  1. LittleFS persistence spike (write/reboot/read-back) — the gating step.
  2. `curl -X POST '…/config?max_current_hard=24&max_current_soft=20'` → `/status` shows 20/24 →
     VCOM keepalive shows `$SL002:20`.
  3. Reboot → `/status` still 20/24, keepalive still `$SL002:20` (persistence).
  4. `curl -X POST '…/config?max_current_soft=40'` with hard=24 → response + status clamp to 24.
- **Flash/RAM measurement** after the slice (roadmap requires it; app currently 472/992 KiB bank).

## Risks
- **LittleFS region on EFM32 (primary):** retired by the spike. If absent, blocks the slice.
- Mongoose 5.x `POST` query-param parsing — `mg_get_http_var(&hm->query_string, …)` works on the
  query string for any method (already used in upstream `web_server_rfid.cpp`); confirmed pattern.
- Default-6 service max may surprise (can't raise charge current until hard is set) — intentional;
  flagged for review.

## Follow-on
**Slice 1.5** grows `setChargeCurrent` into the full claim/setpoint surface
(`enable/disable/claim/release`, min/max/hardware-current, session counters, events) that unlocks
lifting limit/temp_throttle/divert/shaper/scheduler/mqtt per the audit.
