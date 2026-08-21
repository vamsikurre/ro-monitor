# Wiring & Interconnection Specifications

**Document Version:** 2.0  
**Date:** 2026-08-21  
**Scope:** Complete Pinout Mappings, CAT5e Drop Schedules, RS485 Daisy-Chain, 240V AC Isolation, Dry-Contact Opto-Isolation, I2C Environmental Sensor, and Tank Node Wiring.

---

## 1. ESP32-S Central Master Hub Complete Pin Allocation

The ESP32-S (38-Pin Development Board) serves as the Central Telemetry Hub. It gathers telemetry from the local environmental sensor, isolates and reads the Aster controller status lines, and acts as the Modbus/RS485 Bus Master.

### Master Pin Mapping Table

| ESP32 Pin | Signal Label | Connected Device / Module | Module Pin | Direction | Electrical Characteristics & Notes |
| :--- | :--- | :--- | :--- | :---: | :--- |
| **3V3** | `+3.3V_LOGIC` | GY-SHT30-D, XY-485, Optocoupler Boards | `VCC` | Power Out | Regulated 3.3V logic power bus (from ESP32 onboard LDO) |
| **GND** | `GND_LOGIC` | All Local Sub-Modules | `GND` | Power Out | Common DC logic ground |
| **VIN / 5V** | `+5V_RAW` | LM2596 / Mini560 12V $\rightarrow$ 5V Buck | `VOUT+` | Power In | Regulated +5.0V DC main board power (from HLK-20M12 step-down) |
| **GPIO 16** | `RS485_RX` | XY-485 Auto-Flow RS485 Module | `RXD` | Input | UART2 Serial RX (3.3V TTL) |
| **GPIO 17** | `RS485_TX` | XY-485 Auto-Flow RS485 Module | `TXD` | Output | UART2 Serial TX (3.3V TTL) |
| **GPIO 21** | `I2C_SDA` | GY-SHT30-D Temp/Humidity Sensor | `SDA` | Bidirectional | I2C Data Line (4.7kΩ pull-up to 3.3V) |
| **GPIO 22** | `I2C_SCL` | GY-SHT30-D Temp/Humidity Sensor | `SCL` | Output | I2C Clock Line (Standard 100 kHz / Fast 400 kHz) |
| **GPIO 34** | `IN_HPP_AC` | 220V AC Opto Module #1 (HPP Contactor) | `OUT` | Input (GPI) | Active LOW when 240V AC High Pressure Pump is active |
| **GPIO 35** | `IN_RWP_AC` | 220V AC Opto Module #2 (RWP / Aux) | `OUT` | Input (GPI) | Active LOW when 240V AC Raw Water Pump is active |
| **GPIO 32** | `IN_TWT_FLOT`| 8-Ch PC817 Opto Board (Channel 1) | `OUT1` | Input | Active LOW when Treated Water Tank Float switch is tripped |
| **GPIO 33** | `IN_RWT_FLOT`| 8-Ch PC817 Opto Board (Channel 2) | `OUT2` | Input | Active LOW when Raw Water Tank Float switch is tripped |
| **GPIO 25** | `IN_DOS_LVL` | 8-Ch PC817 Opto Board (Channel 3) | `OUT3` | Input | Active LOW when Dosing Chemical Level switch is tripped |
| **GPIO 26** | `IN_RL1_STAT`| 8-Ch PC817 Opto Board (Channel 4) | `OUT4` | Input | Active LOW when Aster RL1 output relay is closed |
| **GPIO 27** | `IN_RL2_STAT`| 8-Ch PC817 Opto Board (Channel 5) | `OUT5` | Input | Active LOW when Aster RL2 output relay is closed |
| **GPIO 14** | `IN_SPARE_1` | 8-Ch PC817 Opto Board (Channel 6) | `OUT6` | Input | Configurable spare opto-isolated digital input |
| **GPIO 12** | `IN_SPARE_2` | 8-Ch PC817 Opto Board (Channel 7) | `OUT7` | Input | Configurable spare opto-isolated digital input |
| **GPIO 13** | `IN_SPARE_3` | 8-Ch PC817 Opto Board (Channel 8) | `OUT8` | Input | Configurable spare opto-isolated digital input |
| **GPIO 2** | `LED_STATUS` | Onboard DevKit Blue LED | Anode | Output | System Heartbeat & Modbus polling activity indicator |
| **GPIO 0** | `BTN_BOOT` | Onboard DevKit BOOT Button | Switch | Input | Factory Reset / AP Provisioning Mode trigger |

