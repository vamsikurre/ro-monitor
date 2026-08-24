# RO-Monitor — Phase B Design: RS485 Ring Completion

**Status:** Draft for review
**Date:** 2026-08-25
**Supersedes:** conflicting sections of `docs/HARDWARE.md`, `docs/RS485_PROTOCOL.md`, `docs/WIRING.md` (corrections listed in §9)

---

## 1. Why this project exists

The stated goal in `plan.txt` is RO plant telemetry. The actual driver, established during design review, is a pump failure:

- The **borewell pump** fills the ground **sump**.
- The **sump motor** lifts water from the sump to the rooftop **Raw Water Tank (RWT)**.
- The sump motor is started by the **RWT float**, wired directly to its starter coil.
- Nothing tells the sump motor about the sump's own level.

When the borewell runs dry and the sump empties while RWT is still below its setpoint, the RWT float keeps the sump motor energised and **the sump motor runs dry**.

The missing logic is:

```
run sump motor  <=>  (RWT below setpoint)  AND  (sump above dry-run level)
```

Telemetry is the instrumentation needed to manage this properly. The dry-run cutoff itself is a hardware change and does not depend on any firmware in this document.

### 1.1 Existing float arrangement (confirmed)

A single sump float already exists, **wired to the borewell starter**, sitting **mid-tank**. It is a *borewell top-up* setpoint.

It cannot be reused for dry-run cutoff, for two independent reasons:

1. **Wrong setpoint.** Mid-tank is a level the sump reaches routinely in healthy operation. Tapping it into the sump motor circuit would stop the sump motor every time the borewell decided to refill.
2. **Circuit coupling.** That contact already sits in the borewell starter's control circuit. Paralleling it into the sump motor circuit couples two control circuits that may not share a neutral — a back-feed path.

**Resolution:** a **new float, lower down, dedicated NC contact**, in series with the RWT float into the sump starter coil. The existing mid-tank float is left entirely untouched.

### 1.2 Target interlock chain

```
RWT float --+-- Sump DRY-RUN float (NC) --+-- ESP32 relay (NC) -- Sump starter coil
            |                             |
       (existing)            (new, Phase C)   (new, Phase C, optional)
```

Every added element is **normally closed** and can only ever *stop* the motor, never start it. De-energised equals today's behaviour. An unplugged ESP32 degrades the system to the current design plus a working dry-run float — never to an unprotected pump.

---

## 2. Scope and build order

Build order **B** was selected: complete the RS485 ring using hardware already in hand, then the ground floor.

| Phase | Content | State |
| :--- | :--- | :--- |
| **A** | Hub (0x00), Dosing (0x01), RWT (0x02) hardware | **Built.** Test sketches only |
| **B** | *This document.* Contract layer, real hub + node firmware, nodes 0x03 + 0x04, local dashboard | To build |
| **C** | Ground floor: dry-run float, node 0x05 (sump), node 0x06 (starter panel), interlock logic | Later |
| **D** | Cloud / push alerts | Later, optional |

### 2.1 Out-of-band recommendation

The **dry-run float (§1.1) requires no firmware and is independent of all Phase B work.** It is an afternoon of electrical work. Fitting it before or during Phase B means the pump is protected for the weeks Phase B takes. Deferring it to Phase C is the one part of build order B that carries avoidable risk.

---

## 3. Section 1 — The contract layer

### 3.1 Root cause of the documentation conflicts

Hub and node live in separate Arduino `.ino` folders, which cannot share a header. Protocol constants, GPIO numbers and the address table were copy-pasted between them and drifted. Three live contradictions resulted (§9). Fixing the three symptoms without fixing the structure guarantees a fourth.

### 3.2 Repository structure

```
firmware/
  platformio.ini          # envs: hub, tank_node, native (tests)
  common/
    protocol.h            # frame layout, CRC16, command codes, node addresses
    pinmap_hub.h          # the ONE ESP32 GPIO table
    pinmap_node.h         # the ONE Nano pin table
  hub/src/main.cpp
  node/src/main.cpp
  test/test_protocol.cpp  # runs on PC under the native env
  bringup/                # existing esp32_hub_test + nano_node_test, moved verbatim
```

`common/` is on the include path for every env. Hub and node compile against the *same* frame definition; a protocol change that breaks one side now fails the build instead of failing on the bus.

PlatformIO is chosen over the Arduino IDE specifically because the IDE cannot share a header across sketch folders. The existing test sketches move to `bringup/` unmodified — they remain the right tool for poking hardware.

