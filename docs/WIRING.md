# Wiring & Interconnection Specifications

**Document Version:** 2.4  
**Date:** 2026-08-25  
**Scope:** Complete Pinout Mappings, CAT5e Drop Schedules, RS485 Daisy-Chain, 240V AC Isolation, 4-Channel Opto-Isolation (Inputs), Aster Terminal Contact Polarity & Alarm Output, 4-Channel Relay Module (Future Asterro Float/Level Emulation), I2C Environmental Sensor, and Tank Node Power & Wiring.

> **Pin allocation authority:** Section 1 below describes the hub **as physically built** and is the single source of truth, per `superpowers/specs/2026-08-25-ro-monitor-phase-b-design.md` §3.5. It equals that spec's table plus `IN_ALARM`, added 2026-08-25. `HARDWARE.md` Section 3.1 is kept identical to it and is corrected — not reconciled — whenever the two diverge.

---

## 1. ESP32-S Central Master Hub Complete Pin Allocation

The ESP32-S serves as the Central Telemetry Hub. It gathers telemetry from the local SHT30 environmental sensor **and the directly-attached dosing tank ultrasonic sensor**, isolates and reads the Aster controller status lines via a 4-channel PC817 optoisolator (all 4 channels used once `IN_ALARM` is fitted), controls a 4-channel relay module for future float/level emulation, and acts as the RS485 Modbus Master for three remote slaves.

### Master Pin Mapping Table

| ESP32 Pin | Signal Label | Connected Device / Module | Module Pin | Direction | Electrical Characteristics & Notes |
| :--- | :--- | :--- | :--- | :---: | :--- |
| **3V3** | `+3.3V_LOGIC` | SHT30 Sensor, XY-485 Module | `VCC` | Power Out | Regulated 3.3V logic power bus (from ESP32 onboard LDO) |
| **GND** | `GND_LOGIC` | All Local Sub-Modules & Opto/Relay | `GND` | Power Out | Common DC logic ground |
| **VIN / 5V** | `+5V_RAW` | LM2596 / Mini560 12V -> 5V Buck | `VOUT+` | Power In | Regulated +5.0V DC main board power (from HLK-20M12 step-down) |
| **GPIO 16** | `RS485_RX` | XY-485 Auto-Flow RS485 Module | `RXD` | Input | UART2 Serial RX (3.3V TTL logic) |
| **GPIO 17** | `RS485_TX` | XY-485 Auto-Flow RS485 Module | `TXD` | Output | UART2 Serial TX (3.3V TTL logic) |
| **GPIO 21** | `I2C_SDA` | GY-SHT30-D Temp/Humidity Sensor | `SDA` | Bidirectional | I2C Data Line (4.7k pull-up to 3.3V) |
| **GPIO 22** | `I2C_SCL` | GY-SHT30-D Temp/Humidity Sensor | `SCL` | Output | I2C Clock Line (Standard 100 kHz / Fast 400 kHz) |
| **GPIO 32** | `IN_TWT_FLOT`| 4-Ch PC817 Opto Board (Channel 1) | `V1` | Input | Active LOW when Aster `TWT FLOTY` loop is closed (`INPUT_PULLUP`) |
| **GPIO 26** | `IN_RL1_STAT`| 4-Ch PC817 Opto Board (Channel 2) | `V2` | Input | Active LOW when Aster RL1 / Multiport valve contact is closed (`INPUT_PULLUP`) |
| **GPIO 25** | `IN_RL2_STAT`| 4-Ch PC817 Opto Board (Channel 3) | `V3` | Input | Active LOW when Aster RL2 / Multiport valve contact is closed (`INPUT_PULLUP`) |
| **GPIO 33** | `IN_ALARM`   | 4-Ch PC817 Opto Board (Channel 4) | `V4` | Input | Active LOW when the Aster `AUX OP` contact is closed. **Confirmed 2026-08-26: normally open, closes on a plant issue — `AUX OP` behaves as `ALARM`** (Section 6.3, `INPUT_PULLUP`) |
| **GPIO 5**  | `US_TRIG_DOS`| AJ-SR04M (Dosing Tank) | `TRIG` | Output | 10 µs trigger pulse. Dosing sensor is wired direct to the hub — see Section 13 |
| **GPIO 4**  | `US_ECHO_DOS`| AJ-SR04M (Dosing Tank) | `ECHO` | Input | Echo pulse width. **5V → 3.3V divider required** (1 kΩ series + 2 kΩ to GND) |
| **GPIO 34** | `IN_HPP_AC` | 220V AC Opto Module #1 (HPP Contactor) | `OUT` | Input (GPI) | Active LOW when HPP contactor is energized. **External 10 kΩ pull-up to 3V3 required** |
| **GPIO 35** | `IN_RWP_AC` | 220V AC Opto Module #2 (RWP Contactor) | `OUT` | Input (GPI) | Active LOW when RWP contactor is energized. **External 10 kΩ pull-up to 3V3 required** |
| **GPIO 36** | `IN_HPP_CT`  | SCT-013-030 clamp, HPP `P` conductor | Tip (3.5 mm) | Input (GPI) | Analog. ADC1_CH0, 11 dB. Rides a shared 1.65 V bias rail — spec §7.3 |
| **GPIO 39** | `IN_RWP_CT`  | SCT-013-030 clamp, RWP `P` conductor | Tip (3.5 mm) | Input (GPI) | Analog. ADC1_CH3, 11 dB. Same bias rail — spec §7.3 |
| **GPIO 27** | `OUT_RLY_TWT`| 4-Ch 5V Relay Module (Relay 1) | `IN1` | Output | Active LOW: Emulates Treated Water Float Contact to Asterro (Phase C) |
| **GPIO 23** | `OUT_RLY_RWT`| 4-Ch 5V Relay Module (Relay 2) | `IN2` | Output | Active LOW: Emulates Raw Water Float Contact to Asterro (Phase C) |
| **GPIO 18** | `OUT_RLY_DOS`| 4-Ch 5V Relay Module (Relay 3) | `IN3` | Output | Active LOW: Emulates Dosing Level Contact to Asterro (Phase C) |
| **GPIO 19** | `OUT_RLY_AUX`| 4-Ch 5V Relay Module (Relay 4) | `IN4` | Output | Active LOW: Auxiliary / Interlock dry contact override (Phase C, spare) |
| **GPIO 2** | `LED_STATUS` | Onboard DevKit Blue LED | Anode | Output | System Heartbeat & Modbus polling activity indicator (No external wire needed) |
| **GPIO 0** | `BTN_BOOT` | Onboard DevKit BOOT Button | Switch | Input | Factory Reset / AP Provisioning Mode trigger (No external wire needed) |

