# RO-Monitor: Industrial RO Plant Remote Telemetry & Monitoring System

A non-invasive, galvanically isolated telemetry and liquid level monitoring system for commercial/industrial Reverse Osmosis (RO) plants.

The system interfaces with an existing **Aster NXT 11** RO controller and distributed storage tanks using an **ESP32-S Master Hub** communicating over a strictly sequenced **RS485 Daisy-Chain Bus** to **Arduino Nano** remote ultrasonic sensor nodes.

---

## 1. System Architecture

```
                                +------------------------------------------+
                                |               230V AC Mains              |
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
|                                         ESP32-S CENTRAL HUB                                        |
|  - Microcontroller: ESP32-S (38-Pin)                                                               |
|  - RS485 Interface: XY-485 Hardware Auto-Flow Control Transceiver (UART2: GPIO16 RX / GPIO17 TX)   |
|  - High Voltage Isolation: 2x 220V AC Optocouplers (HPP Contactor & RWP Relay Active Monitoring)   |
|  - Low Voltage Isolation: 8-Channel PC817 Optocoupler Board (Aster Float & Relay Dry Contacts)     |
|  - Local Environmental: GY-SHT30-D Digital Temperature & Humidity Sensor (I2C: GPIO21 / GPIO22)    |
|  - Power: LM2596 / Mini560 12V -> 5V Buck Converter                                                |
+--------------------------------------------------+-------------------------------------------------+
                                                   |
                                                   | (Internal Link)
                                                   v
+--------------------------------------------------+-------------------------------------------------+
|                                    NODE 0x01: DOSING CHEMICAL TANK                                 |
|  - Microcontroller: Arduino Nano (ATmega328P)                                                      |
|  - Sensor: JSN-SR04T / AJ-SR04M Waterproof Ultrasonic Sensor (Trig: D7, Echo: D8)                  |
|  - RS485 Interface: XY-485 Module (SoftwareSerial: D2 RX / D3 TX)                                  |
|  - Power: 12V -> 5V Buck Converter                                                                 |
+--------------------------------------------------+-------------------------------------------------+
                                                   |
                                                   | CAT5e Cable 1 (Outbound Differential Pair)
                                                   v
+--------------------------------------------------+-------------------------------------------------+
|                                    NODE 0x02: RAW WATER TANK (RWT)                                 |
|  - Microcontroller: Arduino Nano (ATmega328P)                                                      |
|  - Sensor: JSN-SR04T / AJ-SR04M Waterproof Ultrasonic Sensor (Trig: D7, Echo: D8)                  |
|  - RS485 Interface: XY-485 Module                                                                  |
|  - Wiring Feature: Physical Return-Loopback (Outbound Blue Pair -> Return Brown Pair)              |
+--------------------------------------------------+-------------------------------------------------+
                                                   |
                                                   | CAT5e Cable 1 (Return Brown Pair)
                                                   v
+--------------------------------------------------+-------------------------------------------------+
|                                        CENTRAL JUNCTION BOX                                        |
|  - Pass-Through Splice: Cable 1 Return Pair -> Cable 2 Outbound Blue Pair                          |
+--------------------------------------------------+-------------------------------------------------+
                                                   |
                                                   | CAT5e Cable 2 (Outbound Differential Pair)
                                                   v
+--------------------------------------------------+-------------------------------------------------+
|                                  NODE 0x03: TREATED WATER TANK (TWT)                               |
|  - Microcontroller: Arduino Nano (ATmega328P)                                                      |
|  - Sensor: JSN-SR04T / AJ-SR04M Waterproof Ultrasonic Sensor (Trig: D7, Echo: D8)                  |
|  - RS485 Interface: XY-485 Module                                                                  |
|  - Bus Termination: 120Ω End-of-Bus Resistor                                                       |
+----------------------------------------------------------------------------------------------------+
```

---

## 2. Existing Plant Equipment & Reverse Engineering

The monitoring system interfaces passively with the following hardware identified on the RO skid:

| Equipment Component | Identified Model / Marking | Role & Specifications |
| :--- | :--- | :--- |
| **Main RO Controller** | **Aster NXT 11** (`ASTERO-LTE-CPU-22-VER-1.5`) | Microprocessor-based automated RO plant controller (Hongfa `HF3FF` relays, `LM324L` op-amp, `TC7660` charge pump for AC conductivity excitation, `ULN2003AN` relay driver). |
| **HPP Motor Contactor** | **TC Contactor TCDP302** | Definite purpose contactor (240V AC 50Hz coil, 30A FLA / 150A LRA) driving the High-Pressure Pump. |
| **Filter Valve Controller** | **Initiative Engineering** Auto Multiport Valve | Automated sand/carbon media filter backwash cycling. |
| **Flow Measurement** | **Aster F-1200 LG** Rotameter | 200–1200 LPH (4–20 LPM) vertical flow tube. |
| **Membrane Vessel** | **Alfa Aerosol** FRP Pressure Vessel | NSF/ANSI/CAN-61 certified RO membrane housing. |

