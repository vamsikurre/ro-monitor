# RS485 Master-Slave Communication Protocol

**Document Version:** 1.0  
**Date:** 2026-08-19  
**Topology:** Single Master (ESP32 HUB), Multi-Slave (Arduino Nano Tank Nodes)  
**Physical Layer:** RS485 Half-Duplex (9600 bps or 19200 bps, 8 data bits, 1 stop bit, no parity: 8-N-1)

---

## 1. Protocol Architecture & Collision Avoidance

To ensure industrial-grade reliability and avoid bus contention:
1. **Strict Master-Polled Architecture:** Slaves **NEVER** initiate communication autonomously. Slaves only transmit in immediate response to a valid request addressed specifically to them.
2. **Deterministic Sequence:** The ESP32 Hub polls each node sequentially:
   - Poll Node 1 (Dosing Tank) -> Await Response / Timeout ->
   - Poll Node 2 (Raw Water Tank) -> Await Response / Timeout ->
   - Poll Node 3 (Treated Water Tank) -> Await Response / Timeout ->
   - Repeat cycle every **1000 ms** (1 second configurable interval).
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

---

## 3. Node Addressing Scheme

| Node ID | Assigned Tank / Role | Microcontroller | Primary Sensor |
| :---: | :--- | :--- | :--- |
| `0x00` | **ESP32 HUB (Master)** | ESP32-S | SHT30 Ambient, Opto AC/Dry Inputs |
| `0x01` | **Dosing Chemical Tank** | Arduino Nano | Waterproof Ultrasonic (JSN-SR04T) |
| `0x02` | **Raw Water Tank (RWT)** | Arduino Nano | Waterproof Ultrasonic (JSN-SR04T) |
| `0x03` | **Treated Water Tank (TWT)** | Arduino Nano | Waterproof Ultrasonic (JSN-SR04T) |
| `0x04`–`0xFE` | *Reserved for Expansion* | Arduino / ESP | Auxiliary flow, pH, or EC sensors |
| `0xFF` | **Broadcast Address** | All Slaves | Global synchronization / Reset |

---

## 4. Command Specifications

### 4.1. `CMD_PING` (`0x01`)
Used by the Master to verify slave liveness and measure round-trip latency.
- **Request Payload:** None ($N=0$).
- **Response Payload:** `uint8_t status_flags`, `uint16_t firmware_version`.

### 4.2. `CMD_READ_LEVEL` (`0x02`)
Requests processed, filtered water level and telemetry from a tank node.
- **Request Payload:** None ($N=0$).
- **Response Payload (10 Bytes):**
  - `uint16_t distance_mm`: Median-filtered distance from sensor transducer to liquid surface in millimeters.
  - `uint16_t raw_distance_mm`: Unfiltered instantaneous reading in millimeters.
  - `uint8_t level_percent`: Calculated level percentage ($0$ to $100\%$, $255 = \text{Invalid/Error}$).
  - `uint8_t signal_quality`: Quality indicator ($0$ to $100\%$, based on echo stability).
  - `uint8_t sensor_status`: Bitfield ($0 = \text{OK}$, $1 = \text{Blind Zone Overflow}$, $2 = \text{Echo Timeout}$, $3 = \text{Hardware Fault}$).
  - `uint8_t reserved`: Reserved byte.
  - `uint16_t node_uptime_s`: Slave uptime in seconds.

### 4.3. `CMD_READ_RAW_SENSORS` (`0x03`)
Requests raw diagnostic echo timing and temperature (if available).
- **Request Payload:** None ($N=0$).
- **Response Payload (6 Bytes):**
  - `uint32_t echo_time_us`: Raw ultrasonic round-trip time in microseconds.
  - `int16_t internal_temp_deci_c`: Node internal temperature in tenths of °C.

### 4.4. `CMD_GET_CONFIG` (`0x04`)
Reads tank geometry configuration stored in the slave's EEPROM.
- **Request Payload:** None ($N=0$).
- **Response Payload (6 Bytes):**
  - `uint16_t tank_height_mm`: Total tank height in mm.
  - `uint16_t sensor_offset_mm`: Transducer offset above maximum water mark in mm.
  - `uint16_t blind_zone_mm`: Sensor blind zone in mm (typically 200 mm).

### 4.5. `CMD_SET_CONFIG` (`0x05`)
Updates tank geometry configuration in the slave's EEPROM.
- **Request Payload (6 Bytes):**
  - `uint16_t tank_height_mm`
  - `uint16_t sensor_offset_mm`
  - `uint16_t blind_zone_mm`
- **Response Payload (1 Byte):**
  - `uint8_t status` (`0x00 = SUCCESS`, `0x01 = REJECTED`).

---

## 5. CRC-16 Calculation Algorithm (Modbus Standard)

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

## 6. Timeout, Retry, and Error Recovery Strategy

1. **Slave Response Timeout:** Master sets a timer for **100 ms** upon finishing packet transmission. If no complete packet is received within 100 ms, the attempt is marked as `TIMEOUT`.
2. **Retry Logic:** Master retries up to **2 consecutive times** (3 total attempts) before declaring the node `OFFLINE`.
3. **State Transition:**
   - *Online State:* 1 successful response restores `ONLINE` status immediately.
   - *Offline State:* Firmware logs an alert: `[WARN] RS485 Node X OFFLINE` and continues polling the next node in the cycle without stalling the main loop.
   - *Corrupted Packet:* If CRC16 check fails, packet is discarded and treated as a transmission error.
