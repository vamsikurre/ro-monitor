# Wiring & Interconnection Specifications

**Document Version:** 2.5  
**Date:** 2026-08-27  
**Scope:** Complete Pinout Mappings, CAT5e Drop Schedules, RS485 Daisy-Chain, 240V AC Isolation, 4-Channel Opto-Isolation (Inputs), Aster Terminal Contact Polarity & Alarm Output, 4-Channel Relay Module (Future Asterro Float/Level Emulation), I2C Environmental Sensor, and Tank Node Power & Wiring.

> **Pin allocation authority:** Section 1 below describes the hub **as physically built** and is the single source of truth, per `superpowers/specs/2026-08-25-ro-monitor-phase-b-design.md` §3.5. `HARDWARE.md` Section 3.1 is kept identical to it and is corrected — not reconciled — whenever the two diverge.

---

## 0. Build State — What Is Verified, What Is Still To Do

Kept at the top because it is the first thing anyone opening this document in the
RO room needs. **Last updated 2026-08-30.**

> **The hub as it is actually wired is drawn in
> [`images/hardware/wiring-Hub.jpg`](../images/hardware/wiring-Hub.jpg)** —
> every module, every header, every wire colour, drawn from the built board.
> Editable source: the **`Hub`** page of `wiring.drawio` at the repo root — the
> filename is drawio's own export name, so re-exporting overwrites it in place.
>
> **It draws the plant DEPLOYED, not the plant as it stands.** Every field
> connection to the Aster is on it: `LPS` to PC817 `IN3`, `ALARM` to `G13`, and all
> four relay outputs to the float terminals. **None of those wires exist yet** —
> §0.2 and §6.6 are the authority on what is actually connected, and §0.3.2 lists
> what the log looks like while they are not. A picture beats a checklist for
> anyone in a hurry, so read the two together or you will diagnose a fault in a
> wire that was never run.
>
> **And the board itself, photographed 2026-08-30:**
>
> | Photo | What it shows |
> | :--- | :--- |
> | [`hub_board_asbuilt_overview.jpg`](../images/hardware/hub_board_asbuilt_overview.jpg) | The whole perfboard. DevKit, HLK-20M12, RS485 module, the module headers along the right edge, and the mains-side opto boards on flying leads rather than on the board |
> | [`hub_board_asbuilt_psu_rs485.jpg`](../images/hardware/hub_board_asbuilt_psu_rs485.jpg) | HLK-20M12 markings legible (`100-240VAC 0.4A` → `12VDC 1.6A 20W`), the RS485 module's `A+`/`B-` screw terminal and its 4-pin `GND`/`RXD`/`TXD`/`VCC` JST, and the Cat5e landing |
> | [`hub_board_asbuilt_buck_headers.jpg`](../images/hardware/hub_board_asbuilt_buck_headers.jpg) | The 12 V → 5 V buck, both 2-pin distribution terminals, and **the female header strips the DevKit is seated in** |
>
> Start with these when diagnosing: this document explains *why* each connection
> is what it is, the drawing shows *where* it goes, the photos show *what is
> actually there*. **If any two disagree, meter the board** — no drawing,
> photograph or paragraph outranks a continuity test — which is exactly how the
> CT header's pin order was settled on 2026-08-30 (§0.2).

"Verified" below means *observed working on the hardware*, not *designed* and not
*compiled* — the distinction that let `IN_ALARM` sit in three documents and no
firmware for a day (§6.6, `docs/check_pinmap.py`).

### 0.1. Hub `0x00` — verified on the bench, 2026-08-27

| Subsystem | How it was verified |
| :--- | :--- |
| RS485 bus, nodes `0x02` / `0x03` / `0x04` | All three answering every poll cycle |
| Hub SHT30 (RO room) | Live temperature and humidity |
| Dosing ultrasonic (`GPIO 5` / `4`) | Echoing after a wiring fix |
| PC817 ch1-ch4 (`GPIO 26` / `25` / `33` / `32`) | **5 V injected into each input in turn**, each reporting under the right label — which is how the one-place channel rotation in §5 was found |
| 240 V AC optos (`GPIO 34` / `35`) | Validated against live contactors; the earlier "floating" reading was a broken wire at the module, since repaired |
| Hub relays ×4 (`GPIO 27` / `23` / `18` / `19`) | Cycled in sequence |
| Battery-room fan relay (node `0x04`, `D9`) | Clicks on command over RS485 |

### 0.2. Hub board — still to solder

**Corrected 2026-08-28:** nothing is connected to the Aster yet, and the tank
sensors are loose on the bench. The board-side headers are fitted; the field wiring
happens at deployment.

- [ ] **At deployment: connect PC817 `IN3` to the Aster `LPS` `C`/`NO` pair.** Nothing to *move* — `IN3` has never been wired. The output side (`V3` → `GPIO 33`) is already done. §6.6
- [x] **`IN_ALARM` header fitted 2026-08-27** — 2-pin: hub `GND` and `GPIO 13`, to the Aster `AUX OP` `C`/`NO` pair. No optocoupler, no resistor, no module. §6.6
- [x] **CT header fitted 2026-08-27, pin order confirmed with a meter 2026-08-30** — 4-pin, **`3V3` / `SN` / `SP` / `GND`**, not the 5-pin order in §14.0. This document had it as `3V3`/`SP`/`SN`/`GND` until the as-built diagram disagreed and the continuity test settled it; the DevKit's own header runs `3V3 | EN | SN | SP | G34`, so straight-across wiring gives `SN` first. **The breakout must therefore land RWP on pin 2 (`SN` = `GPIO 39`) and HPP on pin 3 (`SP` = `GPIO 36`)** — the reverse of what the old order implied. Leave both otherwise unconnected until the breakout exists.
- [ ] **Reseat or resolder the 4-ch relay header before deployment — one pin is intermittent.** 2026-09-02: the AUX relay (`GPIO 19`, `IN4`) would not click from `/cal`, then started working on its own. The firmware drove the pin and logged it correctly throughout, so this is downstream of the ESP32 and almost certainly a loose header pin.
  **It matters more than the spare relay suggests.** `IN4` carries nothing today, so its silence cost nothing — but `IN1`–`IN3` are on the same header, the same loom and the same handling, and they are TWT float, RWT float and dosing level. An intermittent on `IN1` tells the Aster the treated tank is full: production stops, then resumes, with nothing in any log to explain it.
  **The hub cannot detect this.** These are outputs with no feedback path, so no amount of firmware will ever see it. Physical fix only.
  **Acceptance check:** press all five buttons on `/cal` several times each while flexing the loom. Every press must click. Anything that skips once is not fixed.
- [ ] **Mark pin 1 on every header and mating plug on the hub board.** Not just the new ones: any header with a supply at one end and `GND` at the other shorts the rail if reversed, and most of them are built that way (§1). One pass with paint, once.

**Then verify, before trusting any of it:**

- [ ] Touch the two `IN_ALARM` wires together → alarm latches within one poll, the RO room flashes on the dashboard, a notification arrives
- [ ] Short PC817 `IN3` to `G` → `LPS` reads *Low pressure*; with the alarm also active the alert should name low feed pressure rather than saying "check the panel". Works on the bench with nothing attached to the Aster
- [ ] Confirm the RS485 terminators: **120 Ω at the hub and at node `0x02` only**. `0x03` and `0x04` are mid-chain and must have none — three terminators blunt the differential swing (§10, §12)

### 0.3. Off-board, in order of what blocks what

- [ ] **CT breakout board** — two resistors and two capacitors once, then 1 kΩ + 100 nF + socket per channel. §14.1. Provable with no clamp attached: the pedestal should read ~1650 mV (§14.0)
- [ ] **2 × SCT-013-030** for HPP and RWP. Measure which socket pads the clamp actually reaches — meter it, do not read the silkscreen (§14.2)
- [ ] **6 × SCT-013-030** for the ground floor: three phases each on the sump and borewell motors (§11.3). Neither motor has a readable nameplate, so size from the starter's overload dial and a clamp meter (§11.3.1)
- [ ] **Tank calibration ×4** — RWT, TWT, dosing, and sump when it exists. Until a tank is calibrated its level reads `--`, never a plausible wrong number
- [ ] **Tank node headers `J-LOOP` + `J-PRESS`** — on both `0x02` and `0x03`, plus the 100 R / 1 k / 100 nF. Solder them while the boards are open, sensor or no sensor; the firmware ships the loop reader already. §9.4.2
- [ ] **Node `0x05` and `0x06`** — Phase 2. Node `0x06` has no firmware, and its pin map is currently validated by nothing (see `docs/check_pinmap.py`)

### 0.3.1. Programming the hub — the onboard USB is dead

**As-built 2026-08-28.** The hub ESP32's onboard **CP2102 USB-serial chip is
destroyed** — killed by reverse polarity applied to the `5V` pin. The USB socket on
the module enumerates nothing and never will. **The only serial path to this board
is an external FTDI adapter wired to `TX0` / `RX0`.**

| | |
| :--- | :--- |
| Adapter | FTDI `FT232R`, `VID_0403/PID_6001` — the same one used for the Pro Mini nodes (§10.1) |
| Wiring | FTDI `TX` → ESP32 `RX0`, FTDI `RX` → ESP32 `TX0`, `GND` → `GND` |
| **Voltage jumper** | **3.3 V. NOT the 5 V used for the Pro Minis.** ESP32 pins are not 5 V tolerant; at 5 V the adapter's TX idles above `GPIO 3`'s rating |
| Auto-reset | `RTS` → `EN` **works** — esptool reports `Hard resetting via RTS pin` |
| Boot mode | **Manual, every flash.** `GPIO 0` is not driven by the adapter: hold `BOOT`, tap `EN`, release `BOOT`, then run esptool |

**Symptom if you forget the boot button:** `Failed to connect to ESP32: No serial
data received`. Identical wording to a wrong-port error, which is what makes it
confusing — the port is right, the chip is simply not listening.

**Do not go looking for a CP210x port.** There is no `VID_10C4` device on this
build; searching for one wastes time, as it did on 2026-08-28. `COM6` — the FTDI —
is the correct and only port.

**Keep the Arduino IDE closed while flashing.** Its Serial Monitor holds the port
and produces `PermissionError(13, 'Access is denied')`.

**The DevKit is socketed, not soldered** — it sits in female header strips
(`hub_board_asbuilt_buck_headers.jpg`). So the dead CP2102 is not a permanent
condition of this build: a replacement module pulls in and out with no desoldering,
and the FTDI path above is a workaround by choice, not by necessity. Worth knowing
before anyone plans around it. Re-check the module's own pin labels against §1 if
one is ever swapped; DevKit variants differ in what they print on `SP`/`SN`.

**Order by row pitch, not by pin count.** ESP32 DevKits are sold in more than one
row spacing — commonly 0.9" and 1.0", some 1.1" — and the listings almost never
say which, because the pin *count* and the pin *labels* can be identical across
pitches. A wrong-pitch board arrives looking correct and will not go into the
strips. **TODO: measure this socket and record pins-per-side and hole count here.**

**Measure it by counting holes, not with calipers.** The socket sits on 0.1"
perfboard, so the gap between the two female strips is a whole number of holes,
and that number *is* the spec to order against. Count it once and write it here.

Learned on 2026-09-03: a replacement bought on "38-pin ESP32 DevKit" alone had
matching pin count and matching labels, and a narrower row pitch. It could not be
seated. A module swap that should have taken a minute did not happen at all.

**The lesson worth carrying to the PCB:** one reversed supply took out the
programming interface of the board that runs the plant, and the board still works
only because the ESP32 core survived. `PCB_HUB_MOTHERBOARD.md` §6 now specifies
reverse-polarity protection and a dedicated programming header for exactly this.

### 0.3.2. Reading the log while nothing is connected

On the bench, with sensors unmounted and no Aster wiring, these are the **expected**
readings — none of them is a fault, and each has been mistaken for one:

| Reading | Why |
| :--- | :--- |
| `RWT 171 mm q100 BLIND -> --` | A loose sensor pointing at whatever is 17 cm away. `q100` means the echo is *consistent*, which makes it look convincing |
| `DOS 0 mm NO_ECHO -> --` | Sensor unpowered or aimed at nothing. It reads normally when pointed across a room |
| `TWT 3861 mm OK -> 0%` | Measuring the room. Past `empty`, so 0% is arithmetically right |
| `LPS normal` | `IN3` is unconnected, so the PC817 output transistor is off and `GPIO 33` floats high. **Not** the Aster's `LPS` state |
| `ALARM clear` | `GPIO 13` unconnected, internal pull-up holds it high |
| `HPP/RWP idle, CT --` | No clamps, no breakout. `CT --` is the honest answer |
| `Dosing chemical low (0%)` alert | Follows from `DOS` having no echo. Real alert, meaningless input |

The rule this keeps proving: **a confident-looking number from an unconnected
sensor is the easiest thing in this system to misread as a fault.** Check what is
physically attached before diagnosing anything.

### 0.4. Known open, not blocking

- **An 8-channel PC817 board is owned and unused.** The 4-channel part was fitted
  because it was already to hand (confirmed 2026-08-27). So `RWT FLOTY` and
  `DOS LVL` cost no purchase — but they are **no longer channel-limited, they are
  pin-limited**. With `IN_ALARM` on `GPIO 13`, the only inputs left on this board
  are `12`, `14` and `15`, and 12 and 15 are strapping pins. Two more taps are
  possible; eight are not. **Do not swap the board just because the bigger one
  exists** — it would mean rewiring four working, injection-verified channels for
  no current need. The point of recording it is that the future decision is
  "rewire four channels", not "buy a board and rewire four channels".

- **Node `0x03` sample quality** sat at `q=60` on the bench. Leave `US_TRIG_WIDTH_US` at 10 until the sensor is over water; only widen to 20 if it stays low there (§9.0)
- **Production firmware has never run on hardware.** It compiles and the ported CRC and level maths are proven identical to the field-verified sketch, but BLE pairing and AP+STA coexistence are untested (`firmware/hub_prod/README.md`)

