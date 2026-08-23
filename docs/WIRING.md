# Wiring & Interconnection Specifications

**Document Version:** 2.3  
**Date:** 2026-08-23  
**Scope:** Complete Pinout Mappings, CAT5e Drop Schedules, RS485 Daisy-Chain, 240V AC Isolation, 4-Channel Opto-Isolation (Inputs), 4-Channel Relay Module (Future Asterro Float/Level Emulation), I2C Environmental Sensor, and Tank Node Power & Wiring.

---

## 1. ESP32-S Central Master Hub Complete Pin Allocation

The ESP32-S serves as the Central Telemetry Hub. It gathers telemetry from the local SHT30 environmental sensor, isolates and reads the Aster controller status lines via a 4-channel PC817 optoisolator, controls a 4-channel relay module for future float/level emulation, and acts as the RS485 Modbus Master.

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
| **GPIO 32** | `IN_TWT_FLOT`| 4-Ch PC817 Opto Board (Channel 1) | `V1` / `U1` | Input | Active LOW when Treated Water Tank Float switch is closed (`INPUT_PULLUP`) |
| **GPIO 26** | `IN_RL1_STAT`| 4-Ch PC817 Opto Board (Channel 2) | `V2` / `U2` | Input | Active LOW when Aster RL1 / Multiport valve status is active (`INPUT_PULLUP`) |
| **GPIO 25** | `IN_RL2_STAT`| 4-Ch PC817 Opto Board (Channel 3) | `V3` / `U3` | Input | Active LOW when Aster RL2 / Multiport valve status is active (`INPUT_PULLUP`) |
| **GPIO 34** | `IN_HPP_AC` | 220V AC Opto Module #1 (HPP Contactor) | `OUT` | Input (GPI) | Active LOW when 240V AC High Pressure Pump contactor is energized |
| **GPIO 35** | `IN_RWP_AC` | 220V AC Opto Module #2 (RWP Contactor) | `OUT` | Input (GPI) | Active LOW when 240V AC Raw Water Pump contactor is energized |
| **GPIO 27** | `OUT_RLY_TWT`| 4-Ch 5V Relay Module (Relay 1) | `IN1` | Output | Active LOW: Triggers/Emulates Treated Water Float Contact to Asterro |
| **GPIO 23** | `OUT_RLY_RWT`| 4-Ch 5V Relay Module (Relay 2) | `IN2` | Output | Active LOW: Triggers/Emulates Raw Water Float Contact to Asterro |
| **GPIO 18** | `OUT_RLY_DOS`| 4-Ch 5V Relay Module (Relay 3) | `IN3` | Output | Active LOW: Triggers/Emulates Dosing Level Contact to Asterro |
| **GPIO 19** | `OUT_RLY_AUX`| 4-Ch 5V Relay Module (Relay 4) | `IN4` | Output | Active LOW: Auxiliary / Interlock dry contact override |
| **GPIO 2** | `LED_STATUS` | Onboard DevKit Blue LED | Anode | Output | System Heartbeat & Modbus polling activity indicator (No external wire needed) |
| **GPIO 0** | `BTN_BOOT` | Onboard DevKit BOOT Button | Switch | Input | Factory Reset / AP Provisioning Mode trigger (No external wire needed) |

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

## 5. 4-Channel PC817 Optoisolator Module (817 Module - Sensor Inputs)

Provides galvanic isolation for reading dry contacts and low-voltage status lines from the Asterro controller and Multiport Valve.

### 5.1. Module Architecture & Jumpers
* **Input Side (Left):** `IN1`..`IN4` (+) and `G` (Return) with onboard 3k ohm current-limiting resistors (compatible with 3.3V to 24V DC).
* **Output Side (Right):** `V1`..`V4` (Signal) and `G` (Ground). Acts as open-collector phototransistors.
* **Jumpers:** Keep all black jumper caps installed (Default position).
* **ESP32 Connection:** ESP32 uses internal `INPUT_PULLUP`. No external `VCC` connection is required on the output side.

