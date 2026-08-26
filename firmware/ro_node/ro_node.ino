/*
 * RO Monitor - RS485 Node Firmware (production)
 *
 * ONE binary for all three Arduino nodes. The A0/A1 jumpers select the address
 * AND the personality (phase-B spec 4):
 *
 *   0x02  Raw Water Tank      Nano       AJ-SR04M ultrasonic, end-of-bus 120R
 *   0x03  Treated Water Tank  Nano       AJ-SR04M ultrasonic
 *   0x04  Battery Room        Pro Mini   GY-SHT30-D + exhaust fan relay on D9
 *
 * There is no per-board edit and no per-board sketch: flashing the wrong image
 * onto a board is a failure mode this design simply does not have.
 *
 * Wiring: WIRING.md 9 (jumpers), 10 (battery room), 12 (chain and terminators).
 * Protocol: RS485_PROTOCOL.md - 0xAA 0x55 ADDR CMD LEN payload CRC_L CRC_H,
 * CRC-16/Modbus over every preceding byte, payload fields big-endian.
 *
 * Serial (115200) stays a debug port on all three boards. RS485 is SoftwareSerial
 * on D2/D3 so D0/D1 remain the FTDI header.
 */

// Wrong-target guard. Compiling this for an ESP32 fails deep inside SoftwareSerial
// with an error that says nothing about the actual mistake.
#if !defined(__AVR__)
#error "ro_node is AVR firmware. Select Arduino Nano, or Arduino Pro or Pro Mini (ATmega328P 5V 16MHz) - not the ESP32."
#endif

#include <SoftwareSerial.h>
#include <Wire.h>

#define FW_VERSION       0x0100   // 1.00, reported by CMD_PING

// Set to 1 only after confirming the board has an Optiboot-class bootloader.
// The old ATmegaBOOT bootloader on some Nano clones does not clear WDRF, so a
// watchdog reset there boot-loops the board until you reflash it over ISP.
#define ENABLE_WATCHDOG  0

// ---------------------------------------------------------------- pins
#define PIN_RS485_RX     2   // XY-485 RXD
#define PIN_RS485_TX     3   // XY-485 TXD
#define PIN_US_TRIG      7   // AJ-SR04M TRIG (tank nodes)
#define PIN_US_ECHO      8   // AJ-SR04M ECHO (tank nodes, 5V - divider on ESP32 only)
#define PIN_FAN_RELAY    9   // 1-ch relay IN, ACTIVE LOW (0x04 only)
#define PIN_LED          13  // Onboard activity LED
#define PIN_ADDR_0       A0  // Address jumper bit 0
#define PIN_ADDR_1       A1  // Address jumper bit 1

#define SHT30_I2C_ADDR   0x44

// ---------------------------------------------------------------- protocol
#define PREAMBLE_1       0xAA
#define PREAMBLE_2       0x55
#define RESPONSE_BIT     0x80

#define CMD_PING            0x01
#define CMD_READ_LEVEL      0x02
#define CMD_READ_CLIMATE    0x06
#define CMD_SET_FAN_RELAY   0x07

#define MAX_PAYLOAD      32
#define FRAME_TIMEOUT_MS 20   // gap that abandons a half-received frame

// ---------------------------------------------------------------- tuning knobs
// Ultrasonic. AJ-SR04M: ~200 mm blind zone, useful to ~4.5 m on water.
#define US_TRIG_WIDTH_US 10      // some batches want 20 - WIRING.md 9.0
#define US_TIMEOUT_US    35000UL // ~6 m of flight time
#define BLIND_ZONE_MM    200
#define WINDOW           5       // rolling median depth, one sample per cycle
#define AGREE_MM         25      // samples this close to the median count as good
#define DEAD_CYCLES      10      // consecutive no-echo cycles before "hardware fault"

// No tank calibration here on purpose. A node reports millimetres; the hub turns
// them into a percentage against calibration you can change over Wi-Fi. Tanks get
// re-calibrated, and climbing to a roof with a laptop to edit two numbers does not
// scale. See esp32_hub_test.ino and RS485_PROTOCOL.md 4.2.

