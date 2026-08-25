# System Power Budget & Electrical Calculations

**Document Version:** 1.2  
**Date:** 2026-08-25  
**Power Source:** Hi-Link HLK-20M12 (Mains 100–240V AC to 12V DC @ 20W / 1.67A)

> **v1.1 corrections:** node `0x04` (Battery Room) was missing from every total — added as §2.3 and folded into §3/§4. §5's cable drop charged the whole trunk at a single node's current; it now sums the per-segment loads, which is the honest method and roughly 40% worse than the old figure.
>
> **v1.2 (2026-08-25):** node `0x01` deleted. The dosing tank sensor moved onto the hub's own rail (`WIRING.md` §13), so the hub subsystem grew by one AJ-SR04M and the RS485 trunk lost its first hop and 65 mA of downstream load. Conclusion unchanged: the supply is loafing.

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
       ┌────────────────┬─────────┴────────┬────────────────┐
       │                │                  │                │
 ┌─────▼──────┐  ┌──────▼──────┐  ┌────────▼───────┐ ┌──────▼───────┐
 │  ESP32 HUB │  │ TANK NODES  │  │ BATTERY ROOM   │ │ CAT5e TRUNK  │
 │ Buck (85%) │  │ 0x02 / 0x03 │  │ NODE 0x04      │ │ I²R losses   │
 │ ESP32S     │  │ Buck (85%)  │  │ Buck (85%)     │ │ (§5)         │
 │ + SHT30    │  │ Nano + AJ   │  │ Pro Mini 5V    │ │              │
 │ + MAX485   │  │ + MAX485    │  │ + SHT30        │ │              │
 │ + Optos    │  │ (x2)        │  │ + MAX485       │ │              │
 │ + AJ-SR04M│  │             │  │ + Fan Relay    │ │              │
 │  (dosing)  │  │             │  │                │ │              │
 └────────────┘  └─────────────┘  └────────────────┘ └──────────────┘
