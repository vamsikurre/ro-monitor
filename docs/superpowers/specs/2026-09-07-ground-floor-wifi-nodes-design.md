# Ground-floor Wi-Fi nodes — design

**Date:** 2026-09-07
**Status:** approved, awaiting implementation plan
**Supersedes:** `RS485_PROTOCOL.md` §5 (nodes push to the hub). Nodes are now polled.

## 1. What this adds

Two ESP32 nodes on the ground floor, reached over the house LAN, polled by the
terrace hub, and shown on the dashboard and in RainMaker:

| Node | Where | Reads |
| :--- | :--- | :--- |
| `0x05` Sump | Sump manhole | AJ-SR04M ultrasonic, **or** a 4-20 mA submersible pressure transducer on the same `J-LOOP` / `J-PRESS` provision the tank nodes carry (`WIRING.md` §9.4) |
| `0x06` Utility | Motor starter panel | 3 × CT on the borewell (one per phase), 3 × CT channels on the sump motor (two clamps fitted now, third channel wired and read, clamp added later if needed), sump motor **PUMP ON** dry contact, RWT floaty, SHT30 temperature and humidity |

No pump control. No relay board. The 4-channel relay interlocks in
`WIRING.md` §11.2 are out of scope for this round and stay a Phase-2 option.

## 2. What the panels offer (from `images/groundfloor/`)

**Borewell** — Lauritz Knudsen MK1 direct-on-line starter with an NEC-49
three-phase level controller in series with the coil. The starter has no spare
auxiliary contact; the NEC-49 has no output. Its "Lower Sump Float" input
carries the sump float and its "Over Head Tank Float" input is shorted, so in
AUTO the borewell runs whenever the sump float allows. Both float loops are at
mains potential inside the NEC-49. **Nothing of ours touches this panel except
CT clamps on the motor tails 2/T1 4/T2 6/T3.**

**Sump motor** — Astero Submersible 2.0 HP controller. Terminal strip inside:
`PULSE O/P`, `TWT FLOTY (C, NC)`, `PUMP CONT. (C, NC)`, `RUNNING RELAY O/P (NO, C)`,
`STARTING RELAY O/P (NO, C)`, `PUMP ON (C, NO)`.

- `PUMP ON (C, NO)` is unused and is an isolated dry contact closed while the
  controller has the pump on. **This is the sump-motor run signal**, wired like
  the hub's `IN_ALARM`: two wires to a GPIO with a pull-up.
- `TWT FLOTY` is where the RWT float cable lands. Whether the node can read it
  depends on the voltage the Astero puts across the loop, **to be metered
  before any wire is connected**: low-voltage DC → an optocoupler across the
  loop; mains → not tapped, and `rwt_floty` reports `null`.
- `PUMP CONT.` has one wire to nowhere — an abandoned external-contactor
  option. Tape the loose end, leave the pair alone.
- Do not land anything on a pair that already has wires until the other end is
  known.

## 3. Topology

Hub pulls. A dedicated task on the hub (`gf_task`) polls each configured node
with `HTTP GET http://<ip>/api/telemetry`, 2 s timeout, and writes the result
into `hub_state_t` under the existing lock. It is its own task so a silent node
cannot stall the 2 s RS485 cycle.

| | |
| :--- | :--- |
| Poll period, node online | 5 s |
| Offline after | 3 consecutive misses (≈ 15 s) |
| Poll period, node offline | 30 s — keeps probing, cheaply |
| Back online | first good reply; cadence returns to 5 s |
| IP field empty | node "not fitted": no polling, no alert, cards hatched |

Drops and returns are logged as `EVT_NODE_OFF` / `EVT_NODE_ON` with the node
id, and feed the existing node-lost alert. Nodes are stateless and know nothing
about the hub.

## 4. Node JSON

Nodes report **raw readings only**. Every calibration constant lives on the hub
in NVS and is set from `/cal`, so reflashing a node never loses a number.

### 4.1 Sump `0x05`

```json
{"id":5,"fw":"a3cb57d","uptime_s":3840,"rssi":-64,
 "source":"ultrasonic","distance_mm":1750,"quality":95,"status":"OK",
 "loop_ua":null}
```

```json
{"id":5,"fw":"a3cb57d","uptime_s":3840,"rssi":-64,
 "source":"pressure","distance_mm":null,"quality":null,"status":"OK",
 "loop_ua":11200}
```

- `source` follows the `J-PRESS` shunt: fitted = `pressure`, off =
  `ultrasonic`. Read once at boot, like `ro_node.ino`.
- `status` ∈ `OK | BLIND | NO_ECHO | HW_FAULT`, the same `sensor_status_t` the
  RS485 tank nodes use. On the pressure source `HW_FAULT` means the loop is
  outside 3.5–21 mA: open, unpowered, shorted or miswired (§9.4.4 of
  `WIRING.md`).
- `loop_ua` is the raw loop current. The **hub** converts it, so the
  transducer's range is a `/cal` number and not a node build constant:
  `head_mm = (loop_ua − 4000) × range_mm / 16000`, then the same
  distance-alike convention as the tank nodes, `distance = range − head`,
  so the existing full/empty calibration applies unchanged (§9.4.1).
