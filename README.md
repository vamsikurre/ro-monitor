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

## 4. Detailed Documentation Index

For in-depth technical documentation, refer to the guides in the `docs/` folder:
- **[Hardware Specifications & Pin Allocations](docs/HARDWARE.md)**
- **[Terminal-by-Terminal Wiring Guide](docs/WIRING.md)**
- **[RS485 & Wi-Fi Master-Slave Protocol Specification](docs/RS485_PROTOCOL.md)**
- **[Power Budget & Voltage Drop Calculations](docs/POWER_BUDGET.md)**
- **[Local Web Dashboard & ESP RainMaker Integration Guide](docs/DASHBOARD_AND_RAINMAKER.md)**
- **[Skid Hardware Reverse Engineering Analysis](docs/RO_HARDWARE_ANALYSIS.md)**

