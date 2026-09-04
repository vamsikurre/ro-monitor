# Hub Motherboard — KiCad Walkthrough

**For:** somebody who has never used KiCad, drawing the board specified in
`docs/PCB_HUB_MOTHERBOARD.md`.
**KiCad version:** 10.0.6. Menu paths below are that version's.
**Date:** 2026-09-04

This is not a general KiCad tutorial. There are better ones. This is the path
through KiCad that *this* board needs, in the order this board needs it, with the
specification you already wrote as the thing you are typing in.

The board is a good first PCB on purpose: two layers, every part through-hole,
no mains on the board (`PCB_HUB_MOTHERBOARD.md` §9.3), nothing fast, and a
complete net list written before a single symbol was placed.

---

## 0. What is already here, and what is yours

| File | State | Whose |
| :--- | :--- | :--- |
| `hub.kicad_pro` | Project, with §10's design rules already set | generated |
| `hub.kicad_sch` | Root sheet. **§7 CT front-end drawn as a worked example** | generated |
| `docs/check_schematic.py` | Compares what you draw against §4 | generated |
| `hub.kicad_pcb` | Does not exist yet. §9 onward | **yours** |
| everything else on the sheet | 17 remaining nets | **yours** |

Two rules before anything else:

1. **Save before you run the checker.** It reads the file on disk, not what is on
   your screen. An unsaved sheet checks as unchanged, which is the most confusing
   possible failure.
2. **Do not rename reference designators.** `J_CT1`, `J_RS485`, `J_OPTO` are not
   arbitrary — §5 puts them on the silkscreen, and §9.5 says the next person
   rewires this board without reading any document. `J1` tells them nothing.

---

## 1. Open it, and set the grid

`File > Open Project` → `hub.kicad_pro`. The project window lists the schematic;
double-click it to open **Eeschema**, the schematic editor.

First thing, before drawing: the grid. Your specification is metric and the worked
example is placed on a **1.27 mm** grid, which is 0.05" — half a standard pin
pitch, and the value that makes connector pins land where you expect.

- Units: `View > Units > Millimeters`
- Grid: the toolbar dropdown, or `View > Grid Properties` → **1.27 mm**

If you draw on a finer grid, wires will *look* connected and not be. This is the
single most common beginner fault in KiCad, and ERC is what catches it — which is
why §4 of this walkthrough runs ERC before you have drawn very much.

---

## 2. Read the worked example before adding to it

Zoom to the drawn section (`Home` fits the sheet). You are looking at
`PCB_HUB_MOTHERBOARD.md` §7 — the CT front-end — and it uses every technique the
rest of the board needs. Read it as a specimen:

**The bias divider, left.** `+3V3` at the top, `R1` and `R2` in series to `GND`,
their midpoint tapped by a short wire ending in a label. Two real wires and a
junction. Note the small circle where the tap meets the R1–R2 wire: that is a
**junction**, and it is what makes a T-connection electrically real. Wires that
merely cross are not connected.

**`C1` and `C2`.** They sit across the bias rail, but no wire runs to them from
the divider. Each has a stub ending in a `BIAS` label. **A label is a wire.** Two
things carrying the same label are the same net, anywhere on the sheet. This is
how you avoid dragging a wire across a crowded drawing, and it is what you will
use for almost everything on this board.

**The two channels, right.** Each jack's `T` (tip) goes through a series 1 k to a
node; the node has a 100 nF to ground and a label naming the net that leaves for
the ESP32. Each jack's `S` (sleeve) and `R` (ring) are stubs with labels — `GND`
and `BIAS`.

**The `PWR_FLAG` stubs, lower left.** Nothing on this sheet *produces* 3V3 or
GND; both arrive from the ESP32 module's regulator (§6). ERC does not know that
and would report every power pin as undriven. `PWR_FLAG` is how you tell it "this
rail is fed from somewhere I have not drawn". You will need one more when you draw
§6's buck output.

Click any symbol and press `E` for its properties. Look at `J_CT1`: its
**Footprint field is deliberately empty**. §7 says meter the socket before
committing pads, because SCT-013 leads differ. Empty is a decision, recorded.

---

## 3. Draw one net, end to end

Start with the smallest section in the spec: **§8, the dosing ECHO divider.** Two
resistors. It teaches the whole loop with almost nothing to get wrong.

From §8:

```
J_DOS ECHO ──[ 1k ]──┬──► GPIO 4
                     │
                   [ 2k ]
                     │
                    GND
```

1. **Place a symbol.** Press `A`. In the chooser, type `R`. Pick `Device:R` —
   the plain resistor. Click to place. Press `A` again for the second.
   - `M` moves the part under the cursor, `R` rotates, `E` edits, `Esc` cancels.
   - Set values with `E`: `1k` and `2k`.