---

## 1. ESP32-S Central Master Hub Complete Pin Allocation

The ESP32-S serves as the Central Telemetry Hub. It gathers telemetry from the local SHT30 environmental sensor **and the directly-attached dosing tank ultrasonic sensor**, isolates and reads the Aster controller status lines via a 4-channel PC817 optoisolator (all 4 channels used: `RL1`, `RL2`, `LPS`, `TWT FLOTY` — `IN_ALARM` is a direct input, §6.6), controls a 4-channel relay module for future float/level emulation, and acts as the RS485 Modbus Master for three remote slaves.

### Master Pin Mapping Table

Drawn form of this same table, with the modules and wire colours:
[`images/hardware/wiring-Hub.jpg`](../images/hardware/wiring-Hub.jpg)
(`wiring.drawio`, page `Hub`).


| ESP32 Pin | Signal Label | Connected Device / Module | Module Pin | Direction | Electrical Characteristics & Notes |
| :--- | :--- | :--- | :--- | :---: | :--- |
| **3V3** | `+3.3V_LOGIC` | SHT30 Sensor, XY-485 Module | `VCC` | Power Out | Regulated 3.3V logic power bus (from ESP32 onboard LDO) |
| **GND** | `GND_LOGIC` | All Local Sub-Modules & Opto/Relay | `GND` | Power Out | Common DC logic ground |
| **VIN / 5V** | `+5V_RAW` | LM2596 / Mini560 12V -> 5V Buck | `VOUT+` | Power In | Regulated +5.0V DC main board power (from HLK-20M12 step-down) |
| **GPIO 16** | `RS485_RX` | XY-485 Auto-Flow RS485 Module | `RXD` | Input | UART2 Serial RX (3.3V TTL logic) |
| **GPIO 17** | `RS485_TX` | XY-485 Auto-Flow RS485 Module | `TXD` | Output | UART2 Serial TX (3.3V TTL logic) |
| **GPIO 21** | `I2C_SDA` | GY-SHT30-D Temp/Humidity Sensor | `SDA` | Bidirectional | I2C Data Line (4.7k pull-up to 3.3V) |
| **GPIO 22** | `I2C_SCL` | GY-SHT30-D Temp/Humidity Sensor | `SCL` | Output | I2C Clock Line (Standard 100 kHz / Fast 400 kHz) |
| **GPIO 32** | `IN_TWT_FLOT`| 4-Ch PC817 Opto Board (Channel 4) | `V4` | Input | Active LOW when Aster `TWT FLOTY` loop is closed (`INPUT_PULLUP`) |
| **GPIO 26** | `IN_RL1_STAT`| 4-Ch PC817 Opto Board (Channel 1) | `V1` | Input | Active LOW when Aster RL1 / Multiport valve contact is closed (`INPUT_PULLUP`) |
| **GPIO 25** | `IN_RL2_STAT`| 4-Ch PC817 Opto Board (Channel 2) | `V2` | Input | Active LOW when Aster RL2 / Multiport valve contact is closed (`INPUT_PULLUP`) |
| **GPIO 33** | `IN_LPS`     | 4-Ch PC817 Opto Board (Channel 3) | `V3` | Input | Active LOW when the Aster `LPS` low-pressure contact (`C`/`NO`) is closed. **Took this channel from `IN_ALARM` on 2026-08-27** — §6.6 (`INPUT_PULLUP`) |
| **GPIO 13** | `IN_ALARM`   | Aster `AUX OP` relay `C` / `NO` | **direct** | Input | Active LOW when the Aster `AUX OP` contact closes. **No optocoupler and no external parts** — two wires and `INPUT_PULLUP`, debounced in firmware. §6.6 |
| **GPIO 5**  | `US_TRIG_DOS`| AJ-SR04M (Dosing Tank) | `TRIG` | Output | 10 µs trigger pulse. Dosing sensor is wired direct to the hub — see Section 13 |
| **GPIO 4**  | `US_ECHO_DOS`| AJ-SR04M (Dosing Tank) | `ECHO` | Input | Echo pulse width. **5V → 3.3V divider required. As built: 1 kΩ series + 1.8 kΩ to GND → 3.21 V** (the design figure was 2 kΩ / 3.33 V). 1.8 kΩ is an E24 value that 2 kΩ is not, it sits *under* 3.3 V rather than over it, and the AJ-SR04M's 5 V `ECHO` still clears `V_IH` with room to spare. Do not "correct" the board to the 2 kΩ figure. |
| **GPIO 34** | `IN_HPP_AC` | 220V AC Opto Module #1 (HPP Contactor) | `OUT` | Input (GPI) | Active LOW when HPP contactor is energized. Module carries its own **47 k pull-up to `VCC`** (silkscreen `47K VCC`); wire `VCC` to 3V3 and no external resistor is needed — see §8 |
| **GPIO 35** | `IN_RWP_AC` | 220V AC Opto Module #2 (RWP Contactor) | `OUT` | Input (GPI) | Active LOW when RWP contactor is energized. Same onboard 47 k pull-up — see §8 |
| **GPIO 36** | `IN_HPP_CT`  | SCT-013-030 clamp, HPP `P` conductor | Tip (3.5 mm) | Input (GPI) | Analog. ADC1_CH0, 11 dB. Rides a shared 1.65 V bias rail — spec §7.3 |
| **GPIO 39** | `IN_RWP_CT`  | SCT-013-030 clamp, RWP `P` conductor | Tip (3.5 mm) | Input (GPI) | Analog. ADC1_CH3, 11 dB. Same bias rail — spec §7.3 |
| **GPIO 27** | `OUT_RLY_TWT`| 4-Ch 5V Relay Module (Relay 1) | `IN1` | Output | Active LOW: Emulates Treated Water Float Contact to Asterro (Phase C) |
| **GPIO 23** | `OUT_RLY_RWT`| 4-Ch 5V Relay Module (Relay 2) | `IN2` | Output | Active LOW: Emulates Raw Water Float Contact to Asterro (Phase C) |
| **GPIO 18** | `OUT_RLY_DOS`| 4-Ch 5V Relay Module (Relay 3) | `IN3` | Output | Active LOW: Emulates Dosing Level Contact to Asterro (Phase C) |
| **GPIO 19** | `OUT_RLY_AUX`| 4-Ch 5V Relay Module (Relay 4) | `IN4` | Output | Active LOW: Auxiliary / Interlock dry contact override (Phase C, spare) |
| **GPIO 2** | `LED_STATUS` | Onboard DevKit Blue LED | Anode | Output | System Heartbeat & Modbus polling activity indicator (No external wire needed) |
| **GPIO 0** | `BTN_BOOT` | Onboard DevKit BOOT Button | Switch | Input | Factory Reset / AP Provisioning Mode trigger (No external wire needed) |

**Pin selection constraints — re-check these before any reshuffle:**
* **GPIO 34-39 have no internal pull-up** — but the AC opto modules supply their own. **Corrected 2026-08-27:** the fitted module has a **47 k pull-up from `OUT` to `VCC`**, printed on the silkscreen as `47K VCC`, which is why it has a `VCC` terminal at all (§8). With `VCC` on 3V3 the pin idles high and `pinMode(pin, INPUT)` is correct. No external resistor is required, and the "outstanding hub defect" recorded in phase-B spec §3.5 was a misreading of this part. What *does* float the pin is `VCC` or `OUT` losing continuity — observed once on this build, from a broken wire at the module. 47 k is a weak pull-up, so a parallel 10 k remains available as noise-hardening on a long `OUT` run; that is a tuning choice, not a fix.
* PC817 outputs are open-collector too, so all four dry-contact channels sit on pins that support `INPUT_PULLUP`: GPIO 25/26/32/33.
* **Every header on this board can be plugged in backwards, and several of them short the rail when you do.** This is a board-wide property, not a quirk of one connector: wherever `3V3` or `5V` sits at one end of a header and `GND` at the other, reversal puts them onto each other. Some connectors only swap signals when reversed and are recoverable; the powered ones are not. **The convention is therefore one rule applied everywhere — mark pin 1 on every header and on every mating plug.** A dab of paint is enough, and it is worth more than any per-connector note, because the person who reverses a plug at 11 p.m. is not reading this document.
* **Silkscreen names that are not GPIO numbers, on the DevKit fitted here.** `SP` is `GPIO 36` and `SN` is `GPIO 39` (§14.0). More dangerously, this board breaks out **`SD2` `SD3` `CMD` `SD0` `SD1` `CLK`** along the bottom of both headers — those are **`GPIO 6-11`, the SPI flash**. They look like free pins and using one prevents the board from booting. There are no spare pins hiding there.
* **`GPIO 13` carries `IN_ALARM` directly, with no optocoupler** (§6.6). Of the four pins left free — 12, 13, 14, 15 — **12 and 15 are strapping pins** (12 selects flash voltage at boot), so 13 was taken and **14 is the last comfortable spare on this board**.
* GPIO 6-11 are wired to the SPI flash and unusable. GPIO 1/3 are the USB serial console. GPIO 12, 13, 14 and 15 remain free — 13 is the natural home for a fifth PC817 channel if one is ever added. **They are all ADC2 and therefore useless for analog while Wi-Fi is up**, which is why the two CT channels take `GPIO 36` and `GPIO 39` (ADC1) instead. Those were the last two free ADC1 pins on this hub.
* `GPIO 4` is `ADC2_CH0`. ADC2 is unusable while Wi-Fi is active, but this pin is used as a **digital** input, which is unaffected. Do not repurpose it for analog.
* `GPIO 5` emits a brief pulse at boot (strapping pin). On a `TRIG` line that costs one spurious ranging cycle at power-up and nothing else.
* GPIO 27/23/18/19 drive relay opto inputs only. They idle HIGH (relay de-energized) through reset and boot, which is the fail-safe state defined in Section 7.2.

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

## 5. 4-Channel PC817 Optoisolator Module (Sensor Inputs)

Provides galvanic isolation for reading dry contacts and low-voltage status lines from the Asterro controller and Multiport Valve. The board fitted to the built hub is the **4-channel** part and all four are in use: `RL1`, `RL2`, `LPS`, `TWT FLOTY`. **`LPS` took channel 3 from `IN_ALARM` on 2026-08-27** — that signal needed no optocoupler and moved to a direct input (§6.6), which is what made room for `LPS` without an 8-channel board. Tapping `RWT FLOTY` or `DOS LVL` directly would require swapping in an 8-channel board — neither is needed for Phase B, since those tanks are measured by the RS485 ultrasonic nodes instead.

### 5.1. Module Architecture & Jumpers
* **Input Side (Left):** `IN1`..`IN4` (+) and `G` (Return) with onboard 3k ohm current-limiting resistors (compatible with 3.3V to 24V DC).
* **Output Side (Right):** `V1`..`V4` (Signal) and `G` (Ground). Open-collector phototransistors.
* **Jumpers:** Keep all black jumper caps installed (Default position).
* **ESP32 Connection:** ESP32 uses internal `INPUT_PULLUP`. No external `VCC` connection is required on the output side.

```
            INPUT SIDE (Left)                            OUTPUT SIDE (Right)
    +---------------------------------+          +---------------------------------+
    | [ IN1 ] (RL1 contact loop)      |          | [ V1 ] --> ESP32 GPIO 26        |
    | [ IN2 ] (RL2 contact loop)      |          | [ V2 ] --> ESP32 GPIO 25        |
    | [ IN3 ] (LPS loop, see 6.6)     |          | [ V3 ] --> ESP32 GPIO 33        |
    | [ IN4 ] (TWT FLOTY loop)        |          | [ V4 ] --> ESP32 GPIO 32        |
    | [  G  ] (Common input return)   |          | [  G ] --> ESP32 GND            |
    +---------------------------------+          +---------------------------------+
```

**As-built 2026-08-27, verified by injection.** The channel order above is a
one-place rotation of the order this document carried until now: `RL1` `RL2`
`ALARM` `TWT` rather than `TWT` `RL1` `RL2` `ALARM`. It was checked by injecting
5V into each input in turn and reading the hub's `[Opto Inputs]` line — all four
report under the right label, so the `V1..V4` runs match the table. Verify this
way after any rework, never by tracing the ribbon: a rotation still lights all
four channels, and the label it would cross `ALARM` with is `RL2`. A silently
mislabelled plant-fault flag is the one failure here that does not announce
itself.

### 5.2. Wetting Loop for Volt-Free Contacts

Every Aster contact tapped here is volt-free, so the loop must be wetted from our own 12V rail. Series the contact between +12V and the channel input. The board's onboard 3k resistor sets roughly 4 mA, which is above the HF3FF minimum wetting current and far below its rating.

```
  +12V DC (HLK-20M12 rail) --------> Aster terminal [ C      ]
                                     Aster terminal [ NO/NC  ] --------> PC817 [ INn ]
  12V GND ------------------------------------------------------------> PC817 [  G  ]

  Contact CLOSED -> ~4 mA through the opto LED -> phototransistor conducts -> GPIO reads LOW
  Contact OPEN   -> no current                 -> internal pull-up wins    -> GPIO reads HIGH
```

**Never apply this 12V wetting loop to a terminal the Aster panel energizes itself.** Confirm each tapped pair reads 0V AC and 0V DC across it, in both healthy and faulted states, before wiring. Procedure in Section 6.4.

---

## 6. Aster NXT Terminal Contact Polarity & Alarm Output

**Sources of truth:** `docs/ASTERO-NXTG-1_opt.pdf` (Aster NXT manual) — terminal list p.3, input/output configuration p.8-10, troubleshooting table p.12 — cross-checked against the board silkscreen photographed in `images/IMG_2836.JPG`.

> **Do not infer polarity from the `NO` / `NC` silkscreen alone.** Those marks name the contact form of the **field device at rest**, not the state the panel requires while running. On this controller `LPS` and `HPS` are both silkscreened `C NO` and behave in *opposite* directions.

