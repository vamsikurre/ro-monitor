# Hardware Specifications & Pin Allocations

**Document Version:** 2.0  
**Date:** 2026-08-24  
**System Architecture:** ESP32 HUB (Master) + 4x RS485 Arduino Nano Nodes + 2x Ground Floor Wi-Fi ESP32 Nodes

---

## 1. System Topology Overview

```
 [120Ω Term Resistor]
┌────────────────────┐
│  ESP32-S HUB (0x00)│ (RO Room Bus Master)
│   XY-485 Master    │
└─────────┬──────────┘
          │ (Local link - 1.5m Cat5e 1)
          ▼
┌────────────────────┐
│ DOSING NODE (0x01) │ (RO Room - Dosing Chemical Tank)
│    XY-485 Auto     │
└─────────┬──────────┘
          │
          │ Cat5e 2: Outbound Pair (Pins 4 & 5: Blue / White-Blue - 5m)
          ▼
┌────────────────────┐
│  RWT NODE (0x02)   │ (Roof Top - Raw Water Tank)
│    XY-485 Auto     │
└─────────┬──────────┘
          │
          │ (Loopback Jumper at RWT: Blue -> Brown, White-Blue -> White-Brown)
          │
          │ Cat5e 2: Return Pair (Pins 7 & 8: Brown / White-Brown)
          ▼
┌────────────────────┐
│CENTRAL JUNCTION BOX│ (RO Room Pass-through Splice: Return Pair -> Cat5e 3 Outbound Pair)
└─────────┬──────────┘
          │
          │ Cat5e 3: Outbound Pair (Pins 4 & 5: Blue / White-Blue - 10m)
          ▼
┌────────────────────┐
│  TWT NODE (0x03)   │ (Roof Top - Treated Water Tank)
│    XY-485 Auto     │
└─────────┬──────────┘
          │
          │ Cat5e 4: Extension Outbound Pair
          ▼
┌────────────────────┐
│BATTERY ROOM (0x04) │ (Battery Room - SHT30 Climate & Exhaust Fan)
│    XY-485 Auto     │
│ [120Ω Term Resistor│
└────────────────────┘

========================================================================================
GROUND FLOOR PARKING SUBSYSTEM (WI-FI LAN)
========================================================================================
┌───────────────────────────────┐        ┌─────────────────────────────────────────┐
│     ESP32 NODE 1 (0x05)       │        │           ESP32 NODE 2 (0x06)           │
│  - 3.5m Deep Sump Level       │        │  - Sump & Borewell 240V AC Monitoring   │
│  - JSN-SR04T Sensor           │        │  - 4-Ch Relay Motor Starter Interlock   │
│  - Hi-Link HLK-20M5 (5V DC)   │        │  - Hi-Link HLK-20M5 (5V DC)             │
└───────────────────────────────┘        └─────────────────────────────────────────┘
```

---

## 2. Complete Bill of Materials (BOM)

| Item Description | Model / SKU | Qty | Location / Role |
| :--- | :--- | :---: | :--- |
| **Main RO Bus Power Supply** | Hi-Link `HLK-20M12` (12V / 20W / 1.6A) | 1 | RO Room - Master 12V DC RS485 bus power |
| **Ground Floor Power Supplies**| Hi-Link `HLK-20M5` (5V / 20W / 4.0A) | 2 | Ground Floor - Node 1 & Node 2 dedicated 5V power |
| **Auxiliary Supply** | Hi-Link `HLK-10M05` (5V / 10W / 2.0A) | 1 | Spare / Local 5V high-current logic rail |
| **RS485 Transceiver** | `XY-485` (Auto-Flow Control) | 5 | Hardware auto TX/RX direction switching (1x Hub, 4x Nano) |
| **220V AC Opto Isolator** | 1-Channel 220V AC Optocoupler Module | 4 | 2x RO Room (HPP/RWP) + 2x Ground Floor (Sump/Borewell) |
| **DC Dry-Contact Opto** | 8-Channel PC817 Optocoupler Board | 1 | RO Room - Aster controller dry switch isolation |
| **Relay Modules** | 4-Channel 5V Relay Board | 2 | 1x RO Room (Aster Float Emulation) + 1x Ground Floor (Starter Control) |
| **Relay Module (Single)** | 1-Channel 5V Relay Board | 1 | Battery Room (Exhaust Fan Switching) |
| **Ultrasonic Sensors** | Waterproof Ultrasonic (JSN-SR04T) | 4 | Dosing (`0x01`), RWT (`0x02`), TWT (`0x03`), Ground Sump (`0x05`) |
| **Environmental Sensors** | GY-SHT30-D Digital Temp & Humidity | 2 | 1x RO Room (Hub I2C) + 1x Battery Room (Node 4 I2C) |
| **Microcontrollers** | ESP32-S / ESP32-WROOM-32 | 3 | 1x Central Hub + 2x Ground Floor Nodes |
| **Microcontrollers** | Arduino Nano (ATmega328P) | 4 | 4x RS485 Slave Nodes |

---

## 3. Pin Allocations by Subsystem