> [!NOTE]
> **GPI Pins (GPIO 34 & GPIO 35):** These pins on the ESP32 are input-only and lack internal software pull-up resistors. This is completely compatible with our design because the purchased **220V AC Optocoupler Modules** have built-in onboard 10kΩ/47kΩ hardware pull-up resistors tied to `VCC` (3.3V).

---

## 2. Power Architecture & Voltage Distribution

Power is derived safely from the 230V AC mains line feeding the RO control panel and stepped down to SELV (Safety Extra Low Voltage) levels:

```
 230V AC Mains (From Plant Contactor Panel)
      │
      ▼
 ┌──────────────────────────────────────────────────────────┐
 │  Hi-Link HLK-20M12 Encapsulated Power Supply Module       │
 │  Input: 90–264V AC | Output: 12V DC ±0.2V @ 1.66A (20W)  │
 └─────────────┬────────────────────────────────────────────┘
               │
               ├───────────────────────────────────────────────┐
               │ 12V DC Master Bus                             │ 12V DC to CAT5e Drop Lines
               ▼                                               ▼
 ┌───────────────────────────┐                   ┌───────────────────────────┐
 │ LM2596 / Mini560 Buck     │                   │ Remote Tank Nodes 1, 2, 3 │
 │ Input: 12V DC             │                   │ (Local 12V -> 5V Bucks    │
 │ Output: 5.0V DC (2.0A max)│                   │  at Dosing, RWT, and TWT) │
 └─────────────┬─────────────┘                   └───────────────────────────┘
               │
               ├───────────────────> ESP32 5V (VIN) Pin
               ├───────────────────> XY-485 Master Transceiver VCC (or 3.3V)
               └───────────────────> 8-Ch PC817 VCC (Logic side: 3.3V / 5V)
```

---

## 3. Physical Cable Routing & Central Junction Architecture

The physical installation features a **Central Junction Box** located near the RO skid with three CAT5e cable runs:
1. **Vertical Hub Drop Cable:** Connects the ESP32 Hub & HLK-20M12 PSU to the central junction box.
2. **Left Cable (To RWT Tank):** Carries 12V power, Ground, RS485 Outbound, and RS485 Return back from RWT.
3. **Right Cable (To TWT Tank):** Carries 12V power, Ground, and the spliced RS485 line out to TWT.
4. **Dosing Tank Node:** Located directly at the central junction box.

