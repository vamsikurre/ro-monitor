# Hardware Specifications & Pin Allocations

**Document Version:** 1.0  
**Date:** 2026-08-19  
**System Architecture:** ESP32 HUB (Master) + 3x Arduino Nano Remote Tank Nodes (Slaves)

---

## 1. System Topology Overview (Return-Loopback Daisy-Chain Bus)

```
 [120Ω Term Resistor]
┌────────────────────┐
│    ESP32-S HUB     │ (Bus Master, 0x00)
│   XY-485 Master    │
└─────────┬──────────┘
          │ (Local link)
          ▼
┌────────────────────┐
│    DOSING NODE     │ (Slave Node 0x01)
│    XY-485 Auto     │
└─────────┬──────────┘
          │
          │ CAT5e Cable 1: Outbound Pair (Pins 4 & 5: Blue / White-Blue)
          ▼
┌────────────────────┐
│      RWT NODE      │ (Slave Node 0x02, Raw Water Tank)
│    XY-485 Auto     │
└─────────┬──────────┘
          │
          │ (Loopback Jumper at RWT: Blue -> Brown, White-Blue -> White-Brown)
          │
          │ CAT5e Cable 1: Return Pair (Pins 7 & 8: Brown / White-Brown)
          ▼
┌────────────────────┐
│CENTRAL JUNCTION BOX│ (Pass-through Splice: Return Pair -> Cable 2 Outbound Pair)
└─────────┬──────────┘
          │
          │ CAT5e Cable 2: Outbound Pair (Pins 4 & 5: Blue / White-Blue)
          ▼
┌────────────────────┐
│      TWT NODE      │ (Slave Node 0x03, Treated Water Tank - End of Bus)
│    XY-485 Auto     │
│ [120Ω Term Resistor│
└────────────────────┘
```

> **Strict Daisy-Chain Guarantee:** By looping the RS485 differential pair through the Brown pair of Cable 1, the electrical signal flows through every single node in a 100% linear sequence without branching or stubs. Only two 120Ω termination resistors are used: one at the ESP32 Hub (Start) and one at the TWT Node (End).

---

## 2. Verified Purchased Bill of Materials (BOM)

| Item Description | Model / SKU | Qty Purchased | Primary Role |
| :--- | :--- | :---: | :--- |
| **Main Power Supply** | Hi-Link `HLK-20M12` (12V / 20W / 1.6A) | 1 | Master 12V DC power distribution |
| **Auxiliary Supply** | Hi-Link `HLK-10M05` (5V / 10W / 2.0A) | 1 | Spare / Local 5V high-current logic rail |
| **RS485 Transceiver** | `XY-485` (TTL to RS485 Auto-Flow Control) | Multi | Hardware auto TX/RX direction switching (3.3V/5V compatible) |
| **220V AC Opto Isolator**| 1-Channel 220V AC Optocoupler Isolation Module | 4 | Galvanic isolation for 240V AC HPP / RWP status |
| **DC Dry-Contact Opto** | 8-Channel PC817 Optocoupler Isolation Board | 1 | Galvanic isolation for Aster controller dry switches |
| **Ultrasonic Sensors** | Waterproof Ultrasonic Obstacle Sensor (JSN-SR04T) | 4 | Non-contact liquid level sensing (Dosing, RWT, TWT, +1 spare) |
| **Environmental Sensor**| GY-SHT30-D Digital Temperature & Humidity | 1 | Ambient enclosure / pump-room monitoring |

---

## 3. ESP32-S HUB Pin Allocations (38-Pin Module)

Since the purchased `XY-485` transceiver features **hardware automatic flow control**, manual `DE/~RE` direction pin toggling is eliminated, saving GPIOs and simplifying firmware timing.