// Battery room thermostat, tenths of degC. 38.0 C is the threshold the dashboard
// documents; 2.0 C of hysteresis keeps the contactor from chattering.
#define FAN_ON_DECI_C    380
#define FAN_OFF_DECI_C   360
#define FAULT_VENT_MS    30000UL  // SHT30 dead this long -> ventilate blind
#define FAN_OVERRIDE_MS  300000UL // hub override lapses back to the thermostat

#define CYCLE_MS         1000UL   // sensing cadence when the hub is not polling
#define BUS_QUIET_MS     5000UL   // no poll for this long -> free-run the sensor.
                                  // Must exceed the hub's worst cycle, or a healthy
                                  // hub triggers free-running and the 35 ms ranging
                                  // lands on top of a poll. The retries cover a
                                  // collision; not having them is cheaper.

// ---------------------------------------------------------------- address
// ADDR_MAP is written to fit the boards as they are already jumpered.
// WIRING.md section 9.1 is the authority; if a board is ever re-jumpered,
// that table and this one change in the same commit. docs/check_addrmap.py enforces it.
static const uint8_t ADDR_MAP[4] = {
  0x00,   // 0b00  both GND    -> unassigned, do not join the bus
  0x03,   // 0b01  A1 to GND   -> TWT
  0x02,   // 0b10  A0 to GND   -> RWT (end of bus)
  0x04,   // 0b11  both open   -> Battery Room (climate + fan relay)
};

uint8_t  MY_NODE_ID  = 0x00;
bool     isClimate   = false;   // 0x04 runs the climate personality

SoftwareSerial rs485(PIN_RS485_RX, PIN_RS485_TX);

// ---------------------------------------------------------------- state
uint16_t window[WINDOW];        // rolling raw samples, 0 = no echo
uint8_t  windowCount = 0;
uint8_t  windowNext  = 0;
uint16_t rawMM       = 0;
uint16_t medianMM    = 0;
uint8_t  quality     = 0;
uint8_t  sensorStatus = 2;      // 0 OK, 1 blind zone, 2 echo timeout, 3 hw fault
uint8_t  deadCycles  = 0;

int16_t  tempDeciC   = 0;
uint16_t humDeciPct  = 0;
uint8_t  climateFault = 1;      // 0 OK, 1 SHT30 error
bool     fanOn       = false;
unsigned long lastGoodClimate = 0;
unsigned long fanOverrideUntil = 0;

unsigned long lastCycle = 0;
unsigned long lastPoll  = 0;
bool measurePending = false;    // set after replying: sense while the bus is busy

uint8_t rxBuf[7 + MAX_PAYLOAD];
uint8_t rxLen = 0;
unsigned long rxLastByte = 0;

#if ENABLE_WATCHDOG
#include <avr/wdt.h>
#endif

// ---------------------------------------------------------------- helpers
uint16_t crc16(const uint8_t *buf, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t pos = 0; pos < len; pos++) {
    crc ^= (uint16_t)buf[pos];
    for (uint8_t i = 8; i != 0; i--) {
      crc = (crc & 0x0001) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
  }
  return crc;
}

uint8_t readNodeAddress() {
  pinMode(PIN_ADDR_0, INPUT_PULLUP);
  pinMode(PIN_ADDR_1, INPUT_PULLUP);
  delay(10); // settle pullups

  uint8_t raw = ((digitalRead(PIN_ADDR_1) == HIGH) << 1) | (digitalRead(PIN_ADDR_0) == HIGH);
  return ADDR_MAP[raw];
}

// ---------------------------------------------------------------- ultrasonic
uint16_t pingOnce() {
  digitalWrite(PIN_US_TRIG, LOW);
  delayMicroseconds(4);
  digitalWrite(PIN_US_TRIG, HIGH);
  delayMicroseconds(US_TRIG_WIDTH_US);
  digitalWrite(PIN_US_TRIG, LOW);

  unsigned long duration = pulseIn(PIN_US_ECHO, HIGH, US_TIMEOUT_US);
  if (duration == 0) return 0;                       // no echo
  return (uint16_t)((duration * 10UL) / 58UL);       // mm at ~343 m/s
}