2. **Give them references.** `Tools > Annotate Schematic` assigns `R5`, `R6`.
   Let it. Do not hand-type reference designators for passives — annotation exists
   so they stay unique.
3. **Wire them.** Press `W`, click the first pin, click the second. Wires snap to
   pins; if one does not snap, your grid is wrong (see §1).
4. **Add ground.** Press `P` for the power-symbol chooser, pick `GND`, place it
   under the 2k, wire it.
5. **Name the two ends.** `Place > Global Label` (the menu shows its shortcut).
   Type `US_ECHO_DOS` for the node between the resistors — that is the name §4
   gives GPIO 4. On the top of the 1k, place another reading `ECHO_5V`.
6. **Save** (`Ctrl+S`).
7. **Check it:**

```
python docs/check_schematic.py
```

You should see `drawn and matching` go from 2 to 3, and `US_ECHO_DOS` leave the
"still to draw" list. If instead you get a `FAIL` naming a net you do not
recognise, you have a typo — the checker prints the bad name and the pins it sits
on.

That loop — place, wire, label, save, check — is the whole job. Everything
remaining is more of it.

---

## 4. Run ERC now, not at the end

`Inspect > Electrical Rules Checker` → **Run ERC**.

Run it while the sheet is small and you can still recognise every violation.
People who run ERC for the first time on a finished schematic get sixty warnings
and give up on the tool.

The violations you will actually meet on this board:

| Violation | What it means here |
| :--- | :--- |
| *Pin not connected* | A real dangling pin, or a wire that missed by half a grid step |
| *Input power pin not driven* | A rail with no `PWR_FLAG` and no source. See §2 |
| *Global label only appears once* | Usually the other half is not drawn yet. Harmless now, revisit at the end |
| *Duplicate reference* | You hand-typed a designator. Run annotation |

The worked example passes with **0 errors, 0 warnings**. Keep it that way as you
go; when the count goes up, the thing you just drew is why.

---

## 5. Draw the rest, in this order

Do it in the order the specification is written — it goes from most-constrained to
least, which is also easiest to check.

| Do | Spec | Why this order |
| :--- | :--- | :--- |
| 1 | **§4** ESP32 module + its two headers | Every other net terminates here. Draw it and the labels have somewhere to land |
| 2 | **§5** the thirteen connectors | Mechanical, repetitive, no thinking. Good second task |
| 3 | **§8** ECHO divider | Done in walkthrough §3 |
| 4 | **§6** power: buck, 1000 µF, reverse-polarity MOSFET | The only section with a part you have not used before |
| 5 | **§7** CT front-end | Already drawn |

### For the ESP32 module (§4)

Use two `Connector_Generic:Conn_01x19` symbols — the module is socketed (§3), so
what the board actually contains is *two 19-way headers*, not an ESP32. Drawing it
as headers is honest about what gets soldered.

`E` on each pin's label is not how you name them. Instead, place a global label on
each pin's wire, using the net names from §4's table exactly as written. The
checker compares those strings.

**§4 has two warnings that a schematic can honour and perfboard cannot:**

- `GPIO 6-11` are the SPI flash. Leave those six header positions unconnected and
  put a text note beside them. §9.6 wants `NC — FLASH` on the silkscreen; add that
  as PCB text later, but note it on the schematic now so you do not forget why
  six pins are bare.
- Mark unused pins deliberately with `Place > No Connect Flag` (the `Q` tool).
  An explicit no-connect is the difference between "unused" and "forgotten", and
  ERC treats them differently.

### For the connectors (§5)

`Connector_Generic:Conn_01x02` through `Conn_01x06`, one per row of §5's table.
Set each Value to what it is (`JST-XH 4`), each Reference to the spec's name
(`J_RS485`), and label every pin from the Pinout column.

§5's one global rule — *pin 1 of every connector points to the board edge* — is a
**layout** rule, not a schematic one. It costs nothing now and matters in §7 of
this walkthrough. Write it on the sheet as a text note so it reaches whoever does
the layout.

### For power (§6)

The reverse-polarity P-channel MOSFET is the one genuinely new circuit. §6
specifies it fully: source to `+12V` in, drain to the rail, gate to `GND` through
100 kΩ, 12 V zener gate clamp. Symbols: **`Transistor_FET:Q_PMOS_GDS`** (that one
is in `Transistor_FET`, not `Device` — `Device` only carries the generic
`Q_PMOS`), plus `Device:R` and `Device:D_Zener`.

Draw it exactly as §6 states and resist improving it. It is there because reverse
polarity already destroyed this project's CP2102 once (`WIRING.md` §0.3.1) — this
is a repeat being prevented, not a hypothetical.

---

## 6. Footprints: what gets soldered

A symbol is a *schematic* idea. A footprint is copper. Nothing is fabricable until
every part has one.

`Tools > Assign Footprints` gives three panes: libraries, your parts, candidate
footprints. Assign per §3's decision — **every part through-hole, hand soldered.**

