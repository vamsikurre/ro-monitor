# Utility Node Pump Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The utility node `0x06` reads the sump float and the six clamps, drives three relays to control the borewell and protect the sump motor, and the hub forwards app/dashboard Start/Stop and settings to it.

**Architecture:** All control decisions live in one pure C module on the node (`ctl.c`, no ESP-IDF includes), compiled on the host by `docs/check_ctl.py` exactly as `check_gf.py` does for the hub's pure code. Thin IDF glue around it: relay driver, float input, NVS, an HTTP command endpoint. The hub only parses the node's new telemetry fields, forwards commands and settings over HTTP POST, raises alerts from the node's flags, and draws buttons.

**Tech Stack:** ESP-IDF 5.4.4, C, cJSON, ESP RainMaker (hub), plain JS dashboard, Python 3 + gcc checkers.

**Spec:** `docs/superpowers/specs/2026-11-07-utility-node-pump-control-design.md` (commit `d0ac44d`). Read it first; this plan argues from it.

## Global Constraints

- Relays are **de-energised in the normal state**; a dead node = motors as if the node did not exist (spec §4, §5.3).
- **No spurious START** at power-up, reboot, OTA or reconnect. START is blocked for **30 s after boot** and until ≥ 2 consistent float reads and ≥ 1 clamp read (spec §6.5).
- STOP is **pulsed** (3 s) for float-full; **held** for a person's Stop, single-phasing and dry-run (spec §6.1).
- Persist in NVS: a person's Stop latch (per motor) and the single-phasing latch. Do **not** persist: failed-restart give-up, dry retry count, sump hold (spec §6.5).
- Pressing **Start** clears single-phasing and dry latches if the condition is gone; there is no Reset command (spec §6.4).
- "Borewell controller = NEC-49 / node" and "Protection = Alert only / Act" per motor are separate settings; default **NEC-49** and **Alert only** (spec §6.2).
- No ultrasonics, no RWT float reading: `rwt_floty` is always `null` (spec §3).
- Unit conversion formula is the hub's `gfPhaseDeciAmps()` (`app_cal.c`): `deci = mv * apv_x100 / (10000 * turns)`, `-1` under `CT_NOISE_FLOOR_MV` (15).
- Alert texts ≤ 100 bytes after expansion (`check_alerts.py`).
- Builds: ESP-IDF via `. C:\Espressif\tools\Microsoft.v5.4.4.PowerShell_profile.ps1`; gf_node always with `-B build_util -D SDKCONFIG=build_util/sdkconfig` (`firmware/gf_node/README.md`). **Commit before every build.** Build only in steps that say so.
- Hub poll period is `GF_POLL_MS` = 5000 ms; the node samples every `SAMPLE_PERIOD_MS` = 2000 ms.

## Review Focus

1. **Node reboots mid-pulse or while a stop is held.** Expected: no START on the way up; a held stop that was a person's Stop or single-phasing is re-held from NVS within one cycle after boot; a float-full pulse is simply not resumed. → Task 4 test `boot_restores_held_stops_never_starts`.
2. **Float chatter at the switching point (splash, waves from the borewell inflow).** Expected: no START/STOP flapping; debounce is time-based (5 s), not cycle-counted. → Task 1 test `float_chatter_under_debounce_does_nothing`.
3. **Clamp unplugged or pedestal fault while running** (node reports exactly 0 mV). Expected: never read as "phase lost" or "dry"; but a plugged clamp reading a few mV on a dead conductor IS a lost phase (Task 2 ruling). → Task 2 test `missing_clamp_is_not_single_phasing`.
4. **Hub sends a command while Wi-Fi flaps / duplicate POSTs.** Expected: a repeated Start or Stop is idempotent; wrong or missing token → 403 and no state change. → Task 6 test `bad_token_refused_and_state_unchanged`.
5. **Settings never pushed (fresh node, hub down).** Expected: node runs with safe defaults from NVS or compiled-in (controller = NEC-49, Alert only, thresholds 0 = that check disabled), never acts on a zero threshold. → Task 1 test `zero_thresholds_disable_checks`.

---

## File Structure

