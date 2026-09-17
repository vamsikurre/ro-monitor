# Hub split: ESP32-S3 hub on the ground floor, terrace hub becomes a node

Date: 2026-09-17. Status: approved in conversation, awaiting written review.

## 1. Why

The terrace hub (ESP32 DevKit, 4 MB flash, no PSRAM, fw `5368ad3`) runs out of
heap. Measured 2026-09-16 and again after a power cycle: 42–45 KB free at
steady state, `heap_min` under 300 bytes. One mbedTLS session costs ~42 KB at
the default settings and MQTT already holds one, so a RainMaker OTA cannot
open its TLS connection (`mbedtls_ssl_setup -0x7F00`). Every feature added to
the hub in the last month has come out of that same 42 KB.

Flash is not the constraint (14 % of the app partition free). RAM is. A
16 MB ESP32 has the same 520 KB SRAM; a module with PSRAM does not. The
decision is to move the RainMaker / dashboard / storage role to an
ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM) in the utility room,
and reduce the terrace board to a node that owns the plant's wires.

Decisions taken in the design conversation:

* The S3 is both the new hub and the utility node. The separate utility node
  is cancelled.
* The terrace decides locally for everything that touches a relay or reads the
  RS485 tanks. A LAN outage changes nothing at the plant.
* The current hub is frozen. `4e5aedf` is built but not flashed; no further
  work goes into `hub_prod` as an ESP32 image.
* The S3 carries an SD card for history, read back by the dashboard as
  week / month / year trends.
* The S3 does **not** read the RWT float (§5).

## 2. Roles after the split

```
Terrace (RO room)                       Ground floor (utility room)
ESP32 DevKit on the hub motherboard     ESP32-S3 DevKitC-1 N16R8
firmware/terrace_node                   firmware/hub_prod (target esp32s3)
- RS485 master: 0x02 RWT, 0x03 TWT,     - RainMaker, BLE provisioning, OTA
  0x04 battery room                     - dashboard, /api/*, 24 h RAM history
- dosing ultrasonic (direct)            - SD card: 1-min rows, day ledger
- HPP/RWP AC optos + clamps             - run accounting, day ledger, alerts
- Aster contacts: TWT float, RL1, RL2,  - borewell + sump motor clamps (6 ch)
  LPS, ALARM                            - Astero PUMP ON contact
- relays, relay test                    - utility-room SHT30
- RO-room SHT30, fan policy             - polls: terrace node, sump node
- /cal for tanks, clamps, TDS, fan
- /api/telemetry, POST fan, POST relay
- /ota (raw image over LAN)
                 ^
                 | HTTP GET every 2 s, same as the sump node
                 +---------------------------------------+
```

`gf_node` keeps only the sump role. The utility role moves into the hub.

## 3. Terrace node (`firmware/terrace_node`)

Carved from today's `hub_prod`. Keeps `app_rs485.c`, `app_sensors.c`, the
tank / clamp / TDS / fan parts of `app_cal.c`, and the poll loop's sensor half.
Drops RainMaker, BLE, `dashboard.html`, history, run accounting, the day
ledger, alerts, the cloud watchdog, `esp_rmaker_*` everywhere.

**Network.** Joins the house LAN as a station with credentials in NVS, set
once over serial (the `gf_node` pattern). No AP, no BLE, no mDNS required;
the hub addresses it by IP from its node table. Fixed DHCP reservation on the
router.

**Local decisions kept:**

* Fan policy from battery-room temperature, thresholds and mode in its NVS.
* Relay test pulse and self-expiry.
* The future Relay 1 interlock (float + ultrasonic agreement) belongs here.
  Not part of this project; the split must not make it harder.

**`GET /api/telemetry`** — interpreted values, calibration applied. One
object, keys fixed by the checker in §7:

```
link:     { id: 7, fw, uptime_s, rssi }   (5 = sump, 6 was utility, 0x01 is a retired RS485 address)
tanks:    { rwt|twt|dos: { pct, distance_mm, sensor, online } }
quality:  { rwt|twt: { ppm, t_deci_c, fitted, live, age_s } }
motors:   { hpp|rwp: { running, ac_floating, deci_amps, mv_lo, mv_hi } }
contacts: { twt_float_closed, rl1_active, rl2_active, lps_active, alarm_active }
climate:  { ro_room|battery_room: { temp_deci_c, hum_deci_pct, fault } }
fan:      { on, mode }
rs485:    { errors, last_poll_ms, failures }
```

Missing readings use the same sentinels the hub uses today (`-1`, `null`),
never a plausible zero.

**`POST /api/fan`** body `mode=auto|on|off`. **`POST /api/relay`** body
`idx=N` — one pulse, firmware releases it, no latch (today's `/cal` button).

**`/cal`** — tank distances, TDS k and gating, clamp midpoint and gain for
HPP/RWP, fan thresholds. Same NVS namespace and keys as today, so the reflash
needs no recalibration.

**`POST /ota`** — raw image, copied from `gf_node/main/ota.c` including the
trial-image guard. Image is on trial until one telemetry fetch has been
served, then marked valid within 120 s or rolled back.

**Timing.** Own sensors every 2 s (`POLL_CYCLE_MS`). The JSON is the last
completed cycle; a fetch never blocks on the bus.

## 4. S3 hub (`firmware/hub_prod`, target `esp32s3`)

**Unchanged:** RainMaker node build, device and parameter names, BLE
provisioning, RainMaker OTA, dashboard page and endpoints, run accounting and
day ledger, alerts (including the plant started/stopped pair and the
long-run backstop), the 24 h RAM history, the cloud watchdog, the fan command
path, the reboot-only system service.

**Replaced:** the terrace I/O calls in `poll_task` become one poller,
`terrace_apply()`, shaped like `gf_apply()`: fetch JSON, parse into the
existing `hub_state_t` fields, mark online/offline by the existing miss rule
(`NODE_OFFLINE_MS`, three misses). Fan mode and relay test become forwarded
POSTs to the terrace.

**Moved in:** the utility node's sensor code from `gf_node` — six clamp
channels, PUMP ON contact, SHT30. The `0x06` node entry disappears from the
node list and `/cal`; the sump node `0x05` stays.

**Memory plan.** `CONFIG_SPIRAM=y`, octal mode, `SPIRAM_USE_MALLOC`,
`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, `MBEDTLS_EXTERNAL_MEM_ALLOC`. Dynamic TLS
buffers stay on. The RAM history ring returns to one-minute rows (1440 × 24 B)
and is allocated in PSRAM. Success criterion: `heap_min` after provisioning
and after an OTA both above 100 KB internal, and the OTA completes.

**Node table.** The utility slot in `/cal` becomes the terrace slot (IP:port); the sump slot stays. Seed fields for
HPP and RWP lifetime seconds (write-once, used at cutover).

**Failure behaviour.** Terrace offline → its dashboard sections hatch, roof
pumps are unobservable to run accounting (no invented stops), alerts hold
their latch, node-offline alert names the terrace. Hub offline → plant runs
as before, no cloud, no dashboard.

## 5. S3 hardware

ESP32-S3-WROOM-1-N16R8 on a DevKitC-1. Octal PSRAM owns GPIO 33–37; GPIO 0,
3, 45, 46 are strapping; 19/20 are USB. ADC2 is unusable with Wi-Fi, so all
clamps sit on ADC1 (GPIO 1–10).

| Function | GPIO | Note |
|---|---|---|
| Borewell clamps L1 L2 L3 | 1, 2, 4 | ADC1 |
| Sump motor clamps L1 L2 L3 | 5, 6, 7 | ADC1 |
| SD card SPI CLK / MOSI / MISO / CS | 12 / 11 / 13 / 14 | SPI2 via GPIO matrix |
| SHT30 SDA / SCL | 17 / 18 | ADC2 pins, free |
| Astero PUMP ON contact | 21 | opto, active low |
| Status LED | 48 | on-board RGB |
| Boot button | 0 | on-board |
| spare ADC1 | 8, 9, 10 | 7th/8th clamp or a loop sensor |
| reserved, unused | 38 | RWT float — see below |

Clamp front end: the utility-node spec in `WIRING.md` (burden, midpoint
divider), attenuation matched to the S3's 3.3 V range. SD module: SPI type
rated for 3.3 V logic, own regulator acceptable.

**RWT float is not read.** The float cable runs ~25 m from the terrace to the
Astero through mains conduit — the same capacitive path that put 18–40 V on
the TWT wires — and an opto across the loop would be a second wetting circuit
sharing the Astero's high-impedance sense contact, the arrangement that
latched the TWT on 2026-09-13. One owner per float contact. The field keeps
its "not wired" sentinel. If ownership is wanted later, it is the TWT plan
mirrored (S3 owns the float with a 3 kΩ / 4 mA loop, drives the Astero input
through a relay) and gets its own design.

## 6. SD card storage and long-range trends

* FAT, SPI mode, mounted once at boot. **Never auto-formatted.** Mount
  failure is logged, shown on the dashboard, and the hub runs without it.
* One binary file per local day, `hist/YYYYMMDD.bin`, rows of the RAM
  history record at one-minute period: ~35 KB/day, ~13 MB/year. Append once a
  minute, `fsync` after each row; a power cut loses at most one row.
* The day ledger stays in NVS (34 days) for the run table, and each midnight
  close is also appended to `ledger.bin` so run figures outlive NVS.
* `GET /api/history?from=<epoch>&to=<epoch>` returns at most 720 points,
  every Nth row across the day files, same row shape as today's endpoint so
  the trend strip code is reused. The 24 h view keeps coming from RAM; the
  card is never in the path of the default page.
* Dashboard: range selector on the trend strip — 24 h, 7 d, 30 d, 1 y.

ponytail: FAT on a removable card is the deliberate ceiling; the module's own
16 MB flash could hold ~8 months in a partition with identical file code if
the connector proves unreliable.

## 7. Verification

* `docs/check_terrace_contract.py` — lifts the terrace's telemetry format
  string and the hub's parser keys, fails on drift (pattern of
  `check_telemetry.py`).
* `check_telemetry.py`, `check_offline_view.py`, `check_alerts.py`,
  `check_run_hours.py` follow their code to the hub tree and keep passing.
* `docs/fake_gf_node.py` gains a terrace shape so the S3 can be soaked on a
  bench without the plant.
* Memory acceptance in §4. OTA acceptance: one RainMaker OTA into the S3
  completes on the bench before cutover.

## 8. Build order and cutover

1. Terrace node firmware on the bench (spare DevKit if available; otherwise
   first flashed at step 5). Test: fake bus replies, fan policy, JSON, `/ota`
   and rollback.
2. S3 hub firmware on the bench against the fake terrace. Provision to
   RainMaker as a new node on the bench.
3. Ground-floor board: clamps, PUMP ON, SHT30, SD, power from the utility-room
   supply. S3 goes live reading the ground floor and polling the sump node
   while the old hub still runs the terrace and the cloud.
4. Soak a week: card filling, `heap_min` steady.
5. Cutover, one visit: read old hub lifetime totals from its telemetry;
   serial-flash `terrace_node` onto the terrace board; enter its address and
   the seed totals in the S3 `/cal`; confirm terrace online and tank levels
   back in RainMaker; remove node DB28 from the account.
6. Back-out = reverse of 5 with the serial cable: flash `4e5aedf` onto the
   terrace board, re-add DB28. Nothing on the ground floor is undone.

## 9. Out of scope

Relay 1 interlock; RWT float ownership; separate utility node (cancelled);
any further work on the ESP32 hub image; slab tariffs.