- Percent from a new `CAL_TANK_SUMP` entry: full mm, empty mm, and
  `press_range_mm` (0 = ultrasonic only).

### 4.2 Utility `0x06`

```json
{"id":6,"fw":"a3cb57d","uptime_s":3840,"rssi":-68,
 "bore_mv":[412,405,398],
 "sump_mv":[398,0,0],"sump_on":false,
 "rwt_floty":null,
 "t_deci_c":312,"rh_deci_pct":548,"sht_ok":true}
```

- `bore_mv` / `sump_mv`: per-channel AC RMS millivolts as the node measured
  them, order = clamp order on the node header. Three channels each. A channel
  with no clamp plugged in sits at the bias pedestal and reads a few mV of
  noise; the hub treats anything under `CT_NOISE_FLOOR_MV` as "no clamp" and
  reports that phase as `null`, never as 0 A — the same rule the hub applies
  to its own two channels.

  > **Correction, 2026-09-07 (annotation — the paragraph above is left as
  > written).** "sits at the bias pedestal" is wrong, and `WIRING.md` §11.3
  > copied it. The bias rail reaches an ADC pin only *through the plugged
  > clamp's winding* (bias → socket ring → coil → tip → 1 k → pin), so an
  > **empty socket reads ~0 V at the pin**, not the pedestal. Measured on the
  > hub's identical topology and recorded in `WIRING.md` §14.2 step 2, where
  > it cost an hour on 2026-09-06. The conclusion the paragraph draws is
  > unaffected — below `CT_NOISE_FLOOR_MV` still means "no clamp" and still
  > reports `null`, never 0 A — only the reason given for it. The pedestal
  > check that *does* matter is on a channel that is plugged in: a floating
  > pin swings on mains coupling and RMSes like a running motor, which is why
  > `sensors_util.c`'s `rms_mv()` reports 0 mV rather than a number.

- `sump_on`: the `PUMP ON` contact.
- `rwt_floty`: `true` closed, `false` open, `null` not wired.
- SHT30 at I²C `0x44`, the part the hub and battery node already use.

### 4.3 Node GPIO

| Utility `0x06` | GPIO | Sump `0x05` | GPIO |
| :--- | :---: | :--- | :---: |
| `BORE_CT_L1/L2/L3` | 32 / 33 / 34 (ADC1) | `TRIG` | 5 |
| `SUMP_CT_L1/L2/L3` | 35 / 36 / 39 (ADC1) | `ECHO` (via 1 k / 2 k divider) | 18 |
| `PUMP_ON` (pull-up) | 25 | `J-LOOP` pin 2, loop sense (ADC1) | 34 |
| `RWT_FLOTY` opto (pull-up) | 26 | `J-PRESS` shunt to GND (pull-up) | 25 |
| SHT30 `SDA` / `SCL` | 21 / 22 | | |

All six ADC1 channels on the utility node are taken: six bias networks and
six clamp sockets are built, five clamps are fitted on day one, the sixth
socket (sump L3) waits for a clamp. CT bias network per channel as
`WIRING.md` §11.3 / §14.1.

**Sump node loop provision**, copied from `WIRING.md` §9.4.2 with the ADC
reference changed: `J-LOOP` 1×3 (`12V` / sense / `GND`), 100 R 1 % sense
resistor across pins 2–3, 1 k + 100 nF from the sense node to GPIO 34.
4 mA = 0.40 V, 20 mA = 2.00 V, read at 11 dB attenuation with the eFuse ADC
calibration, well inside the ESP32's linear band. `J-PRESS` 1×2 shorts
GPIO 25 to GND. Both headers are soldered whether or not a transducer is
ever bought.

**The sump node is therefore powered 230 V → 12 V module → buck → 5 V**, not
the 5 V-only HLK-20M5 in the old §11.1: a two-wire loop transducer needs the
12 V, and it is the same rail arrangement the tank nodes use. `J-LOOP` pin 1
takes the 12 V; the ESP32 and the AJ-SR04M take the buck's 5 V.

## 5. Hub state and telemetry

Additions to `hub_state_t`:

```c
tank_state_t    sump;
motor_state_t   borewell, sump_motor;   /* + int16_t phase_da[3]; uint8_t imbalance_pct */
climate_state_t utility_room;
bool            rwt_float_closed;  bool rwt_float_known;
bool            sump_online, utility_online;
int64_t         sump_last_us, utility_last_us;
char            sump_fw[16], utility_fw[16];
```

- Borewell `running` = highest phase ≥ `run_deci_amps` (cal). `deci_amps` =
  highest phase. `imbalance_pct` = (max − min) / max × 100 over the phases.
- Sump motor `running` = `sump_on`. Amps and imbalance from whichever of its
  three channels have a clamp; imbalance needs at least two.
- One clamp calibration per motor (`CAL_CT_BORE`, `CAL_CT_SUMP`) applied to all
  its channels: amps per volt, turns, run threshold, over-current threshold.

