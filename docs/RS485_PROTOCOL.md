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
| `0x02` | **Raw Water Tank (RWT)** | Roof Top RS485 | Arduino Nano | Waterproof Ultrasonic (AJ-SR04M) + 120Ω end-of-bus termination |
| `0x03` | **Treated Water Tank (TWT)** | Roof Top RS485 | Arduino Nano | Waterproof Ultrasonic (AJ-SR04M) |
| `0x04` | **Battery Room Climate & Fan**| Battery Room RS485 | Arduino Pro Mini (5V/16MHz) | GY-SHT30-D (Temp/RH) + 1-Ch Exhaust Fan Relay |
| `0x05` | **Ground Sump Level** | Ground Floor Wi-Fi | ESP32 | Waterproof Ultrasonic (AJ-SR04M - 3.5m Sump) |
| `0x06` | **Ground Motors & Interlock** | Ground Floor Wi-Fi | ESP32 | 2x 220V AC Optos (Sump/Borewell) + 4-Ch Relays |
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
  - `uint16_t distance_mm`: Median-filtered distance from sensor transducer to liquid surface in millimeters.
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

---

## 5. Wi-Fi JSON Protocol Specification (Ground Floor Nodes)

Ground Floor ESP32 nodes push telemetry to the Terrace ESP32 Hub via HTTP POST to `http://ro-hub.local/api/sump` and `http://ro-hub.local/api/motors`:

### 5.1. Ground Sump Node 1 (0x05) Payload:
```json
{
  "node_id": 5,
  "distance_mm": 1750,
  "water_depth_mm": 1750,
  "level_percent": 50,
  "signal_quality": 95,
  "status": "OK",
  "rssi": -64,
  "uptime_s": 3840
}
```

### 5.2. Ground Motor Node 2 (0x06) Payload:
```json
{
  "node_id": 6,
  "sump_motor_active": false,
  "borewell_motor_active": true,
  "sump_float_cutoff": false,
  "borewell_float_cutoff": false,
  "rssi": -68,
  "uptime_s": 3840
}
```

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
3. **Wi-Fi Heartbeat Expiry:** If no HTTP/UDP packet is received from Ground Floor nodes for **10 seconds**, status is set to `OFFLINE` and emergency pump interlocks engage.
