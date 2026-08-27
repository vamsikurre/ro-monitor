# RO-Monitor: Industrial RO Plant Remote Telemetry & Monitoring System

A non-invasive, galvanically isolated telemetry, environmental, and multi-zone liquid level monitoring system for commercial/industrial Reverse Osmosis (RO) plants.

The system interfaces with an existing **Aster NXT 11** RO controller, distributed rooftop/battery-room storage and environmental nodes using an **ESP32-S Master Hub** over an **RS485 Daisy-Chain Bus**, and connects to **Ground Floor Sump and Motor Starter Nodes** over **Local Wi-Fi (LAN)**. It features a responsive **Local Web Dashboard** and native **ESP RainMaker Cloud** integration for iOS & Android mobile telemetry and push notifications.

---

## 1. System Architecture Overview

```
                                      +------------------------------------------+
                                      |              230V AC Mains               |
                                      +--------------------+---------------------+
                                                           |
                                                           v
                                               +-----------------------+
                                               |  Hi-Link HLK-20M12    |
                                               |  (12V DC / 20W Power) |
                                               +-----------+-----------+
                                                           |
                                                           | 12V DC Distributed Bus
                                                           v
 [120Ω Term Resistor]
+----------------------------------------------------------------------------------------------------+
|                                    ESP32-S CENTRAL HUB (RO ROOM)                                   |
|  - Microcontroller: ESP32-S (38-Pin)                                                               |
|  - RS485 Interface: XY-485 Hardware Auto-Flow Control Transceiver (UART2: GPIO16 RX / GPIO17 TX)   |
|  - High Voltage Isolation: 2x 220V AC Optocouplers (HPP Contactor & RWP Relay Active Monitoring)   |
|  - Low Voltage Isolation: 4-Channel PC817 Optocoupler Board (Aster Float, Relay & Alarm Contacts) |
|  - Dosing Tank Level: AJ-SR04M wired DIRECT to hub (GPIO 5 / GPIO 4 + divider) - no RS485 node    |
|  - Local Environmental: GY-SHT30-D Digital Temperature & Humidity Sensor (I2C: GPIO21 / GPIO22)    |
|  - 4-Channel Relay Board: TWT Float, RWT Float, Dosing Level Emulation                             |
|  - Connectivity: Local Web Dashboard Server & ESP RainMaker AWS IoT Agent                          |
+--------------------------------------------------+-------------------------------------------------+
                                                   |
                                                   | Cat5e 1 (Battery Room Outbound Blue Pair)
                                                   v
+--------------------------------------------------+-------------------------------------------------+
|                                  NODE 0x04: BATTERY ROOM CLIMATE & FAN                             |
|  - Microcontroller: Arduino Pro Mini (ATmega328P-AU, 5V / 16MHz) | Location: Battery Room         |
|  - Sensor: GY-SHT30-D Digital Temperature & Humidity Sensor                                        |
|  - Output: 1-Channel Relay Module (Exhaust Fan Control)                                            |
|  - Bus Position: MID-CHAIN - no termination resistor                                               |
+--------------------------------------------------+-------------------------------------------------+
                                                   |
                                                   | Cat5e 2 (Roof Top Outbound Blue Pair)
                                                   v
+--------------------------------------------------+-------------------------------------------------+
|                                  NODE 0x03: TREATED WATER TANK (TWT)                               |
|  - Microcontroller: Arduino Nano (ATmega328P) | Location: Roof Top                                 |
|  - Sensor: AJ-SR04M Waterproof Ultrasonic Sensor                                       |
+--------------------------------------------------+-------------------------------------------------+
                                                   |
                                                   | Cat5e 3 (Roof-to-roof Outbound Blue Pair)
                                                   v
+--------------------------------------------------+-------------------------------------------------+
|                                    NODE 0x02: RAW WATER TANK (RWT)                                 |
|  - Microcontroller: Arduino Nano (ATmega328P) | Location: Roof Top                                 |
|  - Sensor: AJ-SR04M Waterproof Ultrasonic Sensor                                       |
|  - Bus Termination: 120Ω End-of-Bus Resistor                                                       |
+----------------------------------------------------------------------------------------------------+

                                                   ▲
                                                   │ (Wi-Fi 2.4GHz LAN)
                                                   ▼
======================================================================================================
                                    GROUND FLOOR PARKING ECOSYSTEM
======================================================================================================
+--------------------------------------------------+-------------------------------------------------+
|                         ESP32 NODE 1: GROUND SUMP TELEMETRY (0x05)                                 |
|  - Microcontroller: ESP32-WROOM-32 / ESP32-C3                                                      |
|  - Sensor: AJ-SR04M Ultrasonic Sensor (3.5m Deep Sump Pit)                                        |
|  - Power: Hi-Link HLK-20M5 (240V AC -> 5V DC 4A)                                                   |
+--------------------------------------------------+-------------------------------------------------+
                                                   | (Wi-Fi 2.4GHz LAN)
                                                   v
+--------------------------------------------------+-------------------------------------------------+
|                   ESP32 NODE 2: MOTOR MONITORING & STARTER CONTROL (0x06)                          |
|  - Microcontroller: ESP32-WROOM-32 / ESP32-C3                                                      |
|  - Power: Hi-Link HLK-20M5 (240V AC -> 5V DC 4A)                                                   |
|  - Inputs: 2x 220V AC Optocouplers (Sump Motor Active & Borewell Motor Active)                     |
|  - Outputs: 4-Channel Relay Board (Sump Float & Borewell Float Starter Cutoff / Interlock)        |
+----------------------------------------------------------------------------------------------------+
```

