# Wiring & Interconnection Specifications

**Document Version:** 1.0  
**Date:** 2026-08-19  
**Scope:** CAT5e Pinouts, RS485 Bus Topology, 240V AC Optocoupler Isolation, and Dry-Contact Interfaces

---

## 1. Physical Cable Routing & Central Junction Architecture

The physical installation consists of a **Central Junction Box** located near the RO skid with three CAT5e cable runs:
1. **Vertical Hub Drop Cable:** Connects the ESP32 Hub & HLK-20M12 PSU down to the central junction box.
2. **Left Cable (To RWT Tank):** Carries 12V power, Ground, RS485 Outbound, and RS485 Return back from RWT.
3. **Right Cable (To TWT Tank):** Carries 12V power, Ground, and the spliced RS485 line out to TWT.
4. **Dosing Tank Node:** Located right at the central junction box.

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
                         │  - 4x AC Isolators    - 8-Ch DC Opto  │
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

### Detailed Pin-by-Pin Wiring for All 3 CAT5e Cables

#### 1. Vertical Hub Drop Cable (ESP32 Hub $\rightarrow$ Central Junction Box)
| Pin | Wire Color | Allocated Signal | Description |
| :---: | :--- | :--- | :--- |
| **1 & 2** | Orange Pair | **+12V DC Power** | From HLK-20M12 (+12V output) |
| **3 & 6** | Green Pair | **GND (Power & Reference)** | From HLK-20M12 (GND) & ESP32 GND |
| **4** | Blue | **RS485_A (Master Out)** | From Hub XY-485 (A+) |
| **5** | White / Blue | **RS485_B (Master Out)** | From Hub XY-485 (B-) |
| **7 & 8** | Brown Pair | **Shield / Earth GND** | Enclosure chassis ground / Shield drain |

#### 2. Left CAT5e Cable (Central Junction Box $\rightarrow$ RWT Node)
| Pin | Wire Color | Allocated Signal | Description |
| :---: | :--- | :--- | :--- |
| **1 & 2** | Orange Pair | **+12V DC Power** | Connected to 12V bus $\rightarrow$ feeds RWT LM2596 (12V IN) |
| **3 & 6** | Green Pair | **GND** | Connected to GND bus $\rightarrow$ feeds RWT LM2596 (GND IN) |
| **4** | Blue | **RS485_A (Outbound)** | Connected to Hub RS485_A $\rightarrow$ RWT XY-485 (A+) |
| **5** | White / Blue | **RS485_B (Outbound)** | Connected to Hub RS485_B $\rightarrow$ RWT XY-485 (B-) |
| **7** | White / Brown | **RS485_A (Return Loop)** | Jumpered to RWT XY-485 (A+) $\rightarrow$ returns back to Junction |
| **8** | Brown | **RS485_B (Return Loop)** | Jumpered to RWT XY-485 (B-) $\rightarrow$ returns back to Junction |

#### 3. Right CAT5e Cable (Central Junction Box $\rightarrow$ TWT Node)
| Pin | Wire Color | Allocated Signal | Description |
| :---: | :--- | :--- | :--- |
| **1 & 2** | Orange Pair | **+12V DC Power** | Connected to 12V bus $\rightarrow$ feeds TWT LM2596 (12V IN) |
| **3 & 6** | Green Pair | **GND** | Connected to GND bus $\rightarrow$ feeds TWT LM2596 (GND IN) |
| **4** | Blue | **RS485_A (Inbound to TWT)**| Connected to Left Cable Pin 7 (Return A) $\rightarrow$ TWT XY-485 (A+) |
| **5** | White / Blue | **RS485_B (Inbound to TWT)**| Connected to Left Cable Pin 8 (Return B) $\rightarrow$ TWT XY-485 (B-) |
| **7 & 8** | Brown Pair | **Shield / Spare** | Ground drain / Spare |

---

### Termination Resistor Summary
- **ESP32 HUB (Start of Bus):** **120Ω resistor** across Hub `XY-485` (`A+` and `B-`).
- **TWT Node (End of Bus):** **120Ω resistor** across TWT `XY-485` (`A+` and `B-`).
- **Dosing Node & RWT Node:** **No termination resistors**.

### 2.1. Purchased Hardware: `XY-485` Auto-Direction Converter Board
The purchased **XY-485** module includes built-in hardware automatic flow direction control. It automatically switches to TX mode when data is streamed on `TXD` and returns to RX mode when idle.

#### ESP32 HUB to XY-485 Wiring
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

#### Arduino Nano to XY-485 Wiring
```
  Arduino Nano (5V Logic)            XY-485 Module (TTL Side)
 ┌──────────────────────┐           ┌────────────────────────┐
 │ 5V (From local Buck) ├───────────┤ VCC                    │
 │ GND                  ├───────────┤ GND                    │
 │ Pin D2 (Soft RX)     ├───────────┤ RXD                    │
 │ Pin D3 (Soft TX)     ├───────────┤ TXD                    │
 └──────────────────────┘           └───────────┬────────────┘
                                                │ (RS485 Bus Side)
                                                ├─ Terminal A+ ──> CAT5e Blue (RS485_A)
                                                ├─ Terminal B- ──> CAT5e White/Blue (RS485_B)
                                                └─ Terminal E  ──> CAT5e Brown (Earth/GND)
```

---

## 3. High-Voltage 240V AC Opto-Isolation Module Wiring

Using the 4x purchased **1-Channel AC 220V Optocoupler Isolation Modules**:

```
  HIGH VOLTAGE (AC Side)                     LOW VOLTAGE (DC Logic Side)
 ┌───────────────────────────┐              ┌───────────────────────────┐
 │ L (Live)   <── 240V AC HPP│              │ VCC <── +3.3V (ESP32)     │
 │ N (Neutral)<── 240V AC Neu│              │ OUT ──> GPIO 34 (IN_HPP)  │
 └───────────────────────────┘              │ GND <── GND (ESP32)       │
                                            └───────────────────────────┘
```

- **Logic Behavior:**
  - When 240V AC is **PRESENT** (Pump ON): The optocoupler phototransistor turns ON, pulling `OUT` to **LOW (0V)**. The onboard green LED illuminates.
  - When 240V AC is **ABSENT** (Pump OFF): The onboard 47kΩ pull-up resistor pulls `OUT` to **HIGH (3.3V)**.
  - Firmware logic: `pump_running = (digitalRead(GPIO) == LOW)`.

---

## 4. Low-Voltage Dry-Contact Isolation (8-Channel PC817 Board)

Using the purchased **8-Channel PC817 Optocoupler Isolation Board** to monitor Aster controller dry switch lines:

```
 Aster NXT 11 Controller                     8-Channel PC817 Module
 ┌───────────────────────────┐              ┌───────────────────────────┐
 │ TWT FLOTY (NC) ───────────┼──────────────┤ IN1+                      │
 │ Aster Common (C) ─────────┼──────────────┤ IN1-                      │
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