```

---

## 2. Power Consumption Breakdown by Subsystem

### 2.1. ESP32-S HUB Subsystem (12V -> 5V Buck -> 3.3V LDO)

| Component | Operating Voltage | Peak Current | Nominal Current | Worst-Case Power |
| :--- | :--- | :--- | :--- | :--- |
| **ESP32-S Module** (WiFi TX peak) | 3.3V DC | 240 mA | 130 mA | 0.792 W |
| **SHT30 Sensor** | 3.3V DC | 1.5 mA | 0.8 mA | 0.005 W |
| **MAX485 Transceiver** (TX mode) | 5.0V / 3.3V DC | 60 mA | 15 mA | 0.300 W |
| **Optocoupler Inputs** (4x PC817 dry-contact + 2x AC opto module) | 3.3V DC | 15 mA | 6 mA | 0.050 W |
| **Onboard LEDs & Status** | 3.3V DC | 10 mA | 5 mA | 0.033 W |
| **AJ-SR04M** (dosing tank, direct — `WIRING.md` §13) | 5.0V DC | 30 mA | 10 mA | 0.150 W |
| **Subtotal (HUB Low-Voltage Loads)** | — | — | — | **1.330 W** |
| **DC-DC Buck Converter Efficiency ($\eta = 85\%$)** | — | — | — | $\mathbf{1.565\text{ W}}$ |
| **HUB Current Draw on 12V Rail** | **12V DC** | **~130 mA (Peak)** | **~80 mA (Nom)** | **~1.57 W** |

---

### 2.2. Remote Tank Node Subsystem (Per Node: Arduino Nano + Sensor + RS485)

| Component | Operating Voltage | Peak Current | Nominal Current | Worst-Case Power |
| :--- | :--- | :--- | :--- | :--- |
| **Arduino Nano (ATmega328P @ 16MHz)**| 5.0V DC | 35 mA | 25 mA | 0.175 W |
| **AJ-SR04M Ultrasonic Sensor** | 5.0V DC | 30 mA | 10 mA | 0.150 W |
| **MAX485 Transceiver** (TX mode) | 5.0V DC | 60 mA | 5 mA (RX idle)| 0.300 W |
| **Status LEDs & Pull-ups** | 5.0V DC | 8 mA | 4 mA | 0.040 W |
| **Subtotal (Per Node 5V Loads)** | — | — | — | **0.665 W** |
| **DC-DC Buck Converter Efficiency ($\eta = 85\%$)** | — | — | — | $\mathbf{0.782\text{ W}}$ |
| **Current Draw per Node on 12V Rail**| **12V DC** | **~65 mA (Peak)** | **~35 mA (Nom)** | **~0.78 W** |

---

### 2.3. Battery Room Node `0x04` (Pro Mini + SHT30 + Fan Relay)

Different load profile from the tank nodes: no ultrasonic sensor, but a relay coil that is energized whenever the exhaust fan runs. Peak assumes fan ON **and** an RS485 transmission in the same instant.

| Component | Operating Voltage | Peak Current | Nominal Current | Worst-Case Power |
| :--- | :--- | :--- | :--- | :--- |
| **Arduino Pro Mini (ATmega328P @ 16MHz)** | 5.0V DC | 25 mA | 20 mA | 0.125 W |
| **GY-SHT30-D Sensor** | 5.0V DC | 1.5 mA | 0.8 mA | 0.008 W |
| **MAX485 Transceiver** (TX mode) | 5.0V DC | 60 mA | 5 mA (RX idle) | 0.300 W |
| **1-Ch Relay Board** (coil + opto, fan ON) | 5.0V DC | 76 mA | 0 mA (fan OFF) | 0.380 W |
| **Status LED** | 5.0V DC | 5 mA | 2 mA | 0.025 W |
| **Subtotal (Node 0x04 5V Loads)** | — | **~168 mA** | **~28 mA** | **0.838 W** |
| **DC-DC Buck Converter Efficiency ($\eta = 85\%$)** | — | — | — | $\mathbf{0.986\text{ W}}$ |
| **Current Draw on 12V Rail** | **12V DC** | **~82 mA (Peak)** | **~14 mA (Nom)** | **~0.99 W** |

> The Pro Mini's onboard LDO is bypassed — 5V is fed to `VCC` from the node's buck (`WIRING.md` 10.1). Feeding `RAW` from 12V instead would put $(12-5)\times0.168 \approx 1.2\text{ W}$ into a SOT-23 package rated for roughly a third of that.

---

## 3. Total System Peak Power Summary

| Subsystem | Quantity | Worst-Case Peak Power | Worst-Case Current @ 12V |
| :--- | :---: | :---: | :---: |
| **ESP32 HUB Subsystem** (incl. dosing AJ-SR04M) | 1 | 1.57 W | 130 mA |
| **RWT Node `0x02`** | 1 | 0.78 W | 65 mA |
| **TWT Node `0x03`** | 1 | 0.78 W | 65 mA |
| **Battery Room Node `0x04`** | 1 | 0.99 W | 82 mA |
| **CAT5e Trunk I²R Losses** (see §5) | 1 | 0.08 W | — (dissipation, not extra draw) |
| **Total System Worst-Case Load** | — | $\mathbf{4.20\text{ W}}$ | $\mathbf{342\text{ mA}}$ |

Notes on this table:
* **Node `0x04` was absent from it entirely** before v1.1. The v1.0 totals (3.81 W / 311 mA) counted the hub and three tank nodes only.
* v1.2 deleted node `0x01`: its 0.78 W / 65 mA left the table and ~0.18 W / 15 mA of it reappeared inside the hub subsystem as the directly-attached dosing sensor. Net saving ~0.6 W, one buck converter's conversion loss included.
* Cable loss is power burned in the copper, not current the supply must additionally source — it is added to the wattage column but not to the current column.
* Every figure is simultaneous worst case: all four nodes transmitting, the exhaust fan energized and the ESP32 in Wi-Fi TX. Real steady-state draw is roughly a third of this.

---

## 4. Power Supply Headroom & Thermal Margins

$$\text{HLK-20M12 Rated Output Capacity} = 20.0\text{ Watts } (12\text{V} @ 1.67\text{A})$$
$$\text{Total System Worst-Case Demand} = 4.20\text{ Watts } (12\text{V} @ 0.342\text{A})$$
$$\text{Power Supply Utilization} = \frac{4.20\text{ W}}{20.0\text{ W}} \times 100\% = \mathbf{21.0\%}$$
$$\mathbf{Safety Headroom Margin} = \mathbf{+79.0\%} \quad (\approx 4.8\times \text{Safety Factor})$$

### Thermal & Longevity Impact
Because the HLK-20M12 operates at only ~21% of its rated capacity:
1. **Low Internal Self-Heating:** Keeps internal electrolytic capacitors cool, extending continuous operating lifetime beyond 50,000+ hours in warm pump-room environments.
2. **Expansion Capacity:** Ample reserve capacity is available for future peripherals (touchscreen display, additional flow sensors, motorized solenoid valves).

---

## 5. CAT5e Cable Voltage Drop Calculation

- Standard solid-copper 24 AWG CAT5e wire resistance: $R_{\text{single}} \approx 0.084\ \Omega/\text{meter}$.
- Using **2 conductors in parallel** for +12V (Orange pair) and **2 conductors in parallel** for GND (Green pair):
  $$R_{\text{effective}} = \frac{0.084\ \Omega/\text{m}}{2} = 0.042\ \Omega/\text{meter}$$

### Method — sum the segments, do not charge the whole run at one node's current

The previous revision multiplied the entire trunk length by the *furthest node's* 65 mA. That understates the drop: the trunk is shared, so each segment carries the sum of every node downstream of it. Correct form, with $R_{\text{loop}} = 0.084\ \Omega/\text{m}$:

$$\Delta V_{\text{total}} = R_{\text{loop}} \times \sum_{i} L_i \, I_i$$

Segment currents for the as-installed chain `0x00 -> 0x04 -> 0x03 -> 0x02` (the hub and its dosing sensor are powered at the source and contribute nothing to the trunk):

| Segment | Length | Current it carries | Nodes fed downstream |
| :--- | :--- | :--- | :--- |
| Source → `0x04` (`Cat5e 1`) | TBM | **212 mA** | 0x04 + 0x03 + 0x02 |
| `0x04` → `0x03` (`Cat5e 2`) | TBM | **130 mA** | 0x03 + 0x02 |
| `0x03` → `0x02` (`Cat5e 3`) | TBM | **65 mA** | 0x02 |

### Worked bound (10 m assumed for each TBM hop, i.e. ~30 m total)

$$\Delta V = 0.084 \times (10 \times 0.212 + 10 \times 0.130 + 10 \times 0.065) = \mathbf{0.342\text{ V}}$$

- Voltage arriving at RWT node `0x02`: $12.0\text{V} - 0.34\text{V} = \mathbf{11.66\text{V DC}}$.
- The Mini560 / LM2596 buck regulates from $7.0\text{V}$ to $28\text{V DC}$, so 11.66 V leaves **4.7 V of margin** before regulation is even threatened.
- For comparison, the v1.0 single-node method gave 0.273 V for a *longer* 50 m run. Summing correctly over ~30 m gives 0.342 V. Both pass comfortably; the point is that the method now scales honestly if the run grows or nodes are added.

**Re-run this once the TBM lengths in `WIRING.md` §12 are measured.** Substitute the real $L_i$ into the sum above — no other part of this document changes.

> Deleting node `0x01` removed the shortest, most heavily loaded segment from this sum (1.5 m at 277 mA). The trunk now starts at the battery room run.
