# RS485 Master-Slave Communication Protocol

**Document Version:** 1.0  
**Date:** 2026-08-19  
**Topology:** Single Master (ESP32 HUB), Multi-Slave (Arduino Nano Tank Nodes)  
**Physical Layer:** RS485 Half-Duplex (9600 bps or 19200 bps, 8 data bits, 1 stop bit, no parity: 8-N-1)

---

## 1. Protocol Architecture & Collision Avoidance

To ensure industrial-grade reliability and avoid bus contention:
1. **Strict Master-Polled Architecture:** Slaves **NEVER** initiate communication autonomously. Slaves only transmit in immediate response to a valid request addressed specifically to them.
2. **Deterministic Sequence:** The ESP32 Hub polls each node sequentially **by address, not by position on the cable**:
   - Poll Node `0x02` (Raw Water Tank) -> Await Response / Timeout ->
   - Poll Node `0x03` (Treated Water Tank) -> Await Response / Timeout ->
   - Poll Node `0x04` (Battery Room) -> Await Response / Timeout ->
   - Repeat cycle every **1000 ms** (1 second configurable interval).

   The physical chain runs `0x00 -> 0x04 -> 0x03 -> 0x02` (`WIRING.md` §12). Address order and cable order differ on purpose; do not "fix" the poll sequence to match the wiring.

   **Address `0x01` is retired.** The dosing tank is ~1 m from the hub, so its AJ-SR04M is wired straight to the ESP32 (`WIRING.md` §13) and its level is read locally, outside this protocol. The hub must not poll `0x01` — doing so just burns a timeout every cycle.
3. **Guard Time (Turnaround Delay):** A 5 ms delay is observed by master and slaves after toggling `DE/RE` direction pins to allow the RS485 line transceivers to settle.

---

## 2. Frame Structure

Every transmission (both Request and Response) uses the standard binary frame format:

| Byte Index | Field Name | Data Type | Description |
| :---: | :--- | :--- | :--- |
| `0` | **PREAMBLE_1** | `uint8_t` | Fixed sync byte: `0xAA` |
| `1` | **PREAMBLE_2** | `uint8_t` | Fixed sync byte: `0x55` |
| `2` | **NODE_ADDR**  | `uint8_t` | Target Slave Address (`0x01`–`0xFE`), Master = `0x00`, Broadcast = `0xFF` |
| `3` | **COMMAND**    | `uint8_t` | Function / Command Code (Bit 7 = 0 for Request, Bit 7 = 1 for Response) |
| `4` | **PAYLOAD_LEN**| `uint8_t` | Length of payload bytes $N$ ($0 \le N \le 32$) |
| `5` to `4+N` | **PAYLOAD** | `uint8_t[N]` | Command-specific parameter or data bytes |
| `5+N` | **CRC16_L**   | `uint8_t` | Standard Modbus CRC-16 Low Byte |
| `6+N` | **CRC16_H**   | `uint8_t` | Standard Modbus CRC-16 High Byte |

> **Frame Overhead:** 6 bytes (2 Preamble + 1 Addr + 1 Cmd + 1 Length + 2 CRC16).  
> A standard 6-byte payload frame total length is 12 bytes ($\approx 10\text{ ms}$ transmission time at 9600 baud).

> **Two details the table above leaves open, fixed here:** the CRC-16 covers **every
> byte before it, preamble included**, and multi-byte payload fields are **big-endian**
> (the CRC itself is low byte first, as the table says).

> **Implemented by** `firmware/ro_node/ro_node.ino` (all three Arduino nodes, one
> binary) and `firmware/esp32_hub_test/esp32_hub_test.ino` (master). `docs/check_frame.py`
> compiles the CRC out of both and fails if they ever disagree — a master and a slave
> that differ on the CRC talk past each other with no symptom except silence.

---

## 3. Node Addressing Scheme