| File | Responsibility |
|---|---|
| `firmware/gf_node/main/ctl.h` / `ctl.c` (create) | Pure control logic: types, `ctl_step`, `ctl_command`, persistence struct. No IDF. |
| `firmware/gf_node/main/relay.c` (create) | Boot-safe relay GPIOs, START pulse timing. |
| `firmware/gf_node/main/ctl_glue.c` (create) | Mutex around ctl state, NVS load/save, cfg/cmd JSON parsing, called from sample task and web. |
| `firmware/gf_node/main/sensors_util.c` (modify) | Float input on GPIO 26, expose readings to glue, JSON fields. |
| `firmware/gf_node/main/web.c` (modify) | `POST /api/ctl` (command) and `POST /api/ctl/cfg`. |
| `firmware/gf_node/main/gf.h`, `Kconfig.projbuild`, `CMakeLists.txt`, `main.c` (modify) | Pins, token, wiring it together. |
| `docs/check_ctl.py` (create) | Host tests for `ctl.c`. |
| `firmware/hub_prod/main/app_gf.c` / `app_priv.h` (modify) | Parse new fields; POST client; command queue; cfg push. |
| `firmware/hub_prod/main/app_main.c` (modify) | Alerts from node flags; remove hub bore-dry detectors; RainMaker params. |
| `firmware/hub_prod/main/app_web.c` (modify) | `POST /api/motor`; telemetry `ctl` objects; `/cal` fields. |
| `firmware/hub/data/dashboard.html` (modify) | Buttons, mode, float, hold reasons. |
| `docs/WIRING.md` §11.2, `docs/HARDWARE.md` §3.5 (modify) | As-designed wiring of Task 11. |

---

### Task 1: `ctl` core — types, float debounce, borewell auto start/stop, start verify and retry, boot settle

**Files:**
- Create: `firmware/gf_node/main/ctl.h`, `firmware/gf_node/main/ctl.c`
- Test: `docs/check_ctl.py` (gcc-compiles `ctl.c` with a C harness, pattern of `docs/check_gf.py`)

**Interfaces — Produces (used by every later task):**
```c
typedef enum { CTL_BORE = 0, CTL_SUMP = 1 } ctl_motor_t;
typedef enum { CTL_CMD_START, CTL_CMD_STOP } ctl_cmd_t;
typedef enum { CTL_SRC_AUTO, CTL_SRC_APP, CTL_SRC_DASH } ctl_src_t;

/* flag bits - the hub mirrors these values (Task 7 pins them) */
#define CTL_F_START_FAILED   0x0001  /* commanded on, no current after retry */
#define CTL_F_SINGLE_PHASE   0x0002  /* latched */
#define CTL_F_BORE_DRY       0x0004  /* dry stop active (resting or latched) */
#define CTL_F_BORE_DRY_LATCH 0x0008  /* 3 attempts used, waits for a person */
#define CTL_F_MAX_RUN        0x0010  /* alert only, never stops */
#define CTL_F_SUMP_HOLD      0x0020  /* borewell failed + float low */
#define CTL_F_SUMP_DRY       0x0040  /* no-load current */
#define CTL_F_USER_STOP_BORE 0x0080
#define CTL_F_USER_STOP_SUMP 0x0100
#define CTL_F_NOT_SETTLED    0x0200
#define CTL_F_CAP_SUSPECT    0x0400  /* optional, Task 3 */

typedef struct {
    uint16_t bore_apv_x100, sump_apv_x100;  uint8_t bore_turns, sump_turns;
    uint16_t bore_run_da;      /* "drawing current" threshold, as hub cal run_deci_amps */
    uint16_t bore_dry_da;      /* 0 = dry check off */
    uint16_t sump_noload_da;   /* 0 = sump-dry check off */
    uint16_t max_run_min;      /* 0 = off */
    bool     ctrl_node;        /* false = NEC-49 in charge (default) */
    bool     bore_act, sump_act;   /* false = Alert only (default) */
} ctl_cfg_t;

typedef struct {
    int64_t  now_ms;
    int8_t   float_low;        /* 1 low, 0 high, -1 not readable (ctrl_node false) */
    uint16_t bore_mv[3], sump_mv[3];
    bool     pump_on;          /* Astero PUMP ON closed */
} ctl_in_t;

typedef struct { bool bore_stop, bore_start, sump_stop; } ctl_out_t;  /* true = energise */
typedef struct { bool user_stop_bore, user_stop_sump, single_phase; } ctl_persist_t;

typedef struct ctl_state ctl_state_t;  /* defined in ctl.h, opaque to callers by convention */

void     ctl_init(ctl_state_t *s, const ctl_persist_t *p, int64_t boot_ms);
void     ctl_step(ctl_state_t *s, const ctl_cfg_t *c, const ctl_in_t *in, ctl_out_t *out);
uint16_t ctl_flags(const ctl_state_t *s);
bool     ctl_bore_running(const ctl_state_t *s);   /* any phase >= bore_run_da */
int8_t   ctl_float_low(const ctl_state_t *s);      /* debounced: 1 low, 0 high, -1 not read */
int16_t  ctl_deci_amps(uint16_t mv, uint16_t apv_x100, uint8_t turns);  /* == hub gfPhaseDeciAmps */
```
`out->bore_start` true means "begin a START pulse now" (an edge); the relay driver (Task 5) times it. `out->bore_stop` / `sump_stop` are levels.

