# RO Plant Hardware Analysis

**Document Version:** 1.0  
**Date:** 2026-08-19  
**Source Evidence:** 19 High-Resolution Photographs in `/images`

---

## 1. Executive Summary

This analysis provides an evidence-based breakdown of the existing Reverse Osmosis (RO) plant electrical, electronic, and mechanical systems. Every finding is backed by physical photographic evidence from the `images/` directory.

The system is controlled by an **Aster NXT 11** embedded controller built on the **ASTERO-LTE-CPU-22-VER-1.5** PCB, accompanied by an **Initiative Engineering Multiport Valve Controller**, a **TC Contactor TCDP302** high-current motor starter, flowmeters, and sensor transducers.

---

## 2. Image-by-Image Photographic Observations

| Image Filename | Primary Subject | Detailed Photographic Observations |
| :--- | :--- | :--- |
| `IMG_2836.JPG` | Front Panel Overview | Stainless steel skid panel showing the **Aster NXT 11** controller LCD interface (displaying "* SYSTEM FULL !! *"), system status LED indicators, pressure gauges, and vertical flow rotameters. |
| `IMG_2837.JPG` | Enclosure Side/Rear | Controller housing enclosure mounting, external conduit entry, and stainless steel structural frame. |
| `IMG_3372.JPG` | Internal Enclosure | High-voltage compartment: heavy gauge AC wiring, contactor assembly, terminal junction blocks, and AC-to-DC / filtering modules. |
| `IMG_3373.PNG` | Power Junction & Fuses | Close-up of internal AC terminal blocks, auxiliary wiring harnesses, and grounding points. |
| `IMG_3375.PNG` | Control Board Wiring | PCB ribbon cables connecting display panel to the main I/O board, screw terminal landings with blue, black, and red signal wiring. |
| `IMG_3383.JPG` | CPU Board Overview | Full PCB view of **ASTERO-LTE-CPU-22-VER-1.5** showing relay bank (RL1, RL2, ALARM), digital input headers, op-amp signal conditioning, and microcontroller circuitry. |
| `IMG_3384.JPG` | Terminal Markings | Close-up macro of green pluggable terminal blocks showing markings: `HPS`, `LPS`, `TWT FLOTY`, `RWT FLOTY`, `DOS LVL`, `RL1`, `RL2`. |
| `IMG_3385.JPG` | Board Nomenclature & Relays | Clear view of silkscreen label `ASTERO-LTE-CPU-22-VER-1.5`, Hongfa `HF3FF-012-1ZST` 12VDC relays, and barcode label `NXC2509GB018`. |
| `IMG_3389.JPG` | Sensor & Pulse Terminals | Macro shot showing bottom terminal blocks: `COND` (Conductivity probe), `PULSE O/P`, `FLOW 1`, `FLOW 2` (terminals marked with signal, ground, +). |
| `IMG_3395.JPG` | RO Skid Wide View | Full view of skid assembly: **Aster NXT 11** controller, **Alfa Aerosol** certified FRP pressure vessel (NSF/ANSI/CAN-61), and stainless steel pipework. |
| `IMG_3396.JPG` | Flowmeter (Rotameter) | **Aster F-1200 LG** rotameter rated 200–1200 LPH (4–20 LPM) with red float indicating flow rate. |
| `IMG_3399.JPG` | Main PCB & Terminal Block (Top View) | Comprehensive top view of `ASTERO-LTE-CPU-22-VER-1.5` showing relay outputs, dry contact sensor inputs, optocoupler isolators, and analog sections. |
| `IMG_3400.JPG` | PCB Component Layout | Detailed view of IC footprint `IC1`, resistor divider network (`R14`–`R21`, `RN1`), jumper blocks (`J15`, `J16`), and power rail filtering. |
| `IMG_3408.JPG` | Multiport Valve Display | **Initiative Engineering** automated multiport valve controller LCD displaying `* FILTER * BACKWASH TIME : 05` with membrane keypad. |
| `IMG_3409.PNG` | Power & Driver ICs | Close-up of **LM7805** (TO-220 5V linear regulator with heatsink nut), **ULN2003AN** (7-channel Darlington relay driver array), and `HF3FF 012-1ZST` relays (10A 250VAC / 10A 277VAC). |
| `IMG_3410.PNG` | Analog & Charge Pump Section | High-magnification shot showing **LM324L** (Quad operational amplifier for conductivity / analog front-end) and **TC7660** (Charge Pump DC-to-DC voltage converter for negative rail generation in AC conductivity sensing). |
| `IMG_3411.PNG` | Motor Contactor | **TC Contactor TCDP302** Definite Purpose Contactor (Rating: 600V max, FLA 30A, LRA 150A, 1-Phase 120V 2HP / 240V 3HP, UL/CSA/CE certified). |
| `IMG_3414.PNG` | Mains Power/Filter Board | Dedicated input protection board featuring `BLX-A` fuse holder, blue MOV varistor, green toroidal common-mode choke inductor, yellow X2 suppression capacitors, and screw terminal blocks. |
| `IMG_3416.PNG` | Contactor Coil Terminals | Close-up of contactor coil rating **240V 50Hz** with terminal `A2` having green/blue lead connected via spade connector. |