---

## 2. Process Flow & Dashboard Architecture

The system telemetry directly visualizes the plant's water balance and environmental status:

```
[Borewell Pump] ──> [Ground Sump (3.5m)] ──(Sump Motor)──> [RWT (Roof)] ──> [RO Plant: RWP + Dosing + HPP] ──> [TWT (Roof)]
                                                                               │
                                                                               └── [Battery Room: SHT30 + Exhaust Fan]
```

### Local Web Dashboard Features:
- **Zero-Cloud LAN Access:** Hosted directly on the local network (accessible at `http://ro-hub.local` or Hub IP).
- **Animated Liquid Graphic:** Live animated fluid levels for Sump, RWT, Dosing, and TWT tanks.
- **Dynamic Motor Indicators:** Real-time running/idle status for Borewell Pump, Sump Motor, Raw Water Pump (RWP), and High Pressure Pump (HPP).
- **Climate & Ventilation Cards:** RO Room & Battery Room Temperature/Humidity gauges with Exhaust Fan auto/manual controls.

### ESP RainMaker Mobile Cloud Integration:
- **Native iOS & Android App:** Remote monitoring from anywhere in the world.
- **Push Notification Alerts:**
  - `CRITICAL:` Ground Sump Low (< 15%) / Dry Run Risk.
  - `WARNING:` Tank Overflow Risk (Sump/RWT/TWT > 95%).
  - `ALERT:` High Battery Room Temperature (> 38°C) $\to$ Automated Exhaust Fan Trigger.
  - `FAULT:` RO Controller Alarm Relay Activated.

---

## 3. Node Addressing & Telemetry Matrix

| Node ID | Subsystem | Transport | Hardware Platform | Primary Sensors & Actuators |
| :---: | :--- | :--- | :--- | :--- |
| `0x00` | **RO Room (Central Hub)** | Core Master | ESP32-S (38-Pin) | SHT30 (RO Room), AJ-SR04M (Dosing, direct), 2x AC Optos, 4x DC Optos, 4-Ch Relay Board |
| — | **RO Room (Dosing)** | *Direct to hub* | — | AJ-SR04M on hub GPIO 5 / GPIO 4 (address `0x01` retired) |
| `0x02` | **Roof Top (RWT)** | RS485 Bus | Arduino Nano | AJ-SR04M (Raw Water Tank Level) + 120Ω End-of-Bus Term |
| `0x03` | **Roof Top (TWT)** | RS485 Bus | Arduino Nano | AJ-SR04M (Treated Water Tank Level) |
| `0x04` | **Battery Room** | RS485 Bus | Arduino Pro Mini | GY-SHT30-D (Battery Room Temp/RH) + 1-Ch Exhaust Fan Relay (mid-chain, no term) |
| `0x05` | **Ground Floor (Sump)** | Wi-Fi LAN | ESP32 | AJ-SR04M (3.5m Sump Level Sensor) |
| `0x06` | **Ground Floor (Motors)**| Wi-Fi LAN | ESP32 | 2x AC Optos (Sump/Borewell Motors), 4-Ch Relay Board |

---

## 4. Build Phases

The project is built **by site**, terrace first, then ground floor. Each phase is a system that works on its own.

| | Phase 1 — Terrace | Phase 2 — Ground floor |
| :--- | :--- | :--- |
| **Scope** | RO room, battery room, roof tanks | Sump, borewell, starter panel |
| **Nodes** | Hub `0x00`, RWT `0x02`, TWT `0x03`, Battery Room `0x04` | Sump `0x05`, Starter panel `0x06` |
| **Sensing** | 3 tank levels, 2 room climates, Aster contacts, HPP + RWP current | Sump level, sump + borewell current, borewell dry-run |
| **Status** | Node firmware done and bench-tested; hub bring-up in progress | Not started; clamps in hand |

**One exception crosses the line.** The sump **dry-run float** (`superpowers/specs/…-phase-b-design.md` §1.1) is ground-floor work that should be fitted during Phase 1. It needs no firmware and no node — a float, some cable, an afternoon — and until it exists the sump motor can still run dry, which is the failure this project was started to prevent. Terrace-first would otherwise leave that pump unprotected for the whole of Phase 1.