### 3.3 Wire protocol — resolved

`docs/RS485_PROTOCOL.md` wins over the sketches' XOR scheme. XOR-8 misses burst errors, and contactors switching metres away produce burst errors. A bitwise CRC16 is ten lines.

| Byte | Field | Notes |
| :--- | :--- | :--- |
| 0 | `0xAA` | Preamble |
| 1 | `0x55` | Preamble |
| 2 | `NODE_ADDR` | Master `0x00`, slaves `0x01`–`0x04`, broadcast `0xFF` |
| 3 | `COMMAND` | Bit 7 clear = request, set = response |
| 4 | `PAYLOAD_LEN` | N, 0–32 |
| 5 … 4+N | `PAYLOAD` | |
| 5+N | `CRC16_L` | Modbus CRC-16, poly `0xA001`, init `0xFFFF` |
| 6+N | `CRC16_H` | |

CRC is computed over bytes 2 … 4+N (address through payload). Preamble excluded.

### 3.4 Commands

| Code | Name | Direction | Applies to |
| :--- | :--- | :--- | :--- |
| `0x01` | `CMD_PING` | Master → slave | All |
| `0x02` | `CMD_READ_LEVEL` | Master → slave | 0x01, 0x02, 0x03 |
| `0x06` | `CMD_READ_CLIMATE` | Master → slave | 0x04 |
| `0x07` | `CMD_SET_FAN_RELAY` | Master → slave | 0x04 |

**`CMD_READ_LEVEL` response payload — changed from `RS485_PROTOCOL.md` §4.2 (8 bytes):**

| Offset | Type | Field |
| :--- | :--- | :--- |
| 0 | `uint16` | `distance_mm` — median-filtered |
| 2 | `uint16` | `raw_distance_mm` — last instantaneous reading |
| 4 | `uint8` | `sensor_status` |
| 5 | `uint8` | `signal_quality` — 0–100, derived from median spread |
| 6 | `uint16` | `node_uptime_s` |

`level_percent` is **removed from the node payload.** See §4.3.

`sensor_status`: `0 = OK`, `1 = BLIND_ZONE`, `2 = ECHO_TIMEOUT`, `3 = OUT_OF_RANGE`, `4 = HW_FAULT`.

**`CMD_READ_CLIMATE` response payload (6 bytes)** — unchanged from `RS485_PROTOCOL.md` §4.3.

### 3.5 ESP32 hub GPIO map — resolved

`docs/WIRING.md` §1 wins. It matches the hub that is physically built. `docs/HARDWARE.md` §3.1 is wrong and gets corrected, not reconciled.

| GPIO | Signal | Device | Notes |
| :--- | :--- | :--- | :--- |
| 16 | `RS485_RX` | XY-485 `RXD` | UART2 |
| 17 | `RS485_TX` | XY-485 `TXD` | UART2 |
| 21 | `I2C_SDA` | GY-SHT30-D | |
| 22 | `I2C_SCL` | GY-SHT30-D | |
| 32 | `IN_TWT_FLOT` | PC817 ch1 | `INPUT_PULLUP` |
| 26 | `IN_RL1_STAT` | PC817 ch2 | `INPUT_PULLUP` |
| 25 | `IN_RL2_STAT` | PC817 ch3 | `INPUT_PULLUP` |
| 34 | `IN_HPP_AC` | AC opto 1 | **External 10 kΩ pull-up to 3V3 required** |
| 35 | `IN_RWP_AC` | AC opto 2 | **External 10 kΩ pull-up to 3V3 required** |
| 27 | `OUT_RLY_TWT` | Relay `IN1` | Active LOW. Phase C use |
| 23 | `OUT_RLY_RWT` | Relay `IN2` | Active LOW. Phase C use |
| 18 | `OUT_RLY_DOS` | Relay `IN3` | Active LOW. Phase C use |
| 19 | `OUT_RLY_AUX` | Relay `IN4` | Active LOW. Spare |
| 2 | `LED_STATUS` | Onboard LED | |
| 0 | `BTN_BOOT` | Onboard button | Config reset |

GPIO 34 and 35 are input-only pins with **no internal pull-up**. The current sketch's `pinMode(pin, INPUT)` against an open-collector opto output leaves them floating. This is a defect on already-built hardware and must be fixed physically.

### 3.6 Arduino Nano pin map