| Node ID | Assigned Tank / Role | Subsystem / Transport | Microcontroller | Primary Sensors / Actuators |
| :---: | :--- | :--- | :--- | :--- |
| `0x00` | **ESP32 HUB (Master)** | RO Room Core | ESP32-S | SHT30 Ambient, Opto AC/Dry Inputs, 4-Ch Relays |
| ~~`0x01`~~ | **Dosing Chemical Tank** | *Retired — sensor wired direct to hub* | — | Waterproof Ultrasonic (AJ-SR04M) on hub `GPIO 5` / `GPIO 4` |
| `0x02` | **Raw Water Tank (RWT)** | Roof Top RS485 | Arduino Nano | Waterproof Ultrasonic (AJ-SR04M) + 120Ω end-of-bus termination. Optional TDS + DS18B20 pair (§4.5) |
| `0x03` | **Treated Water Tank (TWT)** | Roof Top RS485 | Arduino Nano | Waterproof Ultrasonic (AJ-SR04M). Optional TDS + DS18B20 pair (§4.5) |
| `0x04` | **Battery Room Climate & Fan**| Battery Room RS485 | Arduino Pro Mini (5V/16MHz) | GY-SHT30-D (Temp/RH) + 1-Ch Exhaust Fan Relay |
| `0x05` | **Ground Sump Level** | Ground Floor Wi-Fi (polled) | ESP32 | Waterproof Ultrasonic (AJ-SR04M), **or** a 4-20 mA submersible pressure transducer on the `J-LOOP`/`J-PRESS` provision (§5, `WIRING.md` §11.1) |
| `0x06` | **Ground Utility Room** | Ground Floor Wi-Fi (polled) | ESP32 | 3× CT borewell + 3× CT sump motor, Astero `PUMP ON` dry contact, RWT floaty opto, SHT30. **No relays** — monitoring only (`WIRING.md` §11.2) |
| `0xFF` | **Broadcast Address** | Global Sync | All Slaves | Global synchronization / Bus Reset |

---

## 4. Command Specifications (RS485 Binary Bus)

### 4.1. `CMD_PING` (`0x01`)
Used by the Master to verify slave liveness and measure round-trip latency.
- **Request Payload:** None ($N=0$).
- **Response Payload:** `uint8_t status_flags`, `uint16_t firmware_version`.

### 4.2. `CMD_READ_LEVEL` (`0x02`)
Requests processed, filtered water level and telemetry from tank nodes (`0x02`, `0x03`). The dosing tank level is read locally by the hub and never appears on the bus.
- **Request Payload:** None ($N=0$).
- **Response Payload (10 Bytes):**
  - `uint16_t distance_mm`: Median-filtered distance from sensor transducer to liquid surface in millimeters. A node fitted with a **4-20 mA submersible transducer** instead of an ultrasonic one (`A3` jumpered, `WIRING.md` §9.4) reports `full_scale - head` here so this field keeps its meaning: a number that shrinks as the tank fills. The bus, the hub and the calibration do not know or care which sensor produced it — only the node's boot print says.
  - `uint16_t raw_distance_mm`: Unfiltered instantaneous reading in millimeters.
  - `uint8_t level_percent`: **Always `255` from a node.** Tank geometry is hub-side calibration, not node firmware — a node has no idea how tall its tank is, and re-scaling a tank must not mean climbing to a roof with a laptop. The hub scales `distance_mm` against per-tank full/empty values held in its NVS and editable over its calibration AP. The byte stays in the frame so the layout is fixed; any other value means a node running old firmware.
  - `uint8_t signal_quality`: Quality indicator ($0$ to $100\%$, based on echo stability).
  - `uint8_t sensor_status`: Bitfield ($0 = \text{OK}$, $1 = \text{Blind Zone Overflow}$, $2 = \text{Echo Timeout}$, $3 = \text{Hardware Fault}$).
  - `uint8_t reserved`: Reserved byte.
  - `uint16_t node_uptime_s`: Slave uptime in seconds.

### 4.3. `CMD_READ_CLIMATE` (`0x06`) - Battery Room Node (0x04)
Requests temperature, humidity, and exhaust fan status from Node `0x04`.
- **Request Payload:** None ($N=0$).
- **Response Payload (6 Bytes):**
  - `int16_t temp_deci_c`: Temperature in tenths of °C (e.g. $325 = 32.5^\circ\text{C}$).
  - `uint16_t humidity_deci_pct`: Relative humidity in tenths of % (e.g. $654 = 65.4\%$).
  - `uint8_t fan_relay_state`: `0 = OFF`, `1 = ON`.
  - `uint8_t fault_code`: `0 = OK`, `1 = SHT30 Error`.

### 4.4. `CMD_SET_FAN_RELAY` (`0x07`) - Battery Room Node (0x04)
Commands the battery room exhaust fan state.
- **Request Payload (1 Byte):** `uint8_t desired_state` (`0 = Turn OFF`, `1 = Turn ON`).
- **Response Payload (1 Byte):** `uint8_t current_state` (`0 = OFF`, `1 = ON`).

**The hub owns the thresholds; the node owns the backstop.** Fan policy is a setting
people change — 38 °C in April is not 38 °C in December — so it lives on the hub, in
NVS, editable from the calibration AP. The hub reads `CMD_READ_CLIMATE`, applies its
own hysteresis, and drives the relay with this command. Same principle as tank
calibration (§4.2): the node measures and actuates, the hub decides.

Two safeguards make that safe on a battery room:

1. **The hub re-asserts every 60 s**, even when nothing changes. Silence is not
   "leave it as it is" — see below.
