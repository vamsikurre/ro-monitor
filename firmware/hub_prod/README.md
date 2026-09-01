# RO Monitor — Central Hub, production firmware

ESP-IDF firmware for the ESP32-S central hub. Built the same way as
`gate-controller` in this workspace, deliberately: same framework, same RainMaker
component, same two-slot OTA layout, same BLE pairing flow. If you can update the
gates over the air, you can update this the same way, from the same dashboard.

The bench sketch at `../esp32_hub_test/` stays where it is. It is a self-test
harness — it cycles relays and prints raw millivolts — and that is a different
job from running the plant. Keep it for commissioning a new board.

---

## What it does

| | |
| :--- | :--- |
| **RS485 master** | Polls node `0x02` (RWT), `0x03` (TWT), `0x04` (battery room climate + fan) every 2 s |
| **Local sensing** | RO-room SHT30, dosing-tank ultrasonic, 4 PC817 dry contacts, 2× 240 V opto, 2× SCT-013 clamp |
| **Fan policy** | Owns the battery-room thresholds and commands the relay over RS485; the node keeps a hotter backstop |
| **Local dashboard** | `http://ro-hub.local/` — the existing 1400-line page, embedded in flash |
| **Calibration** | `http://ro-hub.local/cal` — password protected; tanks, clamps, fan thresholds |
| **Cloud** | ESP RainMaker: five devices, live telemetry, push alerts |
| **OTA** | `esp_rmaker_ota_enable_default()` — push from the RainMaker dashboard |

**Nothing in this firmware moves water.** The only actuator it commands is the
battery-room exhaust fan. The four float-emulation relays on the board are driven
to de-energised at boot and then left alone — that is Phase C, and it stays that
way until the staged-adoption criteria in phase-B spec §7.2 are met.

---

## Build and flash

ESP-IDF **v5.2 or newer** (developed and verified against v5.4.4).

```bash
# once per shell
. $IDF_PATH/export.sh                 # Windows: the Espressif PowerShell profile

cd firmware/hub_prod
idf.py set-target esp32               # pulls esp_rainmaker, mdns, rmaker_app_network
idf.py build
idf.py -p COM5 flash monitor
```

`sdkconfig` is generated and **not** tracked — `sdkconfig.defaults` is the source
of truth. Delete `sdkconfig` and rebuild if the config ever looks wrong.

---

## OTA from the RainMaker dashboard

Wired and enabled: `esp_rmaker_ota_enable_default()` in `app_main.c`, two 1.84 MB
app slots in `partitions.csv`, HTTPS transport, 3 retries 5 minutes apart. Push a
job from the dashboard and the node acts on it over its OTA topic. The current
image is 1.5 MB, so a new one has room in the other slot.

Three things were fixed on 2026-09-01 before this could be relied on:

| | Was | Now |
| :--- | :--- | :--- |
| **Automatic rollback** | off — `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` unset, so `ROLLBACK_WAIT_PERIOD=90` was inert | **on.** A new image boots as *pending verify* and is only marked good once it reaches the cloud. If it cannot within 90 s, the ESP reboots into the previous slot on its own |
| **Autofetch period** | `3600` in `sdkconfig.defaults` — the unit is **hours**, range 0-168, so it was out of range, ignored, and the build ran with `0` (ask once per boot) | **24** — a daily catch-up for jobs missed while offline |
| **Reported version** | `PROJECT_VER` unset and a hand-maintained `FW_VERSION "1.0.0"` in `app_priv.h` that never left the header | **The version is the git description.** `CMakeLists.txt` runs `git describe --always --tags --dirty` into `PROJECT_VER`, and the banner, the RainMaker `Firmware` attribute and the dashboard footer all read it back from the app descriptor |

Rollback matters more on this build than on most. The onboard USB-serial is
destroyed (`WIRING.md` §0.3.1) and the local AP is now off, so an OTA image that
boots but cannot connect would leave **no way in at all** short of an FTDI and the
BOOT/EN dance in the plant room. With rollback on, that image reverts itself.

### The version is the commit

`1.0.0` was a number somebody had to remember to bump, and RainMaker refuses an
update whose version equals the running one — so a forgotten bump shows up as an
OTA that silently declines with *"Current running version is same as the new"*,
blaming the version rather than the habit. The version is now whatever
`git describe --always --tags --dirty` says:

| On the device | Means |
| :--- | :--- |
| `a3cb57d` | exactly that commit |
| `a3cb57d-dirty` | that commit **plus uncommitted changes** — you flashed a tree that is not in git |
| `v1.2-4-ga3cb57d` | 4 commits past tag `v1.2`, if anything is ever tagged |

Ordering is not required: RainMaker only checks that the two version strings
*differ*, never that the new one is greater, so a hash is a complete answer.

Commit before you build if you want the hash to mean anything — `-dirty` is the
build telling you it cannot identify what is on the box. Configure re-runs when
`.git/HEAD` or `.git/index` changes, so a fresh commit updates the version
without a manual `idf.py reconfigure`; if a version ever looks stale, that is the
command.

---

## First-boot pairing

Provisioning is over **BLE**, not SoftAP. That used to be what kept the hub's own
access point free to serve `/cal` at the same time; the AP is off as of
2026-09-01 and BLE remains the transport because it does not contend with the
station link.

