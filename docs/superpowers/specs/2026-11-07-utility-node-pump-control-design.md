# Utility node 0x06: borewell controller and sump-motor dry-run protection

Date: 2026-11-07. Status: agreed in conversation, awaiting written review.
Supersedes the utility-node parts of `2026-09-07-ground-floor-wifi-nodes-design.md`
(§4.2, §4.3) where they conflict. The hub-split design (`2026-09-17`, "Plan B")
is deferred, not cancelled: this node is built on the ESP32 hub as it is today.

## 1. Why

On 2026-11-06 the plant stopped with the RWT and the sump both empty. The
borewell's NEC-49 level controller showed MOTOR START, but the borewell was not
running; toggling the NEC-49 to Manual and back to Auto started it. Meanwhile the
sump motor, whose Astero controller has no sump-level input, kept pumping and ran
the sump dry. Nothing alerted.

What was learned while diagnosing it:

* **The NEC-49 does three things only**: reads one float, starts the motor by
  briefly shorting the starter's START button ("ON Button Side"), and stops it by
  opening a contact in the starter coil circuit ("NV Coil Series"). It is powered
  from L1/L2 only and **monitors no phases**. Its MOTOR START LED means "I want the
  motor on", never "the motor is on". Its "Over Head Tank" input is shorted on this
  panel. No documentation exists (unbranded).
* The likely failure mode: the DOL starter's no-volt release dropped the
  contactor (supply dip or brief cut) and the NEC-49 never re-pulsed START,
  because it only pulses on a state change. Overload trip is ruled out: toggling
  the NEC-49 would not have cleared it.
* **The borewell is three-phase** (415 V NEC-49, 3-pole MK1 DOL starter, 1/L1
  3/L2 5/L3 in, 2/T1 4/T2 6/T3 out on a 3-core cable, overload 6-10 A range).
* **The sump motor is single-phase, 3-wire** (Astero mains `P`/`N` only,
  `STARTING RELAY O/P`, pump terminal `B` tied to neutral). Its three clamp
  channels are Common / Start / Run (commit `d4634b9`, `WIRING.md` §11.3).
* **Ultrasonic level sensors are out, everywhere, for good.** The dosing drum
  proved they cannot be trusted (`WIRING.md` §13.2). The sump node `0x05` was
  ultrasonic-only and is therefore not built. Replacing level sensing in the
  other tanks is a separate project.

## 2. Goals

1. **Replace the NEC-49 with the utility node** as the borewell controller,
   driven by the one existing sump float.
2. **Fix the restart gap**: a borewell that is commanded on but not drawing
   current gets START re-pulsed, and an alert if it still does not run.
3. **Protect the sump motor from running dry**, without a second float.
4. **Detect borewell single-phasing** from the three clamps and stop the motor.
5. **Start / Stop buttons for both motors** in the RainMaker app and on the LAN
   dashboard.
6. **Alert-only first.** Every protective action can be held to "notify only"
   until the owner chooses to let the node act.

## 3. Non-goals

