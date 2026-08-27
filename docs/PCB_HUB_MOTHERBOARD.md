# Hub Motherboard — Schematic Specification

**Status:** specification only. No board has been designed, ordered or built.
**Date:** 2026-08-28
**Purpose:** replace the hand-wired perfboard hub with a fabricated 2-layer PCB.

This document is deliberately *tool-independent*. It is the complete net list, so
it serves every route to an actual board:

1. **You draw it** in EasyEDA or KiCad — this is the data-entry sheet.
2. **A design service draws it** — PCBWay and JLCPCB both offer one; this is the
   brief you hand over, and it is specific enough that nobody has to ask you
   questions.
3. **Nobody draws it** — it still stands as the first complete net-level record of
   what the hub actually is, which no other document in this repo contains.

> **On part numbers.** This specification gives exact *electrical* specs — value,
> tolerance, package, voltage rating — and deliberately does **not** give LCSC or
> JLCPCB order codes. I cannot verify a part number against a live catalogue from
> here, and a fabricated order code is worse than none: it looks authoritative and
> arrives as the wrong component. Search LCSC for the spec given and read the
> datasheet.

---

## 1. Why, and what it fixes

The hub works. It was commissioned on the bench on 2026-08-27 and every input and
output was verified (`WIRING.md` §0.1). This is not a rescue.

What it fixes is the failure mode already on record. During commissioning, one of
the 240 V opto channels read as floating, and the cause was **a broken wire at the
module** — not a design fault, a hand-wiring fault. That happened on a bench, in
good light, on a board that had been touched recently. The same joint has to
survive years in a humid room next to pumps that vibrate.

Secondary, and cheaper to value: it retires the pin-1 problem. `WIRING.md` §1
records that every powered header on the current board shorts its supply to
ground if a plug is reversed, and the only mitigation available on perfboard is
paint. On a PCB, keyed connectors make the mistake impossible instead of
merely marked.

## 2. What must close before this is ordered

Ordering now would fabricate a design with three known holes in it. **None of
these block writing the specification; all of them block sending it.**

| Open item | Why it matters to the PCB |
| :--- | :--- |
| PC817 `IN3` still on the Aster `AUX OP` pair, not `LPS` (`WIRING.md` §0.2) | The last live wiring change. Fabricate before it, and the silkscreen labels a connector wrongly |
| CT front-end unproven — no breakout built, no clamp fitted | §7 of this document integrates that circuit onto the motherboard. Integrating an unproven analog circuit is how a board revision happens |
| Phase C float emulation undecided | Four relay outputs exist and drive nothing. If they will ever switch Aster float contacts, the board needs those terminals; if not, the relay header can shrink |

**Recommended order of work:** finish the `IN3` move, build and prove the CT
front-end on perfboard using §14.1 of `WIRING.md`, then fabricate. The perfboard
front-end is ten parts and is the cheapest possible way to validate the analog
section before it is etched into a motherboard.

## 3. Board-level decisions

| Decision | Choice | Reason |
| :--- | :--- | :--- |
| Layers | **2** | Nothing here is fast. A ground plane on the bottom layer is the whole benefit |
| Size | ~**100 × 80 mm** | Fits JLCPCB's cheapest tier and the existing enclosure. Confirm against the enclosure before ordering |
| Thickness / finish | 1.6 mm, HASL | Cheapest standard. No fine pitch anywhere on this board |
| Assembly | **None — hand solder** | Every part is through-hole by choice: it is the build method that already works here, it needs no BOM matching against a stock library, and it makes field repair possible with an iron |
| ESP32 mounting | **Female headers, not soldered down** | The module is the one part with a real failure rate and the one you may want to reflash on a bench. Two 1×19 female headers at 25.4 mm spacing |
| Connectors | **JST-XH, keyed** | This is the pin-1 fix. Every off-board connection gets a keyed connector that physically cannot reverse |

> **Verify the ESP32 header spacing against the actual module before ordering.**
> The board fitted is a 38-pin DevKit whose two rows are ~25.4 mm apart, but clone
> dimensions vary by a millimetre or two and a wrong footprint makes the board
> scrap. Measure the pin rows, do not trust a datasheet for a clone.