```
            INPUT SIDE (Left)                            OUTPUT SIDE (Right)
    +---------------------------------+          +---------------------------------+
    | [ IN1 ] (TWT Float Signal)      |          | [ V1 ] --> ESP32 GPIO 32        |
    | [  G  ] (TWT Float Return)      |          | [  G ] --> ESP32 GND            |
    +---------------------------------+          +---------------------------------+
    | [ IN2 ] (RL1 Multiport Sense)   |          | [ V2 ] --> ESP32 GPIO 26        |
    | [  G  ] (RL1 Circuit Return)    |          | [  G ] --> ESP32 GND            |
    +---------------------------------+          +---------------------------------+
    | [ IN3 ] (RL2 Multiport Sense)   |          | [ V3 ] --> ESP32 GPIO 25        |
    | [  G  ] (RL2 Circuit Return)    |          | [  G ] --> ESP32 GND            |
    +---------------------------------+          +---------------------------------+
    | [ IN4 ] (Spare)                 |          | [ V4 ] --> Unused               |
    | [  G  ] (Spare)                 |          | [  G ] --> Unused               |
    +---------------------------------+          +---------------------------------+
```

---

## 6. 4-Channel Relay Output Module (Asterro Float & Level Emulation)

### 6.1. Relay Module to ESP32 Pin Connections

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

### 6.2. Relay Contacts to Asterro Terminal Interconnections (Dry Contacts)

| Relay Channel | ESP32 GPIO | Target Asterro Terminal | Relay Contacts Used | Emulated State |
| :---: | :---: | :--- | :---: | :--- |
| **Relay 1** | **GPIO 27** | `TWT FLOTY [ C ]` & `[ NC ]` | `COM` & `NC` | **Closed (De-energized)** = Tank Normal / **Open (Energized)** = Tank Full |
| **Relay 2** | **GPIO 23** | `RWT FLOTY [ C ]` & `[ NC ]` | `COM` & `NC` | **Closed (De-energized)** = Tank Normal / **Open (Energized)** = Tank Empty |
| **Relay 3** | **GPIO 18** | `DOS LVL [ C ]` & `[ NC ]`   | `COM` & `NC` | **Closed (De-energized)** = Chemical OK / **Open (Energized)** = Dosing Low |
| **Relay 4** | **GPIO 19** | Spare / `LPS` / `HPS`        | `COM` & `NO` / `NC` | Configurable auxiliary dry-contact trigger |

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

## 7. 240V AC Optocoupler Modules (HPP & RWP Contactor Monitoring)

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

## 8. Remote Arduino Nano Tank Nodes (0x01 Dosing, 0x02 RWT, 0x03 TWT)

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
 |    XY-485 MODULE         | |   JSN-SR04T MODULE    | | ADDRESS JUMPERS |
 | RXD --> D2               | | TRIG --> D7           | | 0x01: A0=0, A1=1|
 | TXD --> D3               | | ECHO --> D8           | | 0x02: A0=1, A1=0|
 | VCC --> 5V (from Buck)   | | VCC  --> 5V (from Buck| | 0x03: A0=0, A1=0|
 | GND --> GND              | | GND  --> GND          | +-----------------+
 | A+  --> RS485 Bus (A+)   | +---------+-------------+
 | B-  --> RS485 Bus (B-)   |           | (Coaxial Cable)
 +--------------------------+           v
                               +-----------------+
                               | Waterproof      |
                               | Transducer Head |
                               +-----------------+
```

---

## 9. RS485 Daisy-Chain Topology & Bus Termination

```
 +----------------+         +----------------+         +----------------+         +----------------+
 | ESP32 HUB      |         | DOSING NODE    |         | RWT NODE       |         | TWT NODE       |
 | Master (0x00)  +---------+ Slave (0x01)   +---------+ Slave (0x02)   +---------+ Slave (0x03)   |
 | [120R Enabled] |         | (No Resistor)  |         | (No Resistor)  |         | [120R Enabled] |
 +----------------+         +----------------+         +----------------+         +----------------+
```

* **Bus Topology:** Strict linear daisy chain (no star/branch topologies).
* **Termination:** Exactly **two 120 ohm resistors** on the entire physical bus:
  1. One at the **ESP32 Central Hub** across `A+` and `B-`.
  2. One at the physical end of the bus (**TWT Tank Node 0x03**) across `A+` and `B-`.