| Pin | Signal | Nodes |
| :--- | :--- | :--- |
| D2 | `RS485_RX` | All (SoftwareSerial) |
| D3 | `RS485_TX` | All (SoftwareSerial) |
| D7 | `US_TRIG` | 0x01, 0x02, 0x03 |
| D8 | `US_ECHO` | 0x01, 0x02, 0x03 |
| A4 | `I2C_SDA` | 0x04 only |
| A5 | `I2C_SCL` | 0x04 only |
| D9 | `OUT_FAN_RLY` | 0x04 only |
| A0 | `ADDR_SEL0` | All |
| A1 | `ADDR_SEL1` | All |
| D13 | `LED_STATUS` | All |

### 3.7 Node addressing — OPEN ITEM

`nano_node_test.ino` and `docs/WIRING.md` §8 publish **contradictory jumper truth tables**. Two nodes are already built and jumpered.

**Resolution method:** both built nodes print their detected ID at boot over USB serial. Read both. Whatever they report is the truth; the table in `common/protocol.h` is written from that observation and both docs are corrected to match.

Address space stays at 2 jumper bits / 4 addresses (`0x01`–`0x04`). A third bit for future expansion is **deliberately skipped** — see §8.

---

## 4. Section 2 — Node firmware

One binary for all four nodes. The address jumper selects both the node ID and the sensor personality (0x01–0x03 ultrasonic, 0x04 climate + fan relay).

### 4.1 The deaf-window defect

`pulseIn(PIN_US_ECHO, HIGH, 35000UL)` blocks for up to 35 ms. `SoftwareSerial` cannot buffer during that block. A 4-byte poll at 9600 baud occupies ~4 ms. With measurement on a free-running 250 ms timer, roughly **15 % of polls are silently lost**.

**Fix:** measure **immediately after transmitting a response**, never on a timer. The blocking window then falls while the master is addressing a *different* node, and the node is guaranteed listening whenever its own turn comes round.

**Cost:** the value returned is one poll cycle (~1 s) old. For water tanks this is immaterial and is documented as expected behaviour.

**Fallback:** if no poll is received for 10 s, resume free-running measurement at 1 Hz so the filter stays warm and the node remains independently useful (`plan.txt` §11).

Rejected alternative: moving RS485 to the Nano hardware UART (D0/D1). It works, but costs USB debugging on nodes that will live on a roof and in a battery room.

### 4.2 Filtering

- **Median of 5 samples**, taken back-to-back.
- **Range gate** against `min_valid_mm` (blind zone, ~250 mm) and `max_valid_mm`. Out-of-range samples are discarded before the median, not fed into it. A reading of 0 mm is `ECHO_TIMEOUT`, never a valid "full tank" — `plan.txt` §15.
- `signal_quality` derived from the spread of the 5 samples.

**Deliberately skipped:** the EMA smoothing and `MAX_LEVEL_CHANGE_PER_UPDATE` rate limiter from `plan.txt` §12. A median already suppresses the failure that actually occurs (a single spurious echo), and a rate limiter would mask a genuine rapid fill. An `EMA_ALPHA` config knob is left in place defaulting to disabled, so it can be switched on if a particular tank surface proves choppy.

### 4.3 Nodes report distance, not percentage

Nodes transmit **raw `distance_mm` plus status**. All tank geometry lives on the hub.

Rationale: recalibrating a rooftop tank becomes editing a web form rather than carrying a ladder and a laptop to the roof and reflashing a Nano. It also puts every calibration constant in one place, as required by `plan.txt` §25.

This is a deliberate departure from `RS485_PROTOCOL.md` §4.2.

### 4.4 Bus turnaround

The XY-485 is auto-direction, so there is no DE/RE pin to manage. The module still holds its driver enabled briefly after the last transmitted character. Slaves observe a **5 ms guard delay** before replying.

---

## 5. Section 3 — Hub firmware

### 5.1 Scheduling

Cooperative `millis()` scheduler. No explicit FreeRTOS tasks — four periodic jobs do not justify them.

| Job | Period |
| :--- | :--- |
| RS485 poll cycle | 1000 ms |
| Opto input sampling | 50 ms |
| SHT30 read | 5000 ms |
| WiFi supervision | 10 000 ms |
| `WebServer::handleClient()` | every loop |

`esp_task_wdt` is fed from `loop()`. No `delay()` anywhere in the operational path (`plan.txt` §16).

### 5.2 RS485 master

Non-blocking `SEND → WAIT → PARSE | TIMEOUT` state machine, one node per tick, strictly sequential — no two slaves may transmit at once (`plan.txt` §24).