### 6.1. Input Terminal Polarity (Verified)

| Terminal | Silkscreen | Field device | At rest (0 bar / normal tank) | **While running normally** | Open circuit is read as |
| :--- | :---: | :--- | :--- | :--- | :--- |
| `LPS` | `C` / `NO` | Low pressure switch, setpoint **below** normal feed pressure | open | **CLOSED** | `LOW PRESSURE!!` |
| `HPS` | `C` / `NO` | High pressure switch, setpoint **above** normal RO pressure | open | **OPEN** | normal — no fault |
| `TWT FLOTY` | `C` / `NC` | Treated water tank float | closed | **CLOSED** | `TW TANK FULL!!` |
| `RWT FLOTY` | `C` / `NC` | Raw water tank float | closed | **CLOSED** | `RW TANK EMPTY!!` |
| `DOS LVL` | `C` / `NC` | Dosing tank low-level float | closed | **CLOSED** | dosing low fault |

**Why `LPS` and `HPS` invert despite identical silkscreen.** Both are the same physical contact form — open at zero pressure, closing once pressure rises past their setpoint. What differs is where that setpoint sits relative to normal operating pressure:

```
  LPS setpoint  ~0.5-1 bar   ----+
                                 |   normal running feed pressure  (~2-3 bar)
                                 +--> LPS sits CLOSED while running
                                        opens only when feed fails      => closed = permissive

                                     normal running RO pressure      (~10-14 bar)
  HPS setpoint  ~18-20 bar   ----+
                                 +--> HPS sits OPEN while running
                                        closes only on over-pressure    => closed = trip
```

Manual p.12 confirms both directions: `LOW PRESSURE!!` lists *"LPS not connected"* as a cause, while the `HIGH PRESSURE!!` row does not — its wiring-error cause is *"check, is it C NO contact"*, i.e. a **closed** loop landed on `HPS` reads as over-pressure. The same page instructs *"Floaty is not connected. Short FLOATY terminal by an external wire link"*, confirming closed = healthy on the `NC`-form inputs.

### 6.2. As-Built Deviations Recorded on This Plant

| Observation | Date | Consequence |
| :--- | :--- | :--- |
| `HPS` terminal has **no field wiring** and the plant runs normally | 2026-08-25 | Consistent with 6.1 (open = healthy). Means there is **no controller-level over-pressure protection**, and **no `HPS` signal exists for the hub to monitor** — do not publish an `hps` state. Confirm on site whether `HI PRESS SW` is `ON` (enabled, watching a switch that can never close) or `OFF` (bypassed) and record the answer here. |
| `LPS` silkscreen is `C` / `NO` | 2026-08-25 | `RO_HARDWARE_ANALYSIS.md` previously recorded `C` / `NC`; corrected there. |

### 6.3. The `ALARM` Terminal Is the Configurable AUX OP

Physically: a 2-screw pair on the upper-right block, immediately right of the third `HF3FF-012-1ZST` and left of `RSV` — the volt-free `C` / `NO` pair of that third relay (`images/IMG_2836.JPG`).

Logically it is the panel's **AUX OP**, and its meaning is a software setting, not a fixed function:

| `AUX OP` setting | Relay energizes when |
| :--- | :--- |
| `ALARM` | any fault is active (this is what our telemetry assumes) |
| `DOSING PUMP` | HPP is running |
| `PMP ON` | RWP is running |

Read the current value under **password 678** (Input Configuration, manual §1.4): press `<` and `>` together, enter `678`, scroll to `AUX OP:`.

> **Resolved 2026-08-26 — observed on the plant.** The pair sits **open in normal running and closes when there is an issue**, which stops the plant. That is `AUX OP` = `ALARM` behaving as this document assumed, on the `C` / `NO` pair, and it is the useful case: a genuine fault flag rather than a pump-run signal. The risk below was worth carrying and is now closed.
>
> *Original risk, kept for the reasoning:* `RL1` / `RL2` are documented here as multiport-valve status, which implies `MPV CNTRL` is `ON`, and the manual states *"IF ON, one has to configure AUX OP as PMPON when there is AMPV in pretreatment."* Had `AUX OP` been `PMP ON`, this terminal would be a pump run signal and every "controller trip" alert built on it would have been wrong.

**What this buys, and its one limitation.** `IN_ALARM` LOW means the controller has faulted — actionable on its own, no inference needed, and the most valuable single bit the Aster offers. What it does *not* carry is **which** fault: the panel multiplexes every condition onto one contact, so the ESP32 knows something tripped but not whether it was high pressure, a level interlock or a sensor. Alert text must therefore say *"controller fault — check the panel"* rather than naming a cause it cannot know.

> **Amended 2026-08-27 — partly solved.** `LPS` is now tapped on its own channel (§6.6), which demultiplexes the commonest cause. `IN_ALARM` closed *with* `IN_LPS` closed is low feed pressure and the alert names it; `IN_ALARM` closed with `IN_LPS` open still has to say "check the panel". So the limitation above holds for every other condition, and no longer for the one that trips most often.

Pairing it with the two AC optos does narrow things usefully: `IN_ALARM` closed while `IN_HPP_AC` reads stopped is a genuine trip, whereas the plant idling with `IN_ALARM` open is simply a satisfied TWT float and needs no alert at all.

### 6.4. Commissioning Procedure — Verify, Then Sense

**Step 1 — ~~confirm `AUX OP` = `ALARM`~~ (6.3). Done 2026-08-26:** observed open in normal running, closed on a plant issue. Proceed to step 2.

**Step 2 — trigger a fault and characterise the contact electrically.** The unwired `HPS` pair is the cheapest trigger: no wire is disturbed and no timer stands in the way.

```
  Short HPS [ C ] to [ NO ] for ~2 s
    -> display shows "HIGH PRESSURE!!", AUX OP relay energizes
    -> measure across the ALARM pair, both states:
         V~ and V= = 0 in both  -> volt-free dry contact  -> Step 3
         230 V~ when tripped    -> switched mains         -> Step 3 (AC opto variant)
         12 V= when tripped     -> switched 12 V          -> Step 3, omit external 12V rail
    -> power down, confirm with an ohmmeter: open idle, ~0 ohm tripped
  Remove the jumper -> fault clears.
```

If shorting `HPS` produces nothing, `HI PRESS SW` is `OFF` (bypassed); use `DOS LVL` (open one wire) instead. The HPP stopping during this test is a normal protective action — do not hold the short or cycle it repeatedly, the contactor pays for it.

**Step 3 — wire the sense channel.**

*Volt-free result (expected):* PC817 channel 3 per Section 5.2 — `+12V -> ALARM [C]`, `ALARM [NO] -> IN3`, `12V GND -> G`, `V3 -> GPIO 33`, `INPUT_PULLUP`, alarm = LOW. No new hardware: the board already fitted had a spare channel.

