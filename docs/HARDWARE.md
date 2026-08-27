# Hardware Specifications & Pin Allocations

**Document Version:** 2.1  
**Date:** 2026-08-25  
**System Architecture:** ESP32 HUB (Master) + 4x RS485 Arduino Nano Nodes + 2x Ground Floor Wi-Fi ESP32 Nodes  
**RS485 chain order:** `0x00 HUB -> 0x04 BATTERY ROOM -> 0x03 TWT -> 0x02 RWT` (terminators at 0x00 and 0x02). Address `0x01` retired — the dosing sensor is wired direct to the hub (`WIRING.md` §13)  
**Node platforms:** `0x02`/`0x03` Arduino Nano, **`0x04` Arduino Pro Mini 5V/16MHz** (`WIRING.md` 10.1)

> **Pin allocation authority:** Section 3.1 below is kept identical to `WIRING.md` Section 1, which describes the hub as physically built. `WIRING.md` wins on conflict — this table is corrected to match it, never the reverse (phase-B spec §3.5 / §9). Aster terminal contact polarity and the `ALARM` output are specified in `WIRING.md` Section 6; read it before wiring anything to the controller.
>
> **Corrected 2026-08-25:** this table previously listed `GPIO 33 IN_RWT_FLOT`, an `IN_DOS_LVL` channel, `GPIO 27` as an input, and an 8-channel PC817 board. None of those exist on the built hub.

---

## 1. System Topology Overview