- Response timeout **100 ms**, up to **3 attempts** per node per cycle.
- The cycle runs nodes back-to-back and then waits until 1000 ms since cycle start. If a cycle overruns (four dead nodes ≈ 1.2 s), the next begins immediately. Cycle time is a floor, not a guarantee.

**Node liveness is time-based, not counter-based:**

| State | Condition |
| :--- | :--- |
| `ONLINE` | Good reading within 3 s |
| `STALE` | Last good reading 3–10 s ago |
| `OFFLINE` | No good reading for > 10 s |
| `SENSOR_ERROR` | Responding, but `sensor_status` is not OK |

`OFFLINE` is never rendered as 0 % (`plan.txt` §15).

### 5.3 Framer

Byte-at-a-time sync on `0xAA 0x55`. The current `available() >= 7` followed by seven blind reads desynchronises permanently on a single stray byte. Resynchronising costs one frame instead of wedging the cycle.

### 5.4 AC input sampling

A 240 V / 50 Hz signal through an opto is a ~100 Hz pulse train; the output is LOW for only part of each 10 ms half-cycle.

The current `readACInputFiltered` samples 10 × 500 µs = **5 ms** and requires 8 of 10 LOW. It will report OFF while the pump is running.

**Fix:** sample across a **25 ms window** (more than one full mains cycle) and latch ON if *any* sample is active. Combined with the external pull-ups from §3.5.

### 5.5 Web server and concurrency

Plain synchronous `WebServer`, not `ESPAsyncWebServer`. Handlers execute inside `loop()`, so they cannot race the poll cycle for `SystemState`. This eliminates the entire concurrency class at zero cost.

**Deliberately skipped:** SSE and WebSockets from `plan.txt` §18. `dashboard.html` already polls at 1 s, which is ample for water.

Endpoints:

| Endpoint | Purpose |
| :--- | :--- |
| `GET /api/telemetry` | Complete `SystemState` as JSON |
| `GET /api/health` | Uptime, RSSI, reset reason, firmware version |
| `GET /api/config` | Current configuration |
| `POST /api/config` | Update tank geometry, thresholds, WiFi |

`/api/status`, `/api/tanks`, `/api/environment`, `/api/inputs` from `plan.txt` §18 collapse into `/api/telemetry`. Split them when a consumer needs a partial fetch.

### 5.6 Tank geometry and configuration

Per-tank, hub-side, stored in NVS via `Preferences`:

```
tank_id, tank_name
empty_distance_mm     // sensor reading with the tank empty
full_distance_mm      // sensor reading with the tank full
min_valid_mm          // blind zone
max_valid_mm
```

```
level_percent = clamp(100 * (empty_distance_mm - distance_mm)
                          / (empty_distance_mm - full_distance_mm), 0, 100)
```

`empty_distance_mm` and `full_distance_mm` are directly measurable with a tape and absorb sensor mounting offset, so no separate offset term is needed.

### 5.7 Logging

Structured single-line levels per `plan.txt` §26. State *transitions* are logged; steady state is not.

---

## 6. Section 4 — Dashboard and cloud

`firmware/esp32_hub_test/dashboard.html` (629 lines) is sound and already polls `/api/telemetry` at 1 s. Three changes, not a rewrite:

1. **Remove the Google Fonts CDN link.** It is dead on a LAN-only hub, which defeats the point of local hosting (`plan.txt` §19). Replace with a system font stack.
2. **Serve gzipped from LittleFS**, so UI edits do not require a firmware reflash.
3. **Render `OFFLINE` / `STALE` / `SENSOR_ERROR` as distinct text labels**, not a differently-coloured 0 % bar. Status must not depend on colour alone (`plan.txt` §17).

### 6.1 Cloud — deferred to Phase D

Any cloud integration is a strict **consumer** of `SystemState`, added last, behind a compile flag. The hub must build and run with it disabled (`plan.txt` §19).

ESP RainMaker as specified in `DASHBOARD_AND_RAINMAKER.md` §3 brings AWS IoT provisioning, node claiming and an app pairing flow. If the requirement is only "notify my phone when the sump is low", an HTTPS POST to ntfy.sh or a Telegram bot is roughly fifteen lines with no provisioning at all. **Decision deferred to Phase D** — noted so the choice is made deliberately rather than by inheritance from the existing document.

---

## 7. Section 5 — Ground floor and interlock (Phase C outline)