---

## 3. Identified System Equipment

### 3.1. Main Controller: Aster NXT 11
- **PCB Model:** `ASTERO-LTE-CPU-22-VER-1.5`
- **Sub-components:**
  - Microcontroller / CPU core (IC1 DIP footprint / sub-module)
  - **LM7805** linear regulator for internal 5V logic supply
  - **ULN2003AN** relay driver IC
  - **LM324L** quad op-amp (used for conductivity signal amplification and filtering)
  - **TC7660** charge pump inverter (used to provide bipolar voltage excitation for conductivity probes to prevent electrode polarization)
  - 3x **Hongfa HF3FF-012-1ZST** power relays (12VDC coil, 10A @ 250VAC switching capacity)
  - Input protection and resistor array networks (`RN1`, `R14`–`R21`)

### 3.2. Motor Starter Contactor
- **Model:** TC Contactor `TCDP302`
- **Coil Voltage:** 240V AC 50Hz (terminals A1 / A2)
- **Power Contacts:** Single/Dual-pole contactor rated up to 30A Full Load Amps (FLA), 150A Locked Rotor Amps (LRA), suitable for driving the High Pressure Pump (HPP) motor (2–3 HP @ 240VAC).

### 3.3. Filter Backwash Controller
- **Manufacturer:** Initiative Engineering
- **Function:** Automated media filter backwash valve controller with timer functionality (`* FILTER * BACKWASH TIME : 05`).

### 3.4. Hydraulic & Filtration System
- **Membrane Housing:** Alfa Aerosol Pressure Vessels (Certified to NSF/ANSI/CAN-61)
- **Rotameter:** Aster F-1200 LG (Scale: 200 – 1200 LPH / 4 – 20 LPM)

---

## 4. Identified PCB Terminal Blocks & Pinout (ASTERO-LTE-CPU-22-VER-1.5)

Based on direct inspection of `IMG_3384.JPG`, `IMG_3385.JPG`, `IMG_3389.JPG`, `IMG_3399.JPG`, and `IMG_3400.JPG`, the primary terminal blocks are mapped as follows:

```
┌─────────────────────────────────────────────────────────────┐
│           ASTERO-LTE-CPU-22-VER-1.5 TERMINAL BLOCKS         │
├─────────────────────────────────────────────────────────────┤
│ Top Block (Relay / Alarm):                                  │
│   [ ALARM ]  [ R ] [ ... ]                                  │
├─────────────────────────────────────────────────────────────┤
│ Main Signal & Relay Block (Top to Bottom):                  │
│   [ HPS       ]  C   | NO    (High Pressure Switch)         │
│   [ LPS       ]  C   | NC    (Low Pressure Switch)          │
│   [ TWT FLOTY ]  C   | NC    (Treated Water Tank Float)     │
│   [ RWT FLOTY ]  C   | NC    (Raw Water Tank Float)         │
│   [ DOS LVL   ]  C   | NC    (Dosing Tank Level Switch)     │
│   [ RL2       ]  C   | NO    (Relay 2 Output - e.g. RWP)    │
│   [ RL1       ]  C   | NO    (Relay 1 Output - e.g. HPP)    │
├─────────────────────────────────────────────────────────────┤
│ Flow Sensor Block:                                          │
│   [ FLOW 1    ]  R   | G   | B  (+5V / Signal / GND)        │
│   [ FLOW 2    ]  ...                                        │
├─────────────────────────────────────────────────────────────┤
│ Bottom Section:                                             │
│   [ COND      ]  Probe Terminal 1 | Probe Terminal 2        │
│   [ PULSE O/P ]  Pulse Output Terminal                      │
└─────────────────────────────────────────────────────────────┘
```