* Any ultrasonic, anywhere. No sump level other than the float.
* Reading the **RWT float** (the Astero's `TWT FLOTY` loop): it runs ~25 m in mains
  conduit and carries induced voltage. The node may *break* that loop with a relay
  contact (§5.3); it never *measures* it. `rwt_floty` stays `null`.
* A second (dry-run) float. Decided against: one float is enough (§6.3 covers dry
  run without it).
* Replacing the Astero. It stays in charge of the sump motor.
* Supply-voltage / phase-voltage monitoring. Single-phasing is detected from
  current only.
* The ESP32-S3 hub split, SD history, RWT/TWT level replacement.

## 4. Anti-goals (would be failures even if everything "works")

* **A dead, rebooting or mis-flashed node must never leave the building unable to
  pump.** With the node unpowered, both starters must work by hand from their own
  buttons, exactly as the NEC-49's Manual mode does today.
* **No spurious START**, ever: not at power-up, not on reboot, not on an OTA, not on
  a Wi-Fi reconnect. A spurious STOP is tolerable; a spurious START is not.
* Nothing the node does may defeat the starter's own **STOP button or overload**.
* Alerts that cry wolf. Thresholds that need real data stay in alert-only until
  they have it.
* Safety that depends on the LAN. All protective logic runs **on the node**; the
  hub only forwards commands and settings and displays state.

## 5. Hardware

Starting from bare parts; nothing of the 09-07 utility board was built.

### 5.1 Carried over from the 09-07 design (unchanged)

ESP32 DevKit (WROOM), HLK-20M5 5 V supply, six CT channels on ADC1 with the shared
bias rail of `WIRING.md` §11.3 / §14.1 (GPIO 32/33/34 borewell L1-L3; 35/36/39 sump
Common/Start/Run, Run socket built but unfitted), Astero `PUMP ON` C/NO on GPIO 25
with pull-up, SHT30 on GPIO 21/22.

### 5.2 New: sump float input (replaces the RWT_FLOTY opto on GPIO 26)

The CRI cable tilt float (already in the sump, 2 m cable, internal switch rated
220 V) moves from the NEC-49 to the node at cutover (§8). The node owns the loop
entirely; nothing else is connected to it.

* Loop powered from a **separate isolated 12 V supply** (not the node's 5 V rail),
  ~**10 mA** wetting current (≈1.2 kΩ) — enough to keep the float's switch contacts
  clean — through a **PC817** LED; transistor side to GPIO 26 with pull-up.
* RC filter and a TVS/clamp on the loop side; firmware debounce (§6).
* The float cable is extended to the panel with the joint above water in a
  junction box. Use one contact pair; insulate the third wire.
* Which pair closes when the float hangs **down** (sump low) is measured on site
  (§9) and recorded.

### 5.3 New: three relay outputs

A relay module with contacts rated ≥ 250 VAC / 10 A, opto-isolated inputs.

| Relay | Contact used | Wired | Energised means |
|---|---|---|---|
| `RLY_BORE_STOP` | **NC** | in series with the starter coil circuit (alongside STOP 95-96 and, until cutover, the NEC-49 "NV Coil Series") | coil circuit open → borewell stops |
| `RLY_BORE_START` | **NO** | across the starter's START button (93-94 / 13-14 side), where the NEC-49 "ON Button Side" lands | START pressed (pulse only, ~1 s) |
| `RLY_SUMP_STOP` | **NC or NO, decided on site** | in the Astero's float loop (`TWT FLOTY` C/NC), so the Astero sees "tank full" | sump motor stops via the Astero's own logic |

* **Every relay is de-energised in the normal state.** Node off = motors behave as
  if the node did not exist. This is the anti-goal in §4, and it is why both STOP
  relays use their NC contact.
* **Output pins must be ones that do not glitch at boot** (not 0, 2, 5, 12, 15, nor
  1/3). Builder picks from the free non-strapping pins; the START pin is the one
  that matters most. Driven to the off level before the relay module is powered or
  enabled, and the START output is gated in firmware until the node has been up for
  a fixed settle time (§6.5).
* `RLY_SUMP_STOP`'s contact form depends on which loop state the Astero reads as
  "full": open loop = full → NC in series; closed loop = full → NO in parallel.
  Determined at commissioning (§9). The relay breaks or bridges the loop; it does
  not measure it, so the induced voltage on that loop is irrelevant beyond the
  contact's voltage rating.
* The borewell coil may be 230 V (the panel's "for 230 V coil connect X to
  neutral" note and the blue neutral at the overload) or 415 V. Measured on site
  (§9); the relay contact must be rated for whichever it is.
* A **bypass switch per motor** at the panel shorts out (or removes) the node's
  contacts, so the node can be taken out of circuit without rewiring.

### 5.4 Panel safety

The borewell starter is 415 V between phases; the sump panel is 230 V. Clamps go on
with the supply isolated and locked off, one conductor per clamp. Both panels carry
green-yellow wires used as live conductors (borewell 4/T2, sump `Y`): label them.

## 6. Control logic (on the node)

The node runs one 2 s cycle (as today) plus a fast path for relay timing. All
amp thresholds are in calibrated amps; the hub already owns the CT calibration
(A/V, turns), so it pushes those values to the node, which keeps them in NVS and
works without the hub (§7.2).

### 6.1 Borewell (Auto, after cutover)

| Condition | Action |
|---|---|
| Float down (sump low), debounced, borewell not running | pulse START |
| Float up (sump full), debounced | open STOP for a few seconds, then release (the DOL latch is broken; releasing STOP does not restart) |
| START pulsed, no current on any phase within **10 s** | pulse START once more; still nothing → alert *borewell commanded on but not running*, stop trying until the float cycles or a person presses Start |
| Running and the float has not risen within **max run time** | alert (float stuck, or the bore is not yielding) — replaces the ultrasonic-based `BORE_DRY_WINDOW` detector, which needed a sump level |
| Running and drawing below the dry threshold (existing `BORE_DRY_DECI_A`, set on `/cal`) for 20 s | alert; in Act also stop, then **timed retry**: restart after 30 min, at most 3 attempts, then latch until a person presses Start (the usual dry-run-relay behaviour: a bore recovers if rested) |
| Running, one phase < ~30 % of the highest for a few seconds | **single-phasing**: alert; in Act also stop and latch until a person presses Start |
| Node boots, float down | treated as "commanded on": START after the settle time (§6.5). This is the restart the NEC-49 never did |

**STOP is pulsed or held depending on why (owner's decision, 2026-11-07):**

* **Pulsed** (open a few seconds, release) for the normal *float up / sump full*
  stop. The panel's START works again immediately.
* **Held open** for a **deliberate stop**: a person's Stop from the app or
  dashboard, a single-phasing stop, and a dry-run stop (including its 30-minute
  rest). While held, the panel's green START cannot run the motor; the per-motor
  **bypass switch** (§5.3) is the way past the node. Before cutover this is also
  what stops the NEC-49 from restarting the motor seconds later.
* Consequences for the build: `RLY_BORE_STOP` must be rated for **continuous**
  energisation (coil heat, module supply budget); a held stop is lost if the node
  loses power (the anti-goal in §4 wins: node off = manual), and is re-applied at
  boot from the NVS latch of §6.5 — re-energising a STOP at boot is allowed, a
  START is not. The same hold rule applies to `RLY_SUMP_STOP`, which is held for
  as long as any sump hold or Stop latch lasts.

Start/stop hysteresis comes from the float's own swing (its free cable length) — no
timers needed for it; the debounce only rejects splash.

### 6.2 Who decides: two separate settings

*(Designer's call, from the review.)*

* **Borewell controller = NEC-49 / node.** With "node", the float logic of §6.1
  (start on low, stop on full, restart after a failed start or a reboot) **always
  runs** — it is the borewell's only controller, and switching it off would leave
  the building without one.
* **Protection = Alert only / Act**, per motor (default Alert only). It governs only
  the *protective* stops: single-phasing, dry current, the sump hold. In Alert only
  they notify and move no relay; in Act they stop the motor.
* Before cutover, the NEC-49 is still in charge of the borewell and the float is
  still on the NEC-49, so the borewell's float logic and the §6.3 sump hold cannot
  run; only the current-based checks (single-phasing, dry threshold, the sump
  no-load check once set) and the buttons apply. Accepted.

### 6.3 Sump motor dry-run (no second float)

| Condition | Action |
|---|---|
| **Borewell has failed** — commanded on but not running, dry-run stop, or single-phasing stop (**not** max-run-time, which a stuck float could cause) — **and float down** | alert *sump draining with borewell down*; in Act, open `RLY_SUMP_STOP`. Released when the borewell is drawing normal current again, or the float comes up. With a dead borewell the hold lasts until a person acts: protecting the pump is chosen over keeping the RWT topped up. This alone would have prevented 2026-11-06 |
| PUMP ON closed and Common current below the **no-load threshold** for 20 s | alert *sump motor running dry*; in Auto, stop. **Threshold is unset (alert disabled) until real running current is seen and it is set on `/cal`** |
| Held off by the node | released only when the reason has cleared **and stayed clear for a few minutes**, so a pump on the edge of dry does not cycle |
| PUMP ON closed, Start winding current 0 while Common draws | alert *start capacitor suspect* (alert only, never acts) — optional, builder's call whether it lands in this release |

Accepted cost of one float: when the borewell fails, the water between the float's
low point and the sump floor is not used. Hanging the float lower trades that back
against a deeper draw before the borewell starts; that is a site adjustment.

### 6.4 Buttons (app and dashboard)

* **Start** and **Stop** for the borewell and for the sump motor.
* **Pressing Start is the acknowledgement**: it clears the single-phasing and
  dry-run latches and starts the motor, if the condition is no longer present
  (designer's call; one command, no separate Reset).
* A **Stop** from the app/dashboard latches: Auto will not restart that motor until
  a Start is pressed (or the mode is changed). Otherwise a remote stop is quietly
  undone a minute later.
* **Start is refused** (with a reason returned and shown) while an interlock holds:
  single-phasing latched, sump motor dry-run hold, node not settled.
* Buttons work in both modes and before cutover (the relays are wired from day
  one). Sump-motor Start means "release `RLY_SUMP_STOP`"; the Astero decides
  whether the pump then runs.
* The panel's own STOP and the overload always win; the node cannot override them.
* Every command and every automatic action is logged with its source (app,
  dashboard, auto) and appears in the hub's event list.

### 6.5 Boot and outage behaviour

* **What survives a reboot (NVS):** a person's Stop latch and the single-phasing
  latch — both are a human decision or a hazard a person must look at.
  **What does not:** the failed-restart give-up, the dry-run retry count and the
  sump hold; they are re-evaluated from live readings. So after an ordinary power
  cut the borewell restarts by itself (the 11-06 fix), but never one that a person
  stopped or that was single-phasing. *(Designer's call.)*

* Relays off at reset. START is blocked for a settle time after boot (e.g. 30 s,
  and until at least two consistent float reads and one clamp read have completed).
* Hub or Wi-Fi down: the node carries on in its current modes with its stored
  settings. Buttons are unavailable; protection is not.
* Node down: everything is manual at the panels (§4).

## 7. Hub, app and dashboard

### 7.1 Telemetry (node → hub, the existing GET, every `GF_POLL_MS` = 5 s)

The existing utility object (`id`, `bore_mv`, `sump_mv`, `sump_on`, SHT30 fields)
gains: float state, each motor's mode, commanded state, hold/latch reasons, and
the last command with its source. `rwt_floty` stays and stays `null`. The contract
checkers (`check_gf.py`, `check_gf_node.py`, `check_telemetry.py`,
`check_pinmap.py`) follow the change.

### 7.2 Commands and settings (hub → node, new)

A POST endpoint on the node for: Start/Stop per motor, mode per motor, and the
calibration and thresholds the node needs (CT scale/turns per channel, borewell
dry threshold, sump no-load threshold, max run time). Node persists settings in
NVS; the hub re-sends them when the node comes back online. The hub's
`app_gf.c` HTTP client is the starting point; the hub currently only GETs.

### 7.3 RainMaker and dashboard

* RainMaker: Start/Stop for each motor and a mode selector per motor on the
  ground-floor device; existing running/amps params stay. New alerts through the
  existing `alert_eval()` path (rearm, latch, 100-byte limit pinned by
  `check_alerts.py`).
* Dashboard: Start/Stop and mode next to each motor's existing unit; float state
  on the sump; hold reasons shown in words.
* `/cal`: the thresholds in §7.2.
* **Detection moves to the node.** The hub's own `bore_dry_by_amps()` /
  `bore_no_yield_now()` are removed (the latter needed the ultrasonic sump level
  that will never exist); the hub raises its alerts from the node's reported
  flags, so one fault gives one alert and the threshold is interpreted in one
  place. The hub's `0x05` sump-node plumbing stays, unused (designer's call:
  removing it is scope this project does not need).
* The **NEC-49 cutover** is a setting, not a firmware change: "borewell controller
  = NEC-49 / node". With "NEC-49", the float logic in §6.1 is off.

## 8. Rollout

1. **Bench**: node with relays driving lamps, a float in a bucket, clamps on a test
   load. Prove: no START at power-up, reboot or OTA; every relay off with the node
   unpowered; buttons; holds; alert texts.
2. **Install, NEC-49 still in charge**: clamps, PUMP ON, relays wired in (STOP in
   series, START in parallel, sump-stop in the Astero loop). Alert-only. Collect
   real running currents for a week; set the sump no-load and borewell dry
   thresholds from them.
3. **Cutover** (one visit, owner's call): float moved from the NEC-49 to the node,
   NEC-49 removed, borewell controller set to "node", borewell Auto. Sump-motor
   protection Auto when the owner is satisfied.
4. **Back-out**: refit the NEC-49 and its float; set controller to "NEC-49". The
   node's relays stay wired and idle.

## 9. Measured on site before wiring (commissioning checklist)

1. Borewell contactor coil voltage (230 V or 415 V) → relay rating.
2. Astero float input: which loop state it reads as "tank full", and the loop
   voltage → `RLY_SUMP_STOP` NC-in-series or NO-in-parallel, and its rating.
3. Float wires: which pair closes with the float hanging down.
4. Running currents: borewell per phase; sump Common / Start / Run; which of `R`/`Y`
   is the Start winding.
5. Cable route and length from the sump to the panel; keep the float cable out of
   mains conduit where possible.

## 10. Left to the builder

**Command auth (designer's call):** the node's command endpoint starts motors, so
it requires a shared token set on both `/cal` pages — unlike `/ota`, which has
none today.

**Node power (designer's call):** feed the HLK-20M5 from the same supply as the
borewell contactor coil (its L-N if the coil is 230 V). Then losing the coil's
phase drops coil and node together, and losing any other phase leaves the node
alive to see the single-phasing.

Exact GPIOs for the relays and the 12 V loop parts (within §5's rules); relay
module choice; the node's command endpoint shape and auth (match the node's existing
`/ota` handling); debounce and settle values within the ranges given; JSON field
names; how the dashboard lays out the buttons; whether the start-capacitor alert
lands now or later; the order of firmware work within §8 step 1.