```
                                ┌──────────────────────────┐
                                │   240V AC Mains Input    │
                                └──────┬────────────┬──────┘
                                       │            │
                         ┌─────────────▼───┐  ┌─────▼────────────┐
                         │   HLK-20M12     │  │  Astero NXT 11   │
                         │ (12V / 20W PSU) │  │  (RO Controller) │
                         └──────┬──────────┘  └─────┬────────────┘
                                │ 12V DC            │ AC & Dry Contacts
                         ┌──────▼───────────────────▼────────────┐
                         │              ESP32 HUB                │
                         │  - LM2596 (12V->5V)   - GY-SHT30-D    │
                         │  - 2x AC Isolators    - 8-Ch DC Opto  │
                         │  - Master XY-485 Module               │
                         └──────────────────┬────────────────────┘
                                            │
                           [ VERTICAL CAT5e DROP CABLE ]
                           (12V Power, GND, RS485 Outbound)
                                            │
                                  ┌─────────▼─────────┐
                                  │  CENTRAL JUNCTION │
                         ┌────────┤       BOX         ├────────┐
                         │        └─────────┬─────────┘        │
                         │                  │                  │
        [ LEFT CAT5e CABLE ]                │       [ RIGHT CAT5e CABLE ]
        (12V, GND, RS485 Out,               │       (12V, GND, RS485 from
         RS485 Return Loop)                 │        Return Splice)
                         │         ┌────────▼────────┐         │
                         │         │   DOSING TANK   │         │
                         │         │ - LM2596 (5V)   │         │
                         │         │ - Arduino Nano  │         │
                         │         │ - JSN-SR04T     │         │
                         │         │ - XY-485 Module │         │
                         │         └─────────────────┘         │
                ┌────────▼────────┐                   ┌────────▼────────┐
                │    RWT TANK     │                   │    TWT TANK     │
                │ - LM2596 (5V)   │                   │ - LM2596 (5V)   │
                │ - Arduino Nano  │                   │ - Arduino Nano  │
                │ - JSN-SR04T     │                   │ - JSN-SR04T     │
                │ - XY-485 Module │                   │ - XY-485 Module │
                │ - Loop Jumper   │                   │ - 120Ω Term Res │
                └─────────────────┘                   └─────────────────┘
```

---

## 4. Detailed Pin-by-Pin Wiring for All 3 CAT5e Cables

### 1. Vertical Hub Drop Cable (ESP32 Hub $\rightarrow$ Central Junction Box)
| Pin | Wire Color | Allocated Signal | Description |
| :---: | :--- | :--- | :--- |
| **1 & 2** | Orange Pair | **+12V DC Power** | From HLK-20M12 (+12V output) |
| **3 & 6** | Green Pair | **GND (Power & Reference)** | From HLK-20M12 (GND) & ESP32 GND |
| **4** | Blue | **RS485_A (Master Out)** | From Hub XY-485 (A+) |
| **5** | White / Blue | **RS485_B (Master Out)** | From Hub XY-485 (B-) |
| **7 & 8** | Brown Pair | **Shield / Earth GND** | Enclosure chassis ground / Shield drain |

### 2. Left CAT5e Cable (Central Junction Box $\rightarrow$ RWT Node)
| Pin | Wire Color | Allocated Signal | Description |
| :---: | :--- | :--- | :--- |
| **1 & 2** | Orange Pair | **+12V DC Power** | Connected to 12V bus $\rightarrow$ feeds RWT LM2596 (12V IN) |
| **3 & 6** | Green Pair | **GND** | Connected to GND bus $\rightarrow$ feeds RWT LM2596 (GND IN) |
| **4** | Blue | **RS485_A (Outbound)** | Connected to Hub RS485_A $\rightarrow$ RWT XY-485 (A+) |
| **5** | White / Blue | **RS485_B (Outbound)** | Connected to Hub RS485_B $\rightarrow$ RWT XY-485 (B-) |
| **7** | White / Brown | **RS485_A (Return Loop)** | Jumpered to RWT XY-485 (A+) $\rightarrow$ returns back to Junction |
| **8** | Brown | **RS485_B (Return Loop)** | Jumpered to RWT XY-485 (B-) $\rightarrow$ returns back to Junction |

### 3. Right CAT5e Cable (Central Junction Box $\rightarrow$ TWT Node)
| Pin | Wire Color | Allocated Signal | Description |
| :---: | :--- | :--- | :--- |
| **1 & 2** | Orange Pair | **+12V DC Power** | Connected to 12V bus $\rightarrow$ feeds TWT LM2596 (12V IN) |
| **3 & 6** | Green Pair | **GND** | Connected to GND bus $\rightarrow$ feeds TWT LM2596 (GND IN) |
| **4** | Blue | **RS485_A (Inbound to TWT)**| Connected to Left Cable Pin 7 (Return A) $\rightarrow$ TWT XY-485 (A+) |
| **5** | White / Blue | **RS485_B (Inbound to TWT)**| Connected to Left Cable Pin 8 (Return B) $\rightarrow$ TWT XY-485 (B-) |
| **7 & 8** | Brown Pair | **Shield / Spare** | Ground drain / Spare |