1. ESP RainMaker app → **Add Device**
2. Scan the QR code printed on the serial console
3. Proof of possession: **`rohub1234`** (static, so pairing never needs the console)
4. Choose the site Wi-Fi

Boot button: hold **3 s** to clear Wi-Fi credentials and re-provision, **10 s**
for a full factory reset.

---

## Reaching it afterwards

Both interfaces stay up, always. Calibration happens standing next to a tank on a
roof, which is where the house Wi-Fi is least reliable — and it is also what you
need when the *router* is the thing that has failed.

| Path | Address |
| :--- | :--- |
| Over the LAN | `http://ro-hub.local/` or the DHCP address |
| Hub's own AP | **Disabled** as of 2026-09-01 (`AP_MODE_ENABLED 0`, `app_priv.h`). Dashboard and `/cal` are LAN-only. Set to `1` and reflash to restore `RO-HUB` / `ro-monitor` → `http://192.168.4.1/` |
| Calibration | `/cal` — user `admin`, password `ro-calibrate` (change it on that page) |

Change the calibration password on first commissioning. It is stored in NVS, so
it survives a reflash but not a factory reset.

---

## Calibration page

Three groups, all range-checked before anything is stored — a typo must not be
able to produce a reading that looks plausible and is wrong.

**Tanks.** Full and empty distances in millimetres, transducer face to liquid
surface. Empty must be the longer distance, and full must clear the 200 mm blind
zone. The live reading is shown next to each field.

**Current clamps.** Amps per volt, turns, and the over-current trip.
- An SCT-013-030 is nominally **30 A/V**, but two-point calibrate against a clamp
  meter. This is a trend instrument; consistency beats absolute accuracy.
- **Turns** is how many times the conductor passes through the jaws. A ~5 A pump
  gives only ~170 mV from a 30 A clamp, so 2–3 turns is usually worth it. The
  reading divides by this, and nothing else records it — write it on the
  enclosure too (`WIRING.md` §14.3).
- The page shows each channel's **pedestal** in millivolts. ~1650 mV means the
  bias breakout is correct. Anything else means no current will be reported at
  all, deliberately: a floating input produces a large and entirely fictional
  RMS figure, which is the worst possible failure for a dry-run detector.

**Fan.** On/off thresholds, clamped to 25.0–55.0 °C with at least 1.0 °C of
hysteresis enforced.

---

## RainMaker devices

Follows `docs/DASHBOARD_AND_RAINMAKER.md` §3.1, minus the Phase-2 ground floor.

```
RO Plant Monitor - XXXX
├── Water Tanks      Raw Water Level, Treated Water Level, Dosing Level
├── Pumps & Motors   HPP/RWP Running, HPP/RWP Current, Over Current
├── Environment      RO Room + Battery Room temperature and humidity
├── Ventilation      Exhaust Fan (read-only), Fan On Above (slider, writable)
└── RO Plant         Status (text), Controller Fault
```

Everything is read-only except the fan threshold. This hub publishes
measurements, and a measurement is not a control. Calibration deliberately lives
on `/cal` rather than in the app: it needs a live distance reading while you
stand next to the tank, which a phone notification cannot give you.

Values are reported **on change past a deadband**, not every cycle — fourteen
parameters at a 2 s cadence is a lot of MQTT for readings that mostly do not move.

## Alerts

Push notifications per `DASHBOARD_AND_RAINMAKER.md` §4, plus over-current, which
post-dates that table.

Each alert latches, needs the value to return past a margin before it can fire
again, and has a five-minute floor between repeats. An alert that cries wolf
teaches somebody to mute the app, and a muted app protects nothing. Over-current
additionally has to persist three cycles: one bad RMS read — a contactor closing
mid-window, a loose 3.5 mm plug — is not a fault.

The controller-fault alert says that something tripped but never *which*. The
Aster multiplexes every condition onto the one `AUX OP` contact (`WIRING.md`
§6.3), so text naming a cause would be a guess.

---

## Verification

The ported logic is checked against the field-verified bench sketch, not just
eyeballed:

```bash
python docs/check_frame.py     # CRC-16 and level maths, all three copies
python docs/check_pinmap.py    # 20 GPIOs across 3 documents + app_priv.h
python docs/check_addrmap.py   # node address jumper table
```

`check_frame.py` compiles `crc16()` and `levelPercent()` out of this firmware
alongside the sketch's copies and sweeps them against each other — a divergence
in the middle of the range, not just at the boundaries, fails the build.

`check_pinmap.py` now treats `app_priv.h` as a fourth source. It used to compare
documents to documents only, and that gap cost a day: `IN_ALARM` on GPIO 33 sat
in all three tables, passed the check, and was read by no firmware at all.

---

## Known limits

- **Ground floor is Phase 2.** Sump level, borewell and sump-motor state come
  from nodes `0x05`/`0x06`, which do not exist yet. They are reported as
  `OFFLINE` rather than as zeros, so the dashboard hatches them instead of
  drawing an empty tank that looks measured.
- **Calibration auth is HTTP Basic over plain HTTP** on the local network. The
  thing protected is a calibration constant; the alternative is provisioning and
  renewing a TLS certificate on an embedded box. Revisit if this ever gains a
  control that moves water.
- **CT channels are sampled one per cycle**, so each refreshes every ~4 s. Motor
  start and stop come from the contactor optos, which are instant, so nothing is
  lost — but the current figure is a trend, not an interlock input.