// One sample per cycle into a rolling window. A five-deep median rides out the
// single wild outlier these sensors throw; averaging would not.
void sampleTank() {
  rawMM = pingOnce();
  window[windowNext] = rawMM;
  windowNext = (uint8_t)((windowNext + 1) % WINDOW);
  if (windowCount < WINDOW) windowCount++;

  uint16_t valid[WINDOW];
  uint8_t n = 0;
  for (uint8_t i = 0; i < windowCount; i++) {
    if (window[i] > 0) valid[n++] = window[i];
  }

  if (n == 0) {
    if (deadCycles < DEAD_CYCLES) deadCycles++;
    medianMM = 0;
    quality = 0;
    sensorStatus = (deadCycles >= DEAD_CYCLES) ? 3 : 2;
    return;
  }
  deadCycles = 0;

  for (uint8_t i = 1; i < n; i++) {                  // insertion sort, n <= 5
    uint16_t v = valid[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && valid[j] > v) { valid[j + 1] = valid[j]; j--; }
    valid[j + 1] = v;
  }
  medianMM = valid[n / 2];

  uint8_t agree = 0;
  for (uint8_t i = 0; i < n; i++) {
    uint16_t d = (valid[i] > medianMM) ? (valid[i] - medianMM) : (medianMM - valid[i]);
    if (d <= AGREE_MM) agree++;
  }
  // Quality is agreement across the window scaled by how much of it is echoing
  // at all: five tight samples = 100, one lonely sample among four timeouts = 20.
  quality = (uint8_t)((uint16_t)agree * 100 / WINDOW);

  sensorStatus = (medianMM < BLIND_ZONE_MM) ? 1 : 0;
}