`/api/telemetry` fills the keys the dashboard already reads —
`tanks.sump`, `pumps.borewell`, `pumps.sump_motor`, `aster.rwt_floty`,
`nodes[0x05]`, `nodes[0x06]` — and adds:

```text
motors.borewell   {amps, phases:[..], imbalance_pct, running}
motors.sump_motor {amps, phases:[..], running}
env.utility_room  {t, rh, state, src:"SHT30 . Node 0x06", age_s}
nodes[]           + "ip", "fw"
```

`hist_rec_t` gains `int8_t sump; int16_t bore_da, sump_da, util_t;`.

## 6. Calibration page

- **New fieldset "Ground floor nodes"**: Sump node IP, Utility node IP. Text,
  dotted-quad, empty = not fitted. NVS keys `gf_sump_ip`, `gf_util_ip`.
  `POST /api/cal/gf`.
- **Tank levels** gains a Sump row: full mm, empty mm, and transducer range
  mm (0 when the ultrasonic is the source). The page shows which source the
  node reported so a wrong shunt is visible from the roof, not the manhole.
- **Current clamps** gains Borewell and Sump motor rows: amps/V ×100, turns,
  run threshold dA, over-current dA.

## 7. Cloud

RainMaker parameters, deadbanded like the rest: `Sump Level` (3 %),
`Borewell` (bool), `Sump Motor` (bool), `Utility Room Temperature` (0.3 °C),
`RWT Float` (bool). Node-lost alert extends to `0x05` / `0x06`. No new alerts
this round; sump-low, borewell dry-run and imbalance alarms follow once the
numbers have been watched.

## 8. Dashboard

- Sump tank, sump motor and borewell cards un-hatch by themselves once the
  keys carry live values.
- Environment row gains **Utility room** (t, RH, age, offline hatch) beside
  RO room and Battery room.
- Borewell card shows per-phase amps and imbalance.
- Node list shows IP and fw for the two Wi-Fi nodes.

## 9. Node firmware — `firmware/gf_node`

One ESP-IDF project, role selected in menuconfig
(`CONFIG_GF_ROLE_SUMP` / `CONFIG_GF_ROLE_UTILITY`).

- **Wi-Fi station, static IP.** SSID, password, IP, gateway, netmask from
  menuconfig. The IP is what gets typed on `/cal`.
- **HTTP server.** `GET /api/telemetry` (§4), `GET /` one-line status text,
  `POST /ota`.
- **OTA.** Hub pattern: `ota_0` / `ota_1` slots, `otadata`, rollback enabled.
  `POST /ota` streams the raw `.bin` body into the passive slot via
  `esp_ota_begin/write/end`, sets the boot partition and reboots. The new image
  calls `esp_ota_mark_app_valid_cancel_rollback()` only after Wi-Fi has an IP
  **and** it has served one `/api/telemetry`; otherwise the bootloader rolls
  back on the next reset. Push from any LAN PC:

  ```sh
  curl --data-binary @build/gf_node.bin http://192.168.1.x/ota
  ```

- **Version** = `git describe --always --tags --dirty` into `PROJECT_VER`, as
  the hub does. Reported in `fw`.
- **Sensors.** Sump: `J-PRESS` read at boot selects the source. Ultrasonic:
  AJ-SR04M trigger/echo with the same blind-zone and quality logic as
  `ro_node.ino`. Pressure: 8-sample ADC average per cycle on GPIO 34,
  reported as microamps, with the 3.5–21 mA sanity band from `ro_node.ino`. Utility: ADC1 RMS over one mains cycle per
  channel, SHT30 over I²C, two digital inputs.

## 10. Testing

- `docs/fake_gf_node.py`: serves both JSON shapes on two ports with knobs for
  distance, mV, contacts and "go silent", so hub polling, offline latch,
  re-probe and cal are proven on the bench with no node hardware.
- `docs/check_telemetry.py` gains the new keys.
- Both node roles build clean; `idf.py size` recorded in the node README.
- Bench OTA: push a build to a node, confirm rollback by pushing one with Wi-Fi
  credentials deliberately wrong.

## 11. Docs to update

- `RS485_PROTOCOL.md` §5: push → pull, final JSON, GPIO table.
- `WIRING.md` §11: three CTs borewell, three CT channels + `PUMP ON` sump
  motor (two clamps fitted), SHT30,
  no relay board, the Astero terminal-strip findings, the meter-first rule.
  §11.1: 12 V supply, `J-LOOP` / `J-PRESS` on the sump node, cross-reference
  §9.4 rather than repeating it.
- `DASHBOARD_AND_RAINMAKER.md`: new parameters and the utility room card.
- `firmware/hub_prod/README.md`: ground-floor fieldset.
- `firmware/gf_node/README.md`: build, menuconfig, flash, OTA.

## 12. Out of scope

Relay interlocks and any pump control; hub-proxied OTA; sump/borewell alerts
beyond node-lost; mDNS discovery of nodes (IPs are typed).