---

## 3. Communication Protocol Summary

- **Physical Layer:** Half-duplex RS485 over CAT5e twisted pair, 9600 bps, 8-N-1.
- **Topology:** Return-loopback strictly linear daisy-chain (eliminates star-topology reflections when tanks are situated in opposite directions from the central skid).
- **Bus Master:** ESP32-S Hub (`0x00`) polls slaves sequentially every 1000 ms.
- **Frame Format:**
  `[0xAA] [0x55] [NODE_ADDR] [COMMAND] [PAYLOAD_LEN] [PAYLOAD (0..32 bytes)] [CRC16_LOW] [CRC16_HIGH]`
- **CRC:** Standard Modbus CRC-16 polynomial (`0xA001`).

---

## 4. Astero Documentation & Reference Findings

The repository includes the official documentation PDF for the Astero controller in [`docs/ASTERO-NXTG-1_opt.pdf`](docs/ASTERO-NXTG-1_opt.pdf). 

### Additional Technical Findings from Online Research:
* **Manufacturer & Ecosystem:** The *Aster* / *Astero* product lines are manufactured and distributed in India by **Aster Technologies** ([astertechnologies.in](https://astertechnologies.in)) and **Embark Water** ([embarkwater.com](https://embarkwater.com)), and distributed via **Filtra Consultants and Engineers Ltd.** ([filtra.in](https://filtra.in)).
* **Model Families:**
  - **Astero NXT / Astero NXTG Series:** Next-generation RO / UF control panels featuring 16x2 backlit LCDs, multi-color status alerts, dry-run/overload motor protections, and automated flushing/backwash routines.
  - **Astero Lite / ASTERO-LTE:** Compact plug-and-play board variants (such as the `ASTERO-LTE-CPU-22-VER-1.5` installed on this plant) designed for space-efficient integration.
  - **Astero RM Series:** Remote-monitoring enabled variants with integrated RS-485 / GSM-GPRS telemetry ports.
* **Terminal Signals:**
  - Inputs: `HPS` (High Pressure Switch), `LPS` (Low Pressure Switch), `TWT FLOTY` (Treated Water Float), `RWT FLOTY` (Raw Water Float), `DOS LVL` (Dosing Level), `COND` (AC Conductivity Probe), `FLOW 1 / 2` (Pulse flow sensors).
  - Outputs: `RL1` (High-Pressure Pump control via contactor), `RL2` (Raw Water Pump / Flush Solenoid), `ALARM` (Buzzer / Warning indicator).

---

## 5. Repository Structure

```
ro-monitor/
├── .gitignore                     # Git ignore rules for build, IDE, OS, and Draw.io files
├── README.md                      # Project overview and system specification
├── architecture.txt               # ASCII network and electrical daisy-chain diagram
├── power_architecture.txt         # DC power distribution hierarchy
├── plan.txt                       # Detailed engineering rollout plan
├── wiring.drawio                  # Draw.io full electrical wiring schematic
├── docs/                          # Detailed engineering documentation
│   ├── ASTERO-NXTG-1_opt.pdf       # Astero NXTG-1 controller user documentation
│   ├── HARDWARE.md                # Bill of materials, pinouts, and hardware specifications
│   ├── POWER_BUDGET.md            # Power calculations, voltage drop, and thermal analysis
│   ├── RO_HARDWARE_ANALYSIS.md    # Image-by-image reverse engineering analysis
│   ├── RS485_PROTOCOL.md          # Complete binary packet spec, commands, and CRC-16
│   └── WIRING.md                  # Comprehensive terminal-by-terminal wiring schedule
└── images/                        # High-resolution photographic evidence and diagrams
    ├── architecture.png           # Visual topology diagram
    ├── hardware/                  # Module close-up photos (optos, power supplies, RS485)
    └── IMG_*.JPG / PNG            # 19 plant inspection photographs
```

---

## 6. Detailed Documentation Index

For in-depth technical documentation, refer to the guides in the `docs/` folder:
- **[Hardware Specifications & Pin Allocations](docs/HARDWARE.md)**
- **[Terminal-by-Terminal Wiring Guide](docs/WIRING.md)**
- **[RS485 Master-Slave Protocol Specification](docs/RS485_PROTOCOL.md)**
- **[Power Budget & Voltage Drop Calculations](docs/POWER_BUDGET.md)**
- **[Skid Hardware Reverse Engineering Analysis](docs/RO_HARDWARE_ANALYSIS.md)**