Values pinned by this task: float debounce **5000 ms**; START verify window **10000 ms**; one retry; STOP pulse **3000 ms**; settle **30000 ms** + 2 float reads + 1 step with any clamp reading.

- [ ] **Step 1: Write the failing tests** in `docs/check_ctl.py` harness (each a C function `t_<name>()` using `CHECK`):
  - `conversion_matches_hub`: for (mv, apv, turns) in {(100,3000,1)→30, (100,3000,2)→15, (10,3000,1)→-1, (15,3000,1)→4}, `ctl_deci_amps` equals those values (same table as `check_gf.py`).
  - `nec_mode_never_drives_float_logic`: `ctrl_node=false`, float low for 60 s → `bore_start` never true, `bore_stop` false.
  - `float_low_starts_after_settle`: `ctrl_node=true`, boot at 0, float low from 0 → no `bore_start` before 30000 ms; exactly one `bore_start` edge at the first step ≥ 30000 ms.
  - `float_chatter_under_debounce_does_nothing`: settled, float alternates every 2 s for 60 s → zero `bore_start` edges, `bore_stop` never true.
  - `float_high_pulses_stop`: running (bore_mv giving ≥ run threshold), float high for 5 s → `bore_stop` true for 3000 ms then false.
  - `no_current_retries_once_then_flags`: float low, no current: one START edge, second edge 10 s later, then `CTL_F_START_FAILED` set and no third edge for 10 min; float going high then low again clears the flag and allows a new attempt.
  - `zero_thresholds_disable_checks`: `bore_dry_da=0, sump_noload_da=0, max_run_min=0`, running at 0.1 A for 1 h → none of `BORE_DRY`, `SUMP_DRY`, `MAX_RUN` set.
- [ ] **Step 2: Run** `python docs/check_ctl.py` → FAIL (`ctl.c` missing).
- [ ] **Step 3: Implement** `ctl.h` / `ctl.c` with the interfaces above. Time-based debounce (store the time the raw float value last changed). `ctl_deci_amps` uses the formula in Global Constraints with `CT_NOISE_FLOOR_MV 15` defined in `ctl.h`.
- [ ] **Step 4: Run** `python docs/check_ctl.py` → `check_ctl: OK`.
- [ ] **Step 5: Commit** `git add firmware/gf_node/main/ctl.[ch] docs/check_ctl.py && git commit -m "gf_node ctl: float-driven borewell start/stop with start verify, pure and host-tested"`

### Task 2: `ctl` borewell protections — single-phasing, dry-run with timed retry, max run time

**Files:** Modify `ctl.c`; Test `docs/check_ctl.py`
**Interfaces:** Consumes Task 1. Produces flag semantics `CTL_F_SINGLE_PHASE`, `CTL_F_BORE_DRY`, `CTL_F_BORE_DRY_LATCH`, `CTL_F_MAX_RUN`.

Pinned values: single-phasing = running and one **present** phase **< 30 %** of the highest for **5000 ms**, needs ≥ 2 present phases. **A phase is present when its raw `bore_mv` is non-zero.** The node's `rms_mv()` returns exactly 0 only when the pedestal is invalid (clamp unplugged / divider fault); a plugged clamp on a dead conductor reads a few mV of noise, which converts to -1 da (under the floor) but is still *present* and counts as 0 A. So 0 mV = ignore the channel; 1–14 mV = present at 0 A (a lost phase); dry = running and highest phase **< bore_dry_da** for **20000 ms**; dry rest **30 min**, **3** attempts, then latch; max run = running continuously **> max_run_min**.

