/*
 * RO Monitor - Battery Room Climate Node (0x04) Test Sketch
 *
 * Board: Arduino Pro Mini, ATmega328P 5V / 16 MHz  (WIRING.md 10.1 - the 3.3V/8MHz
 *        variant starves the XY-485 and halves the SoftwareSerial timing margin).
 *
 * Hardware:
 *  - GY-SHT30-D   SDA -> A4, SCL -> A5   (inner pads, not the edge header)
 *  - XY-485       RXD -> D2, TXD -> D3   (auto-direction, no DE/RE pin)
 *  - 1-ch relay   IN  -> D9, ACTIVE LOW, COM/NO -> exhaust fan AC
 *  - Address      A0 and A1 both left OPEN -> 0x04 (WIRING.md 9.1)
 *
 * Power: +5V from the buck into VCC. Never RAW - WIRING.md 10.1 explains why.
 * Mid-chain node: no 120 ohm terminator here.
 *
 * The fan runs on a local thermostat, not on hub command: if the bus dies, the
 * battery room still ventilates. CMD_SET_FAN_RELAY (0x07) is not implemented yet.
 */

#include <SoftwareSerial.h>
#include <Wire.h>

// Pin Definitions
#define PIN_RS485_RX    2   // Connects to XY-485 RXD
#define PIN_RS485_TX    3   // Connects to XY-485 TXD
#define PIN_FAN_RELAY   9   // 1-Ch relay IN, ACTIVE LOW
#define PIN_LED         13  // Onboard Activity LED
#define PIN_ADDR_0      A0  // Hardware Address Bit 0
#define PIN_ADDR_1      A1  // Hardware Address Bit 1

#define SHT30_I2C_ADDR   0x44
#define CMD_READ_CLIMATE 0x06   // RS485_PROTOCOL.md 4.3

// Thermostat, in tenths of a degree C. Tune on site: 38.0 C is the threshold the
// dashboard documents, 2.0 C of hysteresis keeps the contactor from chattering
// around it. FAN_OFF must stay below FAN_ON or the fan never stops.
#define FAN_ON_DECI_C    380
#define FAN_OFF_DECI_C   360
#define FAULT_VENT_MS    30000UL  // sensor dead this long -> ventilate anyway

// Dynamic Node Address (Determined at boot via A0/A1; 0x00 = unassigned, stay off the bus)
uint8_t MY_NODE_ID = 0x00;

// SoftwareSerial for RS485 (9600 baud)
SoftwareSerial rs485(PIN_RS485_RX, PIN_RS485_TX);

// Read Node Address from A0 & A1 (Internal Pullup: Open = 1, Jumper to GND = 0)
// ADDR_MAP is written to fit the boards as they are already jumpered.
// WIRING.md section 9.1 is the authority; if a board is ever re-jumpered,
// that table and this one change in the same commit. docs/check_addrmap.py enforces it.
static const uint8_t ADDR_MAP[4] = {
  0x00,   // 0b00  both GND    -> unassigned, do not join the bus
  0x03,   // 0b01  A1 to GND   -> TWT
  0x02,   // 0b10  A0 to GND   -> RWT (end of bus)
  0x04,   // 0b11  both open   -> Battery Room (climate + fan relay, this sketch)
};

uint8_t readNodeAddress() {
  pinMode(PIN_ADDR_0, INPUT_PULLUP);
  pinMode(PIN_ADDR_1, INPUT_PULLUP);
  delay(10); // Settle pullups

  uint8_t raw = ((digitalRead(PIN_ADDR_1) == HIGH) << 1) | (digitalRead(PIN_ADDR_0) == HIGH);
  return ADDR_MAP[raw];
}

// Global State
int16_t  tempDeciC   = 0;      // tenths of degC
uint16_t humDeciPct  = 0;      // tenths of %RH
uint8_t  faultCode   = 1;      // 0 = OK, 1 = SHT30 error (RS485_PROTOCOL.md 4.3)
bool     fanOn       = false;
unsigned long lastMeasureTime = 0;
unsigned long lastGoodRead    = 0;

// SHT30 sends a CRC-8 after each 16-bit word. An I2C run soldered to inner pads in
// the hottest room in the building is exactly where a corrupt reading turns up, and
// a corrupt reading here switches a fan and raises a dashboard alarm.
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

  if (sht30Crc8(d[0], d[1]) != d[2]) return false;
  if (sht30Crc8(d[3], d[4]) != d[5]) return false;

  uint16_t rawT = ((uint16_t)d[0] << 8) | d[1];
  uint16_t rawH = ((uint16_t)d[3] << 8) | d[4];

  // Datasheet: T = -45 + 175 * raw / 65535, RH = 100 * raw / 65535, both in tenths
  // here. Integer math only - no float on an ATmega for two one-decimal values.
  tempDeciC  = (int16_t)(-450 + (int16_t)(((int32_t)1750 * rawT) / 65535L));
  humDeciPct = (uint16_t)(((uint32_t)1000 * rawH) / 65535UL);
  return true;
}