## 4. Net list — ESP32 module

All 21 assigned GPIOs, from `app_priv.h`, which `docs/check_pinmap.py` holds
identical to three documents. **The firmware is the authority; if this table and
`app_priv.h` ever disagree, `app_priv.h` wins.**

| GPIO | Net name | Goes to | Notes |
| :---: | :--- | :--- | :--- |
| 16 | `RS485_RX` | `J_RS485` pin 3 | XY-485 `RXD` |
| 17 | `RS485_TX` | `J_RS485` pin 4 | XY-485 `TXD` |
| 21 | `I2C_SDA` | `J_SHT30` pin 3 | 4.7k pull-ups are on the sensor board |
| 22 | `I2C_SCL` | `J_SHT30` pin 4 | |
| 26 | `IN_RL1_STAT` | `J_OPTO` pin 2 | PC817 `V1`, active LOW |
| 25 | `IN_RL2_STAT` | `J_OPTO` pin 3 | PC817 `V2`, active LOW |
| 33 | `IN_LPS` | `J_OPTO` pin 4 | PC817 `V3`, active LOW |
| 32 | `IN_TWT_FLOT` | `J_OPTO` pin 5 | PC817 `V4`, active LOW |
| 13 | `IN_ALARM` | `J_ALARM` pin 2 | **Direct, no opto.** `WIRING.md` §6.6 |
| 34 | `IN_HPP_AC` | `J_AC1` pin 3 | AC opto `OUT`. Module has its own 47k pull-up |
| 35 | `IN_RWP_AC` | `J_AC2` pin 3 | |
| 36 (`SP`) | `IN_HPP_CT` | §7 CT channel 1 | ADC1_CH0. Silkscreened `SP` on the module |
| 39 (`SN`) | `IN_RWP_CT` | §7 CT channel 2 | ADC1_CH3. Silkscreened `SN` |
| 5 | `US_TRIG_DOS` | `J_DOS` pin 3 | |
| 4 | `US_ECHO_DOS` | §8 divider output | **5V→3.3V divider required** |
| 27 | `OUT_RLY_TWT` | `J_RELAY` pin 2 | Active LOW. Phase C |
| 23 | `OUT_RLY_RWT` | `J_RELAY` pin 3 | |
| 18 | `OUT_RLY_DOS` | `J_RELAY` pin 4 | |
| 19 | `OUT_RLY_AUX` | `J_RELAY` pin 5 | |
| 2 | `LED_STATUS` | on-module LED | No external wire |
| 0 | `BTN_BOOT` | on-module button | No external wire |

**Do not route to `GPIO 6-11`.** They are the SPI flash. On this module they are
silkscreened `SD2 SD3 CMD SD0 SD1 CLK`, which contain no digits and read as spare
pins (`WIRING.md` §1). Leave those header positions unconnected and **mark them
`NC — FLASH` in the silkscreen**, which is a thing a PCB can do that perfboard
cannot.

`GPIO 12`, `14`, `15` are unassigned. Bring `14` out to a 3-pin spare header with
`3V3` and `GND`; it is the only comfortable spare (12 and 15 are strapping pins).

## 5. Connectors

All JST-XH, 2.54 mm, keyed. **Pin 1 of every connector points to the board edge**
— one rule, applied everywhere, so orientation is inspectable at a glance.

| Ref | Ways | Purpose | Pinout (1 → n) |
| :--- | :---: | :--- | :--- |
| `J_PWR` | 2 | 12 V in from HLK-20M12 | `+12V`, `GND` |
| `J_RS485` | 4 | XY-485 module | `+5V`, `GND`, `RXD`, `TXD` |
| `J_SHT30` | 4 | GY-SHT30-D, RO room | `3V3`, `GND`, `SDA`, `SCL` |
| `J_OPTO` | 6 | 4-ch PC817 output side | `GND`, `V1`, `V2`, `V3`, `V4`, `GND` |
| `J_ALARM` | 2 | Aster `AUX OP` `C`/`NO` | `GND`, `IN_ALARM` |
| `J_AC1` | 3 | 240 V opto #1, HPP | `3V3`, `GND`, `OUT` |
| `J_AC2` | 3 | 240 V opto #2, RWP | `3V3`, `GND`, `OUT` |
| `J_DOS` | 4 | AJ-SR04M, dosing | `+5V`, `GND`, `TRIG`, `ECHO` |
| `J_RELAY` | 6 | 4-ch relay board | `+5V`, `IN1`, `IN2`, `IN3`, `IN4`, `GND` |
| `J_CT1` / `J_CT2` | — | 3.5 mm sockets, on-board | See §7 |
| `J_SPARE` | 3 | `GPIO 14` | `3V3`, `GPIO14`, `GND` |
| `J_PROG` | 6 | FTDI programming header | `GND`, `3V3`, `RX0`, `TX0`, `EN`, `GPIO0` |