Specified here for continuity; detailed design belongs to its own document.

- **Hardware layer.** New low float → NC contact → in series with the RWT float → sump starter coil. No firmware, no network. The layer that holds when everything else is off.
- **Node 0x05 (sump ultrasonic).** **Bench-test the JSN-SR04T down the actual manhole before building any enclosure.** 3.5 m against a 4.5 m specification, in a narrow shaft that will produce wall echoes. This is the single most likely component in the project to simply not work. Fallback: submersible pressure transducer (~₹1500).
- **Node 0x06 (starter panel).** 2 × AC opto sensing sump and borewell motor state, plus the ESP32 relay in series as a second cutoff.
- **Dry-borewell detection.** Borewell energised for N minutes with sump level not rising means the borewell itself is dry. Alert and optionally stop it. This is the upstream condition that begins the whole failure chain, and it only becomes observable once node 0x05 exists.
- **Fail-safe direction.** ESP32 relay de-energised = closed = current behaviour. WiFi loss, hub crash or a pulled plug degrades to the hardware float.

---

## 8. Deliberate simplifications

| Skipped | Rationale | Add when |
| :--- | :--- | :--- |
| EMA smoothing on top of median | Median handles the real failure (single spurious echo) | A tank surface proves choppy — knob already present |
| `MAX_LEVEL_CHANGE_PER_UPDATE` | Would mask a genuine rapid fill | Most likely never |
| Third address jumper bit | Design is four nodes; a fifth is hypothetical | A fifth RS485 node actually exists |
| SSE / WebSocket telemetry | 1 s polling is ample for water levels | 1 s latency becomes visibly insufficient |
| Split `/api/*` endpoints | One JSON document serves every current consumer | A consumer needs a partial fetch |
| `ESPAsyncWebServer` | Synchronous handlers in `loop()` remove all races | Concurrent request throughput matters |
| Mocked RS485 bus / integration harness | Pure-function tests plus real hardware cover it | The protocol starts changing often |
| Explicit FreeRTOS tasks | Four periodic jobs do not need them | A job appears that must not be starved |

---

## 9. Corrections to existing documents

| Document | Correction |
| :--- | :--- |
| `docs/HARDWARE.md` §3.1 | GPIO table is wrong. GPIO 27 is listed as `IN_RL2_STAT` but is physically a relay output. Also lists GPIO 33 `IN_RWT_FLOT` and an 8-channel PC817 board that are not present. Replace with §3.5 of this document. |
| `docs/RS485_PROTOCOL.md` §4.2 | `level_percent` removed from node payload; geometry moves to the hub (§4.3). Payload shrinks 10 to 8 bytes. |
| `docs/WIRING.md` §8 | Address jumper truth table contradicts `nano_node_test.ino`. Both to be corrected from observed hardware (§3.7). |
| `docs/POWER_BUDGET.md` §5 | Computes a 50 m worst-case run. Actual longest electrical path is Cat5e2 out + return (10 m) + Cat5e3 (10 m) + Cat5e4, roughly 25–30 m to node 0x04. Drop is smaller than stated; conclusion is unchanged. |
| `firmware/esp32_hub_test/esp32_hub_test.ino` | Comment claims GPIO 34/35 have a hardware pull-up. They do not. |
| `README.md` / `architecture.png` | Present relay float-override as current scope. It is Phase C provision only; Phase B is monitoring. |

---

## 10. Open items

None of these block the start of implementation.

1. **Node addresses.** Read the boot output of both built nodes and write `common/protocol.h` from what they report (§3.7).
2. **Tank dimensions.** `empty_distance_mm` / `full_distance_mm` per tank, measured on site. Configuration, not code — the system runs with placeholder values.
3. **Aster `C` terminal reference.** `RO_HARDWARE_ANALYSIS.md` §5.3 flags this as unverified. Only matters for Phase C relay emulation; monitoring via PC817 is unaffected.
4. **Ground floor photographs.** `images/` has 19 photographs of the RO skid and **none of the ground floor starter panel**. Required before Phase C wiring can be designed.

---

## 11. Testing

CRC16, frame encode/decode, the median filter and the distance-to-percent conversion are pure functions. They are covered by an assert-based `test_protocol.cpp` under the PlatformIO `native` env, running on a PC with no hardware attached.

That single test is the thing that fails if hub and node ever disagree about the wire format again — the exact class of defect that produced the three contradictions in §9.

Everything electrical is verified against real hardware using the `bringup/` sketches.