---

## 5. Firmware

| Firmware | Board | Toolchain | Notes |
| :--- | :--- | :--- | :--- |
| `firmware/ro_node/` | Nano ×2, Pro Mini ×1 | Arduino | **One binary for all three RS485 nodes.** The `A0`/`A1` jumpers pick the address *and* the personality: `0x02`/`0x03` ultrasonic, `0x04` climate + fan. Nothing to edit per board. |
| `firmware/hub_prod/` | ESP32-S hub | **ESP-IDF** | **Production hub.** RS485 master, local dashboard, password-protected calibration page, ESP RainMaker cloud with push alerts, and OTA updates from the RainMaker dashboard. Built the same way as `gate-controller`, so one update workflow covers both. See `firmware/hub_prod/README.md`. |
| `firmware/esp32_hub_test/` | ESP32-S hub | Arduino | **Bench self-test.** Cycles all five relays, injects nothing, prints raw millivolts per channel per cycle over USB at 115200. Kept deliberately: commissioning a board and running a plant are different jobs. |

**Flashing the nodes:** jumper first, flash second, then confirm the boot print
(`Node ID: 0x02 … role: RWT ultrasonic`) before putting the board on the bus. Both
jumpers grounded is the deliberate "unassigned" code — the node blinks and stays off the
bus rather than guessing an address. Pro Mini: `Arduino Pro or Pro Mini` / *ATmega328P
(5V, 16 MHz)*, `DTR` wired, and pull the buck's 5 V while the FTDI adapter is connected.

**Production hub, in one block:**

```
cd firmware/hub_prod
idf.py set-target esp32 && idf.py build && idf.py -p COM5 flash monitor
```

Pair from the ESP RainMaker app with proof-of-possession `rohub1234`, over BLE.
After that the dashboard is at `http://ro-hub.local/`, calibration at `/cal`
(user `admin`), and firmware updates are pushed from the RainMaker dashboard —
no cable, exactly like the gate controllers. The hub keeps its own AP up at the
same time, so calibration still works on a roof with no router.

**Calibration is hub-side and needs no cable.** Nodes report millimetres; the hub turns
them into percentages. On the bench sketch, join the hub's Wi-Fi AP (`RO-HUB`) and open `http://192.168.4.1/`
— every tank has a full and an empty distance, and a **Set full = now** button that
captures whatever the sensor reads at that moment. Fill the tank, tap it; drain it, tap
**Set empty = now**. Values persist in NVS across reboots and reflashes of the nodes.
`GET /api/cal` returns the same thing as JSON. Until a tank is calibrated its level
reads `--`, never a plausible wrong number.

**The battery room fan thresholds live on the same page.** The hub reads the room
temperature from node `0x04`, applies its own hysteresis and commands the relay, so
changing the setpoint is a phone, not a programmer on a ladder. Accepted range is
25.0–55.0 °C with ON at least 1.0 °C above OFF. If the hub goes quiet for five
minutes the node falls back to its own hotter backstop (40.0 / 37.0 °C) and keeps
ventilating regardless.

**Checks** (plain Python, run from the repo root — no framework, no CI):

```
python docs/check_addrmap.py   # jumper table agrees across docs and firmware
python docs/check_pinmap.py    # GPIO allocation agrees across 3 docs AND app_priv.h
python docs/check_frame.py     # node, bench hub and production hub agree on
                               # CRC-16, framing and level maths
python docs/check_telemetry.py # /api/telemetry is valid JSON and carries every
                               # key the dashboard reads - it falls back to its
                               # demo simulator on any error, so a broken
                               # contract looks like a working page
```

**Compile both before flashing** — `arduino-cli` ships inside the Arduino IDE install
(`.../Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe`):

```
arduino-cli compile -b arduino:avr:nano  firmware/ro_node
arduino-cli compile -b esp32:esp32:esp32 firmware/esp32_hub_test
cd firmware/hub_prod && idf.py build          # the production hub
```

Each sketch also carries a `#error` guard for the wrong target, so selecting the ESP32
for `ro_node` tells you that in one line instead of failing inside `SoftwareSerial.h`.

---

## 6. Detailed Documentation Index

For in-depth technical documentation, refer to the guides in the `docs/` folder:
- **[Hardware Specifications & Pin Allocations](docs/HARDWARE.md)**
- **[Terminal-by-Terminal Wiring Guide](docs/WIRING.md)**
- **[RS485 & Wi-Fi Master-Slave Protocol Specification](docs/RS485_PROTOCOL.md)**
- **[Power Budget & Voltage Drop Calculations](docs/POWER_BUDGET.md)**
- **[Local Web Dashboard & ESP RainMaker Integration Guide](docs/DASHBOARD_AND_RAINMAKER.md)**
- **[Skid Hardware Reverse Engineering Analysis](docs/RO_HARDWARE_ANALYSIS.md)**