---

## 5. Candidate vs. Confirmed vs. Unknown Signals

### 5.1. Confirmed Signals (Physical Evidence Verified)
1. **HPS (High Pressure Switch Input):** Dry contact input, marked `C` (Common) and `NO` (Normally Open).
2. **LPS (Low Pressure Switch Input):** Dry contact input, marked `C` (Common) and `NC` (Normally Closed).
3. **TWT FLOTY (Treated Water Tank Float):** Switch input marked `C` and `NC`.
4. **RWT FLOTY (Raw Water Tank Float):** Switch input marked `C` and `NC`.
5. **DOS LVL (Dosing Level Switch):** Switch input marked `C` and `NC`.
6. **RL1 Output:** Relay 1 dry contact `C` / `NO` (switches contactor coil / pump power).
7. **RL2 Output:** Relay 2 dry contact `C` / `NO` (switches raw water pump or auxiliary solenoid).
8. **Contactor Coil Control (HPP):** 240V AC 50Hz on contactor A1/A2 coil.
9. **FLOW 1 / FLOW 2:** Flow sensor pulse inputs.
10. **COND:** EC/TDS conductivity probe interface with AC excitation.

### 5.2. Candidate Signals (To Be Monitored by ESP32 HUB)
1. **HPP Status (240V AC):** Monitor voltage across contactor coil or pump supply lines using an AC optocoupler module.
2. **RWP Status (240V AC / Relay):** Monitor voltage across RWP feed or RL2 contact using an AC optocoupler module.
3. **TWT Float Status:** Tap into TWT FLOTY terminals via high-impedance opto-isolated dry contact buffer (or read via RS485 Nano node).
4. **RWT Float Status:** Tap into RWT FLOTY or monitor via remote tank Node #2.
5. **Dosing Level Status:** Tap into DOS LVL or monitor via remote tank Node #1 ultrasonic sensor.

### 5.3. Unknown / Unverified Signals (Requiring Multi-meter Check on Site)
1. **Internal Logic Voltage on C (Common) terminals:** Determine whether `C` is tied to +5V, +12V, or GND through internal pull-up resistors.
2. **PULSE O/P Electrical Characteristics:** Verify if open-collector NPN or 5V logic pulse.
3. **Communication Header `J16`:** Header pins near CPU marked J16 may be factory programming / UART; currently unverified.

---

## 6. Recommended Interfacing Strategy

### 6.1. High-Voltage Monitoring (HPP & RWP - 240V AC)
- **Technique:** Do **NOT** connect 240V AC directly to ESP32 or any low-voltage logic.
- **Circuit:** Dedicated 240V AC Optocoupler Isolation Board (utilizing PC814 / EL814 bidirectional AC optocouplers or PC817 with bridge rectifier + series safety dropping resistor + RC snubber + pull-up to ESP32 3.3V).
- **Safety Standard:** 3.75kV – 5kV galvanic isolation barrier between mains and ESP32 low-voltage DC logic.

### 6.2. Low-Voltage Switch & Relay Monitoring
- **Technique:** Galvanic opto-isolation (PC817 based) for dry contact inputs so that potential ground loops or fault voltages in the Aster controller do not feed back into the ESP32 Hub.

### 6.3. Remote Tank Levels (Dosing, Raw, Treated)
- **Technique:** Dedicated Arduino Nano nodes located at each tank running JSN-SR04T / AJ-SR04M waterproof ultrasonic sensors, transmitting filtered numeric millimeter levels over daisy-chained RS485 (MAX485 / MAX13487) back to the ESP32 Hub.

---

## 7. Safety Notes & Precautions

1. **Galvanic Isolation is Mandatory:** The controller enclosure houses live 240V AC (50Hz) wiring on the main contactor, relays, and power supply. Mains wires must maintain at least 6mm physical creepage/clearance from all ESP32 SELV wiring.
2. **No Mains Control in Phase 1:** ESP32 HUB operates strictly as a **passive monitoring system**. It does not energize or override pump relays or contactors.
3. **Dedicated Power Supply:** System uses an independent **HLK-20M12** (12V DC / 20W) power supply rather than tapping the Aster board's internal LM7805 regulator.