**Pin selection constraints — re-check these before any reshuffle:**
* **GPIO 34-39 have no internal pull-up**, and the AC opto modules present their phototransistor collector on `OUT`. Both are open-collector, so GPIO 34 and 35 need an **external 10 kΩ pull-up to 3V3**; `pinMode(pin, INPUT)` alone leaves them floating. This is a known defect on the already-built hub and must be fixed physically (phase-B spec §3.5).
* PC817 outputs are open-collector too, so all four dry-contact channels sit on pins that support `INPUT_PULLUP`: GPIO 25/26/32/33.
* GPIO 6-11 are wired to the SPI flash and unusable. GPIO 1/3 are the USB serial console. GPIO 12, 13, 14 and 15 remain free — 13 is the natural home for a fifth PC817 channel if one is ever added. **They are all ADC2 and therefore useless for analog while Wi-Fi is up**, which is why the two CT channels take `GPIO 36` and `GPIO 39` (ADC1) instead. Those were the last two free ADC1 pins on this hub.
* `GPIO 4` is `ADC2_CH0`. ADC2 is unusable while Wi-Fi is active, but this pin is used as a **digital** input, which is unaffected. Do not repurpose it for analog.
* `GPIO 5` emits a brief pulse at boot (strapping pin). On a `TRIG` line that costs one spurious ranging cycle at power-up and nothing else.
* GPIO 27/23/18/19 drive relay opto inputs only. They idle HIGH (relay de-energized) through reset and boot, which is the fail-safe state defined in Section 7.2.

---

## 2. Power Architecture & Voltage Distribution

```
 230V AC Mains (From Plant Contactor Panel)
      |
      v
 +----------------------------------------------------------+
 | HLK-20M12 Encapsulated Power Supply                      |
 | Input: 100 - 240V AC (50/60 Hz)                          |
 | Output: 12.0V DC (1.66A, 20W Isolated SELV)             |
 +----------------------------+-----------------------------+
                              |
                              +------------------------------> Main 12V DC CAT5e Trunk Line
                              |                                (Power to Remote Tank Nodes)
                              v
 +----------------------------------------------------------+
 | Central Skid DC-DC Buck Converter (LM2596 / Mini560)     |
 | Input: 12V DC                                            |
 | Output: 5.0V DC (2.0A max)                               |
 +----------------------------+-----------------------------+
                              |
                              +------------------------------> ESP32 5V / VIN Pin
                              +------------------------------> 4-Channel Relay Module JD-VCC (Coil Power)
                              |
                              v
 +----------------------------------------------------------+
 | ESP32 Onboard 3.3V LDO Regulator                         |
 +----------------------------+-----------------------------+
                              |
                              +------------------------------> SHT30 Temp/Humidity Sensor (VIN)
                              +------------------------------> Central XY-485 Module (VCC)
                              +------------------------------> 4-Channel Relay Module VCC (Opto 3.3V)
```

---

## 3. SHT30 Environmental Sensor Wiring (Central Hub)

```
  ESP32-S Hub (3.3V Logic)          GY-SHT30-D Module
 +------------------------+        +-------------------------+
 | 3V3 (3.3V Power Out)   +------->| VIN / VCC               |
 | GND                    +------->| GND                     |
 | GPIO 21 (I2C SDA)      +------->| SDA                     |
 | GPIO 22 (I2C SCL)      +------->| SCL                     |
 |                        |        | AD / ADDR (UNCONNECTED) | Default I2C Address: 0x44
 |                        |        | AL / ALERT (UNCONNECTED)| Alarm pin not required
 +------------------------+        +-------------------------+
```

---

## 4. Central Hub RS485 Transceiver (XY-485) Wiring

```
  ESP32-S Hub (3.3V Logic)          XY-485 Auto-Direction Module
 +------------------------+        +-------------------------+
 | 3V3 (3.3V Power Out)   +------->| VCC                     |
 | GND                    +------->| GND                     |
 | GPIO 16 (UART2 RX)     +------->| RXD                     |
 | GPIO 17 (UART2 TX)     +------->| TXD                     |
 +------------------------+        | A+  --> RS485 Bus (A+)  |
                                   | B-  --> RS485 Bus (B-)  |
                                   +-------------------------+
```

---

## 5. 4-Channel PC817 Optoisolator Module (Sensor Inputs)

Provides galvanic isolation for reading dry contacts and low-voltage status lines from the Asterro controller and Multiport Valve. The board fitted to the built hub is the **4-channel** part; three channels were in use and channel 4 is claimed by `IN_ALARM`, leaving **no spare channel**. Tapping `RWT FLOTY` or `DOS LVL` directly would require swapping in an 8-channel board — neither is needed for Phase B, since those tanks are measured by the RS485 ultrasonic nodes instead.

### 5.1. Module Architecture & Jumpers
* **Input Side (Left):** `IN1`..`IN4` (+) and `G` (Return) with onboard 3k ohm current-limiting resistors (compatible with 3.3V to 24V DC).
* **Output Side (Right):** `V1`..`V4` (Signal) and `G` (Ground). Open-collector phototransistors.
* **Jumpers:** Keep all black jumper caps installed (Default position).
* **ESP32 Connection:** ESP32 uses internal `INPUT_PULLUP`. No external `VCC` connection is required on the output side.

```
            INPUT SIDE (Left)                            OUTPUT SIDE (Right)
    +---------------------------------+          +---------------------------------+
    | [ IN1 ] (TWT FLOTY loop)        |          | [ V1 ] --> ESP32 GPIO 32        |
    | [ IN2 ] (RL1 contact loop)      |          | [ V2 ] --> ESP32 GPIO 26        |
    | [ IN3 ] (RL2 contact loop)      |          | [ V3 ] --> ESP32 GPIO 25        |
    | [ IN4 ] (ALARM / AUX OP loop)   |          | [ V4 ] --> ESP32 GPIO 33        |
    | [  G  ] (Common input return)   |          | [  G ] --> ESP32 GND            |
    +---------------------------------+          +---------------------------------+
```

### 5.2. Wetting Loop for Volt-Free Contacts

Every Aster contact tapped here is volt-free, so the loop must be wetted from our own 12V rail. Series the contact between +12V and the channel input. The board's onboard 3k resistor sets roughly 4 mA, which is above the HF3FF minimum wetting current and far below its rating.

```
  +12V DC (HLK-20M12 rail) --------> Aster terminal [ C      ]
                                     Aster terminal [ NO/NC  ] --------> PC817 [ INn ]
  12V GND ------------------------------------------------------------> PC817 [  G  ]

  Contact CLOSED -> ~4 mA through the opto LED -> phototransistor conducts -> GPIO reads LOW
  Contact OPEN   -> no current                 -> internal pull-up wins    -> GPIO reads HIGH
```