- [ ] **Step 1: Write failing tests:**
  - `single_phase_alert_only_does_not_stop`: `bore_act=false`, phases (60,60,5) da for 6 s → flag set, `bore_stop` false.
  - `single_phase_act_holds_stop_and_blocks_auto`: `bore_act=true` → `bore_stop` held true; float low for 10 min → no START edge.
  - `missing_clamp_is_not_single_phasing`: raw `bore_mv` with one or two channels at exactly 0 mV, the rest drawing, for 1 min → flag never set.
  - `lost_phase_with_clamp_present_is_single_phasing`: `bore_mv` = (two channels drawing ~60 da, third at 3 mV) for 6 s → flag set.
  - `dry_retry_three_times_then_latch`: `bore_act=true, bore_dry_da=20`, running at 10 da: stop held after 20 s; START edge at +30 min; repeat; after the 3rd dry stop `CTL_F_BORE_DRY_LATCH` set and no START for 3 h.
  - `max_run_flags_never_stops`: `max_run_min=60`, running 61 min → `CTL_F_MAX_RUN`, `bore_stop` false.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** in `ctl_step`. A held protective stop sets `out->bore_stop` every step while active.
- [ ] **Step 4: Run** → OK.
- [ ] **Step 5: Commit** `"gf_node ctl: single-phasing, dry-run retry and max-run on the borewell"`

### Task 3: `ctl` sump motor — hold when the borewell has failed, no-load dry, release delay

**Files:** Modify `ctl.c`; Test `docs/check_ctl.py`
**Interfaces:** Consumes Tasks 1–2. Produces `CTL_F_SUMP_HOLD`, `CTL_F_SUMP_DRY`, optional `CTL_F_CAP_SUSPECT`.

Pinned values: hold trigger = float low **and** any of `START_FAILED | BORE_DRY | SINGLE_PHASE` (never `MAX_RUN`); release = borewell running **or** float high, then clear for **5 min**; sump dry = `pump_on` and Common (`sump_mv[0]`) < `sump_noload_da` for **20000 ms**; cap suspect (alert only) = `pump_on`, Common ≥ `sump_noload_da`, Start (`sump_mv[1]`) channel reads `-1`/0 da for 20 s while that channel has ever read > 0.

- [ ] **Step 1: Write failing tests:**
  - `sump_hold_only_when_bore_failed_and_float_low`: START_FAILED + float low → `CTL_F_SUMP_HOLD`; `sump_act=true` → `sump_stop` held. MAX_RUN + float low → no hold.
  - `sump_hold_releases_after_five_clear_minutes`: hold, then borewell running → `sump_stop` still true at +4 min 59 s, false at +5 min.
  - `sump_dry_needs_threshold_and_pump_on`: `sump_noload_da=0` → never; `=30`, pump_on, Common 20 da for 21 s → flag; pump_on false → never.
  - `sump_alert_only_never_energises`: `sump_act=false` with every sump flag set → `sump_stop` false.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → OK.
- [ ] **Step 5: Commit** `"gf_node ctl: sump motor held off when the borewell has failed, no-load dry check"`

### Task 4: `ctl` commands, latches and persistence

**Files:** Modify `ctl.h`/`ctl.c`; Test `docs/check_ctl.py`
**Interfaces — Produces:**
```c
typedef enum { CTL_OK = 0, CTL_REFUSED_SINGLE_PHASE, CTL_REFUSED_SUMP_HOLD, CTL_REFUSED_NOT_SETTLED } ctl_result_t;
ctl_result_t ctl_command(ctl_state_t *s, const ctl_cfg_t *c, ctl_motor_t m, ctl_cmd_t cmd, ctl_src_t src, int64_t now_ms);
void ctl_persist(const ctl_state_t *s, ctl_persist_t *p);      /* what to write to NVS */
bool ctl_persist_changed(const ctl_state_t *s);                /* since last ctl_persist() */
const char *ctl_last_cmd(const ctl_state_t *s);                /* "app:bore:start" etc., static storage */
```
Semantics: Stop (any motor, any mode) sets that motor's user-stop latch and holds its STOP relay. Start clears the user-stop latch, clears single-phasing and dry latches **if the condition is not currently present** (else returns `CTL_REFUSED_SINGLE_PHASE`), refuses while `CTL_F_SUMP_HOLD` for the sump (`CTL_REFUSED_SUMP_HOLD`) and while not settled. Borewell Start in NEC mode = one START edge (the NEC/starter decides thereafter). Sump Start = release `sump_stop`. Repeated commands are idempotent.