### 3.1. ESP32-S Central Hub Pin Allocations (RO Room)
| ESP32 GPIO | Pin Function | Connected Hardware / Circuit | Direction | Interface Notes |
| :--- | :--- | :--- | :--- | :--- |
| **GPIO 16** | `RS485_RX` | XY-485 Module `RXD` | Input | 3.3V UART2 RX |
| **GPIO 17** | `RS485_TX` | XY-485 Module `TXD` | Output | 3.3V UART2 TX |
| **GPIO 21** | `I2C_SDA` | GY-SHT30-D `SDA` (RO Room) | Bidirectional | I2C SDA (4.7kΩ pull-up to 3.3V) |
| **GPIO 22** | `I2C_SCL` | GY-SHT30-D `SCL` (RO Room) | Output | I2C SCL (100kHz standard) |
| **GPIO 34** | `IN_HPP_AC` | 220V AC Optocoupler Module 1 `OUT` | Input | Active LOW when HPP 240V contactor active |
| **GPIO 35** | `IN_RWP_AC` | 220V AC Optocoupler Module 2 `OUT` | Input | Active LOW when RWP 240V active |
| **GPIO 32** | `IN_TWT_FLOT` | 8-Ch PC817 Board Ch 1 `OUT` | Input | Aster TWT Float Contact Monitor |
| **GPIO 33** | `IN_RWT_FLOT` | 8-Ch PC817 Board Ch 2 `OUT` | Input | Aster RWT Float Contact Monitor |
| **GPIO 25** | `IN_DOS_LVL` | 8-Ch PC817 Board Ch 3 `OUT` | Input | Aster Dosing Level Contact Monitor |
| **GPIO 26** | `IN_RL1_STAT` | 8-Ch PC817 Board Ch 4 `OUT` | Input | Aster RL1 Relay Contact Monitor |
| **GPIO 27** | `IN_RL2_STAT` | 8-Ch PC817 Board Ch 5 `OUT` | Input | Aster RL2 Relay Contact Monitor |
| **GPIO 18** | `OUT_RLY_TWT` | 4-Ch Relay Module IN1 | Output | TWT Float Switch Emulation |
| **GPIO 19** | `OUT_RLY_RWT` | 4-Ch Relay Module IN2 | Output | RWT Float Switch Emulation |
| **GPIO 23** | `OUT_RLY_DOS` | 4-Ch Relay Module IN3 | Output | Dosing Level Switch Emulation |
| **GPIO 2** | `LED_STATUS` | System Heartbeat LED | Output | Active HIGH onboard LED |

---

### 3.2. Arduino Nano Tank Nodes Pin Allocations (Nodes 0x01, 0x02, 0x03)
| Nano Pin | Pin Function | Connected Hardware | Direction | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **D2** | `RS485_RX` | XY-485 Module `RXD` | Input | SoftwareSerial RX |
| **D3** | `RS485_TX` | XY-485 Module `TXD` | Output | SoftwareSerial TX |
| **D7** | `US_TRIG` | JSN-SR04T Sensor `TRIG` | Output | 10µs trigger pulse |
| **D8** | `US_ECHO` | JSN-SR04T Sensor `ECHO` | Input | Echo pulse width |
| **A0** | `ADDR_SEL0` | Hardware Node Address Bit 0 | Input | Pull-up jumper |
| **A1** | `ADDR_SEL1` | Hardware Node Address Bit 1 | Input | Pull-up jumper |
| **D13** | `LED_STATUS` | Status / RS485 Activity | Output | Blinks on successful poll |

---

### 3.3. Arduino Nano Battery Room Node (Node 0x04)
| Nano Pin | Pin Function | Connected Hardware | Direction | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **D2** | `RS485_RX` | XY-485 Module `RXD` | Input | SoftwareSerial RX |
| **D3** | `RS485_TX` | XY-485 Module `TXD` | Output | SoftwareSerial TX |
| **A4** | `I2C_SDA` | GY-SHT30-D `SDA` | Bidirectional | Hardware I2C SDA |
| **A5** | `I2C_SCL` | GY-SHT30-D `SCL` | Output | Hardware I2C SCL |
| **D9** | `OUT_FAN_RLY` | 1-Channel Relay Board `IN` | Output | Active LOW Exhaust Fan control |
| **D13** | `LED_STATUS` | Activity LED | Output | Heartbeat indicator |

---

### 3.4. Ground Floor ESP32 Node 1: Sump Telemetry (0x05)
| ESP32 GPIO | Pin Function | Connected Hardware | Direction | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **GPIO 5** | `US_TRIG` | JSN-SR04T Sensor `TRIG` | Output | 10µs ultrasonic trigger |
| **GPIO 18** | `US_ECHO` | JSN-SR04T Sensor `ECHO` | Input | 5V $\to$ 3.3V resistor voltage divider (1kΩ/2kΩ) |
| **GPIO 2** | `LED_STATUS` | Wi-Fi Heartbeat LED | Output | Solid when Wi-Fi connected |

---

### 3.5. Ground Floor ESP32 Node 2: Motor Control & Interlocks (0x06)
| ESP32 GPIO | Pin Function | Connected Hardware | Direction | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **GPIO 34** | `IN_SUMP_AC` | 220V AC Optocoupler 1 `OUT` | Input | Sump Motor 240V AC Active Sense |
| **GPIO 35** | `IN_BORE_AC` | 220V AC Optocoupler 2 `OUT` | Input | Borewell Motor 240V AC Active Sense |
| **GPIO 25** | `OUT_SUMP_FLOT` | 4-Ch Relay Module `IN1` | Output | Dry contact to Sump Motor Starter |
| **GPIO 26** | `OUT_BORE_FLOT` | 4-Ch Relay Module `IN2` | Output | Dry contact to Borewell Starter (Overflow Cutoff) |
| **GPIO 27** | `OUT_AUX_RLY1` | 4-Ch Relay Module `IN3` | Output | Auxiliary remote override |
| **GPIO 14** | `OUT_AUX_RLY2` | 4-Ch Relay Module `IN4` | Output | Auxiliary remote override |
| **GPIO 2** | `LED_STATUS` | Wi-Fi Heartbeat LED | Output | Solid when Wi-Fi connected |