**Never apply this 12V wetting loop to a terminal the Aster panel energizes itself.** Confirm each tapped pair reads 0V AC and 0V DC across it, in both healthy and faulted states, before wiring. Procedure in Section 6.4.

---

## 6. Aster NXT Terminal Contact Polarity & Alarm Output

**Sources of truth:** `docs/ASTERO-NXTG-1_opt.pdf` (Aster NXT manual) — terminal list p.3, input/output configuration p.8-10, troubleshooting table p.12 — cross-checked against the board silkscreen photographed in `images/IMG_2836.JPG`.

> **Do not infer polarity from the `NO` / `NC` silkscreen alone.** Those marks name the contact form of the **field device at rest**, not the state the panel requires while running. On this controller `LPS` and `HPS` are both silkscreened `C NO` and behave in *opposite* directions.

### 6.1. Input Terminal Polarity (Verified)

| Terminal | Silkscreen | Field device | At rest (0 bar / normal tank) | **While running normally** | Open circuit is read as |
| :--- | :---: | :--- | :--- | :--- | :--- |
| `LPS` | `C` / `NO` | Low pressure switch, setpoint **below** normal feed pressure | open | **CLOSED** | `LOW PRESSURE!!` |
| `HPS` | `C` / `NO` | High pressure switch, setpoint **above** normal RO pressure | open | **OPEN** | normal — no fault |
| `TWT FLOTY` | `C` / `NC` | Treated water tank float | closed | **CLOSED** | `TW TANK FULL!!` |
| `RWT FLOTY` | `C` / `NC` | Raw water tank float | closed | **CLOSED** | `RW TANK EMPTY!!` |
| `DOS LVL` | `C` / `NC` | Dosing tank low-level float | closed | **CLOSED** | dosing low fault |

**Why `LPS` and `HPS` invert despite identical silkscreen.** Both are the same physical contact form — open at zero pressure, closing once pressure rises past their setpoint. What differs is where that setpoint sits relative to normal operating pressure:

```
  LPS setpoint  ~0.5-1 bar   ----+
                                 |   normal running feed pressure  (~2-3 bar)
                                 +--> LPS sits CLOSED while running
                                        opens only when feed fails      => closed = permissive

                                     normal running RO pressure      (~10-14 bar)
  HPS setpoint  ~18-20 bar   ----+
                                 +--> HPS sits OPEN while running
                                        closes only on over-pressure    => closed = trip
```

Manual p.12 confirms both directions: `LOW PRESSURE!!` lists *"LPS not connected"* as a cause, while the `HIGH PRESSURE!!` row does not — its wiring-error cause is *"check, is it C NO contact"*, i.e. a **closed** loop landed on `HPS` reads as over-pressure. The same page instructs *"Floaty is not connected. Short FLOATY terminal by an external wire link"*, confirming closed = healthy on the `NC`-form inputs.

### 6.2. As-Built Deviations Recorded on This Plant

| Observation | Date | Consequence |
| :--- | :--- | :--- |
| `HPS` terminal has **no field wiring** and the plant runs normally | 2026-08-25 | Consistent with 6.1 (open = healthy). Means there is **no controller-level over-pressure protection**, and **no `HPS` signal exists for the hub to monitor** — do not publish an `hps` state. Confirm on site whether `HI PRESS SW` is `ON` (enabled, watching a switch that can never close) or `OFF` (bypassed) and record the answer here. |
| `LPS` silkscreen is `C` / `NO` | 2026-08-25 | `RO_HARDWARE_ANALYSIS.md` previously recorded `C` / `NC`; corrected there. |

### 6.3. The `ALARM` Terminal Is the Configurable AUX OP

Physically: a 2-screw pair on the upper-right block, immediately right of the third `HF3FF-012-1ZST` and left of `RSV` — the volt-free `C` / `NO` pair of that third relay (`images/IMG_2836.JPG`).

Logically it is the panel's **AUX OP**, and its meaning is a software setting, not a fixed function:

| `AUX OP` setting | Relay energizes when |
| :--- | :--- |
| `ALARM` | any fault is active (this is what our telemetry assumes) |
| `DOSING PUMP` | HPP is running |
| `PMP ON` | RWP is running |

Read the current value under **password 678** (Input Configuration, manual §1.4): press `<` and `>` together, enter `678`, scroll to `AUX OP:`.

> **Resolved 2026-08-26 — observed on the plant.** The pair sits **open in normal running and closes when there is an issue**, which stops the plant. That is `AUX OP` = `ALARM` behaving as this document assumed, on the `C` / `NO` pair, and it is the useful case: a genuine fault flag rather than a pump-run signal. The risk below was worth carrying and is now closed.
>
> *Original risk, kept for the reasoning:* `RL1` / `RL2` are documented here as multiport-valve status, which implies `MPV CNTRL` is `ON`, and the manual states *"IF ON, one has to configure AUX OP as PMPON when there is AMPV in pretreatment."* Had `AUX OP` been `PMP ON`, this terminal would be a pump run signal and every "controller trip" alert built on it would have been wrong.

**What this buys, and its one limitation.** `IN_ALARM` LOW means the controller has faulted — actionable on its own, no inference needed, and the most valuable single bit the Aster offers. What it does *not* carry is **which** fault: the panel multiplexes every condition onto one contact, so the ESP32 knows something tripped but not whether it was high pressure, a level interlock or a sensor. Alert text must therefore say *"controller fault — check the panel"* rather than naming a cause it cannot know.

Pairing it with the two AC optos does narrow things usefully: `IN_ALARM` closed while `IN_HPP_AC` reads stopped is a genuine trip, whereas the plant idling with `IN_ALARM` open is simply a satisfied TWT float and needs no alert at all.

### 6.4. Commissioning Procedure — Verify, Then Sense

**Step 1 — ~~confirm `AUX OP` = `ALARM`~~ (6.3). Done 2026-08-26:** observed open in normal running, closed on a plant issue. Proceed to step 2.

**Step 2 — trigger a fault and characterise the contact electrically.** The unwired `HPS` pair is the cheapest trigger: no wire is disturbed and no timer stands in the way.

```
  Short HPS [ C ] to [ NO ] for ~2 s
    -> display shows "HIGH PRESSURE!!", AUX OP relay energizes
    -> measure across the ALARM pair, both states:
         V~ and V= = 0 in both  -> volt-free dry contact  -> Step 3
         230 V~ when tripped    -> switched mains         -> Step 3 (AC opto variant)
         12 V= when tripped     -> switched 12 V          -> Step 3, omit external 12V rail
    -> power down, confirm with an ohmmeter: open idle, ~0 ohm tripped
  Remove the jumper -> fault clears.
```