- [ ] **Step 1: Write failing tests:**
  - `user_stop_holds_even_in_alert_only_and_nec_mode`.
  - `start_clears_user_stop_and_starts`.
  - `start_refused_while_phase_still_missing` / `start_clears_single_phase_when_gone`.
  - `repeated_stop_is_idempotent`: two Stops → persist changed once.
  - `boot_restores_held_stops_never_starts`: `ctl_init` with `{user_stop_bore=1, single_phase=1}`, float low, 10 min of steps → `bore_stop` true from the first step, zero START edges.
  - `last_cmd_names_source`: app Start bore → `"app:bore:start"`.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → OK.
- [ ] **Step 5: Commit** `"gf_node ctl: Start/Stop commands, latches, what survives a reboot"`

### Task 5: Node glue — pins, boot-safe relays, float input, NVS, sample loop, telemetry

**Files:** Create `relay.c`, `ctl_glue.c`; modify `gf.h`, `sensors_util.c`, `main.c`, `CMakeLists.txt`, `Kconfig.projbuild`; Test `docs/check_gf_node.py`, `docs/check_pinmap.py`
**Interfaces — Produces:**
```c
/* gf.h */
#define GPIO_SUMP_FLOAT     26   /* PC817 output, low = opto on = float loop closed */
#define GPIO_RLY_BORE_START 16
#define GPIO_RLY_BORE_STOP  17
#define GPIO_RLY_SUMP_STOP  18
void relay_init(void);                       /* first thing in app_main */
void relay_apply(const ctl_out_t *o);        /* levels + 1000 ms START pulse via esp_timer one-shot */
void ctl_glue_init(void);                    /* NVS load (namespace "ctl"), ctl_init */
void ctl_glue_step(const ctl_in_t *in);      /* lock, ctl_step, relay_apply, save if changed */
int  ctl_glue_json(char *buf, size_t len);   /* the fields appended to telemetry */
```
- `relay_init`: drive all three pins to the **off level** with `gpio_set_level` **before** `gpio_set_direction(OUTPUT)`; relays are wired **high-trigger with a 10 kΩ pull-down per input** (Task 11), so a high-Z pin during reset is off.
- Kconfig: remove `GF_RWT_FLOTY_WIRED`; add `GF_FLOAT_LOW_LEVEL` (int 0/1, default 0 = "opto on means sump low") and `GF_CTL_TOKEN` (string, default empty = commands refused).
- `float_low` = -1 when `cfg.ctrl_node` false, else debounced in ctl from the raw GPIO.
- Telemetry additions to the utility JSON: `"float_low":true|false|null,"ctrl_node":bool,"bore_act":bool,"sump_act":bool,"flags":<uint>,"relays":[bore_stop,bore_start,sump_stop],"last_cmd":"<str>"`. `rwt_floty` stays, always `null`.
- Telemetry buffer in `web.c` grows from 512 to 768.

- [ ] **Step 1: Update `docs/check_gf_node.py`** expected-key table with the fields above (types: bool, bool, bool, int, list of 3 bools, str; `float_low` nullable) → run → FAIL. Also add the three relay rows and the `SUMP_FLOAT` row to the GPIO table in `docs/WIRING.md` §11.2 (pins only; Task 11 writes the prose) so `docs/check_pinmap.py` stays green.
- [ ] **Step 2: Implement** the files above. `sensors_sample()` calls `ctl_glue_step()` with the fresh readings at the end of each sample.
- [ ] **Step 3: Run** `python docs/check_gf_node.py`, `python docs/check_ctl.py` and `python docs/check_pinmap.py` → OK.
- [ ] **Step 4: Commit**, then **build** gf_node utility role (`idf.py -B build_util -D SDKCONFIG=build_util/sdkconfig build`) → succeeds with no warnings.

### Task 6: Node command endpoints