// ---------------------------------------------------------------- climate
uint8_t sht30Crc8(uint8_t hi, uint8_t lo) {
  uint8_t data[2] = { hi, lo };
  uint8_t crc = 0xFF;
  for (uint8_t i = 0; i < 2; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

// Single-shot, high repeatability, clock stretching disabled (0x2C06).
bool readSHT30() {
  Wire.beginTransmission(SHT30_I2C_ADDR);
  Wire.write(0x2C);
  Wire.write(0x06);
  if (Wire.endTransmission() != 0) return false;

  delay(20); // 15 ms conversion + margin

  if (Wire.requestFrom((uint8_t)SHT30_I2C_ADDR, (uint8_t)6) != 6) return false;

  uint8_t d[6];
  for (uint8_t i = 0; i < 6; i++) d[i] = Wire.read();

  // The sensor sends a CRC-8 per word. An I2C run soldered to inner pads in the
  // hottest room in the building is where a corrupt reading turns up, and a
  // corrupt reading here switches a fan and raises an alarm.
  if (sht30Crc8(d[0], d[1]) != d[2]) return false;
  if (sht30Crc8(d[3], d[4]) != d[5]) return false;

  uint16_t rawT = ((uint16_t)d[0] << 8) | d[1];
  uint16_t rawH = ((uint16_t)d[3] << 8) | d[4];

  // T = -45 + 175 * raw / 65535, RH = 100 * raw / 65535, both in tenths here.
  // Integer only - no float on an ATmega for two one-decimal values.
  tempDeciC  = (int16_t)(-450 + (int16_t)(((int32_t)1750 * rawT) / 65535L));
  humDeciPct = (uint16_t)(((uint32_t)1000 * rawH) / 65535UL);
  return true;
}

void setFan(bool on) {
  fanOn = on;
  digitalWrite(PIN_FAN_RELAY, on ? LOW : HIGH); // relay board is ACTIVE LOW
}

// The thermostat is local on purpose: if the bus dies, the battery room still
// ventilates. A hub override (CMD_SET_FAN_RELAY) wins, but only until it lapses.
void updateFan() {
  if (millis() < fanOverrideUntil) return;

  if (climateFault != 0) {
    // No trustworthy temperature. Ventilating blind is the safe failure;
    // leaving the room sealed and hot is not.
    if (!fanOn && millis() - lastGoodClimate > FAULT_VENT_MS) setFan(true);
    return;
  }
  if (!fanOn && tempDeciC >= FAN_ON_DECI_C)  setFan(true);
  if (fanOn  && tempDeciC <= FAN_OFF_DECI_C) setFan(false);
}

void sampleClimate() {
  if (readSHT30()) {
    climateFault = 0;
    lastGoodClimate = millis();
  } else {
    climateFault = 1;
  }
  updateFan();
}

// Tenths of a unit as a one-decimal figure, without dragging in float printing.
void printDeci(int16_t deci) {
  Serial.print(deci / 10);
  Serial.print('.');
  Serial.print(abs(deci) % 10);
}

void sampleSensors() {
  if (isClimate) sampleClimate();
  else           sampleTank();
}

// ---------------------------------------------------------------- framing
void sendFrame(uint8_t cmd, const uint8_t *payload, uint8_t len) {
  uint8_t frame[7 + MAX_PAYLOAD];
  frame[0] = PREAMBLE_1;
  frame[1] = PREAMBLE_2;
  frame[2] = MY_NODE_ID;
  frame[3] = (uint8_t)(cmd | RESPONSE_BIT);
  frame[4] = len;
  for (uint8_t i = 0; i < len; i++) frame[5 + i] = payload[i];

  uint16_t crc = crc16(frame, (uint8_t)(5 + len));
  frame[5 + len] = (uint8_t)(crc & 0xFF);
  frame[6 + len] = (uint8_t)(crc >> 8);

  rs485.write(frame, (size_t)(7 + len));
  delay(5); // guard time before the master may transmit again
}

void handleCommand(uint8_t cmd, const uint8_t *payload, uint8_t len) {
  uint8_t out[10];

  switch (cmd) {
    case CMD_PING: {
      out[0] = (uint8_t)(isClimate ? climateFault : sensorStatus);
      out[1] = (uint8_t)(FW_VERSION >> 8);
      out[2] = (uint8_t)(FW_VERSION & 0xFF);
      sendFrame(cmd, out, 3);
      break;
    }

    case CMD_READ_LEVEL: {
      if (isClimate) return;                 // wrong personality, stay quiet
      uint16_t uptime = (uint16_t)(millis() / 1000UL); // wraps at ~18 h, per spec
      out[0] = (uint8_t)(medianMM >> 8);  out[1] = (uint8_t)(medianMM & 0xFF);
      out[2] = (uint8_t)(rawMM >> 8);     out[3] = (uint8_t)(rawMM & 0xFF);
      out[4] = 255;                          // level % is the hub's job, not ours
      out[5] = quality;
      out[6] = sensorStatus;
      out[7] = 0;                            // reserved
      out[8] = (uint8_t)(uptime >> 8);    out[9] = (uint8_t)(uptime & 0xFF);
      sendFrame(cmd, out, 10);
      break;
    }

    case CMD_READ_CLIMATE: {
      if (!isClimate) return;
      out[0] = (uint8_t)(tempDeciC >> 8);   out[1] = (uint8_t)(tempDeciC & 0xFF);
      out[2] = (uint8_t)(humDeciPct >> 8);  out[3] = (uint8_t)(humDeciPct & 0xFF);
      out[4] = (uint8_t)(fanOn ? 1 : 0);
      out[5] = climateFault;
      sendFrame(cmd, out, 6);
      break;
    }

    case CMD_SET_FAN_RELAY: {
      if (!isClimate || len < 1) return;
      setFan(payload[0] != 0);
      fanOverrideUntil = millis() + FAN_OVERRIDE_MS;
      out[0] = (uint8_t)(fanOn ? 1 : 0);
      sendFrame(cmd, out, 1);
      break;
    }

    default:
      break;                                  // unknown command: no reply
  }
}

// Byte-at-a-time so a partial or corrupt frame never blocks the sensing loop.
void serviceBus() {
  if (rxLen > 0 && millis() - rxLastByte > FRAME_TIMEOUT_MS) rxLen = 0;

  while (rs485.available()) {
    uint8_t b = (uint8_t)rs485.read();
    rxLastByte = millis();

    if (rxLen == 0 && b != PREAMBLE_1) continue;      // hunt for sync
    if (rxLen == 1 && b != PREAMBLE_2) { rxLen = 0; continue; }
    if (rxLen == 4 && b > MAX_PAYLOAD) { rxLen = 0; continue; }

    rxBuf[rxLen++] = b;

    if (rxLen >= 5) {
      uint8_t len = rxBuf[4];
      if (rxLen == (uint8_t)(7 + len)) {
        uint16_t want = (uint16_t)rxBuf[5 + len] | ((uint16_t)rxBuf[6 + len] << 8);
        rxLen = 0;
        if (crc16(rxBuf, (uint8_t)(5 + len)) != want) continue;   // corrupt, ignore
        if (rxBuf[2] != MY_NODE_ID) continue;                     // not ours
        if (rxBuf[3] & RESPONSE_BIT) continue;                    // an echo, not a request

        lastPoll = millis();
        digitalWrite(PIN_LED, HIGH);
        handleCommand(rxBuf[3], &rxBuf[5], len);
        digitalWrite(PIN_LED, LOW);

        // Sense now, while the master has moved on to the other nodes. pulseIn
        // blocks up to 35 ms and the SHT30 read 20 ms; doing that between a poll
        // and its reply is what drops frames on a SoftwareSerial slave
        // (phase-B spec 4.1).
        measurePending = true;
      }
    }
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  // Fan off before the pin becomes an output, or the relay clicks on every reset.
  digitalWrite(PIN_FAN_RELAY, HIGH);
  pinMode(PIN_FAN_RELAY, OUTPUT);
  digitalWrite(PIN_FAN_RELAY, HIGH);

  pinMode(PIN_US_TRIG, OUTPUT);
  pinMode(PIN_US_ECHO, INPUT);
  digitalWrite(PIN_US_TRIG, LOW);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Serial.begin(115200);
  while (!Serial && millis() < 2000);
  rs485.begin(9600);

  MY_NODE_ID = readNodeAddress();
  isClimate  = (MY_NODE_ID == 0x04);

  Serial.println(F("RO MONITOR - RS485 NODE"));
  Serial.print(F("Node ID: 0x0"));
  Serial.print(MY_NODE_ID, HEX);
  Serial.print(F("  fw 1.00  role: "));

  switch (MY_NODE_ID) {
    case 0x02:
      Serial.println(F("RWT ultrasonic (end of bus, fit 120R)"));
      break;
    case 0x03:
      Serial.println(F("TWT ultrasonic"));
      break;
    case 0x04:
      Serial.println(F("Battery Room climate + fan"));
      Wire.begin();
      break;
    default:
      // Both jumpers grounded. Never guess an address: two boards answering one
      // poll is the failure the whole table exists to prevent.
      Serial.println(F("UNASSIGNED - both jumpers grounded. Holding off the bus."));
      for (;;) { digitalWrite(PIN_LED, !digitalRead(PIN_LED)); delay(200); }
  }

#if ENABLE_WATCHDOG
  wdt_enable(WDTO_2S);
#endif

  sampleSensors();
}

void loop() {
#if ENABLE_WATCHDOG
  wdt_reset();
#endif

  serviceBus();

  if (measurePending) {
    measurePending = false;
    lastCycle = millis();
    sampleSensors();
    return;
  }

  // Free-run only when the master has gone quiet - otherwise sensing is driven by
  // the poll, which keeps the blocking reads out of the response window.
  if (millis() - lastPoll > BUS_QUIET_MS && millis() - lastCycle >= CYCLE_MS) {
    lastCycle = millis();
    sampleSensors();

    // Every personality prints something every second. A node that prints nothing
    // is indistinguishable from a dead one on a bench, and the battery room node
    // used to do exactly that.
    Serial.print(F("[offline] "));
    if (isClimate) {
      if (climateFault) {
        Serial.print(F("SHT30 FAILED"));
      } else {
        printDeci(tempDeciC);
        Serial.print(F(" C  "));
        printDeci((int16_t)humDeciPct);
        Serial.print(F(" %RH"));
      }
      Serial.print(F("  fan "));
      Serial.println(fanOn ? F("ON") : F("OFF"));
    } else {
      Serial.print(medianMM);
      Serial.print(F(" mm  q="));
      Serial.println(quality);
    }
  }
}