If shorting `HPS` produces nothing, `HI PRESS SW` is `OFF` (bypassed); use `DOS LVL` (open one wire) instead. The HPP stopping during this test is a normal protective action — do not hold the short or cycle it repeatedly, the contactor pays for it.

**Step 3 — wire the sense channel.**

*Volt-free result (expected):* PC817 channel 4 per Section 5.2 — `+12V -> ALARM [C]`, `ALARM [NO] -> IN4`, `12V GND -> G`, `V4 -> GPIO 33`, `INPUT_PULLUP`, alarm = LOW. No new hardware: channel 4 was the spare on the board already fitted.

*Switched-mains result:* do **not** use the PC817. Add a 220V AC opto module wired as in Section 8 (`L` = switched leg, `N` = the other screw), `OUT` -> GPIO 33 **with a 10 kΩ pull-up to 3V3** (same open-collector caveat as GPIO 34/35). Note this is a **fifth** AC opto module; the BOM's four are already allocated.

**Step 4 — debounce in firmware.** The contact bounces and the HPP contactor induces blips on the loop. Require a sustained state:

```c
// IN_ALARM on GPIO 33, INPUT_PULLUP, active LOW. Latch after 200 ms sustained.
static uint32_t t_edge;
static bool raw, alarm;
bool now = !digitalRead(PIN_IN_ALARM);
if (now != raw)            { raw = now; t_edge = millis(); }
else if (millis() - t_edge > 200) { alarm = raw; }
```

### 6.5. Fault-to-Alarm Timing (Do Not Assume It Is Instant)

Configured delays sit between a physical fault and the AUX OP relay picking up. Firmware must latch and timestamp the alarm rather than expect it to mirror a fault edge:

| Fault | Delay before trip / alarm | Setting |
| :--- | :--- | :--- |
| Low pressure | `LO PRESS. DBNCE` (factory **015 s**) then `LPS TRIP` (factory **03 min**, range 03-60 min) | password 678 |
| High pressure | no documented delay | — |
| Pump overload | trips, then **auto-restarts after 15 min** and re-checks current | password 123 (overload current) |

A `LOW PRESSURE!!` message on the display therefore precedes the alarm contact by up to three minutes. Treat display state and alarm contact as two separate observations.

---

## 7. 4-Channel Relay Output Module (Asterro Float & Level Emulation)

### 7.1. Relay Module to ESP32 Pin Connections

```
  ESP32-S Hub (Control)            5V 4-Channel Relay Module
 +------------------------+        +-------------------------+
 | 5V Rail (Buck Output)  +------->| JD-VCC (Remove Jumper)  |
 | 3V3 (3.3V Logic)       +------->| VCC (Opto Anode)        |
 | GND                    +------->| GND                     |
 | GPIO 27                +------->| IN1 (Relay 1 Control)   |
 | GPIO 23                +------->| IN2 (Relay 2 Control)   |
 | GPIO 18                +------->| IN3 (Relay 3 Control)   |
 | GPIO 19                +------->| IN4 (Relay 4 Control)   |
 +------------------------+        +-------------------------+
```

### 7.2. Relay Contacts to Asterro Terminal Interconnections (Dry Contacts)

Polarity follows Section 6.1 and is **not** interchangeable between terminals. Every de-energized state below is "plant permitted to run", so a hub reset, a firmware crash or loss of the 5V coil rail leaves the panel running on its own real switches — fail-safe by construction.

| Relay Channel | ESP32 GPIO | Target Asterro Terminal | Relay Contacts Used | De-energized (default / boot) | Energized |
| :---: | :---: | :--- | :---: | :--- | :--- |
| **Relay 1** | **GPIO 27** | `TWT FLOTY [ C ]` & `[ NC ]` | `COM` & `NC` | **Closed** = tank not full, plant may run | **Open** = `TW TANK FULL!!` |
| **Relay 2** | **GPIO 23** | `RWT FLOTY [ C ]` & `[ NC ]` | `COM` & `NC` | **Closed** = raw water available | **Open** = `RW TANK EMPTY!!` |
| **Relay 3** | **GPIO 18** | `DOS LVL [ C ]` & `[ NC ]`   | `COM` & `NC` | **Closed** = chemical OK | **Open** = dosing low |
| **Relay 4** | **GPIO 19** | Spare — see note below | `COM` & `NC` for `LPS`, `COM` & `NO` for `HPS` | `LPS` form: **closed** = pressure OK. `HPS` form: **open** = pressure OK | trip |

> **Never substitute a hub relay for the real `LPS` or `HPS` switch.** Doing so hands dry-run and over-pressure protection to an ESP32. If a remote stop is wanted, wire Relay 4 **in series** with the real `LPS` loop (`COM`/`NC`, either the switch or the hub can open it) or **in parallel** across `HPS` (`COM`/`NO`, either can close it). The mechanical switch keeps its authority in both arrangements.

```
              Relay Module Dry Contacts                   Asterro Terminal Block
             +-------------------------+                 +-----------------------+
  Relay 1    | COM                     +---------------->| TWT FLOTY [ C  ]      |
  (TWT Float)| NC (Normally Closed)    +---------------->| TWT FLOTY [ NC ]      |
             +-------------------------+                 +-----------------------+
  Relay 2    | COM                     +---------------->| RWT FLOTY [ C  ]      |
  (RWT Float)| NC (Normally Closed)    +---------------->| RWT FLOTY [ NC ]      |
             +-------------------------+                 +-----------------------+
  Relay 3    | COM                     +---------------->| DOS LVL   [ C  ]      |
  (Dos Level)| NC (Normally Closed)    +---------------->| DOS LVL   [ NC ]      |
             +-------------------------+                 +-----------------------+
```

---

## 8. 240V AC Optocoupler Modules (HPP & RWP Contactor Monitoring)

```
  240V AC Side (From Contactor)           240V AC Opto Module          ESP32 Hub
 +-----------------------------+        +---------------------+       +-----------+
 | Switched Phase (e.g. Coil A1)+------->| L                   |       |           |
 | Neutral (e.g. Coil A2)      +------->| N                   |       |           |
 +-----------------------------+        |                 VCC |<------+ 3V3 (3.3V)|
                                        |                 GND +------>| GND       |
                                        |                 OUT +------>| GPIO 34/35|
                                        +---------------------+       +-----------+
```

---

## 9. Remote Arduino Nano Tank Nodes (0x02 RWT, 0x03 TWT)

**Node 0x02 (RWT) is the end of the bus** and carries the second 120 ohm resistor across its `A+` / `B-`, per Section 12. Node 0x03 is mid-chain and carries none.