**Files:** Modify `web.c`, `ctl_glue.c`; Test `docs/check_gf_node.py` (host-compiled parse functions with cJSON, as `check_gf.py` does)
**Interfaces — Produces (consumed by the hub, Task 8):**
- `POST /api/ctl` body `{"token":"…","motor":"bore"|"sump","cmd":"start"|"stop","src":"app"|"dash"}` → 200 `{"ok":true}` or 409 `{"ok":false,"why":"single_phase"|"sump_hold"|"not_settled"}`; bad/missing token → 403, nothing changes; malformed → 400.
- `POST /api/ctl/cfg` body: every `ctl_cfg_t` field by its C name plus `"token"` → 200; persisted to NVS; missing fields keep their current value.
- Pure: `int ctl_parse_cmd(const char *json, const char *token, ctl_motor_t *m, ctl_cmd_t *c, ctl_src_t *src)` (0 ok, 403/400) and `int ctl_parse_cfg(const char *json, const char *token, ctl_cfg_t *cfg)` in `ctl_glue.c` between `/* CTL_PARSE_BEGIN */` / `/* CTL_PARSE_END */`.
- `max_uri_handlers` 3 → 5. Body limit 512 bytes.

- [ ] **Step 1: Failing tests:** `bad_token_refused_and_state_unchanged` (wrong token, empty token, empty `GF_CTL_TOKEN` → 403, cfg struct byte-identical), `partial_cfg_keeps_other_fields`, `unknown_motor_is_400`.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → OK.
- [ ] **Step 5: Commit**, **build** gf_node utility → OK.

### Task 7: Hub parses the node's control fields

**Files:** Modify `firmware/hub_prod/main/app_priv.h` (`gf_util_t`, mirrored `CTL_F_*`), `app_gf.c` (`gf_parse_util`), `docs/check_gf.py`, `docs/fake_gf_node.py`
**Interfaces — Produces:** `gf_util_t` gains `int8_t float_low; bool ctrl_node, bore_act, sump_act; uint16_t flags; bool relays[3]; char last_cmd[24];` and `hub_state_t` gains `gf_ctl_t ctl` with the same fields (copied in `gf_apply()`).
- [ ] **Step 1: Failing test** in `check_gf.py`: parse the Task 5 JSON shape → fields match; an old-shape payload (no ctl fields) still parses with `flags=0`, `float_low=-1` (node not yet reflashed). Add a check that every `CTL_F_*` value in `app_priv.h` equals the one in `gf_node/main/ctl.h` (read both files, compare).
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** `fake_gf_node.py` emits the new fields. **Step 4: Run** → OK.
- [ ] **Step 5: Commit.**

### Task 8: Hub command path — POST client, queue, `/api/motor`, cfg push, RainMaker params

**Files:** Modify `app_gf.c`, `app_priv.h`, `app_web.c`, `app_main.c`, `app_cal.c` (new cal fields)
**Interfaces — Produces:**
```c
typedef struct { uint8_t motor, cmd, src; } gf_cmd_t;      /* values = ctl.h enums */
bool gf_cmd_post(const gf_cmd_t *c);   /* enqueue (xQueueSend, no wait); false if full */
/* gf_task drains the queue before each poll: POST /api/ctl to the utility node */
/* gf_task pushes /api/ctl/cfg on the node's offline->online edge and whenever cal changes */
```
- New cal (NVS, `/cal` "Ground-floor control" fieldset): `ctl_token` (str ≤ 32), `sump_noload_da`, `max_run_min`, `ctrl_node`, `bore_act`, `sump_act`. CT A/V, turns, `run_deci_amps`, `bore_dry_deci_a` already exist and are pushed as `bore_apv_x100`, etc.
- `POST /api/motor` on the hub: `{"motor":"bore"|"sump","cmd":"start"|"stop"}` → enqueues with `src=dash` → 202; queue full → 503. The node's 409 reason is surfaced as the next telemetry's `last_cmd` plus an event.
- RainMaker, ground-floor device: `"Borewell Start"`, `"Borewell Stop"`, `"Sump Motor Start"`, `"Sump Motor Stop"` (bool, `esp.ui.trigger`, write-only), `"Node Controls Borewell"`, `"Borewell Protection Acts"`, `"Sump Protection Acts"` (bool toggles). `write_cb` only enqueues / writes cal; never does HTTP itself.
- [ ] **Step 1: Failing tests:** a host test of the pure body builder `int gf_ctl_body(const gf_cmd_t*, const char *token, char *buf, size_t)` (between `GF_PARSE_BEGIN/END`) produces exactly the Task 6 JSON.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** all hub checkers → OK.
- [ ] **Step 5: Commit**, **build** hub_prod → OK, app partition still ≥ 10 % free.