---

## 5. Bus Termination & Transceiver Interfacing

### Termination Resistor Summary
- **ESP32 HUB (Start of Bus):** **120Ω resistor** across Hub `XY-485` (`A+` and `B-`).
- **TWT Node (End of Bus):** **120Ω resistor** across TWT `XY-485` (`A+` and `B-`).
- **Dosing Node & RWT Node:** **No termination resistors**.

### 5.1. ESP32 HUB to `XY-485` Auto-Direction Module Wiring
The **XY-485** module includes built-in hardware automatic flow direction control. It automatically switches to TX mode when data is streamed on `TXD` and returns to RX mode when idle.

```
  ESP32-S (3.3V Logic)               XY-485 Module (TTL Side)
 ┌──────────────────────┐           ┌────────────────────────┐
 │ 3.3V                 ├───────────┤ VCC                    │
 │ GND                  ├───────────┤ GND                    │
 │ GPIO 16 (UART2 RX)   ├───────────┤ RXD                    │
 │ GPIO 17 (UART2 TX)   ├───────────┤ TXD                    │
 └──────────────────────┘           └───────────┬────────────┘
                                                │ (RS485 Bus Side)
                                                ├─ Terminal A+ ──> CAT5e Blue (RS485_A)
                                                ├─ Terminal B- ──> CAT5e White/Blue (RS485_B)
                                                └─ Terminal E  ──> CAT5e Brown (Earth/GND)
```

---

## 6. GY-SHT30-D Temperature & Humidity Sensor Wiring

The **GY-SHT30-D** digital sensor monitors ambient pump room/enclosure temperature and relative humidity over the I2C bus.

```
  ESP32-S (3.3V Logic)               GY-SHT30-D I2C Sensor Module
 ┌──────────────────────┐           ┌────────────────────────┐
 │ 3.3V                 ├───────────┤ VIN / VCC              │
 │ GND                  ├───────────┤ GND                    │
 │ GPIO 21 (I2C SDA)    ├───────────┤ SDA                    │
 │ GPIO 22 (I2C SCL)    ├───────────┤ SCL                    │
 └──────────────────────┘           │ ADDR (Tied to GND:0x44)│
                                    │ ALR  (Unconnected)     │
                                    └────────────────────────┘
```

### Sensor Specifications & I2C Configuration
| GY-SHT30-D Pin | Connected To | Description / Role |
| :--- | :--- | :--- |
| **VIN / VCC** | ESP32 **3.3V** | Power supply (2.4V – 5.5V operating range) |
| **GND** | ESP32 **GND** | System Ground reference |
| **SDA** | ESP32 **GPIO 21** | Serial Data line (4.7kΩ pull-up to 3.3V included on breakout) |
| **SCL** | ESP32 **GPIO 22** | Serial Clock line (4.7kΩ pull-up to 3.3V included on breakout) |
| **ADDR** | ESP32 **GND** | I2C Address select (GND = `0x44`, 3.3V = `0x45`) |
| **ALR** | N/C (Open) | Alert / Interrupt output (unused) |

---

## 7. High-Voltage 240V AC Opto-Isolation Modules (HPP & RWP)

Using 2x **1-Channel AC 220V Optocoupler Isolation Modules** to safely sense the 240V AC state of the High-Pressure Pump contactor coil and Raw Water Pump feed without direct galvanic connection.