```
                                  12V DC CAT5e Power Bus
                                            |
                                            v
                             +------------------------------+
                             | LM2596 / Mini560 Buck Module |
                             | Input: 12V DC | Output: 5.0V |
                             +--------------+---------------+
                                            |
                                            +-- +5V DC ------+
                                            +-- GND ---------+----------+
                                                             |          |
 +-----------------------------------------------------------v----------v-+
 |                            ARDUINO NANO (ATmega328P)                   |
 |  - 5V Pin  <-- +5V Buck Output                                         |
 |  - GND Pin <-- Common Ground                                           |
 +------+------------+-------------+-------------+-------------+----------+
        |            |             |             |             |
      Pin D2       Pin D3        Pin D7        Pin D8        Pin A0 / A1
     (Soft RX)   (Soft TX)     (US TRIG)     (US ECHO)     (Hardware Address)
        |            |             |             |             |
 +------v------------v------+ +----v-------------v----+ +------v----------+
 |    XY-485 MODULE         | |   AJ-SR04M MODULE     | | ADDRESS JUMPERS |
 | RXD --> D2               | | TRIG --> D7           | | see 9.1 below   |
 | TXD --> D3               | | ECHO --> D8           | | A0 -> GND or open|
 | VCC --> 5V (from Buck)   | | VCC  --> 5V (from Buck| | A1 -> GND or open|
 | GND --> GND              | | GND  --> GND          | +-----------------+
 | A+  --> RS485 Bus (A+)   | +---------+-------------+
 | B-  --> RS485 Bus (B-)   |           | (Coaxial Cable)
 +--------------------------+           v
                               +-----------------+
                               | Waterproof      |
                               | Transducer Head |
                               +-----------------+
```

### 9.0. AJ-SR04M Sensor Notes (applies to every ultrasonic node and the hub's dosing sensor)

The sensors in hand are **AJ-SR04M**, not JSN-SR04T. Electrically interchangeable for our purposes — 5 V supply, 5 V `ECHO` needing the divider, ~20 cm blind zone, `pulseIn` timeout of 35000 µs still covering the useful range — with two board-specific gotchas:

| Check | Detail |
| :--- | :--- |
| **`R19` mode-select pad must be EMPTY** | Unpopulated selects HC-SR04-compatible **Trig/Echo** mode, which is what the firmware drives. A resistor fitted there (47 k / 120 k / 200 k) puts the board into one of its **UART** modes, where it either streams frames unasked or waits for a `0x55` command — and `pulseIn` on `ECHO` then reads nothing at all. Inspect the pad before wiring; a board that measures zero every time is almost always this, not a wiring fault. |
| **Trigger pulse width is a tuning knob** | The firmware issues the nominal **10 µs** `TRIG` pulse. Some AJ-SR04M batches are unreliable at exactly 10 µs and want **20 µs**. If a node returns intermittent zeros or wild outliers with the pad confirmed empty, widen the pulse before suspecting the transducer. Keep it as a named constant, not a magic number. |

**Verified 2026-08-25:** `R19` inspected on **all four sensors** and **empty** on each — Trig/Echo mode, which is what the sketches drive. So a zero reading on this build is wiring, power, or the blind zone, not the mode pad. Check the pad again on any sensor added later; batches differ.

Range: 20–600 cm nominal, realistically dependable to ~4–4.5 m on a flat water surface. That covers all three tanks. The 3.5 m ground sump is inside the figure but with less margin than the datasheet implies — see phase-B spec §7.1, which is already tracking the sump sensor choice as an open decision.

### 9.1. Address Jumper Truth Table (authoritative)

Published here first; `firmware/ro_node/ro_node.ino` carries the same table and `docs/check_addrmap.py` fails the build if the two ever diverge:

```c
bit0 = (digitalRead(A0) == HIGH) ? 1 : 0;   // INPUT_PULLUP: open = 1, jumpered to GND = 0
bit1 = (digitalRead(A1) == HIGH) ? 1 : 0;
raw  = (bit1 << 1) | bit0;
if (raw == 0) return 0x04;                  // both grounded
return raw;                                 // 0x01 / 0x02 / 0x03
```

| `A1` | `A0` | raw | Node ID | Role |
| :---: | :---: | :---: | :---: | :--- |
| open | **GND** | `0b10` | **`0x02`** | RWT (end of bus, 120 Ω) |
| **GND** | open | `0b01` | **`0x03`** | TWT |
| open | open | `0b11` | **`0x04`** | Battery Room climate + fan (Pro Mini, §10). Also the unjumpered default |
| **GND** | **GND** | `0b00` | **unassigned** | Refuse to join the bus — see below |

The mapping is a **lookup table written from the observed hardware**, not an arithmetic identity. The three built boards already carried three distinct jumper codes, so the codes were kept and the table was written around them (§9.2). This is exactly what phase-B spec §3.7 prescribed.

```c
// common/protocol.h  --  raw = (A1 << 1) | A0, INPUT_PULLUP: open = 1, GND = 0
static const uint8_t ADDR_MAP[4] = {
  0x00,   // 0b00  both GND    -> unassigned, do not join the bus
  0x03,   // 0b01  A1 to GND   -> TWT
  0x02,   // 0b10  A0 to GND   -> RWT (end of bus)
  0x04,   // 0b11  both open   -> Battery Room, climate + fan relay
};

uint8_t nodeIdFromJumpers(bool a0_high, bool a1_high) {
  return ADDR_MAP[((a1_high ? 1 : 0) << 1) | (a0_high ? 1 : 0)];
}
```

A node decoding `0x00` must **not** transmit — hold in a fault state and blink the LED. Guessing an address is how you get two boards answering the same poll. Grounding both jumpers is therefore also the deliberate way to bench a spare board while it is physically on the bus.

> The table previously published here had `0x01`/`0x02` swapped, labelled both-grounded `0x03`, and omitted `0x04`. The original bringup decoder returned `raw` directly with a special case for zero, which matched no assignment. Both are superseded by `ADDR_MAP` above. Phase-B spec §3.7 is closed by this.

**Address `0x01` no longer exists in any form** — the dosing sensor moved onto the hub (§13), and no jumper combination produces it.

**The address also selects the node's personality** (spec §4): `0x02`/`0x03` run the ultrasonic build, `0x04` runs climate + fan relay. A misjumpered node therefore doesn't just answer to the wrong ID — it runs the wrong hardware profile and publishes plausible-looking wrong data.

### 9.2. As-Built Jumper Audit (2026-08-25)

The jumpers are staying as they are. `ADDR_MAP` (§9.1) was written to fit them, so **no board needs rewiring**:

| Board | `A0` | `A1` | raw | Resolves to | Action |
| :--- | :---: | :---: | :---: | :---: | :--- |
| Nano #1 | **GND** | open | `0b10` | `0x02` RWT | Label RWT, fit its 120 Ω (§12). No change. |
| Nano #2 | open | **GND** | `0b01` | `0x03` TWT | Label TWT. **No change** — this code used to mean the retired `0x01`. |
| Pro Mini (Battery Room) | open | open | `0b11` | `0x04` Climate | Label Battery Room. **No change** — leave both pads unconnected. |

Why software rather than the soldering iron: the three codes were already distinct, so there was nothing to disambiguate — only a table to write. It avoids lifting a pad on the Pro Mini's inner pads, avoids touching a node that may already be sealed in its enclosure, and puts the assignment in the one place spec §3.1 says addressing belongs.

**The cost, stated plainly:** the unjumpered default is now `0x04` rather than a tank node. A fresh board dropped on the bus claims to be the Battery Room. That is *louder* than the old failure — two nodes answering the same poll produces immediate CRC/collision errors, where a duplicate tank level produces plausible wrong data nobody notices — but it does mean **any new node must be jumpered before it is bussed**, not after.

**If the boards are ever re-jumpered instead, `ADDR_MAP` must change in the same commit.** One authoritative table; a second person re-deriving jumpers from a stale one is precisely the defect class §3.1 exists to prevent.

Confirm by boot print before bussing: each node prints its detected ID at startup. Read all three.

---

### 9.3. Ultrasonic Mounting, Per Location

Four tanks, four different problems. The sensor is the same part in each; the mounting is what decides whether it works.

**Rules that apply everywhere:**

| Rule | Why |
| :--- | :--- |
| Transducer face at least **200 mm above the highest water level** | That is the blind zone. Water inside it returns nothing, and the node reports `sensor_status = 1` rather than a number — safe, but you have lost the top of the scale exactly where "is it full" matters. |
| Mount **perpendicular** to the surface, pointing straight down | A tilted transducer reflects the pulse away from itself. A few degrees is tolerable; ten is not. |
| Keep the **beam cone clear** — ladders, pipes, ropes, wall seams | The beam is 45–75° wide, so at 3 m it lights up a cone over a metre across. Anything it clips answers *earlier* than the water does, and an early echo reads as a **shorter distance, which the dashboard shows as a fuller tank**. Wrong in the dangerous direction. |
| Mount **away from the inflow** | Broken surface and foam scatter the pulse. Put the sensor over still water, opposite the fill point. |
| Fit a **hood or shade** over the face in closed tanks | Condensation forming on, or dripping onto, the transducer kills echoes. This is the most common cause of a sensor that worked for a month and then stopped. |
| Strain-relieve the captive lead at the tank wall | The transducer hangs on its own cable otherwise, and it will eventually hang crooked — see rule 2. |

**Dosing barrel (~50 L, wired direct to the hub, §13).** The blind zone *is* the design problem here: a 50 L drum is only ~550–600 mm deep, so a sensor sitting on the barrel mouth cannot read the top third — the range you care about when deciding whether to top up. **Mount it on a bracket 250–300 mm above the open top**, not on the rim. That is the geometry the hub's `250 mm full / 900 mm empty` defaults assume.

**RWT (plastic, roof, `0x02`).** The straightforward one. Standard rules, no special measures.

**TWT (concrete, wide, roof, `0x03`) — the wide tank an ultrasonic sensor cannot use.**

On paper this is the easiest tank: a large flat surface, walls far from the beam. **As built it is the hardest**, because the only penetration is a small wire pass-through in the roof slab, **hard against the wall**.

The arithmetic rules it out. The beam is ~45° total, so its radius grows ~0.41 m per metre of depth. A transducer 100 mm from a wall has that wall inside its beam from **0.24 m down**, and from there on the wall answers at a fixed ~260 mm regardless of where the water is. The tank reads permanently near-full — and *stably*, with quality at 100 and no jitter, so nothing in the telemetry looks wrong. A confidently wrong treated-water level is worse than no reading at all.

Three ways out, in cost order:

1. **Core a new penetration away from the wall.** Clearance must exceed `0.41 × (depth from transducer to the lowest working surface)` — for a 1.5 m drop, **≥ 0.6 m from any wall**. Cheapest in parts, most invasive in a concrete roof slab.
2. **Submersible pressure transducer through the existing hole** (phase-B spec §7.1). Geometry stops mattering: it hangs on its cable and reads head. The cable passes a wire hole, but the **22 mm stainless body does not** — budget on opening that hole to ~30 mm with a masonry bit, which is a far smaller job than coring 90 mm. The 12 V the loop needs is already at the node.
3. **Leave TWT on the Aster float alone** and accept no continuous level here. It is, after all, the tank whose float already drives the controller.

**This reorders the procurement question in spec §7.1.** The sump has an open manhole and can take a stilling well with a sensor already owned; TWT cannot. If exactly one pressure transducer is bought, the evidence points at TWT for it, not the sump.

#### If the TWT transducer is fitted — three requirements

**1. The enclosure above the tank must breathe.** The node box on the tank roof is planned airtight, and a gauge transducer references atmosphere through a vent tube running inside its cable. Terminate that tube in a sealed box and its reference becomes a fixed volume of air in the sun:

> `ΔP = P × ΔT/T` — a **20 °C swing is 6.8 kPa, which is ~0.7 m of apparent water** on a 1.5 m tank. Even 5 °C is 173 mm. The level appears to rise each morning and fall each evening, and nothing in the reading looks faulty.

Fit a **membrane breather vent** (Gore-Tex / IP68 M12 type) in the enclosure wall — weatherproof, pressure-equalised — and add a replaceable desiccant sachet inside. At the terminal, **leave the vent tube open**: never crimp it into a gland, seal it in heat-shrink, or pot it in epoxy. This single detail is the difference between a transducer that holds calibration for years and one that drifts within a week.

**2. Size the sensor to the tank, not to the catalogue.** TWT is **~1.5 m** deep. Accuracy is quoted as % of *full scale*, so a 0-5 m unit gives ±25 mm here — ±1.7 % of the span actually used — and on a Nano's 10-bit ADC the whole tank occupies only ~245 counts (~6 mm each). A **0-2 m or 0-3 m variant** of the same sensor costs about the same, gives ±10 mm, and ~2.4 mm per ADC count. Buy for the tank you have.

**3. Hang it 50-100 mm off the floor.** Clear of sediment, and it means the bottom 50-100 mm is below the sensor and simply never measured — which the hub's calibration absorbs the moment you set *empty = now* at the working empty level.

**Sump (concrete, deep, ground floor, `0x05`).** The hard one, and the reason phase-B spec §7.1 is still open. A 3.5 m shaft with a ladder, a riser pipe and rough walls is exactly the geometry that produces early echoes, and the borewell inflow foams the surface while it runs.

