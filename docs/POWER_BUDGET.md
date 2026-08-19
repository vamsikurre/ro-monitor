# System Power Budget & Electrical Calculations

**Document Version:** 1.0  
**Date:** 2026-08-19  
**Power Source:** Hi-Link HLK-20M12 (Mains 100–240V AC to 12V DC @ 20W / 1.67A)

---

## 1. Power Architecture Summary

```
                       230V AC Mains (50Hz)
                                │
                        [ 1A Slow Fuse ]
                                │
                     ┌──────────▼──────────┐
                     │  HLK-20M12 (20W)    │
                     │  Output: 12V / 1.67A│
                     └──────────┬──────────┘
                                │
                    12V DC Distribution Rail
            ┌───────────────────┼───────────────────┐
            │                   │                   │
  ┌─────────▼─────────┐ ┌───────▼───────┐ ┌─────────▼─────────┐
  │     ESP32 HUB     │ │  TANK NODE 1  │ │ TANK NODES 2 & 3  │
  │ 12V->5V Buck (85%)│ │(12V->5V Buck) │ │ (12V->5V Bucks)   │
  │ ESP32S + SHT30    │ │ Nano + JSN    │ │ 2x (Nano + JSN    │
  │ + MAX485 + Optos  │ │ + MAX485      │ │    + MAX485)      │
  └───────────────────┘ └───────────────┘ └───────────────────┘
```

---

## 2. Power Consumption Breakdown by Subsystem

### 2.1. ESP32-S HUB Subsystem (12V -> 5V Buck -> 3.3V LDO)

| Component | Operating Voltage | Peak Current | Nominal Current | Worst-Case Power |
| :--- | :--- | :--- | :--- | :--- |
| **ESP32-S Module** (WiFi TX peak) | 3.3V DC | 240 mA | 130 mA | 0.792 W |
| **SHT30 Sensor** | 3.3V DC | 1.5 mA | 0.8 mA | 0.005 W |
| **MAX485 Transceiver** (TX mode) | 5.0V / 3.3V DC | 60 mA | 15 mA | 0.300 W |
| **Optocoupler Inputs** (5 channels) | 3.3V DC | 15 mA | 6 mA | 0.050 W |
| **Onboard LEDs & Status** | 3.3V DC | 10 mA | 5 mA | 0.033 W |
| **Subtotal (HUB Low-Voltage Loads)** | — | — | — | **1.180 W** |
| **DC-DC Buck Converter Efficiency ($\eta = 85\%$)** | — | — | — | $\mathbf{1.388\text{ W}}$ |
| **HUB Current Draw on 12V Rail** | **12V DC** | **~116 mA (Peak)** | **~75 mA (Nom)** | **~1.39 W** |

---

### 2.2. Remote Tank Node Subsystem (Per Node: Arduino Nano + Sensor + RS485)

| Component | Operating Voltage | Peak Current | Nominal Current | Worst-Case Power |
| :--- | :--- | :--- | :--- | :--- |
| **Arduino Nano (ATmega328P @ 16MHz)**| 5.0V DC | 35 mA | 25 mA | 0.175 W |
| **JSN-SR04T Ultrasonic Sensor** | 5.0V DC | 30 mA | 10 mA | 0.150 W |
| **MAX485 Transceiver** (TX mode) | 5.0V DC | 60 mA | 5 mA (RX idle)| 0.300 W |
| **Status LEDs & Pull-ups** | 5.0V DC | 8 mA | 4 mA | 0.040 W |
| **Subtotal (Per Node 5V Loads)** | — | — | — | **0.665 W** |
| **DC-DC Buck Converter Efficiency ($\eta = 85\%$)** | — | — | — | $\mathbf{0.782\text{ W}}$ |
| **Current Draw per Node on 12V Rail**| **12V DC** | **~65 mA (Peak)** | **~35 mA (Nom)** | **~0.78 W** |

---

## 3. Total System Peak Power Summary

| Subsystem | Quantity | Worst-Case Peak Power | Worst-Case Current @ 12V |
| :--- | :---: | :---: | :---: |
| **ESP32 HUB Subsystem** | 1 | 1.39 W | 116 mA |
| **Tank Node 1 (Dosing)** | 1 | 0.78 W | 65 mA |
| **Tank Node 2 (Raw Water)** | 1 | 0.78 W | 65 mA |
| **Tank Node 3 (Treated Water)** | 1 | 0.78 W | 65 mA |
| **CAT5e Cable Distribution Losses (Worst-case 50m)** | 1 | 0.08 W | — |
| **Total System Worst-Case Load** | — | $\mathbf{3.81\text{ W}}$ | $\mathbf{311\text{ mA}}$ |

---

## 4. Power Supply Headroom & Thermal Margins

$$\text{HLK-20M12 Rated Output Capacity} = 20.0\text{ Watts } (12\text{V} @ 1.67\text{A})$$
$$\text{Total System Worst-Case Demand} = 3.81\text{ Watts } (12\text{V} @ 0.311\text{A})$$
$$\text{Power Supply Utilization} = \frac{3.81\text{ W}}{20.0\text{ W}} \times 100\% = \mathbf{19.05\%}$$
$$\mathbf{Safety Headroom Margin} = \mathbf{+80.95\%} \quad (\approx 5.2\times \text{Safety Factor})$$

### Thermal & Longevity Impact
Because the HLK-20M12 operates at only ~20% of its rated capacity:
1. **Low Internal Self-Heating:** Keeps internal electrolytic capacitors cool, extending continuous operating lifetime beyond 50,000+ hours in warm pump-room environments.
2. **Expansion Capacity:** Ample reserve capacity is available for future peripherals (touchscreen display, additional flow sensors, motorized solenoid valves).

---

## 5. CAT5e Cable Voltage Drop Calculation

- Standard solid-copper 24 AWG CAT5e wire resistance: $R_{\text{single}} \approx 0.084\ \Omega/\text{meter}$.
- Using **2 conductors in parallel** for +12V (Orange pair) and **2 conductors in parallel** for GND (Green pair):
  $$R_{\text{effective}} = \frac{0.084\ \Omega/\text{m}}{2} = 0.042\ \Omega/\text{meter}$$

### Drop across a 50-meter cable run to the furthest tank node:
- Round-trip loop distance: $2 \times 50\text{ m} = 100\text{ m}$ of single conductor equivalent.
- Loop resistance $R_{\text{loop}} = 50\text{ m} \times (0.042 + 0.042)\ \Omega/\text{m} = 4.2\ \Omega$.
- Furthest node current $I_{\text{node}} \approx 0.065\text{ A}$.
$$\Delta V = I_{\text{node}} \times R_{\text{loop}} = 0.065\text{ A} \times 4.2\ \Omega = \mathbf{0.273\text{ V}}$$
- Voltage arriving at Tank Node 3: $12.0\text{V} - 0.27\text{V} = \mathbf{11.73\text{V DC}}$.
- Since the tank node buck converter functions stably with any input from $7.0\text{V}$ to $28\text{V DC}$, an input of $11.73\text{V DC}$ ensures $100\%$ rock-solid regulation with negligible power loss.