*Switched-mains result:* do **not** use the PC817. Add a 220V AC opto module wired as in Section 8 (`L` = switched leg, `N` = the other screw), `OUT` -> GPIO 33 (the module's own 47 k to `VCC` covers the pull-up, as on GPIO 34/35 — §8). Note this is a **fifth** AC opto module; the BOM's four are already allocated.

**Step 4 — debounce in firmware.** The contact bounces and the HPP contactor induces blips on the loop. Require a sustained state:

```c
// IN_ALARM on GPIO 13 (moved off PC817 ch3 2026-08-27, section 6.6),
// INPUT_PULLUP, active LOW. Latch after 200 ms sustained. The shipped
// firmware debounces 6-of-8 inside alarm_active() instead - same intent.
static uint32_t t_edge;
static bool raw, alarm;
bool now = !digitalRead(PIN_IN_ALARM);
if (now != raw)            { raw = now; t_edge = millis(); }
else if (millis() - t_edge > 200) { alarm = raw; }
```

### 6.5. Fault-to-Alarm Timing (Do Not Assume It Is Instant)

Configured delays sit between a physical fault and the AUX OP relay picking up. Firmware must latch and timestamp the alarm rather than expect it to mirror a fault edge:

| Fault | Delay before trip / alarm | Setting |
| :--- | :--- | :--- |
| Low pressure | `LO PRESS. DBNCE` (factory **015 s**) then `LPS TRIP` (factory **03 min**, range 03-60 min) | password 678 |
| High pressure | no documented delay | — |
| Pump overload | trips, then **auto-restarts after 15 min** and re-checks current | password 123 (overload current) |

A `LOW PRESSURE!!` message on the display therefore precedes the alarm contact by up to three minutes. Treat display state and alarm contact as two separate observations.

---

### 6.6. Reading `ALARM` Without an Optocoupler — and What That Freed

**Decided 2026-08-27.** `IN_ALARM` moved off PC817 channel 3 to a direct input on
`GPIO 13`, and `LPS` took the vacated channel. Two wires:

```
   ESP32 GND    ----------------- Aster  AUX OP  [ C  ]
   ESP32 GPIO13 ----------------- Aster  AUX OP  [ NO ]

   contact open   -> internal pull-up holds the pin HIGH -> clear
   contact closed -> the two wires are joined           -> fault
```

**Why no optocoupler is needed here.** That terminal is the volt-free `C`/`NO` pair
of the panel's third `HF3FF-012-1ZST` (§6.3). **The relay contact is already the
isolation barrier** — its coil belongs to the Aster, its contacts float free of
the Aster's electronics — so a PC817 behind it would isolate something that is
isolated. Unlike the other four taps, there is no second circuit to keep apart.

**Why the internal pull-up is enough.** It is the one thing the opto path was
genuinely providing that a bare pin does not: §5.2 wets the opto channels at
~4 mA from the 12 V rail through the board's 3 k, where an internal pull-up
manages about **73 µA** — below the `HF3FF`'s datasheet minimum switching load.
Accepted, on three grounds, in increasing order of how much they settle it:

1. **3.3 V is above the fritting voltage** of a sulphide film, so contact closure
   punches through one rather than being blocked by it.
2. **An alarm contact operates a few times a year.** Contact degradation tracks
   operation count; this is not a relay cycling hourly.
3. **The relay sits inside the Aster enclosure** — observed 2026-08-27 — not
   breathing RO-room air. This is the decisive one. Dosing chemistry (sodium
   metabisulphite off-gasses SO₂) is exactly what sulphides silver, and an
   enclosed contact is not exposed to it.

If this input ever does misbehave, the fix is **one 1 kΩ resistor from `GPIO 13`
to 3V3** — ~3.3 mA, which brackets what the opto channels get. It is not fitted,
and it is not needed; it is recorded so nobody has to re-derive it at 2 a.m.

**Noise is handled in firmware, not hardware.** At 45 kΩ this is the
highest-impedance input on the board, on a run that passes contactors. `alarm_active()`
in `app_sensors.c` samples **8 times over ~1.6 ms and requires 6 to agree** — the
same shape the bench sketch used for its AC inputs. Ample against coupled noise,
and nowhere near slow enough to miss a contact that stays closed for minutes.
That mitigation is free, so it is not left as an option.

**Commissioning test, before the Aster is involved at all:** touch the two wires
together. `ALARM` should latch within one poll cycle, the RO room should start
flashing on the dashboard, and a push notification should arrive. That proves the
whole chain — pin, debounce, telemetry, alert — without waiting for a real fault.

**What `LPS` buys on the freed channel.** §6.3's one complaint about `AUX OP` is
that the panel multiplexes every fault onto it: the hub learns that something
tripped, never what. `LPS` **demultiplexes the commonest case**. `ALARM` closed
with `LPS` closed means low feed pressure and the alert says so; `ALARM` closed
with `LPS` open means look at the panel. The firmware sends two different
notifications on exactly that test. Note §6.5's timing caveat still applies — the
display leads the alarm contact by up to the `LPS TRIP` time, factory 3 min.

---

## 7. 4-Channel Relay Output Module (Asterro Float & Level Emulation)

### 7.1. Relay Module to ESP32 Pin Connections

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

### 7.2. Relay Contacts to Asterro Terminal Interconnections (Dry Contacts)

Polarity follows Section 6.1 and is **not** interchangeable between terminals. Every de-energized state below is "plant permitted to run", so a hub reset, a firmware crash or loss of the 5V coil rail leaves the panel running on its own real switches — fail-safe by construction.

| Relay Channel | ESP32 GPIO | Target Asterro Terminal | Relay Contacts Used | De-energized (default / boot) | Energized |
| :---: | :---: | :--- | :---: | :--- | :--- |
| **Relay 1** | **GPIO 27** | `TWT FLOTY [ C ]` & `[ NC ]` | `COM` & `NC` | **Closed** = tank not full, plant may run | **Open** = `TW TANK FULL!!` |
| **Relay 2** | **GPIO 23** | `RWT FLOTY [ C ]` & `[ NC ]` | `COM` & `NC` | **Closed** = raw water available | **Open** = `RW TANK EMPTY!!` |
| **Relay 3** | **GPIO 18** | `DOS LVL [ C ]` & `[ NC ]`   | `COM` & `NC` | **Closed** = chemical OK | **Open** = dosing low |
| **Relay 4** | **GPIO 19** | Spare — see note below | `COM` & `NC` for `LPS`, `COM` & `NO` for `HPS` | `LPS` form: **closed** = pressure OK. `HPS` form: **open** = pressure OK | trip |

**Testing them in the field: `/cal` has a Relay test panel.** Five buttons — the
four hub relays and the battery-room fan — each of which energises its relay for
**5 s** and then releases it. There is no latch and no off button, deliberately:
energised is the *fault* state on every one of these lines, so a control that
stayed on would let somebody stop the plant by clicking a button and walking
away. Listen for the click and meter `COM`–`NC`; **de-energised is closed**. The
fan is on node `0x04`, so it answers on the next poll rather than instantly, and
a test will not override a fan deliberately forced OFF from the app.

> **Never substitute a hub relay for the real `LPS` or `HPS` switch.** Doing so hands dry-run and over-pressure protection to an ESP32. If a remote stop is wanted, wire Relay 4 **in series** with the real `LPS` loop (`COM`/`NC`, either the switch or the hub can open it) or **in parallel** across `HPS` (`COM`/`NO`, either can close it). The mechanical switch keeps its authority in both arrangements.

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

## 8. 240V AC Optocoupler Modules (HPP & RWP Contactor Monitoring)

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

**The `VCC` wire is not optional, and it is the failure point.** This module is
not a bare optocoupler. Its output side carries a **47 k pull-up from `OUT` to
`VCC`**, silkscreened `47K VCC`, and the input side rectifies (`MB6S`), clamps
(5.1 V zener) and smooths (100 µF) the mains before driving the opto LED. So:

* `VCC` on 3V3 is what makes `OUT` idle high. **No external pull-up is needed** —
  the "outstanding 10 kΩ retrofit" this document and phase-B spec §3.5 carried
  until 2026-08-27 was a misreading of the part.
* If `VCC` or `OUT` loses continuity the pin floats, and `digitalRead` reports
  that as `OFF` — identical to a healthy module seeing no mains. **This has
  already happened once on this build** (broken wire at the module).
* Because the input is smoothed, `OUT` should be *steady* low with mains
  present, not a 100 Hz pulse train. Phase-B spec §5.4 assumed a pulse train;
  that is re-opened pending measurement.

> **Measured 2026-08-28, and the withdrawn resistor is half-rehabilitated.**
> `probeACInput()` on real hardware reports **HPP 2693-3134 mV** and
> **RWP 2924-3134 mV** at idle, not the ~3300 mV steady that a 3.3 V pull-up
> implies. The cause is source impedance: the module's 47 k is too stiff to
> replenish the ESP32 ADC's sampling capacitor, so the reading sags and jitters.
> Espressif's guidance is to keep ADC source impedance under about 10 k.
>
> **This does not restore the "defect".** §1 is still right that no external
> pull-up is *needed* for the optocoupler — the module supplies its own, and
> nothing about that was wrong. What is new is a different and unrelated reason
> to want a part there: ADC accuracy, not contact wetting.
>
> **It works as-is.** 2693 mV clears the 2500 mV idle threshold, and with mains
> present the pin sits near 0 V so active detection has enormous margin. But the
> idle margin is ~190 mV rather than the ~800 mV intended.
>
> **Cheapest hardening, if these channels ever read erratically: one 100 nF from
> each of `GPIO 34` / `35` to GND.** A capacitor is the better fix than a stronger
> pull-up here because it supplies the sampling charge without shifting the DC
> level — the thresholds stay where they are. A parallel 10 k would also work and
> would firm up the level, at the cost of re-tuning `AC_IDLE_ABOVE_MV`.

**Validated 2026-08-27.** Both channels read correctly against live contactors, with the sketch's existing 5 ms filter — which also retires the 25 ms rewrite proposed in phase-B spec §5.4 (see that section). The earlier floating reading was the broken wire at the module, not this design.

**Verify with `probeACInput()`**, which the hub sketch runs every cycle. GPIO 34
and 35 are `ADC1_CH6`/`CH7`, so it reads actual millivolts and prints the min/max
spread over a full mains cycle — the only way to tell the three states apart:

| Reading | Means |
| :--- | :--- |
| `~3300-3300 mV` | Idle. Pull-up alive, `VCC` and `OUT` intact. |
| `0-0 mV` | Mains present, smoothed as expected. |
| swings `0` to `~3300` | Mains present but pulsing — spec §5.4's filter fix is needed after all. |
| stuck mid-scale | **Floating.** Check `VCC` and `OUT` continuity at the module. |

---

## 9. Remote Arduino Nano Tank Nodes (0x02 RWT, 0x03 TWT)

**Node 0x02 (RWT) is the end of the bus** and carries the second 120 ohm resistor across its `A+` / `B-`, per Section 12. Node 0x03 is mid-chain and carries none.

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
 |    XY-485 MODULE         | |   AJ-SR04M MODULE     | | ADDRESS JUMPERS |
 | RXD --> D2               | | TRIG --> D7           | | see 9.1 below   |
 | TXD --> D3               | | ECHO --> D8           | | A0 -> GND or open|
 | VCC --> 5V (from Buck)   | | VCC  --> 5V (from Buck| | A1 -> GND or open|
 | GND --> GND              | | GND  --> GND          | +-----------------+
 | A+  --> RS485 Bus (A+)   | +---------+-------------+
 | B-  --> RS485 Bus (B-)   |           | (Coaxial Cable)
 +--------------------------+           v
                               +-----------------+
                               | Waterproof      |
                               | Transducer Head |
                               +-----------------+
```

Two more headers belong on a tank node's board and are not drawn above — `J-LOOP`
(12V / A2 / GND) and `J-PRESS` (A3 / GND). They are the provision for a submersible
pressure transducer and cost nothing to fit while the board is open: **§9.4**.

### 9.0. AJ-SR04M Sensor Notes (applies to every ultrasonic node and the hub's dosing sensor)

The sensors in hand are **AJ-SR04M**, not JSN-SR04T. Electrically interchangeable for our purposes — 5 V supply, 5 V `ECHO` needing the divider, ~20 cm blind zone, `pulseIn` timeout of 35000 µs still covering the useful range — with two board-specific gotchas:

| Check | Detail |
| :--- | :--- |
| **`R19` mode-select pad must be EMPTY** | Unpopulated selects HC-SR04-compatible **Trig/Echo** mode, which is what the firmware drives. A resistor fitted there (47 k / 120 k / 200 k) puts the board into one of its **UART** modes, where it either streams frames unasked or waits for a `0x55` command — and `pulseIn` on `ECHO` then reads nothing at all. Inspect the pad before wiring; a board that measures zero every time is almost always this, not a wiring fault. |
| **Trigger pulse width is a tuning knob** | The firmware issues the nominal **10 µs** `TRIG` pulse. Some AJ-SR04M batches are unreliable at exactly 10 µs and want **20 µs**. If a node returns intermittent zeros or wild outliers with the pad confirmed empty, widen the pulse before suspecting the transducer. Keep it as a named constant, not a magic number. |
| **A ~35.3 ms echo pulse means "too close", not "far away"** | With a target inside the ~200 mm blind zone the module holds `ECHO` high for a fixed **~35.3 ms** rather than not pulsing at all. Converted naively that reads as **~604 cm**, which looks like a confident long-range measurement and is the opposite. Our `US_TIMEOUT_US` is 35 000 µs, so `pulseIn` gives up 263 µs before that pulse ends and correctly returns 0 — the firmware reports no echo where a bare sketch would report 604 cm. **A steady 604 cm from a test sketch means the sensor cannot range, usually because a hand, a cable or a bench edge is inside 20 cm.** Established 2026-09-05, after trigger width, sensor supply and SoftwareSerial interference had each been investigated as causes of a fault that did not exist. |

**Verified 2026-08-25:** `R19` inspected on **all four sensors** and **empty** on each — Trig/Echo mode, which is what the sketches drive. So a zero reading on this build is wiring, power, or the blind zone, not the mode pad. Check the pad again on any sensor added later; batches differ.

Range: 20–600 cm nominal, realistically dependable to ~4–4.5 m on a flat water surface. That covers all three tanks. The 3.5 m ground sump is inside the figure but with less margin than the datasheet implies — see phase-B spec §7.1, which is already tracking the sump sensor choice as an open decision.

### 9.1. Address Jumper Truth Table (authoritative)

Published here first; `firmware/ro_node/ro_node.ino` carries the same table and `docs/check_addrmap.py` fails the build if the two ever diverge:

```c
bit0 = (digitalRead(A0) == HIGH) ? 1 : 0;   // INPUT_PULLUP: open = 1, jumpered to GND = 0
bit1 = (digitalRead(A1) == HIGH) ? 1 : 0;
raw  = (bit1 << 1) | bit0;
return ADDR_MAP[raw];                       // the table below is the lookup;
                                            // 0b00 is UNASSIGNED, not 0x04
```

| `A1` | `A0` | raw | Node ID | Role |
| :---: | :---: | :---: | :---: | :--- |
| open | **GND** | `0b10` | **`0x02`** | RWT (end of bus, 120 Ω) |
| **GND** | open | `0b01` | **`0x03`** | TWT |
| open | open | `0b11` | **`0x04`** | Battery Room climate + fan (Pro Mini, §10). Also the unjumpered default |
| **GND** | **GND** | `0b00` | **unassigned** | Refuse to join the bus — see below |

The mapping is a **lookup table written from the observed hardware**, not an arithmetic identity. The three built boards already carried three distinct jumper codes, so the codes were kept and the table was written around them (§9.2). This is exactly what phase-B spec §3.7 prescribed.

```c
// common/protocol.h  --  raw = (A1 << 1) | A0, INPUT_PULLUP: open = 1, GND = 0
static const uint8_t ADDR_MAP[4] = {
  0x00,   // 0b00  both GND    -> unassigned, do not join the bus
  0x03,   // 0b01  A1 to GND   -> TWT
  0x02,   // 0b10  A0 to GND   -> RWT (end of bus)
  0x04,   // 0b11  both open   -> Battery Room, climate + fan relay
};

uint8_t nodeIdFromJumpers(bool a0_high, bool a1_high) {
  return ADDR_MAP[((a1_high ? 1 : 0) << 1) | (a0_high ? 1 : 0)];
}
```

A node decoding `0x00` must **not** transmit — hold in a fault state and blink the LED. Guessing an address is how you get two boards answering the same poll. Grounding both jumpers is therefore also the deliberate way to bench a spare board while it is physically on the bus.

> The table previously published here had `0x01`/`0x02` swapped, labelled both-grounded `0x03`, and omitted `0x04`. The original bringup decoder returned `raw` directly with a special case for zero, which matched no assignment. Both are superseded by `ADDR_MAP` above. Phase-B spec §3.7 is closed by this.

**Address `0x01` no longer exists in any form** — the dosing sensor moved onto the hub (§13), and no jumper combination produces it.

**The address also selects the node's personality** (spec §4): `0x02`/`0x03` run the ultrasonic build, `0x04` runs climate + fan relay. A misjumpered node therefore doesn't just answer to the wrong ID — it runs the wrong hardware profile and publishes plausible-looking wrong data.

### 9.2. As-Built Jumper Audit (2026-08-25)

The jumpers are staying as they are. `ADDR_MAP` (§9.1) was written to fit them, so **no board needs rewiring**:

| Board | `A0` | `A1` | raw | Resolves to | Action |
| :--- | :---: | :---: | :---: | :---: | :--- |
| Nano #1 | **GND** | open | `0b10` | `0x02` RWT | Label RWT, fit its 120 Ω (§12). No change. |
| Nano #2 | open | **GND** | `0b01` | `0x03` TWT | Label TWT. **No change** — this code used to mean the retired `0x01`. |
| Pro Mini (Battery Room) | open | open | `0b11` | `0x04` Climate | Label Battery Room. **No change** — leave both pads unconnected. |

Why software rather than the soldering iron: the three codes were already distinct, so there was nothing to disambiguate — only a table to write. It avoids lifting a pad on the Pro Mini's inner pads, avoids touching a node that may already be sealed in its enclosure, and puts the assignment in the one place spec §3.1 says addressing belongs.

**The cost, stated plainly:** the unjumpered default is now `0x04` rather than a tank node. A fresh board dropped on the bus claims to be the Battery Room. That is *louder* than the old failure — two nodes answering the same poll produces immediate CRC/collision errors, where a duplicate tank level produces plausible wrong data nobody notices — but it does mean **any new node must be jumpered before it is bussed**, not after.

**If the boards are ever re-jumpered instead, `ADDR_MAP` must change in the same commit.** One authoritative table; a second person re-deriving jumpers from a stale one is precisely the defect class §3.1 exists to prevent.

Confirm by boot print before bussing: each node prints its detected ID at startup. Read all three.

---

### 9.2.1. As-Measured Tank Geometry — fill this in on site

`full` and `empty` live in NVS as two bare millimetre figures. They lose the
measurements they were derived from, so if a sensor is ever remounted or replaced
nobody can recompute them without going back up to the tank. Write the raw numbers
here instead.

**Three heights per tank, and take all three from the inside floor.** One datum
means every value is a subtraction and no gap goes unrecorded. Mixing references —
"bottom outlet to overflow" plus "floor to face" — leaves the height of the bottom
outlet above the floor unmeasured, and the two figures then cannot be combined.

```
   S = floor -> transducer FACE      (not the roof slab, not the bracket)
   B = floor -> bottom outlet        (the level below which there is no usable water)
   W = floor -> overflow outlet      (the maximum water level)

   empty = S - B        0% when the bottom outlet runs dry
   full  = S - W        100% at the overflow
```

Measuring to the FACE rather than to the roof means the transducer's protrusion is
already accounted for — one fewer number to record and to get wrong.

**`W - B` is the usable depth**, and it is a free cross-check: if it does not match
the tank's nominal capacity, one of the three measurements is wrong.

| Tank | S: floor → face | B: floor → bottom outlet | W: floor → overflow | `full` = S−W | `empty` = S−B | Usable depth W−B | Measured |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| RWT `0x02` | | | | | | | |
| TWT `0x03` | | | | | | | |
| Dosing (hub) | | | | | | | |
| Sump `0x05` | | | | | | | Phase 2 |

**Check before saving: `full` must be at least 200 mm.** That is the blind zone, so
the face has to sit at least 200 mm above the overflow outlet. If it does not, the
top of the scale is unusable and the fix is a stand-off that raises the sensor, not
a number typed into `/cal`.

**Why zero is the bottom outlet and not the floor.** `empty = S` would put 0% at a
bone-dry floor, which reports usable-looking water that the pump cannot actually
reach. Referencing zero to the bottom outlet instead makes 0% mean "no *usable*
water", which is the number that matters — particularly for RWT, which feeds the
RWP, and for anything that ever informs dry-run detection (§7.2). If a tank's
bottom outlet is level with its floor, `B = 0` and the two definitions coincide.

**Refine opportunistically.** These figures only need to be close enough that the
gauge is not nonsense. `/cal` shows the percentage the current calibration produces
against the live distance, so the next time a tank is visibly near full or near
empty, read the live millimetres and set the matching value. Everything is in NVS
and editable from a phone.

### 9.3. Ultrasonic Mounting, Per Location

Four tanks, four different problems. The sensor is the same part in each; the mounting is what decides whether it works.

**Rules that apply everywhere:**

| Rule | Why |
| :--- | :--- |
| Transducer face at least **200 mm above the highest water level** | That is the blind zone. Water inside it returns nothing, and the node reports `sensor_status = 1` rather than a number — safe, but you have lost the top of the scale exactly where "is it full" matters. |
| Mount **perpendicular** to the surface, pointing straight down | A tilted transducer reflects the pulse away from itself. A few degrees is tolerable; ten is not. |
| Keep the **beam cone clear** — ladders, pipes, ropes, wall seams | The beam is 45–75° wide, so at 3 m it lights up a cone over a metre across. Anything it clips answers *earlier* than the water does, and an early echo reads as a **shorter distance, which the dashboard shows as a fuller tank**. Wrong in the dangerous direction. |
| Mount **away from the inflow** | Broken surface and foam scatter the pulse. Put the sensor over still water, opposite the fill point. |
| Fit a **hood or shade** over the face in closed tanks | Condensation forming on, or dripping onto, the transducer kills echoes. This is the most common cause of a sensor that worked for a month and then stopped. |

> **The beam-cone rule got more important on 2026-08-28, not less.** `levelPercent()`
> used to return "no level" for any distance shorter than the calibrated `full`
> mark. It now returns **100%** for anything between the 200 mm blind zone and
> `full`, because a barrel filled above its mark is a real measurement and
> reporting it as a fault meant the **overflow alert never fired at all** — the
> alert is gated on having a level, so a genuinely overflowing tank was invisible
> exactly when it mattered.
>
> The cost is on this row. An obstruction that answers from **200–299 mm** used to
> read as a *fault*, which is visible and safe. It now reads as a **steady 100%**,
> which is plausible, and it will hold an overflow alert on forever. That is the
> "wrong in the dangerous direction" this table warns about, and the change
> widened the band it applies to by 100 mm at the top.
>
> It is still the right trade — an obstruction anywhere from 300 to 1500 mm
> *already* read as a falsely-full tank, so this extends an existing hazard rather
> than inventing one, and it buys back overflow detection. But it means the beam
> cone must actually be clear, not approximately clear.
>
> **How to tell the two apart, from the console summary:** a real water surface
> ripples, so its distance wanders by a few millimetres and `q` moves around. An
> obstruction is rock-steady — the same distance, `q100`, cycle after cycle, for
> hours. If a tank sits at exactly 100% with an unmoving distance, suspect the
> mounting before believing the level.
| Strain-relieve the captive lead at the tank wall | The transducer hangs on its own cable otherwise, and it will eventually hang crooked — see rule 2. |

**Dosing barrel (~50 L, wired direct to the hub, §13).** The blind zone *is* the design problem here: a 50 L drum is only ~550–600 mm deep, so a sensor sitting on the barrel mouth cannot read the top third — the range you care about when deciding whether to top up. **Mount it on a bracket 250–300 mm above the open top**, not on the rim. That is the geometry the hub's `250 mm full / 900 mm empty` defaults assume.

**RWT (plastic, roof, `0x02`).** The straightforward one. Standard rules, no special measures.

**TWT (concrete, wide, roof, `0x03`) — the wide tank an ultrasonic sensor cannot use.**

On paper this is the easiest tank: a large flat surface, walls far from the beam. **As built it is the hardest**, because the only penetration is a small wire pass-through in the roof slab, **hard against the wall**.

The arithmetic rules it out. The beam is ~45° total, so its radius grows ~0.41 m per metre of depth. A transducer 100 mm from a wall has that wall inside its beam from **0.24 m down**, and from there on the wall answers at a fixed ~260 mm regardless of where the water is. The tank reads permanently near-full — and *stably*, with quality at 100 and no jitter, so nothing in the telemetry looks wrong. A confidently wrong treated-water level is worse than no reading at all.

Three ways out, in cost order:

1. **Core a new penetration away from the wall.** Clearance must exceed `0.41 × (depth from transducer to the lowest working surface)` — for a 1.5 m drop, **≥ 0.6 m from any wall**. Cheapest in parts, most invasive in a concrete roof slab.
2. **Submersible pressure transducer through the existing hole** (phase-B spec §7.1; node wiring and the firmware provision are in §9.4). Geometry stops mattering: it hangs on its cable and reads head. The cable passes a wire hole, but the **22 mm stainless body does not** — budget on opening that hole to ~30 mm with a masonry bit, which is a far smaller job than coring 90 mm. The 12 V the loop needs is already at the node.
3. **Leave TWT on the Aster float alone** and accept no continuous level here. It is, after all, the tank whose float already drives the controller.

**This reorders the procurement question in spec §7.1.** The sump has an open manhole and can take a stilling well with a sensor already owned; TWT cannot. If exactly one pressure transducer is bought, the evidence points at TWT for it, not the sump.

#### If the TWT transducer is fitted — three requirements

**1. The enclosure above the tank must breathe.** The node box on the tank roof is planned airtight, and a gauge transducer references atmosphere through a vent tube running inside its cable. Terminate that tube in a sealed box and its reference becomes a fixed volume of air in the sun:

> `ΔP = P × ΔT/T` — a **20 °C swing is 6.8 kPa, which is ~0.7 m of apparent water** on a 1.5 m tank. Even 5 °C is 173 mm. The level appears to rise each morning and fall each evening, and nothing in the reading looks faulty.

Fit a **membrane breather vent** (Gore-Tex / IP68 M12 type) in the enclosure wall — weatherproof, pressure-equalised — and add a replaceable desiccant sachet inside. At the terminal, **leave the vent tube open**: never crimp it into a gland, seal it in heat-shrink, or pot it in epoxy. This single detail is the difference between a transducer that holds calibration for years and one that drifts within a week.

**2. Size the sensor to the tank, not to the catalogue.** TWT is **~1.5 m** deep. Accuracy is quoted as % of *full scale*, so a 0-5 m unit gives ±25 mm here — ±1.7 % of the span actually used — and on a Nano's 10-bit ADC the whole tank occupies only ~245 counts (~6 mm each). A **0-2 m or 0-3 m variant** of the same sensor costs about the same, gives ±10 mm, and ~2.4 mm per ADC count. Buy for the tank you have.

**3. Hang it 50-100 mm off the floor.** Clear of sediment, and it means the bottom 50-100 mm is below the sensor and simply never measured — which the hub's calibration absorbs the moment you set *empty = now* at the working empty level.

**Sump (concrete, deep, ground floor, `0x05`).** The hard one, and the reason phase-B spec §7.1 is still open. A 3.5 m shaft with a ladder, a riser pipe and rough walls is exactly the geometry that produces early echoes, and the borewell inflow foams the surface while it runs.

Fit a **stilling well** rather than hoping: a ~100 mm PVC pipe hung vertically, open at the bottom, vented at the top, sensor on the cap. The water inside follows the sump but stays still, and the pipe wall stops the beam ever seeing the shaft. A few hundred rupees, and it addresses turbulence and false echoes in one part.

**Cable routing near the PV installation.** The roof array has no string inverters — the DC runs down to the battery room, where every inverter lives. That is also where node `0x04` sits and where the RS485 chain passes on its way to the roof, so the electrically noisy room is the battery room, not the roof. Keep the Cat5e clear of inverter AC and DC runs there, cross at 90 degrees rather than running parallel, and do not share a conduit with the PV strings on their way down. A 4-20 mA loop feeding a 10-bit ADC will show every bit of noise it picks up as jitter in the level.

**Judging any of these from the data, not by argument:** the node reports `raw` alongside the filtered `median`, plus an echo-quality figure. Clean tracking is a median that moves smoothly with quality at 100. False echoes look like a median stepping between two values with quality stuck below 100. Log a day of it before concluding anything about a sensor.

---

### 9.4. Pressure Transducer Provision on a Tank Node (fitted or not)

§9.3 argues that TWT is the tank an ultrasonic sensor cannot read. This section is
the provision that makes acting on that argument a one-hour job instead of a
redesign: **the node firmware already reads a 4-20 mA loop**, so procuring the
sensor is the only long-lead item. Nothing needs to change on the hub, in the
RS485 protocol, on the dashboard, or in the alerts.

**What is already done, today:**

| Piece | State |
| :--- | :--- |
| Node firmware `fw 1.01` | Reads the loop on `A2` and reports millimetres exactly the way the ultrasonic does. Shipped on **all** node boards — one binary, as ever. |
| Selection | `A3` jumpered to GND. No jumper, and `A2` is never read: an unfitted, floating input cannot invent a level. |
| Hub, protocol, `/cal`, alerts | **Untouched.** See "why the reading is inverted" below — that is what buys this. |
| On the board | Nothing fitted. Two through-holes' worth of parts (§9.4.4) and the loop wiring. |

#### 9.4.1. Why the node reports `RANGE - head`, not head

An ultrasonic sensor measures the **air gap** and its reading *shrinks* as the tank
fills. A submersible transducer measures **head** and its reading *grows*. The hub's
`levelPercent()` — and `cal_set_tank()`, which refuses `empty <= full` precisely to
catch a swapped pair — assumes the first.

So the node subtracts: it reports `PRESS_RANGE_MM - head`. That is still a number
that shrinks as the tank fills, so every consumer downstream keeps working with no
special case anywhere and no second code path to keep in step.

The two figures you type into `/cal` are then **offsets from the sensor's full
scale** rather than air gaps. They are still just "the number the node reports at
this water line", which is exactly what the `/cal` page shows you live — so the
calibration procedure does not change either.

**A useful consequence:** `/cal` is a two-point fit over a reading that is linear in
loop current, so it absorbs the sense resistor's tolerance, the ADC reference error
*and* a wrong `PRESS_RANGE_MM` — the **percentage stays correct** even if those
constants are off. They only need to be right for the raw millimetres to be honest.
The one hard requirement is that `PRESS_RANGE_MM` is **not smaller than the deepest
head the sensor will see**, or the reading clamps into the blind zone and the tank
reports no level at all.

#### 9.4.2. The two headers to fit now

Two 0.1" headers on the node board, both fitted **whether or not the sensor is ever
bought**. That is the whole provision: soldered now with the board open on a bench,
not later on a ladder.

```
   J-LOOP  (1x3)                          J-PRESS  (1x2)
  +---------------+                      +-----------+
  | 1  12V       -|--> transducer ( + )  | 1  A3    -|--\
  | 2  A2        -|--> transducer ( - )  | 2  GND   -|--/  shunt fitted = pressure
  | 3  GND       -|                      +-----------+     shunt off    = ultrasonic
  +---------------+
        ^                                      ^
        +-- mark pin 1. 12V is on it.          +-- same shunt as the A0/A1
            Nothing else identifies                address jumpers, §9.1.
            this header.
```

**`J-LOOP` pin 1 takes 12 V from the buck's INPUT side**, not its 5 V output — the
same pair that feeds the buck (§9, block diagram). A 2-wire transducer needs the
loop voltage; the Nano's 5 V cannot supply it.

**The passives live across `J-LOOP` pins 2 and 3, on the board:**

```
   J-LOOP pin 1  o---------------------> ( + ) transducer
                                              |
                                           ( - ) loop return
                                              |
   J-LOOP pin 2  o------------------------+---+
                                          |
                                    [ 100 R 1% ]        <- sense resistor
                                          |
   J-LOOP pin 3  o-------------------+----+
                                     |
                                    GND

   and from the pin-2 node to the Nano:

   pin 2 node ---[ 1 k ]---+--- Nano A2
                           |
                        ][ 100 nF
                           |
                          GND
```

* **Sense resistor to GND, not high-side.** The voltage across it *is* the signal,
  referenced to the ADC's own ground. 100 R gives **0.40 V at 4 mA and 2.00 V at
  20 mA** into the 5 V ADC — no divider, no op-amp.
* **Fit the 100 R at the same time as the header, before any sensor exists.** It is
  what makes an empty `J-LOOP` read **0 V** instead of floating: jumper `J-PRESS`
  with nothing plugged in and the node reports a *sensor fault*, not a plausible
  number. Without the resistor, that same mistake reads noise as a water level.
* **`1 k` in series and `100 nF` to ground at the Nano pin.** The pair is an RC
  filter — a 4-20 mA loop run across a roof shows its pickup as jitter in the level
  (§9.3, last paragraph) — *and* it is what survives the one destructive wiring
  mistake this header allows: **12 V landed on pin 2**. The 1 k holds the fault into
  the ADC's clamp diode to ~7 mA, which the ATmega takes; without it the pin is
  gone. Neither part shifts the reading — the ADC input draws under a microamp, so
  the drop across the 1 k is under a millivolt, and `/cal`'s two-point fit absorbs
  even that.
* **Why 100 R and not 150 R — this is the constraint that sizes the whole loop.**
  The 12 V does not arrive as 12 V: `POWER_BUDGET.md` §5.1 works the Cat5e drop to
  `0x03` at **0.30 V over 10 m hops, 0.74 V over 25 m**. What is left after the sense
  resistor's burden is what the transducer runs on, and that burden is worst at
  **20 mA — a full tank**. At 150 R the sensor would see **8.7 V on a short run**,
  under the 9 V minimum most 2-wire parts specify, and it would fail *only when the
  tank fills*: correct while empty, drifting as the water rises. 100 R holds 9 V out
  to ~25 m hops and costs ~6 mm per ADC count on a 0-2 m sensor, against a filter
  whose agreement window is 25 mm. **Read the actual minimum off the datasheet before
  ordering** — a 12-36 V part cannot work on this bus at any sense resistance, and
  the fix would be a 12→24 V boost module at this node alone.
* **Ground.** The loop return and the Nano share one ground. They already do — same
  buck, same 12 V pair.
* **`J-PRESS` is reversal-safe and `J-LOOP` is not.** A 2-pin shunt shorts the same
  two pins whichever way round it goes. `J-LOOP` reversed puts 12 V on the GND pin
  and back-feeds the transducer, so mark pin 1 and, if the sensor is ever unplugged
  in the field, use a keyed 3-pin JST-XH shell rather than bare dupont.
* The **vent tube must stay open** and the enclosure must breathe. That is not
  optional and it is not a detail: see §9.3, requirement 1, for the 0.7 m of
  apparent level a sealed vent invents on a sunny afternoon.

**Both headers go on all tank nodes (`0x02` and `0x03`), not just TWT.** One binary,
one board layout — the RWT node then needs only a sensor and a shunt if its
ultrasonic ever fouls, and a spare board is a spare board for either tank.

#### 9.4.3. Commissioning, in order

1. **Set `PRESS_RANGE_MM` to the sensor's full scale** in `ro_node.ino` and reflash
   the node. This is the one value that is not settable over the network — it is a
   property of the part, and the part is not changing without a ladder anyway.
   `PRESS_SENSE_OHMS` likewise, if you did not fit 100 R.
2. **Plug the transducer into `J-LOOP` with the `J-PRESS` shunt off.** The node
   stays on the ultrasonic, so nothing changes yet.
3. **Meter across `J-LOOP` pins 2-3, sensor dry and in air.** Expect **~0.40 V**
   (4 mA). Nothing, or a rail, means the loop is wrong — find that now, not after
   the sensor is down a tank.
4. **Fit the `J-PRESS` shunt and reset.** The boot print states the source outright:
   `Level source: 4-20 mA pressure transducer on A2, 2000 mm full scale`.
5. **Hang the sensor 50-100 mm off the floor** (§9.3, requirement 3) and watch the
   console: `1450 mm(P) q=100`. The `(P)` marks the pressure source. **The number
   must fall as the tank fills.** If it rises, the loop is backwards somewhere.
6. **Calibrate from `/cal`,** which shows the live reading and the resulting
   percentage: at the working empty level type what it reads as `empty`, at the full
   mark type what it reads as `full`. Record both, and the water lines they came
   from, in §9.2.1 — the same as for an ultrasonic sensor.

#### 9.4.4. Bill of materials, and what the loop tells you when it breaks

| Part | Qty | Note |
| :--- | :---: | :--- |
| Submersible 4-20 mA level transducer, **0-2 m** | 1 | Buy for the tank, not the catalogue — §9.3, requirement 2. Cable length ≥ tank depth + run to the node. |
| 100 R, 1 %, 0.25 W metal film | 1 | Sense resistor. **100 R, not 150 R** — the loop's voltage headroom sizes it, `POWER_BUDGET.md` §5.1. |
| 1 k, 0.25 W | 1 | Series into `A2`. Filter, and the only thing between a miswired 12 V and a dead ADC pin. |
| 100 nF ceramic | 1 | Nano `A2` to GND. |
| 1x3 0.1" pin header (`J-LOOP`) | 1 | 12V / A2 / GND. **Fit now**, with the 100 R, even with no sensor. |
| 1x2 0.1" pin header + shunt (`J-PRESS`) | 1 | `A3` / GND. Same shunt as the address jumpers, §9.1. **Fit now, leave the shunt off.** |
| M12 membrane breather vent + desiccant sachet | 1 | Enclosure, non-negotiable — §9.3, requirement 1. |
| Masonry bit to open the wire hole to ~30 mm | 1 | The 22 mm body will not pass a wire pass-through. |

**The failure signature is the point of choosing 4-20 mA.** A cut cable, a dead loop
supply or a failed sensor all read **below 4 mA**, which the node returns as *no
reading* — `sensor_status` 2, then 3 after ten cycles, and the dashboard shows a
sensor fault. A 0-5 V sensor in the same state reads 0 V, which is indistinguishable
from an empty tank: the alert fires, someone climbs to the roof, and the tank is
full. That live zero is worth the extra wire.

---

### 9.5. Water Quality — TDS Probe and DS18B20, per tank

Two probes per tank node, in the water: an analog TDS meter and a DS18B20. **Both
or neither** — the firmware will not report TDS without a temperature to
compensate it with, for the reason in §9.5.1.

The point is not either number on its own. It is **salt rejection**:

```
rejection % = (RWT ppm - TWT ppm) / RWT ppm
```

Permeate TDS rising is not a fault by itself — it rises when the *feed* rises,
which is a fact about the borewell, not about the plant. A falling rejection
percentage is a failing membrane regardless of what the source water is doing,
and it is the one number worth looking at daily. It needs both tanks, which is
why the probes come in pairs.

#### 9.5.1. Why the DS18B20 is not optional

Conductivity moves about **2 % per °C**, and every TDS figure is quoted at 25 °C.
A roof tank in Guntur swings well over 20 °C between a January morning and a May
afternoon, so an uncompensated probe reads **tens of percent apart** with nothing
having changed in the water. That is a confidently wrong number of exactly the
kind §9.3 warns about for the ultrasonic.

So the node gates the whole feature on the DS18B20's 1-Wire presence pulse: no
temperature sensor, no TDS reading, no invented values. If the water-quality
figures stop appearing, suspect the DS18B20 and its pullup first.

A useful side effect of the ratio: both probes sit in the same weather at roughly
the same water temperature, so most of the residual temperature error cancels in
the rejection figure even though it corrupts either reading alone.

#### 9.5.2. Wiring

```
   Nano                          DS18B20 (waterproof, 3-wire)
  +-------------+
  | +5V     o---+---------------o RED    (VDD - use normal power, NOT parasitic)
  |             |
  |          [ 4k7 ]   <-- REQUIRED. 1-Wire is open-drain and does
  |             |          NOTHING without it. Not supplied with the probe.
  | D4      o---+---------------o YELLOW (DATA)
  | GND     o-------------------o BLACK  (GND)
  +-------------+

   Nano                          TDS board (analog)
  +-------------+
  | D5      o-------------------o VCC    (board is powered only while sampling)
  | A6      o-------------------o AOUT   (0-2.3 V)
  | GND     o-------------------o GND
  +-------------+
```

| Signal | Pin | Note |
| :--- | :---: | :--- |
| DS18B20 data | `D4` | **4.7 kΩ pullup to +5 V is mandatory.** Buy two; they are not in the probe's bag |
| TDS power | `D5` | Drives the board's `VCC`. See below |
| TDS analog out | `A6` | `A6`/`A7` are **analog-only** on the Nano — no digital function at all — so spending one here costs nothing that could have been used otherwise |

**The TDS board is powered from a GPIO, not from +5 V.** Electrodes sitting in
water under a continuous DC bias polarise and plate, and the reading drifts over
months in a way that looks exactly like a real trend. The firmware energises the
board for one cycle in ten, reads, and switches it off — roughly a 10 % duty
cycle. The DFRobot-style board draws a few milliamps, well inside a pin's 20 mA;
**if a different board draws more, drive it through a small MOSFET rather than
raising the pin current.**

**Both probes must be in the same water**, close together, or the compensation is
compensating for a temperature the TDS probe never saw.

#### 9.5.3. Calibration and what the numbers can mean

The hub stores a **k factor per tank** (`tds_k_x100`, NVS, default `1.00`).
Calibrate against a reference solution: if a 707 ppm sachet reads 640, set
`k = 707 / 640 = 1.10`. The hub clamps k to 0.50–2.00 — a probe needing more
correction than that is broken or in the wrong solution, and accepting the number
would bake the fault in.

| Reading | Rated? | Reported? |
| :--- | :---: | :--- |
| 0–1000 ppm | yes | yes |
| 1000–3000 ppm | **no** | **yes** — a brackish source is a fact worth seeing, just a less accurate one |
| > 3000 ppm | no | no — the cubic is extrapolating, so the figure would be arithmetic rather than a measurement |

> **Meter your borewell with a handheld before trusting the RWT figure.** These
> probes are rated 0–1000 ppm and groundwater around Guntur can sit above that.
> A pinned feed reading does not just lose accuracy — it takes the rejection
> percentage with it, since the feed is the denominator.

**Expect to clean the electrodes.** This is a trend instrument, not an analytical
one. A slow drift over months is as likely to be deposits on the probe as a
change in the water; the rejection ratio is more robust to that than either raw
figure, because both probes foul in the same direction.

---

## 10. Battery Room Node (0x04: SHT30 & Exhaust Fan)

> **Who decides the fan:** the hub. Thresholds live in hub NVS and are set from the calibration AP (`RS485_PROTOCOL.md` §4.4), so changing them needs a phone, not a programmer on a ladder. The node keeps a hotter backstop — on 40.0 °C, off 37.0 °C — that takes over only if no command arrives for five minutes, plus a fail-safe that ventilates if the SHT30 goes unreadable. The relay is wired so that both of those, and a de-energised board, leave the room ventilated rather than sealed.

**Mid-chain node — no termination resistor.** 0x04 sits between the Dosing node and the TWT node in the as-installed run (Section 12). If a 120 ohm resistor was fitted here under the earlier end-of-bus assumption, **remove it**: three terminators on one bus over-load the drivers and blunt the differential swing.

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
 |            ARDUINO PRO MINI  (ATmega328P-AU, 5V / 16 MHz)              |
 |  - VCC Pin <-- +5V Buck Output      *** NOT the RAW pin - see 10.1 *** |
 |  - GND Pin <-- Common Ground                                           |
 +------+------------+-------------+-------------+-------------+----------+
        |            |             |             |             |
      Pin D2       Pin D3        Pin A4        Pin A5        Pin D9
     (Soft RX)   (Soft TX)      (I2C SDA)     (I2C SCL)     (Relay IN)
                              [inner pad]    [inner pad]
        |            |             |             |             |
 +------v------------v------+ +----v-------------v----+ +------v----------+
 |    XY-485 MODULE         | |   GY-SHT30-D SENSOR   | | 1-CH RELAY BOARD|
 | RXD --> D2               | | SDA --> A4            | | IN1 --> D9      |
 | TXD --> D3               | | SCL --> A5            | | VCC --> 5V      |
 | A+  --> RS485 Bus (A+)   | | VCC --> 5V            | | GND --> GND     |
 | B-  --> RS485 Bus (B-)   | | GND --> GND           | | COM/NO -> Fan AC|
 +--------------------------+ +-----------------------+ +-----------------+

 Programming header (6-pin FTDI / CP2102 adapter, 5V setting):
   DTR -> DTR   TXO -> RX   RXI -> TX   VCC -> 5V   GND -> GND
```

### 10.1. Pro Mini Build Notes (0x04 only)

This node is the one Pro Mini in the fleet; the other three RS485 nodes are Nanos. Same ATmega328P, same `pinmap_node.h`, same binary — only the board mechanics differ.

| Item | Requirement | Why |
| :--- | :--- | :--- |
| **Power feed** | +5V from the Mini560 / LM2596 into **`VCC`**. Leave **`RAW` unconnected.** | `RAW` goes through the Pro Mini's SOT-23 LDO. Dropping 12V→5V at this node's ~168 mA peak dissipates **~1.2 W** in a package good for ~0.4 W, and the relay coil alone brushes the LDO's ~150 mA rating. It would brown out or thermally shut down — in the hottest room in the building, on the node whose job is reporting that room's temperature. |
| **Board variant** | 5V / 16 MHz **only**. Verify: 9–12V on `RAW`, nothing else connected, measure `VCC` = **5.0V**. | ~5 mA flows unloaded, so the LDO is safe for this one test. A 3.3V board starves the XY-485 and the 5V relay coil, and 8 MHz halves the SoftwareSerial bit-timing margin at 9600 baud. |
| **`A4` / `A5`** | Solder wires to the **two inner pads** (labelled on the underside). They are not on the edge headers — those run `A3 A2 A1 A0`. | I²C is hardware TWI on `PC4`/`PC5` and cannot be moved to other pins. If a board truly omits the breakouts, the fallback is soldering to TQFP pins 27 (`PC4`/SDA) and 28 (`PC5`/SCL). |
| **`A0` / `A1` address jumpers** | **Leave both unconnected** — that is `0x04` under `ADDR_MAP` (§9.1). | No wiring needed, but it means an unjumpered board *is* a Battery Room node. Never put a second unjumpered board on the bus: two nodes answering `0x04` collide on every climate poll. |
| **Programming** | FTDI / CP2102 adapter set to 5V, **`DTR` wired** (auto-reset). IDE: board *Arduino Pro or Pro Mini*, processor *ATmega328P (5V, 16 MHz)*. | Without `DTR` every upload times out. |
| **During programming** | **Disconnect the buck's 5V** while the adapter is plugged in. | Two supplies fighting on `VCC` kills adapters. |
| **RS485 on `D2`/`D3`** | Unchanged — SoftwareSerial, not the hardware UART. | On a Pro Mini `D0`/`D1` *are* the FTDI header. Keeping RS485 off them leaves the debug port usable without desoldering, which is the same reasoning as phase-B spec §199 and matters more here. |

**As-built 2026-08-26:** the bench FTDI adapter in use has **`DTR` unconnected**. Uploads to this board therefore need the reset button tapped the instant the IDE switches from *Compiling* to *Uploading*, and opening the Serial Monitor does not restart the sketch. Symptom when mistimed: `not in sync: resp=0x00`, ten attempts. One wire from the adapter's `DTR` (or `RTS`) to the `DTR`/`GRN` pad ends it — the 100 nF cap is already on the board.

**Failed 2026-09-04 — the shared +5V feed.** The §10 block diagram has the
XY-485 module, the GY-SHT30-D and the relay board all taking `VCC` from the same
+5V point. That one wire came off while this node was being reflashed, and the
failure it produces is worth knowing by its signature, because it took a long
detour to find:

| What you see | Where |
| :--- | :--- |
| Node silent on the bus — answers nothing, not one command | hub: `RS485 node offline: 0x04 Battery` |
| Battery room temperature and humidity gone, fan drops to OFF | hub: `BAT 0.0 C 0.0 %RH fan OFF NODE 0x04 OFFLINE` |
| The MCU is alive and talking normally over USB | `Node ID: 0x04  fw 1.01  role: Battery Room climate + fan` |

**That third line is the trap.** With the buck's 5V disconnected for programming
(as the table above requires) the Pro Mini runs happily on the adapter's 5V and
prints its boot banner, while the XY-485 has no supply to drive the bus with and
the SHT30 has none to be read. From the hub the node is simply dead. From a
serial terminal it is perfectly healthy. Nothing distinguishes it from a bus
fault, a dead transceiver, or stale node firmware — all three of which were
investigated first.

**Check this before firmware.** If a node is reachable over USB but invisible on
the bus, the shared +5V is the first thing to meter, not the last. The boot
banner already proves the MCU runs and the address jumpers read correctly (§9.1),
so it narrows the fault to power or the transceiver in one line.

**What isolated it:** the hub's per-node, per-command failure counters
(`rs485_error_report()`). `0x04/CLIMATE` failing 100% of polls with zero failures
on any other node or command rules out the bus, the terminators and the trunk in
one reading — a cable fault does not pick one node and spare the two sharing its
wire pair. The aggregate `rs485 err` count could not have shown that.

**For the PCB:** this is the second hand-wiring fault of the class
`PCB_HUB_MOTHERBOARD.md` §1 cites as the board's justification, after the broken
wire at a 240 V opto module. Note that spec covers the **hub only** — §11 scopes
node circuitry out — so the nodes still carry this exact risk, on flying leads,
in the hottest room in the building. Keyed connectors and a per-module supply pin
would have made this failure a non-event. The node boards deserve the same
treatment, and this is the second data point saying so.

---

## 11. Ground Floor Wi-Fi Nodes Wiring (Parking)

### 11.1. Ground Floor ESP32 Node 1: Sump Ultrasonic Level (0x05)
* **Power Source:** 230V AC $\to$ Hi-Link `HLK-20M5` (5V DC 4A)
* **ESP32 5V / VIN:** `+5V DC` from HLK-20M5
* **ESP32 GND:** `GND` from HLK-20M5
* **AJ-SR04M Sensor:**
  * `VCC` $\to$ `5V DC`
  * `GND` $\to$ `GND`
  * `TRIG` $\to$ `ESP32 GPIO 5`
  * `ECHO` $\to$ `1kΩ resistor` $\to$ `ESP32 GPIO 18` *(and 2kΩ from GPIO 18 to GND for 3.3V voltage divider)*

### 11.2. Ground Floor ESP32 Node 2: Motor AC Sensing & Float Relays (0x06)
* **Power Source:** 230V AC $\to$ Hi-Link `HLK-20M5` (5V DC 4A)
* **220V AC Optocoupler 1 (Sump Motor Monitor):**
  * `AC L / N` $\to$ Connected across Sump Motor Starter Contactor 240V Coil
  * `DC VCC` $\to$ `3.3V` from ESP32 — **not optional**, it feeds the onboard 47 k pull-up (§8)
  * `DC GND` $\to$ `GND`
  * `DC OUT` $\to$ `ESP32 GPIO 16` — **moved from GPIO 34 on 2026-08-27**, see §11.3
* **220V AC Optocoupler 2 (Borewell Motor Monitor):**
  * `AC L / N` $\to$ Connected across Borewell Motor Starter Contactor 240V Coil
  * `DC VCC` $\to$ `3.3V` from ESP32
  * `DC GND` $\to$ `GND`
  * `DC OUT` $\to$ `ESP32 GPIO 17` — **moved from GPIO 35 on 2026-08-27**, see §11.3
* **4-Channel Relay Board (Starter Interlocks):**
  * `VCC` $\to$ `5V DC` from HLK-20M5
  * `GND` $\to$ `GND`
  * `IN1` (Sump Low Float Cutoff) $\to$ `ESP32 GPIO 25`
  * `IN2` (Borewell High Float Cutoff) $\to$ `ESP32 GPIO 26`
  * `IN3` (Aux Manual Override 1) $\to$ `ESP32 GPIO 27`
  * `IN4` (Aux Manual Override 2) $\to$ `ESP32 GPIO 14`

### 11.3. Node 0x06 Current Clamps — Six Channels, Three Per Motor

Both ground-floor motors are 3-phase and **all three phases of each are clamped**, for the imbalance figure rather than for fault detection — phase-B spec §7.3 has the reasoning and records that this reverses an earlier two-per-motor decision.

**This is why the AC optos moved off `GPIO 34`/`35`.** Those are `ADC1_CH6`/`CH7`, and six analog channels need every ADC1 pin the module exposes. The optos need no ADC — the 240 V module supplies its own 47 k pull-up to `VCC` (§8) — so they take ordinary inputs and the analog pins go where only analog will do.

| Channel | GPIO | ADC1 | Conductor |
| :--- | :---: | :---: | :--- |
| `IN_SUMP_CT_L1` | **32** | CH4 | Sump starter, phase L1 |
| `IN_SUMP_CT_L2` | **33** | CH5 | Sump starter, phase L2 |
| `IN_SUMP_CT_L3` | **34** | CH6 | Sump starter, phase L3 |
| `IN_BORE_CT_L1` | **35** | CH7 | Borewell starter, phase L1 |
| `IN_BORE_CT_L2` | **36** | CH0 | Borewell starter, phase L2 |
| `IN_BORE_CT_L3` | **39** | CH3 | Borewell starter, phase L3 |

**ADC1 on this node is now full.** `GPIO 37`/`38` are the only other ADC1 channels on an ESP32 and are not broken out on a WROOM; everything else free is ADC2, which is dead whenever Wi-Fi is up.

**One shared bias rail, six series networks** — the same topology as the hub's two channels in §14, with the divider counted once:

```
                            +3.3V (ESP32 3V3 pin)
                                    |
                                   [ ] R1  10k
                                    |
        BIAS RAIL  o-----------------+---------o  ( 1.65 V )
        (1.65 V)   |                 |         |
                   |                [ ] R2 10k  --+-- C1 10uF
                   |                 |           --+-- C2 0.1uF
                   |                GND            |
                   |                              GND
                   |
       to the RING of all six 3.5 mm sockets

  each socket TIP -> [ ] 1k -> its ADC pin, with 100nF from that pin to GND
```

So the node's interface board is **two resistors and two capacitors once, plus one 1 k, one 100 nF and one socket per channel** — 14 passives and six sockets.

#### 11.3.1. Sizing these channels with no nameplate to read

**Confirmed on site 2026-08-27: neither motor has a readable nameplate.** The sump
motor sits at the bottom of the sump and the borewell pump is down the bore. Only
the HPP and RWP plates in the RO room can be read. So the usual "check the
nameplate FLC" step is not available for the two motors this section is about,
and any figure quoted for them is a guess until measured.

Three substitutes, in order of how much they are worth:

| Source | What it tells you | Trust |
| :--- | :--- | :--- |
| **The overload relay dial in the starter** | Whoever commissioned the motor set this from the nameplate, so the dial *is* the FLC, second-hand. Read the setting, not just the relay's range. | **Best available.** It also reflects what is actually installed, which a nameplate on a replaced pump would not |
| **A clamp meter on one phase while running** | The real running current, today, under real head | **Definitive** for sizing and for turns. You need the meter anyway — §14.2 and spec §7.3 both require a two-point calibration against one |
| Contactor rating, and the MCB or fuse feeding it | Upper bounds only. Protection is sized above FLC, often 1.5-2x | Rules out a wrong clamp; will not size one |

**The realistic risk here is the opposite of oversizing.** A 30 A clamp on a 3 A
motor produces about 100 mV, and the firmware has to pull that out of a 1.65 V
pedestal — resolution, not saturation, is what limits these channels. So expect
to fit **turns**, and decide the count on site from the measured running current:
aim for roughly a third to a half of full scale.

**Turns trade running resolution against start-current headroom, and that trade is
deliberate here.** Three turns of a 3 A conductor present 9 A to the clamp, which
reads well — but a direct-on-line start pulls 6-8x FLC, so those same three turns
present 60 A or more and the clamp clips for the second or two of the start. That
is accepted: these channels exist for running-current trend and phase imbalance
(spec §7.3), and start and stop are timed by the contactor optos, which do not
saturate. If locked-rotor current ever becomes the thing being measured, drop to
one turn and lose the resolution instead.

**Write the turn count on the enclosure** (§14.3). With no nameplate anywhere on
these two motors, a future reader has no way to re-derive it — the stored scale
factor and the turn count are the only record that the reading means amps.

**The starter is 415 V between phases, not 240 V.** Split-core clamps break no conductor, but this panel is more dangerous open than the RO skid. Fit them with the supply isolated and locked off, one conductor per clamp — a clamp around two phases reads their vector sum, not either current.

---

## 12. Complete RS485 Daisy-Chain Topology & Bus Termination

```
 +----------------+     +----------------+     +----------------+     +----------------+
 | ESP32 HUB      |     | BATTERY ROOM   |     | TWT NODE       |     | RWT NODE       |
 | Master (0x00)  +-----+ Slave (0x04)   +-----+ Slave (0x03)   +-----+ Slave (0x02)   |
 | [120Ω Enabled] |     | (No Resistor)  |     | (No Resistor)  |     | [120Ω Enabled] |
 +----------------+     +----------------+     +----------------+     +----------------+
   RO Room               Battery Room           Roof Top               Roof Top
        Cat5e 1 (TBM)          Cat5e 2 (TBM)          Cat5e 3 (TBM)

 Dosing tank sensor is NOT on this bus - it is wired direct to the hub (Section 13).
```

* **Bus Topology:** Strict linear daisy chain (no star/branch topologies). Physical order is `0x00 -> 0x04 -> 0x03 -> 0x02`. Node addresses are logical and deliberately do **not** follow the cable order — the hub's poll sequence is by address, not by position on the bus. Address `0x01` is retired and unassigned.
* **Termination:** Exactly **two 120 ohm resistors** on the entire physical bus:
  1. One at the **ESP32 Central Hub** (RO Room) across `A+` and `B-`.
  2. One at the physical end of the bus — **RWT Node 0x02** — across `A+` and `B-`.
* **No return loopback, no junction-box splice.** Earlier revisions routed the bus up to a mid-chain RWT and back down through the brown pair to an RO room splice. With RWT last, every hop is a single outbound pair and both spare pairs stay free for the 12V rail.
* **All cable lengths are TBM** and must be measured on site before re-running the power budget. The old 1.5 m `Cat5e 1` drop belonged to the deleted dosing node; the trunk now starts with the hub-to-battery-room run.

### 12.1. Cat5e Conductor Colour Code (authoritative)

**T568B pair colours, one assignment for every drop on the bus.** This table is the single source; `POWER_BUDGET.md` §5 assumes it for the drop calculation and the README's architecture diagram refers to it. Wire every drop the same way, including the spare pair, so any cable can be swapped for any other without a meter.

| Pair | Conductors | Carries | Why this pair |
| :---: | :--- | :--- | :--- |
| **1** | **Blue** = `A+`<br>**White-Blue** = `B-` | RS485 differential | A twisted pair is not optional here — the whole noise immunity of RS485 comes from the two conductors sharing the same interference. Blue is the pair furthest from the power pairs in most cable layups |
| **2** | **Orange + White-Orange**, both paralleled | **+12V** | Two conductors halve the resistance to 0.042 Ω/m, which is what `POWER_BUDGET.md` §5 assumes |
| **3** | **Green + White-Green**, both paralleled | **GND** | Same, for the return |
| **4** | **Brown + White-Brown** | **Spare — land it anyway** | Terminated at both ends, unused. It is the repair path when a conductor breaks, and the free run if a node ever grows a second sensor |

**Rules that go with it:**

* **`A+` is solid blue, `B-` is white-blue. Everywhere.** Swapped A/B is the single most common bus fault, and it presents as a node that simply never answers — indistinguishable from a dead node until you meter it. Consistency matters far more than which colour got which polarity.
* **Power and signal deliberately do not share a pair.** +12V and GND each get a full pair, the same arrangement PoE uses. Splitting them across pair members would unbalance the run and couple supply noise straight into the differential.
* **At a mid-chain node, both cables land on the same terminals** — incoming and outgoing on `A+`/`B-`/`+12V`/`GND` together. The bus stays one continuous line; **no stub longer than ~300 mm**, and no star.
* **If STP is used**, ground the shield at the **hub end only**. Grounding both ends invites a loop current down the drain wire.
* **Label both ends of every drop** with its segment (`HUB→0x04`, `0x04→0x03`, `0x03→0x02`) before it goes into a conduit.

### 12.2. Transceiver Count — Resolved

The bus needs **4** XY-485 modules: one at the hub and one per remaining slave (`0x04`, `0x03`, `0x02`). An earlier revision of this section described deferring the RWT node because the design then called for five. Deleting node `0x01` (Section 13) removed the fifth. **Build the whole bus in one pass — there is no interim state and the 120 Ω lives at RWT `0x02` from the start.**

---

## 13. Dosing Tank Level — Direct to Hub (No RS485 Node)

The dosing tank sits roughly **100 cm** from the hub enclosure, well inside the AJ-SR04M's own captive transducer lead. A Nano, a buck converter, an XY-485 module, an enclosure and a 1.5 m Cat5e drop existed only to carry one distance reading across one metre. All of it is deleted; the sensor lands on the hub directly.

```
  ESP32-S Hub                        AJ-SR04M Module (at the hub)
 +------------------------+         +--------------------------------+
 | 5V (Buck Output)       +-------->| VCC   (5V - do NOT use 3V3)    |
 | GND                    +-------->| GND                            |
 | GPIO 5                 +-------->| TRIG                           |
 |                        |         |                                |
 | GPIO 4  <--+-- 1kΩ ----+---------+ ECHO  (5V logic - divide it)   |
 |            |                     +---------------+----------------+
 |          1.8kΩ                                   | 2.5 m coax
 | GND  <-----+                                     v
 +------------------------+                +--------------------+
                                           | Waterproof         |
   ECHO divider: 5.0V x 1.8k/(1k+1.8k)     | Transducer Head    |
              = 3.21V at GPIO 4            | (in dosing tank)   |
                                           +--------------------+
```

**AJ-SR04M checks apply here too** — `R19` mode pad empty, `TRIG` width tunable. See Section 9.0; a dosing sensor reading a flat zero is that pad, not the wiring.

**Why the divider is not optional.** `ECHO` idles low and swings to a hard 5 V. The ESP32 is 3.3 V logic with no 5 V tolerance on its GPIOs; a bare connection stresses the pad every ranging cycle. **Built with 1 kΩ / 1.8 kΩ** — an E24 value, and it lands at 3.21 V, just under the rail instead of just over it. Section 11.1 still specifies 1 kΩ / 2 kΩ for the unbuilt ground-floor node; fit 1.8 kΩ there too when it is built, so the whole plant carries one divider.

**Scheduling — do not read this sensor inside a poll window.** `pulseIn(GPIO4, HIGH, 35000UL)` blocks for up to 35 ms. Phase-B spec §4.1 documents this as fatal *on a Nano slave*, where a blocked `SoftwareSerial` silently drops ~15 % of incoming polls. On the hub the risk is different and milder: the hub is the master and slaves never transmit unbidden (`RS485_PROTOCOL.md` §1.1), so nothing is missed — but a 35 ms stall inside the poll/response sequence still eats into the turnaround guard time. Read it at a fixed point in the 1 s cycle, **after** the last slave response and before serving the dashboard. There is ~800 ms of idle cycle to put it in.

**What this removes from the build:** one Arduino Nano, one XY-485 transceiver, one Mini560 buck, one enclosure, one 1.5 m Cat5e drop, one RS485 address, and one more thing that can go offline. What it adds: two GPIOs and two resistors.

---

## 14. Motor Current Clamps — Hub Wiring (HPP & RWP)

Two SCT-013-030 split-core clamps on the RO skid panel, read by the hub's last two free **ADC1** pins. Rationale, calibration and the four further channels at the ground panel are in phase-B spec §7.3.

**Why the bias network exists at all:** the clamp's output is AC, swinging either side of zero. An ESP32 ADC reads 0-3.3 V only, so the whole waveform is lifted onto a 1.65 V pedestal and the firmware subtracts that offset again in software. **One pedestal serves both channels** — the divider is shared, not duplicated.

```
                            +3.3V (ESP32 3V3 pin)
                                    |
                                   [ ] R1  10k
                                    |
        BIAS RAIL  o-----------------+-------------------o  ( 1.65 V )
        (1.65 V)   |                 |                   |
                   |                [ ] R2  10k        --+--  C1  10uF
                   |                 |                 --+--  C2  0.1uF
                   |                GND                  |
                   |                                    GND
                   |
      +------------+------------------------------+
      |                                           |
      |  CHANNEL 1 - HPP                          |  CHANNEL 2 - RWP
      |                                           |
  +---+----------------+                    +-----+--------------+
  | 3.5 mm SOCKET  [S] |                    | 3.5 mm SOCKET  [S] |
  | SCT-013-030    [T] |                    | SCT-013-030    [T] |
  +----------------+---+                    +----------------+---+
                   |                                         |
                  [ ] R3 1k                                 [ ] R4 1k
                   |                                         |
                   +-----o GPIO 36 (ADC1_CH0)                +-----o GPIO 39 (ADC1_CH3)
                   |                                         |
                 --+-- C3 100nF                            --+-- C4 100nF
                 --+--                                     --+--
                   |                                         |
                  GND                                       GND


  Clamp jaws:   HPP  ->  around the  P  conductor only        (never P and N together)
                RWP  ->  around the  P  conductor only
```

### 14.0. Hub-side header — AS BUILT

> **Built 2026-08-27 as a 4-pin header. Pin order metered 2026-08-30:
> `3V3` / `SN` / `SP` / `GND`.**
>
> This section said `3V3`/`SP`/`SN`/`GND` for three days on nothing but assumption.
> The as-built diagram disagreed, a continuity test settled it, and the middle two
> are the ones that decide **which clamp reads which motor**: **pin 2 is `SN` =
> `GPIO 39` = RWP, pin 3 is `SP` = `GPIO 36` = HPP.** Wire the breakout to that, not
> to the order a document remembers.
>
> That is not the 5-pin arrangement designed below, and the difference has one consequence
> that needs a physical guard rather than a document note:
>
> **Plugging the breakout in backwards shorts 3V3 to GND**, because supply and
> ground sit at opposite ends of the connector. This is **not special to this
> header** — every powered header on the board behaves the same way, and §1's
> pin-selection notes carry the board-wide rule that covers all of them: mark pin 1
> on every header and every mating plug. The only thing worth saying here is that
> the 5-pin order below would have been immune by construction, being a palindrome,
> and the 4-pin one is not.
>
> **Second, smaller consequence:** `SP` and `SN` sit adjacent with no screened pin
> between them, where the 5-pin order put `3V3` in the gap. Two millivolt channels
> side by side on a shared bias rail can couple. The per-channel 100 nF at each ADC
> pin (`C3`/`C4`, §14.1) covers most of it and the runs are short, so this is not
> worth resoldering — but **if HPP and RWP current ever look suspiciously
> correlated, this is the first thing to suspect.**
>
> The 5-pin design is kept below unchanged, because it is what a second board
> should use and because it explains what the built one gave up.

### 14.0.1. The 5-pin design (use this on any future board)

The clamps and their bias network live on a small breakout board built when the
sensors arrive. All the hub needs today is the connector that board will plug
into: **one 1×5 0.1" pin header**, carrying 3V3, both ADC pins, and two grounds.

```
     ESP32 HUB BOARD                                CT BREAKOUT (built later)
  +---------------------+
  | 1  GND             -|---------------------> R2 bottom, C1 / C2, C3 / C4
  | 2  GPIO 36  ADC1_CH0|---------------------> HPP channel, after R3 / C3
  | 3  3V3             -|---------------------> R1 top  (bias divider supply)
  | 4  GPIO 39  ADC1_CH3|---------------------> RWP channel, after R4 / C4
  | 5  GND             -|---------------------> second ground return
  +---------------------+
        ^
        +-- mark pin 1 on the board. Nothing else identifies this header.
```

**`GPIO 36` and `GPIO 39` are not silkscreened "36" and "39".** On the DevKit
fitted to this hub they are the two pins between `EN` and `G34` on the left
header, printed **`SP`** and **`SN`**:

```
   3V3 | EN | SN | SP | G34 | G35 | G32 | G33 | G25 | ...
               ^    ^
               |    +-- SP = SENSOR_VP = GPIO 36 = IN_HPP_CT
               +------- SN = SENSOR_VN = GPIO 39 = IN_RWP_CT
```

The abbreviation is because both double as the ADC pre-amp sensor inputs, which
is the same reason they are input-only with no internal pull-up. **That mapping is
fixed in the silicon**, so it holds whichever order a particular board prints the
two labels in. Verified against the fitted board 2026-08-27 — it cost a round of
"there is no pin 36 on this board" first.

**Why this pin order, and not 3V3-GND-A-A-GND.** Three properties, all free:

1. **3V3 sits between the two analog pins**, so the channels are never adjacent.
   The rail is AC-ground through `C1`/`C2`, so it screens one channel from the
   other — worth having when both carry millivolt signals derived from the same
   panel.
2. **A ground flanks each analog pin**, giving both a return path alongside the
   signal rather than somewhere across the board.
3. **It is reversal-safe.** The order is a palindrome except for pins 2 and 4:
   plug the breakout in backwards and `GND` meets `GND`, `3V3` meets `3V3`, and
   the *only* consequence is HPP and RWP swapping labels. The obvious ordering
   `3V3-GND-A-A-GND` puts 3V3 onto a ground pin when reversed, which shorts the
   rail. A header that cannot be plugged in destructively does not need a key.

**Leave `GPIO 36` and `GPIO 39` unconnected until the breakout exists.** They are
input-only pins with no internal pull-up, so an empty header floats them and the
ADC reads noise. Harmless — nothing acts on a current reading yet — and the
firmware reports it as `breakout not fitted` rather than as a plausible number.

**What the log should say at each stage** (`probeCTInput()` runs every cycle):

| Stage | Reading | Verdict |
| :--- | :--- | :--- |
| Header fitted, no breakout | wanders | `no breakout - pin floating` |
| Breakout fitted, no clamp | `~1650-1650 mV` | `pedestal OK, no current` |
| Clamp fitted, motor idle | `~1650-1650 mV` | `pedestal OK, no current` |
| Clamp fitted, motor running | swings past `1750` | `current flowing` |
| Divider wired wrong | `~0` or `~3300` steady | `pedestal at rail - check R1/R2` |

That second row is §14.2's meter check, available from the serial log instead of
a multimeter — so the breakout can be built and proven correct **before** any
clamp arrives.

### 14.1. Bill of materials

| Ref | Part | Qty | Note |
| :--- | :--- | :---: | :--- |
| — | SCT-013-**030** clamp (30 A, 1 V output) | 2 | Voltage output: burden resistor is **inside**. Do not add one |
| R1, R2 | 10 kΩ 1/4 W | 2 | Bias divider. 1 % preferred, but only for stability — the offset is measured in firmware anyway |
| C1 | 10 µF monolithic ceramic (106) | 1 | Holds the pedestal stiff against the CT's own current draw. Ceramic over electrolytic on purpose: no polarity to reverse, and nothing to dry out in a warm panel |
| C2 | 0.1 µF ceramic | 1 | Across C1, for the high-frequency end |
| R3, R4 | 1 kΩ 1/4 W | 2 | Series protection. Limits fault current into the ADC pad to ~3 mA |
| C3, C4 | 100 nF ceramic | 2 | With R3/R4, a ~1.6 kHz low-pass. Well clear of 50 Hz, kills contactor hash |
| — | 1×5 0.1" pin header (hub side) | 1 | The connector in §14.0. Fit now; snap 5 pins off a strip |
| — | 3.5 mm socket breakout (TRRS module, ~₹19) | 2 | One per channel. A labelled breakout is easier than a bare jack — but see the pad warning below. Panel-mount sockets are the alternative if the connector must sit on the enclosure face (§14.3) |

Everything but the clamps fits on one scrap of perfboard, plugging into the §14.0 header. (This previously paired the job with a `GPIO 34`/`GPIO 35` pull-up retrofit; that turned out not to be needed — §1, §8.)

### 14.2. Two things to check with a meter before trusting the wiring

1. **Which socket pads the clamp actually reaches — measure, do not read the silkscreen.** Two things conspire here. Clones differ: most SCT-013 leads use **tip and sleeve**, some tip and ring. And a 3-conductor **TRS plug in a 4-contact TRRS socket** lands its sleeve on the socket's **`RING2`** pad, not on `SLEEVE`. So the pad marked `SLEEVE` may read open while an unlabelled-looking one carries the winding. Plug a clamp in, close its jaws, and meter between pads: the winding reads a few tens of ohms, everything else reads open. Wire the two that show the winding.
2. **The pedestal, before connecting any clamp.** Power the hub, measure `GPIO 36` and `GPIO 39` to GND: both should sit at **1.6-1.7 V**. `probeCTInput()` prints the same figure every cycle (§14.0), so the meter is a cross-check rather than the only way to see it. If one reads 0 V or 3.3 V, the divider is wrong and the ADC will clip half the waveform — which looks like a plausible-but-wrong current reading, not like a fault.

### 14.3. Panel practice

* **Fit the clamps with the supply isolated.** Split-core means no conductor is broken, but the panel is live when open.
* **Panel-mount the sockets.** A 3.5 mm plug hanging inside a vibrating starter enclosure is a connector that will fail intermittently, which is the worst way for it to fail. Alternatively cut the plugs off and terminate into screw terminals.
* **Route the clamp leads away from the mains bundles**, and twist each pair. These are millivolt signals sitting beside contactors.
* **Small loads: use turns.** A ~5 A pump gives only ~170 mV from a 30 A clamp. Passing the conductor through the jaws 2-3 times multiplies the signal by that count; the 13 × 13 mm window takes three turns of 2.5 sqmm. **Write the turn count on the enclosure** — the calibration divides by it, and a future reader has no other way to know.

### 14.4. Firmware outline

```c
// GPIO 36 / 39 are ADC1 - they keep working with Wi-Fi up, unlike ADC2.
analogSetPinAttenuation(IN_HPP_CT, ADC_11db);   // full 0-3.3 V span

// ~200 ms of samples, DC offset measured rather than assumed, then RMS.
// Blocking: call it where the dosing read already sits - after the last RS485
// reply, never between a poll and its response.
float readAmpsRMS(uint8_t pin, float ampsPerVolt, uint8_t turns) {
  const uint16_t N = 400;
  uint32_t sum = 0;
  uint16_t s[N];
  for (uint16_t i = 0; i < N; i++) { s[i] = analogRead(pin); sum += s[i]; delayMicroseconds(500); }

  float mean = (float)sum / N;                   // the live pedestal, not a constant
  float acc = 0;
  for (uint16_t i = 0; i < N; i++) { float d = s[i] - mean; acc += d * d; }

  float counts = sqrtf(acc / N);
  return counts * (3.3f / 4095.0f) * ampsPerVolt / turns;
}
```

`ampsPerVolt` starts at 30.0 for a 30 A / 1 V clamp and is then **corrected by a two-point calibration against a clamp meter** — the ESP32's ADC is nonlinear enough that the nominal figure is a starting guess. Store it hub-side beside the tank calibration, so it is editable over the AP rather than compiled in.