```
  HIGH VOLTAGE (AC 240V Line)                LOW VOLTAGE (ESP32 Side)
 ┌───────────────────────────┐              ┌───────────────────────────┐
 │ L (Live)   <── Contactor  │              │ VCC <── +3.3V (ESP32)     │
 │ N (Neutral)<── Neutral Bus│              │ OUT ──> GPIO 34 / GPIO 35 │
 └───────────────────────────┘              │ GND <── GND (ESP32)       │
                                            └───────────────────────────┘
```

### AC Optocoupler Connection Mapping
| Module Instance | AC Input High-Voltage Side | DC Output Low-Voltage Side | ESP32 Pin | Signal State Logic |
| :--- | :--- | :--- | :---: | :--- |
| **AC Module #1** | **Contactor Coil A1/A2** (HPP 240V AC) | `VCC` $\rightarrow$ 3.3V, `GND` $\rightarrow$ GND, `OUT` | **GPIO 34** | **LOW (0V)** = HPP ON<br>**HIGH (3.3V)** = HPP OFF |
| **AC Module #2** | **RWP Motor Line** (RWP 240V AC) | `VCC` $\rightarrow$ 3.3V, `GND` $\rightarrow$ GND, `OUT` | **GPIO 35** | **LOW (0V)** = RWP ON<br>**HIGH (3.3V)** = RWP OFF |

- **Operating Characteristics:**
  - 3.75 kV – 5.0 kV AC RMS galvanic isolation barrier.
  - Onboard green indicator LED illuminates when 240V AC is energized.
  - Onboard 10kΩ–47kΩ pull-up resistor holds the output line at 3.3V when no AC current flows.

---

## 8. Low-Voltage Dry-Contact Isolation (8-Channel PC817 Board)

The **8-Channel PC817 Optocoupler Isolation Board** provides total galvanic isolation between the Aster controller’s internal switch lines and the ESP32 GPIOs.

```
  Aster NXT 11 Control Board                  8-Channel PC817 Module
 ┌───────────────────────────┐              ┌───────────────────────────┐
 │ TWT FLOTY (NC) ───────────┼──────────────┤ IN1+                      │
 │ Aster Common (C) ─────────┼──────────────┤ IN1-                      │
 │ RWT FLOTY (NC) ───────────┼──────────────┤ IN2+                      │
 │ Aster Common (C) ─────────┼──────────────┤ IN2-                      │
 │ DOS LVL (NC) ─────────────┼──────────────┤ IN3+                      │
 │ Aster Common (C) ─────────┼──────────────┤ IN3-                      │
 │ RL1 Status (NO) ──────────┼──────────────┤ IN4+                      │
 │ Aster Common (C) ─────────┼──────────────┤ IN4-                      │
 │ RL2 Status (NO) ──────────┼──────────────┤ IN5+                      │
 │ Aster Common (C) ─────────┼──────────────┤ IN5-                      │
 └───────────────────────────┘              └─────────────┬─────────────┘
                                                          │ DC Output Side (To ESP32)
                                                          ├─ VCC  ──> +3.3V (ESP32)
                                                          ├─ GND  ──> GND (ESP32)
                                                          ├─ OUT1 ──> GPIO 32 (IN_TWT_FLOT)
                                                          ├─ OUT2 ──> GPIO 33 (IN_RWT_FLOT)
                                                          ├─ OUT3 ──> GPIO 25 (IN_DOS_LVL)
                                                          ├─ OUT4 ──> GPIO 26 (IN_RL1_STAT)
                                                          ├─ OUT5 ──> GPIO 27 (IN_RL2_STAT)
                                                          ├─ OUT6 ──> GPIO 14 (Spare)
                                                          ├─ OUT7 ──> GPIO 12 (Spare)
                                                          └─ OUT8 ──> GPIO 13 (Spare)
```