2. **The node reverts to its own backstop after 5 minutes without a command**: on at
   **40.0 °C**, off at **37.0 °C**. Deliberately hotter and wider than any hub setting,
   so it never fights hub policy and only acts when the hub, the bus or the cable has
   failed. A battery room keeps ventilating when the network does not.

Independently of both, the node ventilates if its SHT30 has been unreadable for 30 s.
That path can only ever turn the fan **on**, so it cannot be commanded into a
dangerous state by a hub that is confused.

**Threshold values are validated at the hub, not trusted:** 25.0–55.0 °C, with `ON` at
least 1.0 °C above `OFF`. A typo on a phone must not be able to disable ventilation.

### 4.5. `CMD_READ_WATER_QUALITY` (`0x08`) — Tank Nodes (`0x02`, `0x03`)

Requests TDS and water temperature from a tank node that has the optional probe pair fitted (`WIRING.md` §9.5). A node without it answers with both fault bits set, so the command is always safe to send.

- **Request Payload:** None ($N=0$).
- **Response Payload (6 Bytes):**
  - `uint16_t tds_mv`: Raw TDS probe output in millivolts. **Not ppm** — see below.
  - `int16_t water_temp_deci_c`: Water temperature in tenths of °C, from the DS18B20 in the same tank.
  - `uint8_t status`: Bitfield. `0x01` = TDS unusable, `0x02` = temperature unusable. **`0` is the only value that makes the other fields meaningful.**
  - `uint8_t reserved`: Reserved byte.

**Millivolts, not ppm — the same division of labour as §4.2.** Converting to ppm is a floating-point cubic with a per-probe calibration factor, and two probes of the same part number do not agree out of the bag. Putting it on the hub keeps float off an ATmega that deliberately has none anywhere else, and it means recalibrating a probe against a reference solution is a form on `/cal` rather than a laptop on a roof. The hub's `tdsPPM()` does the conversion and refuses to produce a number it cannot justify, exactly as `levelPercent()` does.

**The two fault bits are set and cleared together, on purpose.** Conductivity moves about **2 % per °C** and every TDS figure is quoted at 25 °C, so an uncompensated reading drifts tens of percent across a season with nothing having changed in the water. A node that cannot read its DS18B20 therefore declines to report TDS either, rather than publishing a number that looks like a measurement. The presence pulse on the 1-Wire bus is what gates the whole feature: no sensor, no readings, no invented values.

**Two things the node refuses to report as temperature:** a scratchpad that fails its Dallas CRC-8, and exactly `85.0 °C` — the DS18B20's power-on default, which means the conversion never ran rather than that the tank is boiling.

**Polling cadence.** The node refreshes this every **10 s** (a 12-bit conversion alone takes 750 ms, so it is started on one cycle and collected on the next — it never blocks the poll loop). The hub asks every **10 poll cycles**, i.e. ~20 s. Asking faster only re-reads a number the node has not updated, at the cost of bus time the tank levels want.

**A failure here does not mark the node offline.** The level does that. A tank node with a working ultrasonic and a dead TDS probe is a node that is very much alive, and conflating the two would raise a bus alert for a water-quality fault.

---

## 5. Wi-Fi nodes — polled JSON (Ground Floor)

**Supersedes the push design below.** The two ground-floor nodes never
initiate anything — the same master-polled principle as §1, just over the
house LAN instead of RS485. A dedicated hub task (`gf_task`) does
`HTTP GET http://<ip>/api/telemetry`, one node at a time, **5 s** between polls
while a node answers, **2 s timeout** per request. Three consecutive misses
(≈15 s) mark it `OFFLINE`; an offline node is still polled, but only every
**30 s**, cheaply, until it answers again — the same reply that took it
offline puts it back at the 5 s cadence. The IP for each node is typed on
`/cal` → "Ground floor nodes" (`gf_sump_ip` / `gf_util_ip` in NVS). **An empty
IP means "not fitted"**: never polled, never alerted on, cards hatched — the
same meaning an empty field has everywhere else on that page. Nodes are
stateless and know nothing about the hub; a reflash never loses a
calibration number because none of them live on the node.

### 5.1. Sump `0x05`

```json
{"id":5,"fw":"a3cb57d","uptime_s":3840,"rssi":-64,
 "source":"ultrasonic","distance_mm":1750,"quality":95,"status":"OK",
 "loop_ua":null}
```

```json
{"id":5,"fw":"a3cb57d","uptime_s":3840,"rssi":-64,
 "source":"pressure","distance_mm":null,"quality":null,"status":"OK",
 "loop_ua":11200}
```

- `source` follows the node's `J-PRESS` shunt, read once at boot: fitted =
  `pressure`, off = `ultrasonic`.
