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

> **Superseded 2026-08-26 by the shipped firmware.** `ro_node` and `esp32_hub_test` compute the CRC over **every byte before it, preamble included** (`RS485_PROTOCOL.md` §2). Both ends agree and `docs/check_frame.py` compiles the two implementations together and fails if they ever diverge — but a third implementation written from this line would not interoperate, so the line is corrected rather than left to be discovered on a bus. Including the preamble costs two bytes of CRC input and removes an offset that has to be right in two places.

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

> **Superseded 2026-08-26 by the shipped firmware.** This section's instinct was right — the node must not compute a level — but the implementation kept `RS485_PROTOCOL.md` §4.2's **10-byte layout** rather than shrinking to 8. The node sends `level_percent = 255` always, and the hub scales millimetres against its own per-tank calibration (§4.3's conclusion, reached the same way for the same reason).
>
> Keeping the wider layout was deliberate, and two bytes is a cheap price:
> - The frame layout stays fixed, so nodes and hub of different vintages stay mutually parseable — a value other than `255` in that byte identifies old firmware instead of silently shifting every subsequent field.
> - The `reserved` byte survives, and it is where the sensor-type flag goes if a tank ever moves to a 4-20 mA pressure transducer (§7.1). Shrinking the payload would delete the slot that migration path depends on.
>
> `sensor_status` as shipped uses `0 = OK`, `1 = BLIND_ZONE`, `2 = ECHO_TIMEOUT`, `3 = HW_FAULT` (no distinct `OUT_OF_RANGE` — a reading past the calibrated empty distance is a hub-side judgement now, not a node-side one).

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
| 33 | `IN_ALARM` | PC817 ch4 | `INPUT_PULLUP`. Added 2026-08-25 — Aster `AUX OP` contact. **Confirmed 2026-08-26** as a genuine fault flag: normally open, closes on a plant issue (`WIRING.md` §6.3) |
| 5 | `US_TRIG_DOS` | AJ-SR04M (dosing) | Added 2026-08-25. Dosing node `0x01` deleted, sensor wired direct — `WIRING.md` §13 |
| 4 | `US_ECHO_DOS` | AJ-SR04M (dosing) | **1 kΩ/2 kΩ divider, 5V→3.3V.** Digital use only — `GPIO 4` is `ADC2` and ADC2 is dead while Wi-Fi is up |
| 34 | `IN_HPP_AC` | AC opto 1 | **External 10 kΩ pull-up to 3V3 required** |
| 35 | `IN_RWP_AC` | AC opto 2 | **External 10 kΩ pull-up to 3V3 required** |
| 27 | `OUT_RLY_TWT` | Relay `IN1` | Active LOW. Phase C use |
| 23 | `OUT_RLY_RWT` | Relay `IN2` | Active LOW. Phase C use |
| 18 | `OUT_RLY_DOS` | Relay `IN3` | Active LOW. Phase C use |
| 19 | `OUT_RLY_AUX` | Relay `IN4` | Active LOW. Spare |
| 2 | `LED_STATUS` | Onboard LED | |
| 0 | `BTN_BOOT` | Onboard button | Config reset |

GPIO 34 and 35 are input-only pins with **no internal pull-up**. The current sketch's `pinMode(pin, INPUT)` against an open-collector opto output leaves them floating. This is a defect on already-built hardware and must be fixed physically.

### 3.6 Node pin map (Nano 0x01-0x03, Pro Mini 0x04)

> **Amended 2026-08-25 (a):** node `0x04` is built on an Arduino Pro Mini (5V / 16 MHz), not a Nano. Same ATmega328P, same pin functions, same binary. The board differences are mechanical only (5V to `VCC` not `RAW`, `A4`/`A5` on inner pads, FTDI programming) and are specified in `WIRING.md` 10.1.
>
> **Amended 2026-08-25 (b):** node `0x01` (dosing) is **deleted**. The tank is ~1 m from the hub, so the sensor is wired direct to the ESP32 (`WIRING.md` §13). The fleet is three slaves: `0x02` and `0x03` ultrasonic Nanos, `0x04` climate Pro Mini. If either already-built node was jumpered as `0x01`, re-jumper it to `0x02` or `0x03`. The dashboard contract is unchanged — `tanks.dosing` keeps its shape and is sourced locally instead of over the bus; `nodes[]` drops to three entries.

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

### 3.7 Node addressing — RESOLVED 2026-08-25

`nano_node_test.ino` and `docs/WIRING.md` published **contradictory jumper truth tables** (`0x01`/`0x02` swapped, both-grounded mislabelled `0x03`, `0x04` missing).

**Resolved by writing the table from the observed hardware — no rewiring.** The three built boards presented three *distinct* jumper codes, so there was never an ambiguity to resolve with a soldering iron, only a lookup table to author. `common/protocol.h` carries it and `WIRING.md` §9.1 publishes it:

```c
static const uint8_t ADDR_MAP[4] = {  // raw = (A1 << 1) | A0, open = 1, GND = 0
  0x00,   // 0b00  both GND   -> unassigned, must not transmit
  0x03,   // 0b01  A1 to GND  -> TWT        (Nano #2, as built)
  0x02,   // 0b10  A0 to GND  -> RWT        (Nano #1, as built)
  0x04,   // 0b11  both open  -> Battery Rm (Pro Mini, as built)
};
```

Consequences recorded in `WIRING.md` §9.2: the arithmetic `return raw` decoder in `nano_node_test.ino:39-44` is superseded; the unjumpered default becomes `0x04`, so **a new board must be jumpered before it joins the bus**; both-grounded is now an explicit "do not transmit" code, usable to bench a spare while it sits on the bus. Confirm each node by boot print before bussing.

Address space stays at 2 jumper bits / 4 addresses (`0x01`–`0x04`). A third bit for future expansion is **deliberately skipped** — see §8.

---

## 4. Section 2 — Node firmware

One binary for all three nodes. The address jumper selects both the node ID and the sensor personality (`0x02`/`0x03` ultrasonic, `0x04` climate + fan relay). `0x01` remains a legal jumper setting but is unassigned — see §3.6 amendment (b).

### 4.1 The deaf-window defect

> **Amended 2026-08-25:** the sensors are **AJ-SR04M**, not JSN-SR04T. Same Trig/Echo interface and the same 35 ms timeout, but the `R19` mode pad must be empty or the board runs in UART mode and `ECHO` never pulses, and the `TRIG` width (10 µs nominal) is a per-batch tuning knob. See `WIRING.md` §9.0.

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

Replaced rather than patched. The previous `firmware/esp32_hub_test/dashboard.html` was a card grid — it listed readings *about* the plant. The `dashboard.png` sketch is a **process flow**, and that is the correct organising principle: the page should be the plant.

New file: **`firmware/hub/data/dashboard.html`** — 29 KB raw, **9.1 KB gzipped**, served from LittleFS.

**Structure — a true elevation, not a flat flow.** The plant is drawn at real building height, because the lift is the failure mode:

| Band | Contents |
| :--- | :--- |
| Roof of RO room | Solar array, RWT, TWT |
| Terrace | RO room (the treatment train), Battery Room beside it |
| Ground floor / parking | Sump, Sump Motor |
| Below grade | Borewell |

**Two tilted PV rows sit above the roof**, one over each storage tank, low at the left and high at the right with a small gap between the rows, on steel posts landing on the roof slab. The panel stretches horizontally only, so the tilt geometry, post heights and the inter-row gap hold at any window width. The array is labelled once in the level column, not captioned per row.

**Drawn as an architectural section.** The RO room roof spans only the rooms, so the terrace planters stand in the open rather than under a slab; the terrace floor and grade run the full width. Each level carries a hatched structural slab at its floor, the RO room and battery room sit as built volumes on the terrace slab, and earth hatching marks everything below grade. Level labels use a diamond elevation marker. Occupied space reads as a building; the leftover space is furnished with dimmed wireframe scenery — a large and a small potted plant on the terrace, and a pickup, a boxy SUV and a hatchback on the ground floor, which is parking. Scenery is non-interactive and drawn well below the plant in contrast so it never competes with live data. The riser from the sump motor to RWT crosses two full bands, so the head the sump motor works against is visible rather than implied — that is the pump this project exists to protect.

**The treatment train follows the Aster mimic panel.** A photograph of the controller's own process display fixes the order:

```
RWT → RWP → FILTER → MF → [LPS] → HPP → [HPS] → RO membrane → [RSV/drain] → TWT
```

The dashboard draws RWP, FILTER, MF, HPP, the RO membrane (a horizontal pressure vessel), and TWT. FILTER, MF and the membrane are **passive equipment**: dimmer outline, no status chip, because the hub does not instrument them — the page must not imply knowledge it does not have. LPS, HPS and the RSV reject valve are on the Aster mimic but are not drawn; they are Aster-internal signals the hub does not currently tap.

FILTER is drawn as the large pressure vessel it is, against MF as a small cartridge housing.

Every item on the train sits in a fixed-height anchor box so horizontal runs share one centreline instead of staircasing between differently-sized vessels. Column widths are set to *item width + a constant*, which spaces the train evenly rather than leaving the wide membrane stranded. Pipes anchor to the box for horizontal runs (the centreline) but to the **glyph itself** for vertical runs — anchoring vertically to the box would leave risers floating well above and below the equipment.

**Room air** — temperature and humidity — reads in the bottom-right corner of each room: the hub SHT30 inside the RO room, node 0x04 inside the battery room. Bottom-right keeps it clear of the membrane-to-TWT riser.

**Dosing is a tee, not an inline vessel.** Process water runs RWP → FILTER → MF → HPP directly, and dosing injects **upstream of MF**. The dosing tank sits *below* that line and feeds it through a narrow injection line terminating in a junction marker on the main run, drawn thinner and with its own dash period. Water does not flow through the chemical drum. The tee sits immediately upstream of HPP, which is where antiscalant is actually injected.

**The sump motor is submersible**, so it is drawn *inside* the sump rather than beside it: a slim canned unit with a discharge riser and an intake screen, submerged in the tank fill. Its discharge leaves the sump sideways before rising, which keeps the riser in its own column clear of the terrace planters. Its status chip sits under the sump caption.

**Pumps and the fan do not share a glyph.** Surface pumps (RWP, borewell) draw a centrifugal impeller — three thin curved blades in a round casing. HPP is a **vertical** multistage set — motor on top, lantern, chamber stack, and a base carrying suction and discharge on one level — matching the unit on site and the Aster mimic symbol. Because a vertical pump ports at its base rather than its mid-height, it carries a connection-line marker that the router uses for horizontal runs, so the pipework drops to meet the base instead of crossing the chamber stack. The exhaust fan draws an axial fan — four broad blades, guard ring, square wall housing with mounting holes. Different equipment doing different jobs should not look identical.

Pipes are an SVG overlay routed orthogonally between measured element rects after layout and redrawn on resize, so the grid can reflow without pipework drifting off its vessels. Unit captions carry an opaque background to mask pipes running behind them, as on a real P&ID.

Below the plant, a four-card instrument strip: RO Room climate, Battery Room climate + exhaust fan, Aster isolated contacts, and the node table.

**Alerts float; they never resize the plant.** They are translucent with a blur and carry a hatched leading edge in the same language as the structural slabs, so they read as part of the drawing rather than as chrome sitting on it. They stack fixed at the **bottom right**, over the earth hatch, each with a dismiss control. In normal flow every appearing or clearing alert changed the available height and rescaled the whole drawing — that was the original defect. Top-of-screen floating then failed both ways: reserving space for them left a dead band above the solar array, and removing the reserve let them bury it. Bottom right needs no reserve and covers nothing. A dismissed alert stays dismissed only while its condition holds; once the condition clears the key is dropped, so a recurrence shows again.

**Encoding decisions.**

- **Fluid identity by colour.** Raw water `#3E8F5F`, treated water `#2B8FD4`. Both validated: ΔE 17.8 normal vision, 17.3 deuteranopia.
- **The additive by texture, not hue.** A third hue collided with blue under deuteranopia (ΔE 2.8), so the dosing tank uses a diagonal hatch. This is also truer — the plant has two fluids and one chemical additive.
- **Status colours reserved** and never reused for a fluid: warn `#B08F1A`, fault `#D93E68` (ΔE 22.4 normal, 10.0 deutan). **Healthy state gets no status hue** — a running pump is shown by its flowing pipe and a text label, not a green dot. Alarm-oriented colouring: colour means "look at this".
- **Every status carries a text label.** Colour is never the only channel (`plan.txt` §17).
- **`OFFLINE` draws a hatch and no liquid**, with the readout replaced by "NO DATA" — an offline tank is not an empty tank (`plan.txt` §15).

**The plant is built once and mutated in place.** Rebuilding its DOM on every poll restarted every CSS transition and dash animation, so levels and flow visibly jumped each second. A structure signature (layout mode plus each vessel/machine online-vs-dead state) decides when a rebuild is genuinely needed; otherwise the level transforms, readouts, chips and rotor colours are updated on the existing nodes. Pipes are redrawn only when the flow signature changes or on resize. Measured: one build and four pipe redraws across nine seconds of ticks, with the 0.9s level transition intact.

**Motion.** Two sine layers bob **vertically** in opposite phase across each liquid surface. They originally drifted horizontally, but the dominant layer read as a left-to-right sweep — and on the hatched dosing tank it slid the whole pattern like a progress bar. Vertical motion keeps the surface alive with no directional read. Amplitude was then raised and a lit crest line added on top of the fill, because the first pass was too subtle to notice — the closing edges of the crest path fall outside the vessel clip, so only the surface line shows. levels transition over 900 ms; pipes carry a dash animation only while their pump is energised; impellers rotate only while running. All of it is suppressed under `prefers-reduced-motion`.

**Constraints honoured.** No external requests — system font stacks, no CDN (`plan.txt` §19). Monospace tabular figures throughout, so digits do not jitter as values animate.

**The plant fits the viewport without scrolling.** The fit is height-bound, so a plant laid out at the wrapper width shrinks away from both edges and wastes them. Instead it is laid out *wider* by the same factor `availW / scale`, so the height fit lands it at exactly the full width — two passes, since widening can reflow a caption and change the height. Scaling is about the top left with a manual centring offset, used only when the min-width stops it filling. The wrapper height is set to the scaled height. The instrument cards sit below the fold deliberately — they are reference material, while the plant is the at-a-glance view. Because the scale is a CSS transform, pipe routing measures **layout coordinates** (walking `offsetLeft`/`offsetTop`) rather than `getBoundingClientRect`, so the transform never drags pipework off its vessels.

**Narrow screens linearise rather than scroll.** Below 900 px the plant re-lays out as a single column in process order — borewell at the top, TWT at the bottom, dosing on a side branch teeing into the vertical main run. Elevation moves off the band labels and onto each unit, which gains a caption naming where it physically sits (`Below ground`, `Ground floor · parking`, `Terrace · RO room`, `Roof of RO room`). Both layouts share one set of unit builders and one router; only the grid placement and the pipe entry/exit sides differ.

**Untrusted input.** Telemetry strings (`role`, `src`, `fw`, `reset_reason`, `sensor`) originate at RS485 and Wi-Fi nodes — a trust boundary. Plain-text nodes are built with `textContent`; authored markup escapes every interpolation through `esc()`. Nothing from the wire reaches `innerHTML` unescaped.

**Demo mode.** With no hub present the page runs a simulator modelling the exact failure this project exists to fix — the borewell goes dry, the sump drains, and the sump motor keeps pulling because RWT is still low. It switches to live silently on the first successful `/api/telemetry` response.

### 6.2 `/api/telemetry` contract

The dashboard's demo object is the authoritative shape the hub must serve. Top-level keys: `sys` (uptime_s, rssi, fw, reset_reason), `rs485` (online, total, errors, last_poll_ms), `tanks` (sump, rwt, dosing, twt — each pct, distance_mm, state, sensor), `pumps` (borewell, sump_motor, rwp, hpp — each on, state), `aster` (twt_floty, rwt_floty, sump_floty, dos_lvl, rl1, rl2, alarm), `env` (ro_room, battery_room — t, rh, state, src, age_s, and fan on battery_room), and `nodes[]` (id, role, link, state, age_s).

### 6.1 Cloud — deferred to Phase D

Any cloud integration is a strict **consumer** of `SystemState`, added last, behind a compile flag. The hub must build and run with it disabled (`plan.txt` §19).

ESP RainMaker as specified in `DASHBOARD_AND_RAINMAKER.md` §3 brings AWS IoT provisioning, node claiming and an app pairing flow. If the requirement is only "notify my phone when the sump is low", an HTTPS POST to ntfy.sh or a Telegram bot is roughly fifteen lines with no provisioning at all. **Decision deferred to Phase D** — noted so the choice is made deliberately rather than by inheritance from the existing document.

---

## 7. Section 5 — Ground floor and interlock (Phase C outline)

Specified here for continuity; detailed design belongs to its own document.

- **Hardware layer.** New low float → NC contact → in series with the RWT float → sump starter coil. No firmware, no network. The layer that holds when everything else is off.
- **Node 0x05 (sump level).** See §7.1 — sensor choice is unresolved and is the largest open procurement question in Phase C.
- **Node 0x06 (starter panel).** 2 × AC opto sensing sump and borewell motor state, plus the ESP32 relay in series as a second cutoff.
- **Dry-borewell detection.** Borewell energised for N minutes with sump level not rising means the borewell itself is dry. **See §7.2** — costed against a flow sensor and a current clamp, and staged: observe first, interlock only once the detection has earned trust.
- **Fail-safe direction.** ESP32 relay de-energised = closed = current behaviour. WiFi loss, hub crash or a pulled plug degrades to the hardware float.

### 7.1 Sump level sensor — unresolved

**This sensor buys better data, not better safety.** Dry-run protection is the float in series with the starter coil (§1.1); it functions regardless of what measures the sump, or whether anything does. Evaluate the sensor purely as "is accurate sump telemetry worth the money", not as pump protection.

**Step 1 — test what is already owned.** `docs/HARDWARE.md` §2 lists four AJ-SR04M units, one earmarked for the sump. **Lower it down the actual manhole and take readings at two or three different water levels before ordering anything.** One hour, no cost.

The concern is real but unproven: 3.5 m against a 4.5 m specification, in a narrow shaft that produces wall echoes, in an atmosphere that fogs transducer faces. This remains the single most likely component in the project to simply not work — but "likely to fail" is not "known to fail".

**Judge step 1 from the data.** The node reports `raw` beside the filtered `median` and an echo-quality figure (`WIRING.md` §9.3). Clean tracking is a smoothly moving median at quality 100. False echoes are a median stepping between two values with quality stuck below 100. Note which way the error runs: an object clipped by the beam answers *earlier* than the water, so a false echo reads **short, which presents as a fuller sump** — the direction that hides a problem rather than announcing one.

**Step 1b — before spending ₹6,071, fit a stilling well.** A ~100 mm PVC pipe hung vertically in the shaft, open at the bottom, vented at the top, sensor on the cap. The water inside follows the sump but stays still, and the pipe wall stops the beam ever seeing the ladder, the riser or the walls. A few hundred rupees, and it removes both failure mechanisms at once. **A bare AJ-SR04M failing in an open shaft is not evidence that ultrasonic measurement fails here** — it is evidence the shaft is a bad acoustic environment, which is what a stilling well is for. Only a sensor that misbehaves *inside a stilling well* justifies step 2.

**Amended 2026-08-26 — TWT may need this sensor before the sump does.** TWT's only penetration is a small wire pass-through in the roof slab, hard against the wall (`WIRING.md` §9.3). An ultrasonic sensor there sees the wall from 0.24 m down and reads permanently near-full, stably and without any quality warning. TWT cannot take a stilling well through that hole; the sump, with its open manhole, can. So the first transducer bought should probably go to TWT, and the sump should first be tried with the AJ-SR04M already owned inside a stilling well. That is the opposite of the order this section assumed.

**Step 2 — if it still fails, the fallback is a submersible 4-20 mA pressure transducer.**

Reference part evaluated: DFRobot `KIT0139` — 0-5 m, 4-20 mA, 0.5 % accuracy (25 mm over range), 316L stainless, IP68, 12-36 V, 5 m cable. **₹6,071 inc. GST.**

**Match the range to the tank.** Accuracy is % of full scale, so a 0-5 m unit on the 1.5 m TWT gives ±25 mm — ±1.7 % of the span actually used — and only ~245 counts of a Nano's 10-bit ADC across the whole tank. A 0-2 m or 0-3 m variant costs about the same and roughly triples the useful resolution. The 0-5 m figure above was quoted for the 3.5 m sump; it is the wrong part for TWT.

> Corrects an earlier estimate of ~₹1500 in this document, which was wrong by roughly 4×.

Three procurement constraints to verify **before** ordering:

1. **Cable length.** 5 m of cable against a 3.5 m sump leaves 1.5 m to reach the node enclosure. Measure the actual run first. This cable generally **cannot be spliced** — see (2).
2. **Vent tube.** 4-20 mA level transducers reference atmosphere through a vent running inside the cable. Splicing breaks it. The dry end must terminate somewhere genuinely dry **and genuinely vented** — sealing it in an airtight box is worse than damp, because the reference volume then tracks temperature: `dP = P x dT/T` makes a 20 C swing 6.8 kPa, about **0.7 m of apparent water**. A membrane breather vent in the enclosure wall plus a desiccant sachet is the fix; `WIRING.md` 9.3 carries the detail.
3. **Supply voltage.** 12-36 V. Node 0x05 is currently specced with an HLK-20M5 (5 V). Switching to this sensor means a 12 V supply plus a buck for the ESP32 — a BOM change, not a drop-in.

**Signal conditioning.** The DFRobot kit includes a current-to-voltage converter board. Feed it to an **ADS1115** (16-bit I2C ADC, ~₹250) rather than the ESP32's internal ADC, which is nonlinear enough to discard the 0.5 % accuracy being paid for. On an AVR node (if an RS485 tank ever switches) the internal ADC is adequate instead: 4-20 mA across 250 Ω is 1-5 V, which is ~819 counts of the Nano's 10-bit range over 5 m — about 6 mm.

**What the switch costs in firmware — deliberately small.** Since tank calibration moved to the hub, a node reports only millimetres, so a sensor swap is node-local:

1. **Node:** replace the `pulseIn` ranging and its median window with an averaged ADC read. Framing, addressing, poll scheduling and the personality switch are untouched. It gets *simpler* — the 35 ms `pulseIn` block that shaped the node's whole timing design disappears.
2. **Hub:** a pressure sensor reports **depth above the sensor**, the inverse of distance-down-to-surface, so full becomes the *larger* number. `levelPercent()` currently rejects `full > empty` as inverted calibration; allowing either direction is a two-line change and the same calibration page then serves both sensor types.
3. **Protocol:** the `reserved` byte in the `CMD_READ_LEVEL` payload is where a sensor-type flag goes when the hub needs to know which semantic it is receiving. The slot exists; nothing to change until then.

**Density, for any pressure sensor.** These read head pressure, so the reading scales with fluid density. Water is 1.0. Anything else (the dosing chemical, were this ever fitted there) needs a density factor — though calibrating by filling to a known level and using **set full = now** absorbs it without a separate constant.

**Trade-off summary.** Ultrasonic: ~₹500, already owned, may not work in this environment, no immersion fouling. Pressure transducer: ~₹6,300 all-in including ADS1115 and PSU change, 25 mm accuracy, immune to wall echoes and fogging, but sits permanently in the water and will need periodic diaphragm cleaning and eventual attention if the sump silts up.

### 7.2 Borewell dry-run detection — unresolved

**The failure being addressed.** The borewell's own aquifer runs dry while the starter stays energised, and the borewell motor runs dry. This is upstream of, and separate from, the sump-motor dry-run in §1.1 — that one is fixed by a float; this one has no float that can see it, because the water that has gone missing is underground.

Today nothing observes it. The first sign is a failed pump.

#### Options, costed

| Approach | Cost | Detects | Weakness |
| :--- | :--- | :--- | :--- |
| **Flow sensor** — YF-DN50, 2" Hall, 10-200 L/min | **₹1,828** inc. GST | Motor energised with no delivery, within seconds. Also litres/day, i.e. aquifer trend | Impeller and bearing in sandy borewell water — a wear item. Jammed reads as "dry" while water flows. Needs plumbing work |
| **Current transformer** — SCT-013 clamp on the motor feed | **~₹400** | Dry run (a dry pump draws markedly less), plus locked rotor and overload | No volume data. Threshold must be learned per pump. |
| **Sump level not rising** while the borewell contactor is energised | **₹0** | A dry borewell, over minutes | Needs node `0x05` to exist. Slow, and confounded by simultaneous draw from the sump |

**Protection and telemetry are different jobs.** The clamp cannot be jammed by sand and catches electrical faults the flow sensor cannot see; the flow sensor counts litres, which is the only one of the three that says anything about the aquifer itself. Choosing between them is choosing which job matters — buying both is defensible and still under ₹2,300.

#### Staged adoption — observe first, interlock later

Decided 2026-08-26, and it is the right shape for anything that can stop a pump:

**Stage 1 — non-intrusive monitoring.** Sensors installed, readings logged and alerted on, **nothing wired into any motor control circuit.** The system is a witness, not an actor. Run it long enough to see a real dry-borewell event and a full season of normal operation.

**Stage 2 — interlock, only once stage 1 has earned it.** An NC relay in the borewell starter's control circuit, in the same fail-safe direction as §1.2: de-energised equals today's behaviour, and the added element can only ever *stop* the motor, never start it.

**Graduation criteria — write these down before stage 1 starts, not after.** Promote to stage 2 only when the detection has run for a defined period with **zero false positives**, and every alert it did raise was independently corroborated. A dry-run detector that trips spuriously does not protect a pump; it teaches somebody to bypass it, and a bypassed interlock protects nothing.

**Sensor notes if the flow sensor is chosen.** Match the body to the delivery pipe — DN50 is 2" BSP, and reducers either side of a 1.5" line add turbulence and two failure points. Install downstream of the non-return valve, with unions, and support the pipe: the 1.75 MPa rating is nominal for a plastic body, and hammer on shutdown spikes well above static pressure. **The K-factor is unknown until measured** — clone sensors vary, so fill a known volume, count pulses, and store litres-per-pulse hub-side alongside the tank calibration, not in node firmware.

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
| `docs/HARDWARE.md` §3.1 | ~~GPIO table is wrong. GPIO 27 is listed as `IN_RL2_STAT` but is physically a relay output. Also lists GPIO 33 `IN_RWT_FLOT` and an 8-channel PC817 board that are not present. Replace with §3.5 of this document.~~ **Applied 2026-08-25.** GPIO 33 now carries `IN_ALARM` on PC817 ch4 (§3.5), and the BOM records the 4-channel board. |
| `docs/RO_HARDWARE_ANALYSIS.md` §4, §5.1 | ~~`LPS` recorded as `C`/`NC`.~~ **Corrected 2026-08-25** to `C`/`NO` from the board silkscreen and manual p.12. `HPS` reads open as normal and is unwired on this plant. `ALARM` is the configurable `AUX OP`, not a dedicated alarm relay. Polarity now specified once, in `WIRING.md` §6. |
| `docs/RS485_PROTOCOL.md` §4.2 | `level_percent` removed from node payload; geometry moves to the hub (§4.3). Payload shrinks 10 to 8 bytes. |
| `docs/WIRING.md` §9 | ~~Address jumper truth table contradicts `nano_node_test.ino`. Both to be corrected from observed hardware (§3.7).~~ **Applied 2026-08-25.** Firmware's decoder is authoritative; the table is now published once, in `WIRING.md` §9.1, with the as-built audit in §9.2. (Section renumbered §8 → §9 when the contact-polarity section was inserted.) |
| `docs/POWER_BUDGET.md` §5 | Computes a 50 m worst-case run. ~~Actual longest electrical path is Cat5e2 out + return (10 m) + Cat5e3 (10 m) + Cat5e4, roughly 25–30 m to node 0x04.~~ **Superseded 2026-08-25:** the chain was re-ordered to `0x00 -> 0x01 -> 0x04 -> 0x03 -> 0x02` with no return loopback, so the longest path now ends at node `0x02` and the per-hop lengths need re-measuring (`WIRING.md` §12). Drop is smaller than stated either way; conclusion is unchanged. |
| `firmware/esp32_hub_test/esp32_hub_test.ino` | Comment claims GPIO 34/35 have a hardware pull-up. They do not. |
| `README.md` / `architecture.png` | Present relay float-override as current scope. It is Phase C provision only; Phase B is monitoring. |

---

## 10. Open items

None of these block the start of implementation.

1. **Node addresses.** Read the boot output of both built nodes and write `common/protocol.h` from what they report (§3.7).
2. **Tank dimensions.** `empty_distance_mm` / `full_distance_mm` per tank, measured on site. Configuration, not code — the system runs with placeholder values.
3. **Aster `C` terminal reference.** `RO_HARDWARE_ANALYSIS.md` §5.3 flags this as unverified. Only matters for Phase C relay emulation; monitoring via PC817 is unaffected.
4. **Ground floor photographs.** `images/` has 19 photographs of the RO skid and **none of the ground floor starter panel**. Required before Phase C wiring can be designed.
5. **Borewell dry-run detection (§7.2).** Three options costed: ₹1,828 flow sensor, ~₹400 current clamp, or ₹0 by inference once node 0x05 exists. Stage 1 is monitoring only; nothing enters a motor control circuit until the detection has run without false positives. Phase C.
6. **Sump sensor choice (§7.1).** Settled by lowering the AJ-SR04M already owned down the actual manhole, and — if it misbehaves — by fitting a ~₹300 stilling well before concluding anything. Only a sensor that fails *inside* a stilling well justifies the ₹6,071 transducer. Decide before ordering, not after. Phase C only.

---

## 11. Testing

CRC16, frame encode/decode, the median filter and the distance-to-percent conversion are pure functions. They are covered by an assert-based `test_protocol.cpp` under the PlatformIO `native` env, running on a PC with no hardware attached.

That single test is the thing that fails if hub and node ever disagree about the wire format again — the exact class of defect that produced the three contradictions in §9.

Everything electrical is verified against real hardware using the `bringup/` sketches.