void setFan(bool on) {
  fanOn = on;
  digitalWrite(PIN_FAN_RELAY, on ? LOW : HIGH); // relay board is ACTIVE LOW
}

void updateFan() {
  if (faultCode != 0) {
    // No trustworthy temperature. Ventilating a battery room blind is the safe
    // failure; leaving it sealed and hot is not.
    if (!fanOn && millis() - lastGoodRead > FAULT_VENT_MS) {
      Serial.println(F("!! SHT30 down > 30 s - ventilating on fail-safe"));
      setFan(true);
    }
    return;
  }
  if (!fanOn && tempDeciC >= FAN_ON_DECI_C)  setFan(true);
  if (fanOn  && tempDeciC <= FAN_OFF_DECI_C) setFan(false);
}

void printDeci(int16_t deci) {
  Serial.print(deci / 10);
  Serial.print('.');
  Serial.print(abs(deci) % 10);
}

void setup() {
  // Fan OFF before the pin becomes an output, or the relay clicks on every reset.
  digitalWrite(PIN_FAN_RELAY, HIGH);
  pinMode(PIN_FAN_RELAY, OUTPUT);
  digitalWrite(PIN_FAN_RELAY, HIGH);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Serial.begin(115200);
  while (!Serial && millis() < 2000); // Allow USB to connect

  Wire.begin();
  rs485.begin(9600);

  // Read Address from Jumpers (A0 / A1)
  MY_NODE_ID = readNodeAddress();

  Serial.println(F("========================================"));
  Serial.println(F("  RO MONITOR - BATTERY ROOM NODE TEST   "));
  Serial.println(F("========================================"));
  Serial.print(F("Auto-Detected Node ID: 0x0"));
  Serial.println(MY_NODE_ID, HEX);

  // This sketch is the climate personality. Flashed onto a jumpered tank Nano it
  // would report battery room data under a tank address - refuse rather than lie.
  if (MY_NODE_ID != 0x04) {
    Serial.println(F("!! Jumpers do not decode to 0x04. Wrong board or wrong sketch."));
    Serial.println(F("!! Battery Room = both A0 and A1 OPEN. Holding off the bus."));
    for (;;) { digitalWrite(PIN_LED, !digitalRead(PIN_LED)); delay(200); }
  }

  Serial.print(F("Fan ON at "));   printDeci(FAN_ON_DECI_C);
  Serial.print(F(" C, OFF at "));  printDeci(FAN_OFF_DECI_C);
  Serial.println(F(" C (local thermostat, hub does not command it)"));
  Serial.println(F("Listening for ESP32 Hub climate polls on RS485..."));
}

void loop() {
  // 1. Read climate every 2 s (the SHT30 self-heats if hammered), run the thermostat
  if (millis() - lastMeasureTime >= 2000) {
    lastMeasureTime = millis();

    if (readSHT30()) {
      faultCode = 0;
      lastGoodRead = millis();
    } else {
      faultCode = 1;
    }
    updateFan();

    Serial.print(F("[SHT30] "));
    if (faultCode == 0) {
      printDeci(tempDeciC);
      Serial.print(F(" C  "));
      printDeci((int16_t)humDeciPct);
      Serial.print(F(" %RH"));
    } else {
      Serial.print(F("FAILED / NOT DETECTED"));
    }
    Serial.print(F("  |  Fan: "));
    Serial.println(fanOn ? F("ON") : F("OFF"));
  }

  // 2. Check for incoming RS485 request from ESP32 Hub
  // Frame structure: [0xAA] [0x55] [NODE_ID] [CMD]
  if (rs485.available() >= 4) {
    if (rs485.read() == 0xAA) {
      if (rs485.peek() == 0x55) {
        rs485.read(); // Consume 0x55
        uint8_t targetNode = rs485.read();
        uint8_t command    = rs485.read();

        if (targetNode == MY_NODE_ID && command == CMD_READ_CLIMATE) {
          digitalWrite(PIN_LED, HIGH);

          Serial.println(F(">>> [RS485] Climate poll received! Sending reply..."));

          // Response: [0xAA][0x55][ID][CMD|0x80][T_HI][T_LO][RH_HI][RH_LO][FAN][FAULT][CS]
          uint8_t payload[6] = {
            (uint8_t)((tempDeciC >> 8) & 0xFF), (uint8_t)(tempDeciC & 0xFF),
            (uint8_t)((humDeciPct >> 8) & 0xFF), (uint8_t)(humDeciPct & 0xFF),
            (uint8_t)(fanOn ? 1 : 0), faultCode
          };
          uint8_t checksum = MY_NODE_ID ^ (uint8_t)(command | 0x80);
          for (uint8_t i = 0; i < 6; i++) checksum ^= payload[i];

          rs485.write(0xAA);
          rs485.write(0x55);
          rs485.write(MY_NODE_ID);
          rs485.write((uint8_t)(command | 0x80));
          rs485.write(payload, 6);
          rs485.write(checksum);

          delay(5); // Guard time before releasing the bus
          digitalWrite(PIN_LED, LOW);
        }
      }
    }
  }
}
