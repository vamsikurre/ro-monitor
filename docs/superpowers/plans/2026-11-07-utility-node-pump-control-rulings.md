# Rulings made while executing the utility node pump-control plan

Every decision the controller took on the owner's behalf during subagent-driven execution (branch utility-node-ctl, c64fd25..bfb7475), in the order made, each with what it costs if wrong. Deferred minor findings are listed after.

## Rulings

1. add `int8_t ctl_float_low(const ctl_state_t*)` to Task 1's interface — T5 telemetry must report the debounced value — cost if wrong: one extra accessor.
1. single-phasing treats raw 0 mV as "no clamp" and 1-14 mV as "present at 0 A"; added test lost_phase_with_clamp_present_is_single_phasing — rms_mv() returns exactly 0 only on pedestal failure — cost if wrong: an unusually quiet plugged channel reading 0 mV hides a lost phase (overload remains the backstop).
1. Task 5 adds the relay/float pin rows to WIRING §11.2 so check_pinmap stays green — avoids a red checker across six tasks — cost: none.
1. Task 8's route is verified by the gf_ctl_body host test + build, not check_telemetry.py — that checker validates telemetry keys — cost: /api/motor's HTTP wiring only proven at the bench.
1. node token is a Kconfig string (GF_CTL_TOKEN), hub's copy on /cal — node has no settings page — cost: changing the token needs a node rebuild.
1. Task 12 is not dispatched; it is handed to the owner as a bench checklist — it needs physical hardware — cost: the branch is unproven on hardware until the owner runs it.
1. boot settle's "1 clamp read" means one completed ctl_step after boot (every step follows a full sample), not a channel above the noise floor — the literal reading never settles with both motors idle, so the borewell would never auto-start — cost if wrong: none; a step without clamp data cannot happen in the glue.
1. one STOP pulse per float-high episode is correct (spec §6.1: releasing STOP does not restart; a motor still running after a pulse is START_FAILED's mirror and is out of scope) — cost if wrong: a stop that did not take goes unnoticed until the next float cycle.
1. in NEC mode the current-based protections run after a NEC settle = 30 s + 1 step (no float exists); a dry rest ends with release + one START edge (the NEC was running the motor when dry hit, and a released STOP does not restart a DOL starter) — cost if wrong: the node restarts a borewell the NEC might not have wanted running (it only can after a 30 min rest, and the START is a button press the NEC itself would make).
1. reviewer minor "simultaneous bore_start and bore_stop when single-phase latches during a dry rest" pulled into the fix round — a START edge with STOP held is contradictory relay output — cost: none.
1. reviewer minor "dry_attempts reset after 20 s healthy" pulled in: reset only after 10 min continuous healthy running — 20 s lets a bore that briefly recovers cycle dry every 30 min forever — cost if wrong: a bore that recovers for 10+ min between dry spells is retried indefinitely (alerts still fire each time).
1. in Alert only, no rest/retry/attempt counting: CTL_F_BORE_DRY simply follows the debounced dry condition — the flapping/latching in alert-only was meaningless (no relay acts) — cost: none.
1. sump hold also requires the borewell not running — a running borewell is by definition not failed, and without it a stale START_FAILED re-triggers the hold the moment it releases — cost if wrong: none found.
1. reviewer minor "unplugged Common clamp (0 mV) counts as sump dry" pulled into a fix round — Review Focus #3 says a pedestal fault must never read as dry; same presence rule as single-phasing (raw 0 mV = no clamp) — cost if wrong: none; a real dry pump with a plugged clamp still reads 1+ mV.
1. persisted holds apply from the first step after boot, before settle (a STOP at boot is allowed, spec §6.5); user-stop holds in any mode; a persisted single-phase latch holds only when bore_act — alert-only latches must not stop a motor — cost if wrong: none.
1. "condition still present" for single-phasing = measured now (motor drawing current with a phase < 30 %); with the motor held off it cannot be measured, so Start clears the latch and the 5 s detector re-trips if the phase is still missing — cost if wrong: one 5 s run on two phases per Start press.
1. every person-initiated borewell Start sets start verify (start_try) in both modes, so a Start that does not take raises START_FAILED — closes Task 2's deferred minor — cost: none.
1. node-mode borewell Start with the debounced float high (sump full) is refused with a new CTL_REFUSED_SUMP_FULL — otherwise the float-high stop has already fired for this episode and the borewell would overfill the sump — cost if wrong: a person cannot top up a full sump from the app (the panel button still works).
1. sump Start also clears an Act SUMP_DRY stop (the person acknowledges); still refused while SUMP_HOLD — cost if wrong: a pump re-run dry once, re-detected in 20 s.
1. reviewer minor 1 (START emitted into a full tank when the float flips between command and step) pulled in: re-check float before emitting, drop pend_start if it fails — it is a START path into a full sump — cost: none.
1. reviewer minor 2 pulled in: node-mode borewell Start with the float unreadable (float_db == -1 after settle) is refused with new CTL_REFUSED_NO_FLOAT — the node's only level sensor is unknown — cost if wrong: an app Start refused while the float input is faulty (panel button still works).
1. reviewer minor 4 pulled in (test the sump relay release, not just the flag) — the relay is what the person sees — cost: none.
1. pull reviewer minor 1 (relay pins never explicitly muxed to GPIO; use gpio_config with pull-down after setting level 0) and minor 3 (log failed nvs_set_*) into a fix round — a relay that silently never energises, or a persisted Stop silently lost, only shows on the bench — cost: none.
1. reviewer minor 2 (a Stop command reaches the relay only at the next step, ≤ ~3 s) → Task 6: ctl_glue_command applies a Stop to the relay immediately — a person's Stop should be immediate — cost: none.
1. 409 "why" values extend to the refusals Task 4 added: sump_full, no_float; CTL_REFUSED_INVALID → 400 — the plan predates those enum values — cost: none.
1. security review (automated) MEDIUM: web.c body-read loop retries HTTPD_SOCK_ERR_TIMEOUT forever — a slow client stalls the single httpd task, blocking telemetry and Stop commands. Ruling: real; joins the fix round (cap retries, then 408) — cost: none.
1. fix round pulls in reviewer minors 1 (cap recv timeouts at 3 → 408), 2 (unknown cfg keys → 400, so the hub never believes a misspelt setting applied), 3 (bore_apv_x100/sump_apv_x100 = 0 and bore_run_da = 0 → 400: they make the borewell never/always "running" on a 415 V controller), 4 (write *m/*c only on success), 5 (bounds-check the why table, fallback 500), 7 (skip the NVS write when cfg is unchanged) — all small, all on the path that can start a motor — cost if wrong: none.
1. every forwarded command and the node's answer (ok / why) is logged as a hub event with its source — spec §6.4 "every command logged", and Task 4's reviewer noted refusals must be logged by the wiring — cost: none.
1. real — a web page opened on any LAN browser could start the borewell or disable protection. Joins Task 8's fix round: /api/motor requires Content-Type: application/json (a cross-site form cannot send it without a CORS preflight the hub never answers), and every state-changing POST (/api/motor, /api/cal/*) rejects a request whose Origin or Referer is present and is not the hub's own host — cost if wrong: a tool that posts without those headers still works; a browser on another origin is refused.
1. minors 1 (token-mismatch log once per status change), 2 (push cfg when the utility node IP changes) and 3 (serialise cal_ctl read-modify-write) pulled into the fix round — cheap, on the control path — cost: none.
1. keep the hub's bore_dry / bore_dry_why telemetry keys and EVT_BORE_DRY_ON/OFF events, now driven by the node's GF_CTL_F_BORE_DRY edge (why = BORE_DRY_BY_AMPS) — the dashboard and history decode them, and renumbering events relabels history — cost: none.
1. alert texts choose the "(alert only)" variant from the node's reported bore_act / sump_act at the moment the flag rises — the node is what acted or did not — cost: none.
1. SINGLE_PHASE stopped text loses "the" ("…once supply is fixed.") to fit 100 bytes with a 29-byte worst-case phase string — the plan's exact text overflows the RainMaker limit — cost: one word.
1. all six minors pulled in — this is the document a 415 V panel is wired from (415 V coil: feed node from L-N and stop/re-spec the STOP relay; label green-yellow live; sump 230 V; float polarity knob; START pulse + terminal numbers; relay.c §11.5 cite; JD-VCC wording) — cost: none.
1. final I3 — "Node Controls Borewell" leaves RainMaker (one tap would remove the borewell's only float logic after cutover); ctrl_node stays on /cal only, and the hub pushes ctrl_node only when its cal holds an explicitly saved value (a wiped hub NVS must not switch the node back to NEC mode) — cost if wrong: cutover/back-out needs the /cal page, not the phone.
1. final I4 — node POST /ota requires the ctl token (X-Ctl-Token header, constant-time compare, empty configured token refuses OTA); README/WIRING curl examples updated — the token on /api/ctl is meaningless if anyone on the LAN (or a cross-site no-cors POST) can reflash the relay controller — cost if wrong: an unconfigured node can only be flashed over USB.
1. final I1 (EV log shows every accepted command as "refused": node answers 200, dashboard tests 202) and I2 (no docs for setting CONFIG_GF_CTL_TOKEN; must never go in tracked sdkconfig.util) — fix.
1. final M1 (sump Start refused by SUMP_HOLD in Alert only) — fix: hold refuses Start only when sump_act — Alert only moves no relay and imposes no interlock — cost: none.
1. final triage T2 "BORE_DRY_LATCH not cleared when bore_act → false" — fix: the alert-only branch clears DRY_LATCH and dry_attempts — stale latch kept alerting and re-held STOP on toggling Act back — cost: none.
1. final M4/M6 leftovers and stale comments — fix (app_priv.h:467,487-488; dashboard dry_why===2 live banner; ctl.h:8; app_priv.h:729 comment placement).
1. final M2 parked — automatic node actions are visible as EVT_BORE_ON/OFF plus flag alerts; a separate "auto" log adds a field for no new information — cost if wrong: the event list does not say "auto" next to automatic starts.
1. final M3 parked — a mode change does not release a person's Stop latch; deliberately safer than the spec's "or the mode is changed" — cost if wrong: after changing mode a person must also press Start.
1. T4 did not close T2's "NEC-mode dry-rest restart sets no start_try" — ctl.c:210 still sets start_try only in node mode; parked — in NEC mode the NEC's own contact may legitimately block START, so START_FAILED there would cry wolf — cost if wrong: a NEC-mode dry restart that does not take raises no alert (EVT_BORE_ON never appears).
1. /ota token also applies to the sump role (shared ota.c) — kept: the sump node is not being built (ultrasonics retired) and an authenticated OTA is the safer default — cost if wrong: a future sump node needs a token set before its first OTA.
1. residual — /cal draws the "node controls borewell" box from hub cal, so on a never-saved/wiped hub the first /cal Save pushes it unticked and can silently take a node out of node mode — parked and surfaced to the owner (no second fix wave); workaround: tick the box deliberately on the first /cal save after cutover — cost if wrong: after a hub NVS wipe, the first /cal save can switch the borewell back to NEC mode with no NEC fitted.
1. residual — node token length not limited in Kconfig while the hub caps it at 32 — parked and surfaced; workaround: use a token of ≤ 32 characters — cost if wrong: a longer node token can never match and every command is refused (fails safe).

## Deferred minor findings

- Task 1: minor (deferred): float_reads counts any reads, not "consistent" ones (5 s debounce still gates action)
- Task 1: minor (deferred): no re-pulse/flag if motor keeps running after a float-high STOP pulse
- Task 1: minor (deferred): retry path relies on ctl.c:234 clearing start_try; add a comment
- Task 1: minor (deferred): zero_thresholds_disable_checks trivially passes until Tasks 2-3 land
- Task 1: minor (deferred): ctl.c:59 settle comment over-long and still mentions clamp read
- Task 1: minor (deferred): settle_with_all_clamps_idle_starts overlaps float_low_starts_after_settle
- Task 2: minor (deferred): no tests for 30 % boundary, single present phase, float-high rest end
- Task 2: minor (deferred): MAX_RUN flag stays up if max_run_min set to 0 mid-run
- Task 2: minor (deferred): NEC-mode dry-rest restart sets no start_try, so a restart that does not take raises no START_FAILED
- Task 2: minor (deferred): BORE_DRY_LATCH not cleared if bore_act toggled true→false while latched (Task 4 Start clears it)
- Task 3: minor (deferred): sump_act false→true with SUMP_DRY set leaves sd_until_ms stale (≤20 s delay)
- Task 3: minor (deferred): no explicit test NEC mode + float low + START_FAILED → no SUMP_HOLD (true by construction)
- Task 3: minor (deferred): ctl.h:8 CT_NOISE_FLOOR_MV comment says "<15 mV = no clamp", no longer the whole story (0 mV = no clamp, 1-14 = 0 A)
- Task 4: minor (deferred): ctl_persist casts const away (brief mandated the const signature)
- Task 4: minor (deferred): dry-latch test seeds state directly rather than reaching it by behaviour
- Task 4: minor (deferred): no boot-restore test with only user_stop_bore in Alert-only node mode
- Task 4: minor (deferred): invalid_command test memcmp's whole struct including padding
- Task 5: minor (deferred): ctl_glue_json splice length unchecked (256 vs ~180 bytes)
- Task 5: minor (deferred): check_gf_node.py:193 line-join artifact
- Task 6: minor (deferred): RED step skipped (tests written after code; they exercise the extracted code)
- Task 6: minor (deferred): cfg_set memcmp includes padding (worst case a redundant save)
- Task 7: minor (deferred): app_priv.h "Everything the dashboard and RainMaker read" comment now sits above the GF_CTL_F block, not hub_state_t
- Task 7: minor (deferred): gf_ctl_t duplicates gf_util_t control fields field-by-field
- Task 8: minor (deferred): no way to clear a stored ctl token from /cal (help text should say so)
- Task 8: minor (deferred): toggles start from hub cal at boot until the node first reports
- Task 8: minor (deferred): s_last_ctl[] written by RainMaker task and poll task unlocked (word-atomic; worst case a redundant report)
- Task 8: minor (deferred): a POST with neither Origin nor Referer passes the CSRF gate (old browsers + no-referrer policy); /api/motor still JSON-only
- Task 8: minor (deferred): DNS rebinding passes same_origin (needs the password anyway) — add a line to the ponytail comment
- Task 8: minor (deferred): duplicate tentative definitions s_dev_ro_room / s_dev_battery in app_main.c (pre-existing)
- Task 9: minor (deferred): check_alerts WIDE_S keyed on substring 'lost a phase' (fragile if a second %s is added)
- Task 9: minor (deferred): MAX_RUN minutes from the hub's run accounting, may lag the node's timer slightly
- Task 10: minor (deferred): ctlFill treats an empty/absent state as live (stricter: state !== "ONLINE")
- Task 10: minor (deferred): button result text can vanish on a structural rebuild
- Task 10: minor (deferred): /api/telemetry worst-case size vs json[4900] unmeasured (overflow returns logged 500, not memory-unsafe) → bench
- Task 11: minor (deferred): HARDWARE BOM relay row still says "JD-VCC opto" without the isolation caveat