### Task 9: Hub alerts from node flags; remove the hub's own bore-dry detectors

**Files:** Modify `app_main.c` (alerts; delete the `BORE_DRY_BEGIN/END` block and `bore_dry` state, `s_al_bore_dry` re-sourced), `app_priv.h`; delete `docs/check_bore_dry.py`; Test `docs/check_alerts.py`
Alert texts (exact, each ≤ 100 bytes), raised on the flag's rising edge through `alert_eval()` with the existing rearm:
- `START_FAILED`: `"Borewell was started but draws no current. Check the starter and the supply."`
- `SINGLE_PHASE`: `"Borewell lost a phase (%s). Stopped: press Start once the supply is fixed."` / alert-only variant ending `"Not stopped (alert only)."`
- `BORE_DRY`: `"Borewell running dry (%d.%d A). Resting 30 min before retry."`; latch: `"Borewell dry 3 times. Stopped until you press Start."`
- `MAX_RUN`: `"Borewell has run %u min and the sump float has not risen."`
- `SUMP_HOLD`: `"Sump draining with the borewell down. Sump motor held off."` (alert-only: `"... Sump motor NOT stopped (alert only)."`)
- `SUMP_DRY`: `"Sump motor running dry (%d.%d A)."`
- `CAP_SUSPECT` (if Task 3 shipped it): `"Sump motor Start winding draws nothing. Check the start capacitor."`
- [ ] **Step 1:** add the texts to `check_alerts.py`'s inventory → run → FAIL. **Step 2: Implement.** **Step 3: Run** `check_alerts.py`, `check_telemetry.py`, `check_offline_view.py` → OK (fix keys the dashboard read from `bore_dry`).
- [ ] **Step 4: Commit.**

### Task 10: Dashboard and `/cal`

**Files:** Modify `firmware/hub/data/dashboard.html`, `app_web.c` (telemetry `motors.borewell.ctl` / `motors.sump_motor.ctl`: `{flags, act, user_stop, last_cmd}`, plus `ground.float_low`, `ground.ctrl_node`)
- Each motor unit: Start / Stop buttons (POST `/api/motor`), a small "Alert only" / "Acts" tag, hold reasons in words from `flags` (one sentence each, the alert texts without numbers). Sump tank: "Float: low / high / not read". Offline: buttons disabled, hatched like other readings.
- [ ] **Step 1:** add the new keys to `check_telemetry.py` and `check_offline_view.py` → FAIL. **Step 2: Implement.** **Step 3:** `node --check` on the extracted `<script>`; run both checkers → OK; open `dashboard.html?demo=1` locally and confirm buttons and reasons render.
- [ ] **Step 4: Commit**, **build** hub_prod → OK.

### Task 11: Wiring documentation and pin map

**Files:** Modify `docs/WIRING.md` §11.2 (replace the RWT-floaty paragraph with the float loop and add a relay table), `docs/HARDWARE.md` §3.5 (BOM: 3-ch relay module high-trigger, 3 × 10 kΩ, isolated 12 V module, PC817, 1.2 kΩ 0.25 W, 1N4148, 10 kΩ, 100 nF ×2, TVS SMBJ15CA, bypass switches, junction box); Test `docs/check_pinmap.py`
- Content: the wiring in the "Wiring" section below, verbatim tables.
- [ ] **Step 1: Run** `check_pinmap.py` → FAIL (doc lacks relay pins). **Step 2:** write the docs. **Step 3: Run** → `node 0x06 ... GPIOs agree`. **Step 4: Commit.**

### Task 12: Bench acceptance (spec §8 step 1) — manual, recorded