**`J_PROG` exists because the module's USB cannot be relied on.** On the current
hub it is already gone (`WIRING.md` §0.3.1), and a board that can only be
programmed through a chip that has died once should not be built. Bringing `EN` and
`GPIO 0` out as well as `RX`/`TX` means an adapter with `DTR`/`RTS` can enter boot
mode on its own, which removes the hold-BOOT-tap-EN dance the present build needs
for every flash. **Label the header `3V3 ONLY`** in the silkscreen — a 5 V adapter
on `RX0` is out of spec for the ESP32 and is the other half of how the current
board got damaged.

`J_OPTO` carries `GND` at both ends deliberately: it is the widest signal
connector and the return sits beside the signals rather than across the board.

## 6. Power

```
  J_PWR +12V ──┬──────────────────────────► +12V rail (opto wetting loops, §5.2)
               │
               └──► [ Mini560 / LM2596 buck, 12V → 5.0V ]  ──► +5V rail
                                                                 │
                                              ESP32 5V pin ◄─────┤
                                              J_RS485, J_DOS ◄───┤
                                              J_RELAY ◄──────────┘

  3V3 comes from the ESP32 module's own regulator, as it does today.
  Feeds: J_SHT30, J_AC1, J_AC2, J_SPARE, and the §7 CT bias divider.
```

- **Buck module on female headers, not soldered down.** Same reasoning as the
  ESP32: it is a module with a failure rate.
- **1000 µF electrolytic on +5V** near the relay header. Four relay coils
  energising is the largest current step on the board.
- **100 nF ceramic** at each module's supply pin.
- **Do not draw the relay board's 5 V through the ESP32.** It goes straight from
  the buck to `J_RELAY`.
- **Reverse-polarity protection on `J_PWR`, mandatory.** A **P-channel MOSFET ideal
  diode** on the 12 V input: source to `+12V` in, drain to the rail, gate to `GND`
  through a 100 kΩ, with a 12 V zener gate clamp. Roughly 20 mV of drop, versus
  ~400 mV for a series Schottky, and it protects against exactly the event that has
  already happened on this project — **the hub's onboard CP2102 was destroyed by
  reverse polarity on the `5V` pin** (`WIRING.md` §0.3.1). This is not a
  hypothetical failure being guarded against; it is a repeat being prevented.

> **The 3V3 budget is the one thing to check with a meter before ordering.** The
> ESP32 module's onboard regulator now feeds two AC opto modules, the SHT30, the
> CT bias divider and a spare header. It is adequate today on perfboard, which is
> evidence, not proof. Measure 3V3 under full load with both AC channels active
> before committing to a layout that has no room for a separate regulator.

## 7. CT front-end, integrated

The circuit is `WIRING.md` §14.1 unchanged, moved on-board. Integrating it is the
main functional gain of this board beyond reliability: on perfboard the two
millivolt channels run adjacent with no screen (`WIRING.md` §14.0), and a ground
plane fixes that properly.

```
  3V3 ──[ R1 10k ]──┬── BIAS RAIL (1.65 V) ──► ring of J_CT1 and J_CT2
                    │
              [ R2 10k ]      C1 10µF ║ C2 100nF to GND
                    │
                   GND

  J_CT1 tip ──[ R3 1k ]──┬──► GPIO 36 (SP)     C3 100nF to GND
  J_CT2 tip ──[ R4 1k ]──┬──► GPIO 39 (SN)     C4 100nF to GND
```