### 8-Channel Optocoupler Channel Allocation Table
| Channel | Aster Terminal Source | Aster Wire / Function | PC817 Input | PC817 Output | ESP32 GPIO | Logic State Meaning |
| :---: | :--- | :--- | :---: | :---: | :---: | :--- |
| **CH 1** | `TWT FLOTY` & `C` | Treated Tank Float Switch | `IN1+` / `IN1-` | `OUT1` | **GPIO 32** | **LOW** = Float Closed (Normal) / **HIGH** = Float Open (Full) |
| **CH 2** | `RWT FLOTY` & `C` | Raw Tank Float Switch | `IN2+` / `IN2-` | `OUT2` | **GPIO 33** | **LOW** = Float Closed (Normal) / **HIGH** = Float Open (Low) |
| **CH 3** | `DOS LVL` & `C` | Dosing Tank Low Level | `IN3+` / `IN3-` | `OUT3` | **GPIO 25** | **LOW** = Chemical OK / **HIGH** = Chemical Low |
| **CH 4** | `RL1` & `C` | Relay 1 (HPP Command) | `IN4+` / `IN4-` | `OUT4` | **GPIO 26** | **LOW** = Relay Closed (ON) / **HIGH** = Relay Open (OFF) |
| **CH 5** | `RL2` & `C` | Relay 2 (RWP Command) | `IN5+` / `IN5-` | `OUT5` | **GPIO 27** | **LOW** = Relay Closed (ON) / **HIGH** = Relay Open (OFF) |
| **CH 6** | `HPS` & `C` (Optional) | High Pressure Switch | `IN6+` / `IN6-` | `OUT6` | **GPIO 14** | **LOW** = Switch Tripped / **HIGH** = Normal |
| **CH 7** | `LPS` & `C` (Optional) | Low Pressure Switch | `IN7+` / `IN7-` | `OUT7` | **GPIO 12** | **LOW** = Switch Tripped / **HIGH** = Normal |
| **CH 8** | Spare Header | Expansion Input | `IN8+` / `IN8-` | `OUT8` | **GPIO 13** | General Purpose Opto Input |

---

## 9. Arduino Nano Remote Tank Node Wiring (Nodes 0x01, 0x02, 0x03)

Each remote tank location (Dosing, RWT, and TWT) houses an **Arduino Nano**, a **JSN-SR04T waterproof ultrasonic sensor**, an **XY-485 transceiver**, and a **step-down buck converter**:

```
                                  12V DC CAT5e Power Bus
                                            │
                                            ▼
                             ┌──────────────────────────────┐
                             │ LM2596 / Mini560 Buck Module │
                             │ Input: 12V DC | Output: 5.0V │
                             └──────────────┬───────────────┘
                                            │
                                            ├────────────────────────────────┐
                                            │ +5V DC                         │ GND
                                            ▼                                ▼
 ┌─────────────────────────────────────────────────────────────────────────────────┐
 │                            ARDUINO NANO (ATmega328P)                            │
 │  - 5V Pin  <── +5V Buck Output                                                  │
 │  - GND Pin <── GND                                                              │
 └──────┬────────────┬─────────────┬─────────────┬─────────────┬─────────────┬─────┘
        │            │             │             │             │             │
        ▼            ▼             ▼             ▼             ▼             ▼
      Pin D2       Pin D3        Pin D7        Pin D8        Pin A0        Pin A1
     (Soft RX)   (Soft TX)     (US TRIG)     (US ECHO)     (ADDR BIT0)   (ADDR BIT1)
        │            │             │             │             │             │
   ┌────┴────────────┴────┐   ┌────┴─────────────┴────┐   ┌────┴─────────────┴────┐
   │    XY-485 MODULE     │   │   JSN-SR04T MODULE    │   │  HARDWARE ADDRESS DIP │
   │ RXD ──> D2           │   │ TRIG ──> D7           │   │ 0x01: A0=GND, A1=OPEN │
   │ TXD ──> D3           │   │ ECHO ──> D8           │   │ 0x02: A0=OPEN, A1=GND │
   │ VCC ──> 5V           │   │ VCC  ──> 5V           │   │ 0x03: A0=GND, A1=GND  │
   │ GND ──> GND          │   │ GND  ──> GND          │   │ (Internal Pull-ups)   │
   └──────────┬───────────┘   └───────────┬───────────┘   └───────────────────────┘
              │                           │ (Coaxial Cable)
              ├─ Terminal A+              ▼
              ├─ Terminal B-        ┌───────────┐
              └─ Terminal E         │Transducer │
                                    └───────────┘
```