Lamps on the relay contacts, float in a bucket, clamps on a test load (a kettle or fan), node + hub on the bench LAN. Record results in `docs/WIRING.md` §11.2 "Bench, <date>".
- [ ] Node unpowered: all three contacts in their resting state (meter): STOP NCs closed, START NO open.
- [ ] Power-cycle the node 20 times with the float low and `ctrl_node=true`: the START lamp never lights before 30 s; it pulses once after.
- [ ] OTA the node with the float low: no START during or after the update until settled.
- [ ] Each app button and each dashboard button: correct lamp, event logged with source.
- [ ] Stop from the app, power-cycle the node: STOP lamp re-lit within one cycle, START never.
- [ ] Wrong token on `/cal`: buttons refused, node state unchanged.
- [ ] Unplug one clamp of a "running" set: no single-phasing alert.
- [ ] Commit the recorded results.

---

## Wiring (Task 11 writes this into `WIRING.md` §11.2)

### Node GPIO map (utility `0x06`)

| GPIO | Signal | Notes |
|---|---|---|
| 32 / 33 / 34 | Borewell clamps L1 / L2 / L3 | ADC1, §11.3 bias network |
| 35 / 36 / 39 | Sump Common / Start / Run | ADC1; Run socket unfitted |
| 25 | Astero `PUMP ON` | C → GND, NO → GPIO 25, 10 kΩ to 3V3 |
| 26 | Sump float (PC817 output) | 10 kΩ to 3V3, 100 nF to GND |
| 16 | `RLY_BORE_START` | high = energise |
| 17 | `RLY_BORE_STOP` | high = energise |
| 18 | `RLY_SUMP_STOP` | high = energise |
| 21 / 22 | SHT30 SDA / SCL | |

### Relay module

3-channel 5 V relay module **with a high/low trigger jumper set to HIGH**, contacts ≥ 250 VAC 10 A, JD-VCC opto isolation. `VCC`/`JD-VCC` from the node's 5 V, `GND` common, `IN1..3` from GPIO 16/17/18, **10 kΩ from each `IN` to GND** so a pin floating during reset or flashing reads off.

### Borewell starter (MK1 DOL + NEC-49)

| Relay | Contact | Before cutover | After cutover (NEC-49 removed) |
|---|---|---|---|
| `RLY_BORE_STOP` | COM + **NC** | in series with the NEC-49 **"NV Coil Series"** pair (break one of its two wires and insert COM/NC) | the two "NV Coil Series" wires join through COM/NC only |
| `RLY_BORE_START` | COM + **NO** | in parallel with the NEC-49 **"ON Button Side"** pair (both land on the same two terminals) | COM/NO alone across those two terminals |

A **2-pole bypass switch**: pole 1 shorts the `RLY_BORE_STOP` COM-NC pair, pole 2 opens the `RLY_BORE_START` COM-NO line. Bypass ON = node out of the circuit, starter on its own buttons.

### Sump motor (Astero)

| Relay | Contact | Wiring (chosen at commissioning, spec §9 item 2) |
|---|---|---|
| `RLY_SUMP_STOP` | COM + **NC** | if an **open** float loop reads as "tank full": in series with one wire of the `TWT FLOTY` loop |
| | COM + **NO** | if a **closed** loop reads as "tank full": across the two `TWT FLOTY` terminals |

Bypass switch: shorts the NC (series case) or opens the NO (parallel case).

### Sump float loop (isolated, ~9 mA)

```
 Isolated 12 V module (+) ── 1.2 kΩ 0.25 W ──► PC817 pin 1 (anode)
                                               PC817 pin 2 (cathode) ── float wire A ─┐
 Isolated 12 V module (−) ───────────────────────────────────────── float wire B ─┘
   1N4148 reverse across pins 1-2;  SMBJ15CA TVS across float wires A-B at the board
 PC817 pin 4 (collector) ── GPIO 26 (10 kΩ to 3V3, 100 nF to GND)
 PC817 pin 3 (emitter)   ── node GND
```
The 12 V module's output is **not** connected to the node's ground. Float wires A/B are the pair that closes with the float **hanging down** (measured on site); third wire insulated. Joint to the extension cable above water in a junction box; route away from mains conduit where possible.

### Power

HLK-20M5 fed from the **same L-N as the borewell contactor coil** (if the coil is 230 V), through a 1 A fuse. The isolated 12 V module from the same feed, own fuse.

### Clamps

As `WIRING.md` §11.3: borewell T1 / T2 / T3 → channels 1-3; sump **B (Common)** → ch 4, **Start winding** → ch 5, Run → ch 6 (later). Isolate and lock off before fitting.
