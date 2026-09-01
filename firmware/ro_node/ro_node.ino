/*
 * RO Monitor - RS485 Node Firmware (production)
 *
 * ONE binary for all three Arduino nodes. The A0/A1 jumpers select the address
 * AND the personality (phase-B spec 4):
 *
 *   0x02  Raw Water Tank      Nano       AJ-SR04M ultrasonic, end-of-bus 120R
 *                                           (+ optional TDS/DS18B20 pair, 9.5)
 *   0x03  Treated Water Tank  Nano       AJ-SR04M ultrasonic, or a 4-20 mA
 *                                           submersible transducer if the J-PRESS
 *                                           shunt is on (WIRING.md 9.4)
 *   0x04  Battery Room        Pro Mini   GY-SHT30-D + exhaust fan relay on D9
 *
 * There is no per-board edit and no per-board sketch: flashing the wrong image
 * onto a board is a failure mode this design simply does not have.
 *
 * Wiring: WIRING.md 9 (jumpers), 9.4 (pressure transducer), 10 (battery room),
 * 12 (chain and terminators).
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

#define FW_VERSION       0x0101   // 1.01, reported by CMD_PING

// Set to 1 only after confirming the board has an Optiboot-class bootloader.
// The old ATmegaBOOT bootloader on some Nano clones does not clear WDRF, so a
// watchdog reset there boot-loops the board until you reflash it over ISP.
#define ENABLE_WATCHDOG  0

// ---------------------------------------------------------------- pins
#define PIN_RS485_RX     2   // XY-485 RXD
#define PIN_RS485_TX     3   // XY-485 TXD
#define PIN_US_TRIG      7   // AJ-SR04M TRIG (tank nodes)
#define PIN_US_ECHO      8   // AJ-SR04M ECHO (tank nodes, 5V - divider on ESP32 only)
#define PIN_PRESS_SENSE  A2  // J-LOOP pin 2: 4-20 mA loop across the 100R sense resistor
#define PIN_PRESS_FIT    A3  // J-PRESS shunt to GND = transducer fitted, ignore the ultrasonic
#define PIN_WATER_TEMP   4   // DS18B20 1-Wire data. NEEDS a 4k7 pullup to +5V
#define PIN_TDS_POWER    5   // TDS board VCC, driven so the probe is not DC-biased 24/7
#define PIN_TDS_SENSE    A6  // TDS analog out. A6/A7 are analog-ONLY on the Nano, so
                             // spending one here costs nothing that could be used otherwise
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
#define CMD_READ_WQ         0x08   // TDS + water temperature (tank nodes)

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

// Submersible pressure transducer - the fallback for TWT, where the only roof
// penetration is hard against a wall and the beam cone reads that wall forever
// (WIRING.md 9.3). Wiring and commissioning: 9.4. Nothing here is active unless A3 is jumpered to GND; without
// the jumper A2 is never read, so an unfitted, floating input cannot invent a level.
//
// The node reports RANGE - head, not head: a distance that still SHRINKS as the
// tank fills, so the hub's calibration, its level maths, the alerts and the /cal
// page all work untouched. The two figures typed into /cal are then offsets from
// the sensor's full scale rather than air gaps - and because that page is a
// two-point calibration, it absorbs the sense resistor's tolerance for free.
#define PRESS_RANGE_MM   2000    // sensor full scale at 20 mA. MUST match the part fitted.
#define PRESS_SENSE_OHMS 100     // loop resistor: 4 mA = 0.40 V, 20 mA = 2.00 V into a 5 V ADC.
                                 // 100R not 150R because 20 mA through 150R eats 3 V of a 12 V
                                 // loop and the sensor browns out AT FULL TANK - WIRING.md 9.4.2
#define PRESS_ADC_MV     5000    // AVcc, the ADC reference
#define PRESS_MIN_UA     3500    // under this the loop is open or unpowered, not empty
#define PRESS_MAX_UA     21000   // over this it is shorted or miswired
#define PRESS_SAMPLES    8       // averaged per cycle; the median window rides on top of that

// Water quality - TDS probe + DS18B20, one pair per tank (WIRING.md 9.5).
//
// The node reports MILLIVOLTS and a temperature, never ppm. Same rule as the
// tank levels: a node measures, the hub converts against calibration that can be
// changed over Wi-Fi. It also keeps the ppm cubic - which is floating point -
// off an ATmega that deliberately has no float anywhere else.
//
// Nothing here runs unless a DS18B20 answers its presence pulse. That single
// check gates the whole feature: the two sensors are installed together, in the
// same water, and TDS without a temperature is uncompensated by ~2 %/degC, which
// over a seasonal swing is tens of percent of confidently wrong reading. No
// temperature therefore means no TDS either, deliberately.
#define WQ_PERIOD_MS     10000UL // TDS moves slowly; 10 s is generous
#define WQ_CONV_MS       760     // DS18B20 12-bit conversion, +10 ms margin. This
                                 // doubles as the TDS board's settling time, which
                                 // is why neither costs a blocking delay.
#define TDS_SAMPLES      8
#define TDS_ADC_MV       5000    // AVcc. Probe and ADC share the buck's 5 V rail,
                                 // so the reading is ratiometric and supply droop
                                 // largely cancels.
#define WQ_FAULT_TDS     0x01
#define WQ_FAULT_TEMP    0x02

// No tank calibration here on purpose. A node reports millimetres; the hub turns
// them into a percentage against calibration you can change over Wi-Fi. Tanks get
// re-calibrated, and climbing to a roof with a laptop to edit two numbers does not
// scale. See esp32_hub_test.ino and RS485_PROTOCOL.md 4.2.

// Fan policy lives on the HUB, where it can be changed from a phone instead of
// with a programmer on a ladder (CMD_SET_FAN_RELAY, RS485_PROTOCOL.md 4.4).
// What follows is the backstop for when the hub stops talking - deliberately
// wider and hotter than any sane hub setting, so it never fights hub policy and
// only acts when nothing else will. A battery room must keep ventilating when
// the bus dies.
#define BACKSTOP_ON_DECI_C   400
#define BACKSTOP_OFF_DECI_C  370
#define HUB_SILENT_MS    300000UL // no fan command for 5 min -> backstop takes over
#define FAULT_VENT_MS    30000UL  // SHT30 dead this long -> ventilate blind

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
bool     pressureFitted = false; // A3 jumpered: level comes from the 4-20 mA loop

uint16_t tdsMV       = 0;
int16_t  waterTempDeciC = 0;
uint8_t  wqStatus    = WQ_FAULT_TDS | WQ_FAULT_TEMP;  // nothing fitted until proven
bool     wqBusy      = false;
unsigned long wqLastMs = 0;
unsigned long wqStartedMs = 0;

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
unsigned long lastFanCommand = 0;   // 0 = the hub has never commanded the fan

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

// ---------------------------------------------------------------- pressure
// Pure, so docs/check_frame.py can compile it on the host and pin the endpoints.
// Returns what pingOnce() returns: millimetres, 0 = no usable reading.
uint16_t pressureMM(uint16_t counts) {
  uint16_t mV = (uint16_t)(((uint32_t)counts * PRESS_ADC_MV) / 1023UL);
  uint32_t uA = ((uint32_t)mV * 1000UL) / PRESS_SENSE_OHMS;

  // A 4-20 mA loop cannot legitimately read below 4 mA. That is the whole point
  // of the live zero: a cut cable, a dead loop supply or a failed sensor is
  // distinguishable from an empty tank, which a 0-5 V sensor never is.
  if (uA < PRESS_MIN_UA || uA > PRESS_MAX_UA) return 0;

  int32_t head = ((int32_t)uA - 4000L) * (int32_t)PRESS_RANGE_MM / 16000L;
  if (head < 0) head = 0;
  if (head > (int32_t)PRESS_RANGE_MM - 1) head = (int32_t)PRESS_RANGE_MM - 1;
  return (uint16_t)((int32_t)PRESS_RANGE_MM - head);   // distance-alike, see above
}

uint16_t readPressureOnce() {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < PRESS_SAMPLES; i++) sum += (uint32_t)analogRead(PIN_PRESS_SENSE);
  return pressureMM((uint16_t)(sum / PRESS_SAMPLES));
}

// ---------------------------------------------------------------- filtering
// One sample per cycle into a rolling window. A five-deep median rides out the
// single wild outlier these sensors throw; averaging would not.
void sampleTank() {
  rawMM = pressureFitted ? readPressureOnce() : pingOnce();
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

// ------------------------------------------------------------ water quality
// 1-Wire, bit-banged. No library, for the same reason readSHT30() hand-rolls its
// I2C: one device on a dedicated pin with SKIP ROM is the simplest case the bus
// has - no search, no multi-drop, no parasitic power - and every reply is CRC
// checked, so a timing bug shows up as a reported fault rather than a plausible
// wrong temperature.
//
// Interrupts are masked only inside each time slot (~15 us), never across a whole
// byte. At 9600 baud a SoftwareSerial bit is 104 us, so that jitter is harmless -
// and this only ever runs from measurePending, after a reply, which is where the
// 35 ms pulseIn already lives.

void owDriveLow() {
  digitalWrite(PIN_WATER_TEMP, LOW);   // clears the pullup too, when it goes INPUT
  pinMode(PIN_WATER_TEMP, OUTPUT);
}

void owRelease() {
  pinMode(PIN_WATER_TEMP, INPUT);      // external 4k7 pulls it back up
}

bool owReset() {
  owDriveLow();
  delayMicroseconds(480);
  owRelease();
  delayMicroseconds(70);
  bool present = (digitalRead(PIN_WATER_TEMP) == LOW);
  delayMicroseconds(410);
  return present;
}

void owWriteBit(uint8_t b) {
  noInterrupts();
  owDriveLow();
  delayMicroseconds(b ? 6 : 60);
  owRelease();
  interrupts();
  delayMicroseconds(b ? 64 : 10);
}

uint8_t owReadBit() {
  noInterrupts();
  owDriveLow();
  delayMicroseconds(3);
  owRelease();
  delayMicroseconds(10);
  uint8_t b = (uint8_t)digitalRead(PIN_WATER_TEMP);
  interrupts();
  delayMicroseconds(53);
  return b;
}

void owWrite(uint8_t v) {
  for (uint8_t i = 0; i < 8; i++) { owWriteBit(v & 0x01); v >>= 1; }
}

uint8_t owRead() {
  uint8_t v = 0;
  for (uint8_t i = 0; i < 8; i++) { v >>= 1; if (owReadBit()) v |= 0x80; }
  return v;
}

// Dallas CRC-8: the same polynomial as the SHT30's, reflected. Not the same
// function - do not be tempted to share sht30Crc8(), the bit order differs.
uint8_t owCrc8(const uint8_t *d, uint8_t n) {
  uint8_t crc = 0;
  while (n--) {
    uint8_t b = *d++;
    for (uint8_t i = 0; i < 8; i++) {
      uint8_t mix = (uint8_t)((crc ^ b) & 0x01);
      crc >>= 1;
      if (mix) crc ^= 0x8C;
      b >>= 1;
    }
  }
  return crc;
}

uint16_t readTdsMV() {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < TDS_SAMPLES; i++) sum += (uint32_t)analogRead(PIN_TDS_SENSE);
  return (uint16_t)(((sum / TDS_SAMPLES) * TDS_ADC_MV) / 1023UL);
}

// Two-stage and non-blocking. Stage one starts the DS18B20 conversion and powers
// the TDS board; stage two, a cycle later, collects both. The 750 ms the sensor
// needs is time this node was going to spend anyway.
void sampleWaterQuality() {
  unsigned long now = millis();

  if (!wqBusy) {
    if (wqLastMs != 0 && now - wqLastMs < WQ_PERIOD_MS) return;

    // Presence pulse first, so a node with nothing fitted - which is every node
    // today - does one 1 ms reset every 10 s and touches nothing else.
    if (!owReset()) {
      wqStatus = WQ_FAULT_TDS | WQ_FAULT_TEMP;
      tdsMV = 0;
      waterTempDeciC = 0;
      wqLastMs = now;
      return;
    }
    owWrite(0xCC);            // SKIP ROM - the only device on this pin
    owWrite(0x44);            // CONVERT T, and do NOT wait for it
    digitalWrite(PIN_TDS_POWER, HIGH);
    wqBusy = true;
    wqStartedMs = now;
    return;
  }

  if (now - wqStartedMs < WQ_CONV_MS) return;

  uint16_t mv = readTdsMV();              // read before removing power
  digitalWrite(PIN_TDS_POWER, LOW);
  wqBusy = false;
  wqLastMs = now;

  uint8_t sp[9];
  if (!owReset()) { wqStatus = WQ_FAULT_TDS | WQ_FAULT_TEMP; return; }
  owWrite(0xCC);
  owWrite(0xBE);                          // READ SCRATCHPAD
  for (uint8_t i = 0; i < 9; i++) sp[i] = owRead();

  if (owCrc8(sp, 8) != sp[8]) {
    // A corrupt scratchpad is a wiring or pullup problem, and it must not become
    // a temperature. Both flags again: see the coupling note above.
    wqStatus = WQ_FAULT_TDS | WQ_FAULT_TEMP;
    return;
  }

  int16_t raw = (int16_t)(((uint16_t)sp[1] << 8) | sp[0]);   // 1/16 degC, signed
  waterTempDeciC = (int16_t)(((int32_t)raw * 10) / 16);
  tdsMV = mv;

  // 85.0 C is the DS18B20's power-on scratchpad default. Reading exactly that
  // means the conversion never ran, not that the tank is boiling.
  wqStatus = (waterTempDeciC == 850) ? (WQ_FAULT_TDS | WQ_FAULT_TEMP) : 0;
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

// Fail-safe first, in both modes: no trustworthy temperature for long enough and
// the fan goes on regardless of who is in charge. Ventilating a battery room
// blind is the safe failure; leaving it sealed and hot is not. This can only
// ever turn the fan ON, so it cannot fight the hub into a dangerous state.
void ventOnSensorFault() {
  if (climateFault != 0 && !fanOn && millis() - lastGoodClimate > FAULT_VENT_MS) {
    setFan(true);
  }
}

// The hub owns the thresholds and commands the relay. If it goes quiet for five
// minutes the backstop below takes over - hotter and wider than any hub setting,
// so it is a floor under the policy rather than a competitor to it.
void updateFan() {
  ventOnSensorFault();

  bool hubTalking = (lastFanCommand != 0) && (millis() - lastFanCommand < HUB_SILENT_MS);
  if (hubTalking || climateFault != 0) return;

  if (!fanOn && tempDeciC >= BACKSTOP_ON_DECI_C)  setFan(true);
  if (fanOn  && tempDeciC <= BACKSTOP_OFF_DECI_C) setFan(false);
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
  if (isClimate) {
    sampleClimate();
  } else {
    sampleTank();
    sampleWaterQuality();
  }
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

    case CMD_READ_WQ: {
      if (isClimate) return;                 // tank personality only
      out[0] = (uint8_t)(tdsMV >> 8);            out[1] = (uint8_t)(tdsMV & 0xFF);
      out[2] = (uint8_t)(waterTempDeciC >> 8);   out[3] = (uint8_t)(waterTempDeciC & 0xFF);
      out[4] = wqStatus;
      out[5] = 0;                            // reserved
      sendFrame(cmd, out, 6);
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
      lastFanCommand = millis();   // hub is alive and in charge; backstop stands down
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
  pinMode(PIN_PRESS_FIT, INPUT_PULLUP);

  // TDS board de-energised before the pin becomes an output, so a reset does not
  // put a DC bias across the probe for however long the boot takes.
  digitalWrite(PIN_TDS_POWER, LOW);
  pinMode(PIN_TDS_POWER, OUTPUT);
  digitalWrite(PIN_TDS_POWER, LOW);
  pinMode(PIN_WATER_TEMP, INPUT);
  digitalWrite(PIN_US_TRIG, LOW);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Serial.begin(115200);
  while (!Serial && millis() < 2000);
  rs485.begin(9600);

  MY_NODE_ID = readNodeAddress();
  isClimate  = (MY_NODE_ID == 0x04);
  pressureFitted = !isClimate && (digitalRead(PIN_PRESS_FIT) == LOW);

  Serial.println(F("RO MONITOR - RS485 NODE"));
  Serial.print(F("Node ID: 0x0"));
  Serial.print(MY_NODE_ID, HEX);
  Serial.print(F("  fw 1.01  role: "));

  switch (MY_NODE_ID) {
    case 0x02:
      Serial.println(F("RWT tank (end of bus, fit 120R)"));
      break;
    case 0x03:
      Serial.println(F("TWT tank"));
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

  // Which sensor a tank node is actually reading is not something to infer from
  // the numbers later. Print it once, where it is read.
  if (!isClimate) {
    Serial.print(F("Level source: "));
    if (pressureFitted) {
      Serial.print(F("4-20 mA pressure transducer on A2, "));
      Serial.print(PRESS_RANGE_MM);
      Serial.println(F(" mm full scale"));
    } else {
      Serial.println(F("AJ-SR04M ultrasonic on D7/D8"));
    }
    Serial.print(F("Water quality: "));
    Serial.println(owReset() ? F("DS18B20 present, TDS enabled")
                             : F("no DS18B20 - TDS and water temp not reported"));
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
      Serial.print(pressureFitted ? F(" mm(P) q=") : F(" mm  q="));
      Serial.print(quality);
      if (wqStatus == 0) {
        Serial.print(F("  TDS "));
        Serial.print(tdsMV);
        Serial.print(F(" mV @ "));
        printDeci(waterTempDeciC);
        Serial.print(F(" C"));
      }
      Serial.println();
    }
  }
}