```
 [120Ω Term Resistor]
┌────────────────────┐
│  ESP32-S HUB (0x00)│ (RO Room Bus Master)
│   XY-485 Master    │
│ + AJ-SR04M direct │ (Dosing tank, ~1 m - no node, see WIRING.md 13)
└─────────┬──────────┘
          │ Cat5e 1: Outbound Pair (Pins 4 & 5: Blue / White-Blue - length TBM)
          ▼
┌────────────────────┐
│BATTERY ROOM (0x04) │ (Battery Room - SHT30 Climate & Exhaust Fan)
│  Pro Mini 5V/16MHz │  NO termination resistor - mid-chain
└─────────┬──────────┘
          │
          │ Cat5e 2: Outbound Pair (Pins 4 & 5: Blue / White-Blue - length TBM)
          ▼
┌────────────────────┐
│  TWT NODE (0x03)   │ (Roof Top - Treated Water Tank)
│    XY-485 Auto     │
└─────────┬──────────┘
          │
          │ Cat5e 3: Roof-to-roof Outbound Pair (length TBM)
          ▼
┌────────────────────┐
│  RWT NODE (0x02)   │ (Roof Top - Raw Water Tank)
│    XY-485 Auto     │
│ [120Ω Term Resistor│  END OF BUS
└────────────────────┘

========================================================================================
GROUND FLOOR PARKING SUBSYSTEM (WI-FI LAN)
========================================================================================
┌───────────────────────────────┐        ┌─────────────────────────────────────────┐
│     ESP32 NODE 1 (0x05)       │        │           ESP32 NODE 2 (0x06)           │
│  - 3.5m Deep Sump Level       │        │  - Sump & Borewell 240V AC Monitoring   │
│  - AJ-SR04M Sensor           │        │  - 4-Ch Relay Motor Starter Interlock   │
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
| **RS485 Transceiver** | `XY-485` (Auto-Flow Control) | 4 | Hardware auto TX/RX direction switching (1x Hub, 3x slave nodes) |
| **Current Clamp** | `SCT-013-030` split-core CT, 30 A / 1 V output | 8 | 1x HPP + 1x RWP on the hub (`WIRING.md` §14), then **3 phases each** on the ground-floor sump and borewell motors (§11.3). Voltage-output variant: burden resistor is inside, do not add one. Sump and borewell have **no readable nameplate** (submersible, confirmed 2026-08-27) — size from the starter overload dial and a clamp meter, and expect to need 2-3 turns for resolution (`WIRING.md` §11.3.1) |
| **220V AC Opto Isolator** | 1-Channel 220V AC Optocoupler Module | 4 | 2x RO Room (HPP/RWP) + 2x Ground Floor (Sump/Borewell). **A 5th is needed only if the Aster `ALARM` output measures as switched mains** — see `WIRING.md` 6.4 |
| **DC Dry-Contact Opto** | 4-Channel PC817 Optocoupler Board | 1 | RO Room - Aster controller dry switch isolation. **As-built part**; all 4 channels used: `RL1`, `RL2`, `LPS`, `TWT FLOTY`. `IN_ALARM` needed no optocoupler and moved to a direct input on GPIO 13, which freed channel 3 for `LPS` without buying an 8-channel board (`WIRING.md` §6.6) |
| **Relay Modules** | 4-Channel 5V Relay Board | 2 | 1x RO Room (Aster Float Emulation) + 1x Ground Floor (Starter Control) |
| **Relay Module (Single)** | 1-Channel 5V Relay Board | 1 | Battery Room (Exhaust Fan Switching) |
| **Ultrasonic Sensors** | Waterproof Ultrasonic (**AJ-SR04M**) | 4 | **Dosing (wired direct to hub)**, RWT (`0x02`), TWT (`0x03`), Ground Sump (`0x05`). Confirm `R19` mode pad is empty (Trig/Echo mode) — `WIRING.md` §9.0 |
| **Resistors** | 1 kΩ + 2 kΩ (1/4 W) | 2 pairs | `ECHO` 5V→3.3V dividers: hub dosing sensor + ground-floor sump sensor |
| **Environmental Sensors** | GY-SHT30-D Digital Temp & Humidity | 2 | 1x RO Room (Hub I2C) + 1x Battery Room (Node 4 I2C) |
| **Microcontrollers** | ESP32-S / ESP32-WROOM-32 | 3 | 1x Central Hub + 2x Ground Floor Nodes |
| **Microcontrollers** | Arduino Nano (ATmega328P) | 2 | RS485 Slave Nodes `0x02`, `0x03` |
| **Microcontrollers** | Arduino Pro Mini (ATmega328P-AU, **5V / 16 MHz**) | 1 | RS485 Slave Node `0x04` Battery Room. Needs an FTDI/CP2102 adapter to program — see `WIRING.md` 10.1 |

---

## 3. Pin Allocations by Subsystem

### 3.1. ESP32-S Central Hub Pin Allocations (RO Room)
| ESP32 GPIO | Pin Function | Connected Hardware / Circuit | Direction | Interface Notes |
| :--- | :--- | :--- | :--- | :--- |
| **GPIO 16** | `RS485_RX` | XY-485 Module `RXD` | Input | 3.3V UART2 RX |
| **GPIO 17** | `RS485_TX` | XY-485 Module `TXD` | Output | 3.3V UART2 TX |
| **GPIO 21** | `I2C_SDA` | GY-SHT30-D `SDA` (RO Room) | Bidirectional | I2C SDA (4.7kΩ pull-up to 3.3V) |
| **GPIO 22** | `I2C_SCL` | GY-SHT30-D `SCL` (RO Room) | Output | I2C SCL (100kHz standard) |
| **GPIO 34** | `IN_HPP_AC` | 220V AC Optocoupler Module 1 `OUT` | Input | Active LOW when HPP 240V contactor active. Onboard 47 k pull-up to `VCC`; wire `VCC` to 3V3 |
| **GPIO 35** | `IN_RWP_AC` | 220V AC Optocoupler Module 2 `OUT` | Input | Active LOW when RWP 240V active. Onboard 47 k pull-up to `VCC`; wire `VCC` to 3V3 |
| **GPIO 32** | `IN_TWT_FLOT` | 4-Ch PC817 Board Ch 4 `V4` | Input | Aster `TWT FLOTY` loop. LOW = closed = tank not full |
| **GPIO 26** | `IN_RL1_STAT` | 4-Ch PC817 Board Ch 1 `V1` | Input | Aster RL1 contact. LOW = closed = output active |
| **GPIO 25** | `IN_RL2_STAT` | 4-Ch PC817 Board Ch 2 `V2` | Input | Aster RL2 contact. LOW = closed = output active |
| **GPIO 33** | `IN_LPS` | 4-Ch PC817 Board Ch 3 `V3` | Input | Aster `LPS` low-pressure contact, `C`/`NO`. Took this channel from `IN_ALARM` on 2026-08-27 |
| **GPIO 13** | `IN_ALARM` | Aster `AUX OP` relay `C`/`NO`, **direct** | Input | No optocoupler, no external parts — the relay contact is already the isolation barrier and the relay is enclosed. `INPUT_PULLUP`, debounced 6-of-8 in firmware (`WIRING.md` §6.6) |
| **GPIO 36** | `IN_HPP_CT` | SCT-013-030 clamp (HPP) | Input | Analog, ADC1_CH0. Motor current for health trending — spec §7.3 |
| **GPIO 39** | `IN_RWP_CT` | SCT-013-030 clamp (RWP) | Input | Analog, ADC1_CH3. Motor current for health trending — spec §7.3 |
| **GPIO 27** | `OUT_RLY_TWT` | 4-Ch Relay Module IN1 | Output | TWT Float Switch Emulation (`COM`/`NC`, de-energized = closed). Phase C |
| **GPIO 23** | `OUT_RLY_RWT` | 4-Ch Relay Module IN2 | Output | RWT Float Switch Emulation (`COM`/`NC`, de-energized = closed). Phase C |
| **GPIO 18** | `OUT_RLY_DOS` | 4-Ch Relay Module IN3 | Output | Dosing Level Switch Emulation (`COM`/`NC`, de-energized = closed). Phase C |
| **GPIO 19** | `OUT_RLY_AUX` | 4-Ch Relay Module IN4 | Output | Auxiliary / interlock override. Polarity depends on target terminal (`WIRING.md` 7.2). Phase C |
| **GPIO 5** | `US_TRIG_DOS` | AJ-SR04M (Dosing) `TRIG` | Output | 10 µs trigger. Sensor wired direct to hub (`WIRING.md` §13) |
| **GPIO 4** | `US_ECHO_DOS` | AJ-SR04M (Dosing) `ECHO` | Input | Echo width. **1 kΩ/2 kΩ divider required** (5V→3.3V) |
| **GPIO 2** | `LED_STATUS` | System Heartbeat LED | Output | Active HIGH onboard LED |
| **GPIO 0** | `BTN_BOOT` | Onboard BOOT Button | Input | Factory reset / provisioning trigger |

**Constraints:** GPIO 34-39 have no internal pull-up, but the AC opto module supplies a 47 k pull-up from `OUT` to `VCC` (**corrected 2026-08-27** — `WIRING.md` §1), so 34 and 35 need no external resistor, only `VCC` on 3V3. The four open-collector PC817 channels sit on pull-up-capable pins (25/26/32/33). GPIO 6-11 are flash, 1/3 are the serial console. **Updated 2026-08-27:** 4/5 went to the dosing ultrasonic and **13 now carries `IN_ALARM` directly** (§6.6), so of what is left **12 and 15 are strapping pins** — 12 selects flash voltage at boot — which leaves **GPIO 14 as the last comfortable spare on this board**.

---

### 3.2. Arduino Nano Tank Nodes Pin Allocations (Nodes 0x02, 0x03)
| Nano Pin | Pin Function | Connected Hardware | Direction | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **D2** | `RS485_RX` | XY-485 Module `RXD` | Input | SoftwareSerial RX |
| **D3** | `RS485_TX` | XY-485 Module `TXD` | Output | SoftwareSerial TX |
| **D7** | `US_TRIG` | AJ-SR04M Sensor `TRIG` | Output | 10µs trigger pulse |
| **D8** | `US_ECHO` | AJ-SR04M Sensor `ECHO` | Input | Echo pulse width |
| **A0** | `ADDR_SEL0` | Hardware Node Address Bit 0 | Input | `INPUT_PULLUP`: open = 1, GND = 0. `ADDR_MAP` lookup: `WIRING.md` §9.1 |
| **A1** | `ADDR_SEL1` | Hardware Node Address Bit 1 | Input | `0x02` = A0 GND. `0x03` = A1 GND. `0x04` = both open. Both GND = unassigned |
| **D13** | `LED_STATUS` | Status / RS485 Activity | Output | Blinks on successful poll |

---

### 3.3. Arduino Pro Mini Battery Room Node (Node 0x04)

Built on a **Pro Mini (5V / 16 MHz)**, not a Nano. Pin functions are identical — same ATmega328P, same firmware binary — but the power feed, the `A4`/`A5` breakout and programming differ. Build notes: `WIRING.md` 10.1.

| Pro Mini Pin | Pin Function | Connected Hardware | Direction | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **VCC** | `+5V_IN` | Mini560 / LM2596 buck `VOUT+` | Power In | **Feed here, not `RAW`.** `RAW` routes through a SOT-23 LDO that cannot dissipate 12V→5V at this node's load |
| **D2** | `RS485_RX` | XY-485 Module `RXD` | Input | SoftwareSerial RX. `D0`/`D1` stay free for the FTDI header |
| **D3** | `RS485_TX` | XY-485 Module `TXD` | Output | SoftwareSerial TX |
| **A4** | `I2C_SDA` | GY-SHT30-D `SDA` | Bidirectional | Hardware I2C SDA (`PC4`). **Inner pad — not on the edge header** |
| **A5** | `I2C_SCL` | GY-SHT30-D `SCL` | Output | Hardware I2C SCL (`PC5`). **Inner pad — not on the edge header** |
| **D9** | `OUT_FAN_RLY` | 1-Channel Relay Board `IN` | Output | Active LOW Exhaust Fan control |
| **D13** | `LED_STATUS` | Onboard LED | Output | Heartbeat indicator |
| **A0 / A1** | `ADDR_SEL0/1` | Address jumpers | Input | **`0x04` = both left unconnected** (as-built). Leave both pads open — `WIRING.md` §9.1 |
| **DTR / TXO / RXI** | Programming | FTDI / CP2102 adapter (5V) | — | `DTR` must be wired or uploads time out. Disconnect the buck 5V while the adapter is attached |

---

### 3.4. Ground Floor ESP32 Node 1: Sump Telemetry (0x05)
| ESP32 GPIO | Pin Function | Connected Hardware | Direction | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **GPIO 5** | `US_TRIG` | AJ-SR04M Sensor `TRIG` | Output | 10µs ultrasonic trigger |
| **GPIO 18** | `US_ECHO` | AJ-SR04M Sensor `ECHO` | Input | 5V $\to$ 3.3V resistor voltage divider (1kΩ/2kΩ) |
| **GPIO 2** | `LED_STATUS` | Wi-Fi Heartbeat LED | Output | Solid when Wi-Fi connected |

---

### 3.5. Ground Floor ESP32 Node 2: Motor Control & Interlocks (0x06)
| ESP32 GPIO | Pin Function | Connected Hardware | Direction | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **GPIO 16** | `IN_SUMP_AC` | 220V AC Optocoupler 1 `OUT` | Input | Sump Motor 240V AC Active Sense. **Moved off GPIO 34 (2026-08-27)** — that pin is ADC1 and is needed for a CT channel |
| **GPIO 17** | `IN_BORE_AC` | 220V AC Optocoupler 2 `OUT` | Input | Borewell Motor 240V AC Active Sense. **Moved off GPIO 35** |
| **GPIO 32** | `IN_SUMP_CT_L1` | SCT-013-030 clamp | Input | Analog, ADC1_CH4. Sump phase L1 — `WIRING.md` §11.3 |
| **GPIO 33** | `IN_SUMP_CT_L2` | SCT-013-030 clamp | Input | Analog, ADC1_CH5. Sump phase L2 |
| **GPIO 34** | `IN_SUMP_CT_L3` | SCT-013-030 clamp | Input | Analog, ADC1_CH6. Sump phase L3 |
| **GPIO 35** | `IN_BORE_CT_L1` | SCT-013-030 clamp | Input | Analog, ADC1_CH7. Borewell phase L1 |
| **GPIO 36** | `IN_BORE_CT_L2` | SCT-013-030 clamp | Input | Analog, ADC1_CH0. Borewell phase L2 |
| **GPIO 39** | `IN_BORE_CT_L3` | SCT-013-030 clamp | Input | Analog, ADC1_CH3. Borewell phase L3 |
| **GPIO 25** | `OUT_SUMP_FLOT` | 4-Ch Relay Module `IN1` | Output | Dry contact to Sump Motor Starter |
| **GPIO 26** | `OUT_BORE_FLOT` | 4-Ch Relay Module `IN2` | Output | Dry contact to Borewell Starter (Overflow Cutoff) |
| **GPIO 27** | `OUT_AUX_RLY1` | 4-Ch Relay Module `IN3` | Output | Auxiliary remote override |
| **GPIO 14** | `OUT_AUX_RLY2` | 4-Ch Relay Module `IN4` | Output | Auxiliary remote override |
| **GPIO 2** | `LED_STATUS` | Wi-Fi Heartbeat LED | Output | Solid when Wi-Fi connected |