- `status` is the same `sensor_status_t` the RS485 tank nodes use: `OK |
  BLIND | NO_ECHO | HW_FAULT`. On the pressure source, `HW_FAULT` means the
  loop current is outside **3.5-21 mA** — a cut cable, a dead supply or a
  short, not a level (`WIRING.md` §9.4.4).
- `loop_ua` is the raw loop current in microamps. **The hub converts it**,
  not the node — the transducer's full-scale range is a `/cal` number
  (`CAL_TANK_SUMP.press_range_mm`, 0 = ultrasonic only), never a node build
  constant, so a reflash never loses it: `head_mm = (loop_ua − 4000) ×
  range_mm / 16000`, then `distance = range − head`, so the same
  distance-shrinks-as-it-fills convention and the same full/empty
  calibration as every other tank apply unchanged (`WIRING.md` §9.4.1).

### 5.2. Utility `0x06`

```json
{"id":6,"fw":"a3cb57d","uptime_s":3840,"rssi":-68,
 "bore_mv":[412,405,398],
 "sump_mv":[398,0,0],"sump_on":false,
 "rwt_floty":null,
 "t_deci_c":312,"rh_deci_pct":548,"sht_ok":true}
```

- `bore_mv` / `sump_mv`: three per-channel AC RMS millivolts each, exactly as
  the node's ADC read them — no amps, no calibration, that is all hub-side.
  The **node** tests its own bias pedestal first: a midpoint outside
  **1250-2050 mV** (the same divider tolerance as the hub's own two CT
  channels) means nothing plausible is plugged in, and it reports **0 mV**
  for that channel rather than a real-looking number — the wire format has
  no null to give it instead (`WIRING.md` §11.3). The **hub** then
  applies its own, simpler rule on top of whatever it receives: anything
  under its `CT_NOISE_FLOOR_MV` (15 mV) is "no clamp fitted", reported as
  `null`, **never as 0 A** — a floating pin in a motor panel produces a
  large reading that looks exactly like a running motor, and 0 A would be
  read as a real measurement instead of a missing sensor.
- `sump_on`: the Astero `PUMP ON` dry contact, closed while the controller
  has the pump on. This is the sump motor's `running` state; the borewell
  has no contact of its own and its `running` comes from current instead,
  with a hysteresis band so a phase reading dithering at the threshold
  cannot flip the state every cycle and flood the cloud with messages.
- `rwt_floty`: `true` closed, `false` open, `null` not wired — this loop is
  only ever tapped after being metered (`WIRING.md` §11.4).
- SHT30 at I²C `0x44`, the same part the hub and the battery-room node use.
- A dying sump sensor's readings decay to nothing within about ten seconds
  rather than freezing at the last good value, and the hub additionally
  refuses to report a level when a node's own `status` says the sensor is
  faulted.

### 5.3. Node GPIO

| Utility `0x06` | GPIO | Sump `0x05` | GPIO |
| :--- | :---: | :--- | :---: |
| `BORE_CT_L1/L2/L3` | 32 / 33 / 34 (ADC1) | `TRIG` | 5 |
| `SUMP_CT_L1/L2/L3` | 35 / 36 / 39 (ADC1) | `ECHO` (via 1 k / 2 k divider) | 18 |
| `PUMP_ON` (pull-up) | 25 | `J-LOOP` pin 2, loop sense (ADC1) | 34 |
| `RWT_FLOTY` opto (pull-up) | 26 | `J-PRESS` shunt to GND (pull-up) | 25 |
| SHT30 `SDA` / `SCL` | 21 / 22 | | |

Wiring, the bias network and the loop provision are `WIRING.md` §11.

---

## 6. CRC-16 Calculation Algorithm (Modbus Standard)

Both ESP32 and Arduino Nano use the identical CRC-16 polynomial ($X^{16} + X^{15} + X^2 + 1$, represented by `0xA001` reversed):

```cpp
uint16_t calculate_crc16(const uint8_t *buffer, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t pos = 0; pos < length; pos++) {
        crc ^= (uint16_t)buffer[pos];
        for (int i = 8; i != 0; i--) {
            if ((crc & 0x0001) != 0) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}
```

---

## 7. Timeout, Retry, and Error Recovery Strategy

1. **Slave Response Timeout:** Master sets a timer for **100 ms** upon finishing packet transmission. If no complete packet is received within 100 ms, the attempt is marked as `TIMEOUT`.
2. **Retry Logic:** Master retries up to **2 consecutive times** (3 total attempts) before declaring the node `OFFLINE`.
3. **Wi-Fi Heartbeat Expiry:** After 3 consecutive missed polls (≈15 s) from a Ground Floor node (§5), status is set to `OFFLINE`, the cards for that node hatch on the dashboard, and the node-lost alert names it. Nothing moves water — there is no interlock to engage.