### Arduino Nano Tank Node Pinout Table
| Nano Pin | Pin Function | Connected Hardware / Circuit | Direction | Electrical Interface / Notes |
| :--- | :--- | :--- | :---: | :--- |
| **5V** | `+5V_POWER` | Local LM2596 Buck Converter Output | Power In | Regulated 5.0V DC |
| **GND** | `GND` | Common Ground Bus | Power In | Common 0V DC |
| **D2** | `RS485_RX` | XY-485 Module `RXD` | Input | SoftwareSerial RX (5V TTL) |
| **D3** | `RS485_TX` | XY-485 Module `TXD` | Output | SoftwareSerial TX (5V TTL) |
| **D7** | `US_TRIG` | JSN-SR04T Module `TRIG` | Output | 10µs ultrasonic trigger pulse |
| **D8** | `US_ECHO` | JSN-SR04T Module `ECHO` | Input | Echo return pulse width ($150\mu\text{s} - 25\text{ms}$) |
| **A0** | `ADDR_SEL0` | Address Select Bit 0 | Input | Tied to GND for Address Bit 0 = 0 (Internal `INPUT_PULLUP`) |
| **A1** | `ADDR_SEL1` | Address Select Bit 1 | Input | Tied to GND for Address Bit 1 = 0 (Internal `INPUT_PULLUP`) |
| **D13** | `LED_STATUS`| Onboard Activity LED | Output | Flashes on valid Modbus poll received and response sent |
��─────┤ IN1-                      │
 │ RWT FLOTY (NC) ───────────┼──────────────┤ IN2+                      │
 │ Aster Common (C) ─────────┼──────────────┤ IN2-                      │
 │ DOS LVL (NC) ─────────────┼──────────────┤ IN3+                      │
 │ Aster Common (C) ─────────┼──────────────┤ IN3-                      │
 │ RL1 Status (NO) ──────────┼──────────────┤ IN4+                      │
 │ Aster Common (C) ─────────┼──────────────┤ IN4-                      │
 └───────────────────────────┘              └─────────────┬─────────────┘
                                                          │ Output Side (To ESP32)
                                                          ├─ VCC ──> +3.3V (ESP32)
                                                          ├─ GND ──> GND (ESP32)
                                                          ├─ OUT1 ──> GPIO 32 (IN_TWT_FLOT)
                                                          ├─ OUT2 ──> GPIO 33 (IN_RWT_FLOT)
                                                          ├─ OUT3 ──> GPIO 25 (IN_DOS_LVL)
                                                          └─ OUT4 ──> GPIO 26 (IN_RL1_STAT)
```

---

## 5. Ultrasonic Sensor (JSN-SR04T) Wiring at Tank Node

Using the 4x purchased **Waterproof Ultrasonic Obstacle Sensors**:

```
  Arduino Nano (5V Logic)            JSN-SR04T Waterproof Module
 ┌───────────────────────┐          ┌───────────────────────────┐
 │ +5V (From local Buck) ├──────────┤ 5V (VCC)                  │
 │ GND                   ├──────────┤ GND                       │
 │ Pin D7                ├──────────┤ TRIG                      │
 │ Pin D8                ├──────────┤ ECHO                      │
 └───────────────────────┘          └─────────────┬─────────────┘
                                                  │ (Coaxial Cable)
                                            ┌─────▼─────┐
                                            │ Waterproof│
                                            │ Transducer│
                                            │ (At Tank) │
                                            └───────────┘
```