| ESP32 GPIO | Pin Function | Connected Hardware / Circuit | Direction | Electrical Interface / Notes |
| :--- | :--- | :--- | :--- | :--- |
| **GPIO 16** | `RS485_RX` | XY-485 Module `RXD` | Input | 3.3V UART2 RX |
| **GPIO 17** | `RS485_TX` | XY-485 Module `TXD` | Output | 3.3V UART2 TX |
| **GPIO 21** | `I2C_SDA` | GY-SHT30-D `SDA` | Bidirectional | I2C SDA (with 4.7kΩ pull-up to 3.3V) |
| **GPIO 22** | `I2C_SCL` | GY-SHT30-D `SCL` | Output | I2C SCL (Standard 100kHz) |
| **GPIO 34** | `IN_HPP_AC` | 220V AC Optocoupler Module 1 `OUT` | Input (GPI) | Active LOW when 240V HPP is energized |
| **GPIO 35** | `IN_RWP_AC` | 220V AC Optocoupler Module 2 `OUT` | Input (GPI) | Active LOW when 240V RWP is energized |
| **GPIO 32** | `IN_TWT_FLOT` | 8-Ch PC817 Board Ch 1 `OUT` | Input | Treated Water Float Switch Monitor |
| **GPIO 33** | `IN_RWT_FLOT` | 8-Ch PC817 Board Ch 2 `OUT` | Input | Raw Water Float Switch Monitor |
| **GPIO 25** | `IN_DOS_LVL` | 8-Ch PC817 Board Ch 3 `OUT` | Input | Dosing Tank Low Level Switch Monitor |
| **GPIO 26** | `IN_RL1_STAT` | 8-Ch PC817 Board Ch 4 `OUT` | Input | Aster RL1 Relay Contact Monitor |
| **GPIO 27** | `IN_RL2_STAT` | 8-Ch PC817 Board Ch 5 `OUT` | Input | Aster RL2 Relay Contact Monitor |
| **GPIO 2** | `LED_STATUS` | System Health / Heartbeat LED | Output | Active HIGH (Built-in onboard LED) |
| **GPIO 0** | `BTN_BOOT` | Factory Reset / AP Mode Button | Input | Active LOW (Pull-up) |

---

## 4. Arduino Nano Tank Node Pin Allocations

| Nano Pin | Pin Function | Connected Hardware / Circuit | Direction | Electrical Interface / Notes |
| :--- | :--- | :--- | :--- | :--- |
| **D2** | `RS485_RX` | XY-485 Module `RXD` | Input | SoftwareSerial / AltSoftSerial RX |
| **D3** | `RS485_TX` | XY-485 Module `TXD` | Output | SoftwareSerial / AltSoftSerial TX |
| **D7** | `US_TRIG` | JSN-SR04T Sensor `TRIG` | Output | 10µs trigger pulse (5V logic) |
| **D8** | `US_ECHO` | JSN-SR04T Sensor `ECHO` | Input | Pulse width proportional to distance |
| **A0** | `ADDR_SEL0` | Hardware Node Address Bit 0 | Input | DIP Switch or solder jumper (Pull-up) |
| **A1** | `ADDR_SEL1` | Hardware Node Address Bit 1 | Input | DIP Switch or solder jumper (Pull-up) |
| **D13** | `LED_STATUS`| Status / RS485 Activity LED | Output | Nano onboard LED (Blinks on poll response) |

---

## 4. Peripheral Components Specification

### 4.1. RS485 Transceivers
- **Transceiver IC:** Maxim `MAX485ESA` or `MAX13487EESA` (Half-Duplex RS485).
- **Termination:** 120Ω resistor at the extreme ends of the bus (ESP32 Hub and furthest Tank Node).
- **Biasing:** 4.7kΩ pull-up on RS485 A line to +5V / 3.3V, 4.7kΩ pull-down on RS485 B line to GND to ensure stable idle state.

### 4.2. Ultrasonic Sensors
- **Model:** JSN-SR04T 2.0 / 3.0 or AJ-SR04M waterproof transceiver probe.
- **Operating Voltage:** 5V DC (supplied from local node buck converter).
- **Blind Zone (Dead Band):** 20 cm (firmware must flag readings `< 20 cm` as overflow / sensor saturation).
- **Maximum Range:** 450 cm (4.5 meters).
- **Resolution:** 1 mm (hardware timing precision ~3mm).

### 4.3. SHT30 Environmental Sensor
- **Interface:** Standard I2C (Address `0x44` default or `0x45`).
- **Operating Voltage:** 3.3V DC.
- **Accuracy:** ±2% RH, ±0.2°C.

---

## 5. Power Architecture

1. **Mains Supply:** 230V AC (50Hz) connected via input protection fuse (`BLX-A` / 1A slow-blow) to **Hi-Link HLK-20M12** (12V DC, 20W, 1.67A).
2. **HUB Local Step-Down:** High-efficiency DC-DC Buck converter (e.g. MP1584EN / LM2596) stepping down 12V DC to 5.0V DC for ESP32 and peripheral sensors.
3. **CAT5e Power Distribution:** Clean 12V DC is routed through dedicated CAT5e twisted pairs to each remote tank node, avoiding high line-drop current at 5V.
4. **Remote Node Step-Down:** Each tank node incorporates an onboard mini buck converter (12V -> 5V) powering the Arduino Nano and JSN-SR04T sensor.