Fit a **stilling well** rather than hoping: a ~100 mm PVC pipe hung vertically, open at the bottom, vented at the top, sensor on the cap. The water inside follows the sump but stays still, and the pipe wall stops the beam ever seeing the shaft. A few hundred rupees, and it addresses turbulence and false echoes in one part.

**Cable routing near the PV installation.** The roof array has no string inverters — the DC runs down to the battery room, where every inverter lives. That is also where node `0x04` sits and where the RS485 chain passes on its way to the roof, so the electrically noisy room is the battery room, not the roof. Keep the Cat5e clear of inverter AC and DC runs there, cross at 90 degrees rather than running parallel, and do not share a conduit with the PV strings on their way down. A 4-20 mA loop feeding a 10-bit ADC will show every bit of noise it picks up as jitter in the level.

**Judging any of these from the data, not by argument:** the node reports `raw` alongside the filtered `median`, plus an echo-quality figure. Clean tracking is a median that moves smoothly with quality at 100. False echoes look like a median stepping between two values with quality stuck below 100. Log a day of it before concluding anything about a sensor.

---

## 10. Battery Room Node (0x04: SHT30 & Exhaust Fan)

**Mid-chain node — no termination resistor.** 0x04 sits between the Dosing node and the TWT node in the as-installed run (Section 12). If a 120 ohm resistor was fitted here under the earlier end-of-bus assumption, **remove it**: three terminators on one bus over-load the drivers and blunt the differential swing.

```
                                  12V DC CAT5e Power Bus
                                            |
                                            v
                             +------------------------------+
                             | LM2596 / Mini560 Buck Module |
                             | Input: 12V DC | Output: 5.0V |
                             +--------------+---------------+
                                            |
                                            +-- +5V DC ------+
                                            +-- GND ---------+----------+
                                                             |          |
 +-----------------------------------------------------------v----------v-+
 |            ARDUINO PRO MINI  (ATmega328P-AU, 5V / 16 MHz)              |
 |  - VCC Pin <-- +5V Buck Output      *** NOT the RAW pin - see 10.1 *** |
 |  - GND Pin <-- Common Ground                                           |
 +------+------------+-------------+-------------+-------------+----------+
        |            |             |             |             |
      Pin D2       Pin D3        Pin A4        Pin A5        Pin D9
     (Soft RX)   (Soft TX)      (I2C SDA)     (I2C SCL)     (Relay IN)
                              [inner pad]    [inner pad]
        |            |             |             |             |
 +------v------------v------+ +----v-------------v----+ +------v----------+
 |    XY-485 MODULE         | |   GY-SHT30-D SENSOR   | | 1-CH RELAY BOARD|
 | RXD --> D2               | | SDA --> A4            | | IN1 --> D9      |
 | TXD --> D3               | | SCL --> A5            | | VCC --> 5V      |
 | A+  --> RS485 Bus (A+)   | | VCC --> 5V            | | GND --> GND     |
 | B-  --> RS485 Bus (B-)   | | GND --> GND           | | COM/NO -> Fan AC|
 +--------------------------+ +-----------------------+ +-----------------+

 Programming header (6-pin FTDI / CP2102 adapter, 5V setting):
   DTR -> DTR   TXO -> RX   RXI -> TX   VCC -> 5V   GND -> GND
```

### 10.1. Pro Mini Build Notes (0x04 only)

This node is the one Pro Mini in the fleet; the other three RS485 nodes are Nanos. Same ATmega328P, same `pinmap_node.h`, same binary — only the board mechanics differ.

| Item | Requirement | Why |
| :--- | :--- | :--- |
| **Power feed** | +5V from the Mini560 / LM2596 into **`VCC`**. Leave **`RAW` unconnected.** | `RAW` goes through the Pro Mini's SOT-23 LDO. Dropping 12V→5V at this node's ~168 mA peak dissipates **~1.2 W** in a package good for ~0.4 W, and the relay coil alone brushes the LDO's ~150 mA rating. It would brown out or thermally shut down — in the hottest room in the building, on the node whose job is reporting that room's temperature. |
| **Board variant** | 5V / 16 MHz **only**. Verify: 9–12V on `RAW`, nothing else connected, measure `VCC` = **5.0V**. | ~5 mA flows unloaded, so the LDO is safe for this one test. A 3.3V board starves the XY-485 and the 5V relay coil, and 8 MHz halves the SoftwareSerial bit-timing margin at 9600 baud. |
| **`A4` / `A5`** | Solder wires to the **two inner pads** (labelled on the underside). They are not on the edge headers — those run `A3 A2 A1 A0`. | I²C is hardware TWI on `PC4`/`PC5` and cannot be moved to other pins. If a board truly omits the breakouts, the fallback is soldering to TQFP pins 27 (`PC4`/SDA) and 28 (`PC5`/SCL). |
| **`A0` / `A1` address jumpers** | **Leave both unconnected** — that is `0x04` under `ADDR_MAP` (§9.1). | No wiring needed, but it means an unjumpered board *is* a Battery Room node. Never put a second unjumpered board on the bus: two nodes answering `0x04` collide on every climate poll. |
| **Programming** | FTDI / CP2102 adapter set to 5V, **`DTR` wired** (auto-reset). IDE: board *Arduino Pro or Pro Mini*, processor *ATmega328P (5V, 16 MHz)*. | Without `DTR` every upload times out. |
| **During programming** | **Disconnect the buck's 5V** while the adapter is plugged in. | Two supplies fighting on `VCC` kills adapters. |
| **RS485 on `D2`/`D3`** | Unchanged — SoftwareSerial, not the hardware UART. | On a Pro Mini `D0`/`D1` *are* the FTDI header. Keeping RS485 off them leaves the debug port usable without desoldering, which is the same reasoning as phase-B spec §199 and matters more here. |

**As-built 2026-08-26:** the bench FTDI adapter in use has **`DTR` unconnected**. Uploads to this board therefore need the reset button tapped the instant the IDE switches from *Compiling* to *Uploading*, and opening the Serial Monitor does not restart the sketch. Symptom when mistimed: `not in sync: resp=0x00`, ten attempts. One wire from the adapter's `DTR` (or `RTS`) to the `DTR`/`GRN` pad ends it — the 100 nF cap is already on the board.

---

## 11. Ground Floor Wi-Fi Nodes Wiring (Parking)

### 11.1. Ground Floor ESP32 Node 1: Sump Ultrasonic Level (0x05)
* **Power Source:** 230V AC $\to$ Hi-Link `HLK-20M5` (5V DC 4A)
* **ESP32 5V / VIN:** `+5V DC` from HLK-20M5
* **ESP32 GND:** `GND` from HLK-20M5
* **AJ-SR04M Sensor:**
  * `VCC` $\to$ `5V DC`
  * `GND` $\to$ `GND`
  * `TRIG` $\to$ `ESP32 GPIO 5`
  * `ECHO` $\to$ `1kΩ resistor` $\to$ `ESP32 GPIO 18` *(and 2kΩ from GPIO 18 to GND for 3.3V voltage divider)*