| Ref | Part | Note |
| :--- | :--- | :--- |
| R1, R2 | 10 kΩ, 1%, 1/4 W axial | 1% for stability only; firmware measures the offset |
| R3, R4 | 1 kΩ, 1/4 W axial | Limits fault current into the ADC pad to ~3 mA |
| C1 | 10 µF monolithic **ceramic** | Not electrolytic: no polarity to reverse, nothing to dry out in a warm room |
| C2 | 100 nF ceramic | HF end, across C1 |
| C3, C4 | 100 nF ceramic | With R3/R4, ~1.6 kHz low-pass. Clear of 50 Hz, kills contactor hash |
| `J_CT1/2` | 3.5 mm stereo socket, PCB mount | **See the pad warning below** |

> **Meter the socket before committing the footprint.** `WIRING.md` §14.2: most
> SCT-013 leads use tip and sleeve, some use tip and ring, and a 3-conductor TRS
> plug in a 4-contact TRRS socket lands its sleeve on `RING2`, not `SLEEVE`. Plug a
> clamp in, close its jaws, and meter for the winding — a few tens of ohms — then
> assign the footprint pads to the two that show it. Getting this wrong on a
> fabricated board means cutting traces.

## 8. Dosing ultrasonic level shift

`ECHO` is 5 V logic and `GPIO 4` is not 5 V tolerant.

```
  J_DOS ECHO ──[ 1k ]──┬──► GPIO 4
                       │
                     [ 2k ]
                       │
                      GND        → 5.0 V × 2/3 = 3.33 V
```

Keep this a divider, not a level-shifter IC: it is two resistors, and `TRIG` needs
no shifting because a 3.3 V output drives the module's input fine — which is
already proven on the bench.

## 9. Layout rules

1. **Ground plane on the bottom layer, one plane, no splits.** No signal here is
   fast enough to justify a split, and a split plane is how a beginner creates a
   return-path problem that is invisible until it is not.
2. **Keep the CT section together and away from the relay header.** The bias
   divider, R3/R4 and both sockets in one corner, with plane underneath. The relay
   header is the noisiest thing on the board.
3. **`J_AC1` / `J_AC2` carry only DC.** The 240 V is on the *other* side of those
   modules, which sit off-board. No mains reaches this PCB — do not let a
   reviewer talk you into creepage clearances that are not needed.
4. **The 12 V wetting loops to the PC817 inputs are off-board**, on the Aster
   side. Only the opto *output* side reaches `J_OPTO`.
5. **Silkscreen every connector with its signal names, not just `J_OPTO`.** The
   whole point is that the next person does not need this document to rewire it.
6. **Mark the flash pins `NC — FLASH`** at the ESP32 header positions for
   `GPIO 6-11`.
7. Mounting holes: 4 × M3, positioned from the enclosure, not from the board.

## 10. JLCPCB order parameters

| Parameter | Value |
| :--- | :--- |
| Layers | 2 |
| Dimensions | ~100 × 80 mm (confirm against enclosure) |
| Quantity | 5 (their minimum; keep spares — this board will get a revision) |
| Thickness | 1.6 mm |
| Surface finish | HASL with lead, or lead-free — either is fine, nothing here is fine pitch |
| Min trace / spacing | 6 mil / 6 mil is ample. Do not chase 5/5 |
| Copper weight | 1 oz |
| Assembly | **No.** Hand solder |
| Silkscreen | White on green — cheapest, and the most legible for hand assembly |

Expect a few dollars for the boards plus shipping to India, and one to two weeks.
**Order 5, expect to use 2, and assume revision 1 exists** — a first board from a
first-time layout almost always has something to fix, and planning for that is
cheaper than being surprised by it.

## 11. What this board deliberately does not do

- **No RS485 transceiver on-board.** The XY-485 module stays external. It is
  auto-direction, it works, and integrating an unfamiliar part into a first PCB
  adds a failure mode for no gain.
- **No relay coils on-board.** The 4-channel relay board stays external. Coil
  current and switched contacts do not belong on a signal motherboard.
- **No mains anywhere.** The AC opto modules stay external, as in §9.3.
- **No node `0x06` circuitry.** Different board, different enclosure, different
  floor. Six CT channels there are specified in `WIRING.md` §11.3.