| Part | Library | Footprint |
| :--- | :--- | :--- |
| Resistors | `Resistor_THT` | `R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal` |
| Ceramics | `Capacitor_THT` | `C_Disc_D5.0mm_W2.5mm_P5.00mm` |
| 1000 µF (§6) | `Capacitor_THT` | `CP_Radial_D10.0mm_P5.00mm`, or `D12.5mm` — **measure the can you bought** |
| JST-XH | `Connector_JST` | `JST_XH_B<n>B-XH-A_1x0<n>_P2.50mm_Vertical` — note `B4B`, not `B04B` |
| ESP32 headers | `Connector_PinSocket_2.54mm` | `PinSocket_1x19_P2.54mm_Vertical` |
| CT jacks | — | **leave unset.** §7: meter first |

The ones already assigned in the worked example are correct; use them as the
pattern.

> **The ESP32 header footprint is the one that can scrap the board.** §3 says
> ~25.4 mm between rows and to measure rather than trust a datasheet. As of
> 2026-09-04 that number is unverified against the module actually fitted, and a
> same-pin-count board with different row pitch has already been bought by
> mistake once (`WIRING.md` §0.3.1). **Count the holes on the perfboard socket
> and confirm before you commit this footprint.**

---

## 7. The board: placement, plane, routing

`Tools > Update PCB from Schematic` (`F8`) creates `hub.kicad_pcb` and drops every
footprint in a pile with a **ratsnest** — thin lines showing what must connect.
Routing is turning ratsnest lines into copper.

Design rules are already set from §10: 6 mil clearance, 6 mil minimum track,
a `Default` net class at 10 mil and a `Power` class at 0.6 mm carrying
`GND`/`+3V3`/`+5V`/`+12V`. Check them at `File > Board Setup`; do not tighten
them. §10: *do not chase 5/5.*

**Order of work:**

1. **Board outline first.** Draw a rectangle on the `Edge.Cuts` layer — §3 says
   ~100 × 80 mm, confirmed against the enclosure. Nothing places sensibly before
   the outline exists.
2. **Mounting holes.** §9.7: four M3, positioned *from the enclosure*, not from
   the board.
3. **Place, then route.** Placement is most of layout quality. §9 tells you the
   two constraints that matter:
   - **§9.2** — CT section together, in one corner, away from the relay header.
     The relay header is the noisiest thing on the board and the CT signals are
     millivolts.
   - **§5** — pin 1 of every connector to the board edge.
4. **Ground plane.** `Place > Add Filled Zone`, whole board, `B.Cu`, net `GND`.
   §9.1: **one plane, no splits.** A split plane is how a beginner creates a
   return-path problem that is invisible until it is not.
5. **Route the rest on `F.Cu`.** Press `X` and click a pad. With a solid ground
   plane underneath you rarely need a second signal layer for a board this slow.
6. **DRC.** `Inspect > Design Rules Checker`. Must reach zero before you order.
   Unrouted nets are reported here too, which is how you know you are done.

I am not generating the layout for you, and that is deliberate rather than
laziness: placement is judgement about a physical object — where the enclosure
holes are, which way the loom leaves, what you want to reach with a probe — and
none of that is in any file I can read.

---

## 8. Before you order

§2 of the specification gates fabrication on three open items, and there is now a
fourth. **All four block sending, none block drawing.**

| Gate | Where |
| :--- | :--- |
| PC817 `IN3` still on Aster `AUX OP`, not `LPS` | `WIRING.md` §0.2 |
| CT front-end unproven — no breakout built, no clamp fitted | spec §2, §7 |
| Phase C float emulation undecided | spec §2 |
| **Hub browns out on Wi-Fi TX; cause not yet identified** | this is new, 2026-09-04 |

That fourth one matters to this board specifically. §6 already requires a
**1000 µF on +5V** and **100 nF at each module supply pin** — neither of which
exists on the current perfboard. If the brownout turns out to be the missing bulk
capacitance, this PCB already fixes it. If it turns out to be the buck module's
transient response, §6's power section needs revisiting *before* layout is
finalised. Either way, do not fabricate a copy of a board whose fault you have
not explained.

Then, per §10: 2 layer, 1.6 mm, HASL, 1 oz, 6/6, no assembly, white on green.
`File > Fabrication Outputs > Gerbers` plus drill files, zip them, upload.
**Order 5, expect to use 2, assume revision 1 exists.**

---

## 9. The loop, one more time

```
place  ->  wire  ->  label  ->  save  ->  python docs/check_schematic.py
                                      ->  Inspect > Electrical Rules Checker
```

The checker tells you whether you drew what you specified. ERC tells you whether
what you drew is electrically coherent. They catch different things and you need
both. Neither tells you whether the specification is right — that is what §2's
gates are for.