### 11.2. Ground Floor ESP32 Node 2: Motor AC Sensing & Float Relays (0x06)
* **Power Source:** 230V AC $\to$ Hi-Link `HLK-20M5` (5V DC 4A)
* **220V AC Optocoupler 1 (Sump Motor Monitor):**
  * `AC L / N` $\to$ Connected across Sump Motor Starter Contactor 240V Coil
  * `DC VCC` $\to$ `3.3V` from ESP32
  * `DC GND` $\to$ `GND`
  * `DC OUT` $\to$ `ESP32 GPIO 34`
* **220V AC Optocoupler 2 (Borewell Motor Monitor):**
  * `AC L / N` $\to$ Connected across Borewell Motor Starter Contactor 240V Coil
  * `DC VCC` $\to$ `3.3V` from ESP32
  * `DC GND` $\to$ `GND`
  * `DC OUT` $\to$ `ESP32 GPIO 35`
* **4-Channel Relay Board (Starter Interlocks):**
  * `VCC` $\to$ `5V DC` from HLK-20M5
  * `GND` $\to$ `GND`
  * `IN1` (Sump Low Float Cutoff) $\to$ `ESP32 GPIO 25`
  * `IN2` (Borewell High Float Cutoff) $\to$ `ESP32 GPIO 26`
  * `IN3` (Aux Manual Override 1) $\to$ `ESP32 GPIO 27`
  * `IN4` (Aux Manual Override 2) $\to$ `ESP32 GPIO 14`

---

## 12. Complete RS485 Daisy-Chain Topology & Bus Termination

```
 +----------------+     +----------------+     +----------------+     +----------------+
 | ESP32 HUB      |     | BATTERY ROOM   |     | TWT NODE       |     | RWT NODE       |
 | Master (0x00)  +-----+ Slave (0x04)   +-----+ Slave (0x03)   +-----+ Slave (0x02)   |
 | [120Ω Enabled] |     | (No Resistor)  |     | (No Resistor)  |     | [120Ω Enabled] |
 +----------------+     +----------------+     +----------------+     +----------------+
   RO Room               Battery Room           Roof Top               Roof Top
        Cat5e 1 (TBM)          Cat5e 2 (TBM)          Cat5e 3 (TBM)

 Dosing tank sensor is NOT on this bus - it is wired direct to the hub (Section 13).
```

* **Bus Topology:** Strict linear daisy chain (no star/branch topologies). Physical order is `0x00 -> 0x04 -> 0x03 -> 0x02`. Node addresses are logical and deliberately do **not** follow the cable order — the hub's poll sequence is by address, not by position on the bus. Address `0x01` is retired and unassigned.
* **Termination:** Exactly **two 120 ohm resistors** on the entire physical bus:
  1. One at the **ESP32 Central Hub** (RO Room) across `A+` and `B-`.
  2. One at the physical end of the bus — **RWT Node 0x02** — across `A+` and `B-`.
* **No return loopback, no junction-box splice.** Earlier revisions routed the bus up to a mid-chain RWT and back down through the brown pair to an RO room splice. With RWT last, every hop is a single outbound pair and both spare pairs stay free for the 12V rail.
* **All cable lengths are TBM** and must be measured on site before re-running the power budget. The old 1.5 m `Cat5e 1` drop belonged to the deleted dosing node; the trunk now starts with the hub-to-battery-room run.

### 12.1. Transceiver Count — Resolved

The bus needs **4** XY-485 modules: one at the hub and one per remaining slave (`0x04`, `0x03`, `0x02`). An earlier revision of this section described deferring the RWT node because the design then called for five. Deleting node `0x01` (Section 13) removed the fifth. **Build the whole bus in one pass — there is no interim state and the 120 Ω lives at RWT `0x02` from the start.**

---

## 13. Dosing Tank Level — Direct to Hub (No RS485 Node)

The dosing tank sits roughly **100 cm** from the hub enclosure, well inside the AJ-SR04M's own captive transducer lead. A Nano, a buck converter, an XY-485 module, an enclosure and a 1.5 m Cat5e drop existed only to carry one distance reading across one metre. All of it is deleted; the sensor lands on the hub directly.

```
  ESP32-S Hub                        AJ-SR04M Module (at the hub)
 +------------------------+         +--------------------------------+
 | 5V (Buck Output)       +-------->| VCC   (5V - do NOT use 3V3)    |
 | GND                    +-------->| GND                            |
 | GPIO 5                 +-------->| TRIG                           |
 |                        |         |                                |
 | GPIO 4  <--+-- 1kΩ ----+---------+ ECHO  (5V logic - divide it)   |
 |            |                     +---------------+----------------+
 |           2kΩ                                    | 2.5 m coax
 | GND  <-----+                                     v
 +------------------------+                +--------------------+
                                           | Waterproof         |
   ECHO divider: 5.0V x 2k/(1k+2k)         | Transducer Head    |
              = 3.33V at GPIO 4            | (in dosing tank)   |
                                           +--------------------+
```

**AJ-SR04M checks apply here too** — `R19` mode pad empty, `TRIG` width tunable. See Section 9.0; a dosing sensor reading a flat zero is that pad, not the wiring.

**Why the divider is not optional.** `ECHO` idles low and swings to a hard 5 V. The ESP32 is 3.3 V logic with no 5 V tolerance on its GPIOs; a bare connection stresses the pad every ranging cycle. Same 1 kΩ / 2 kΩ arrangement as ground-floor node 1 (Section 11.1).

**Scheduling — do not read this sensor inside a poll window.** `pulseIn(GPIO4, HIGH, 35000UL)` blocks for up to 35 ms. Phase-B spec §4.1 documents this as fatal *on a Nano slave*, where a blocked `SoftwareSerial` silently drops ~15 % of incoming polls. On the hub the risk is different and milder: the hub is the master and slaves never transmit unbidden (`RS485_PROTOCOL.md` §1.1), so nothing is missed — but a 35 ms stall inside the poll/response sequence still eats into the turnaround guard time. Read it at a fixed point in the 1 s cycle, **after** the last slave response and before serving the dashboard. There is ~800 ms of idle cycle to put it in.

**What this removes from the build:** one Arduino Nano, one XY-485 transceiver, one Mini560 buck, one enclosure, one 1.5 m Cat5e drop, one RS485 address, and one more thing that can go offline. What it adds: two GPIOs and two resistors.
