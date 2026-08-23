/*
 * RO Monitor - Arduino Nano Tank Node Test Sketch
 * 
 * Hardware Setup:
 * - JSN-SR04T: TRIG -> Pin D7, ECHO -> Pin D8, VCC -> 5V, GND -> GND
 * - XY-485: RXD -> Pin D2, TXD -> Pin D3, VCC -> 5V, GND -> GND
 * - Status LED: Pin 13 (Onboard LED)
 * - Node Address: Default to Node 1 (0x01)
 */

#include <SoftwareSerial.h>

// Pin Definitions
#define PIN_RS485_RX    2   // Connects to XY-485 RXD
#define PIN_RS485_TX    3   // Connects to XY-485 TXD
#define PIN_US_TRIG     7   // JSN-SR04T Trigger Pin
#define PIN_US_ECHO     8   // JSN-SR04T Echo Pin
#define PIN_LED         13  // Onboard Activity LED

// Configuration
const uint8_t MY_NODE_ID = 0x01; // Default Test Node ID (Dosing / Tank 1)

// SoftwareSerial for RS485 (9600 baud)
SoftwareSerial rs485(PIN_RS485_RX, PIN_RS485_TX);

// Global Variables
unsigned long lastMeasureTime = 0;
uint16_t currentDistanceMM = 0;

// Function to measure distance using JSN-SR04T
uint16_t measureDistanceMM() {
  digitalWrite(PIN_US_TRIG, LOW);
  delayMicroseconds(4);
  digitalWrite(PIN_US_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_US_TRIG, LOW);

  // Measure echo pulse (35ms timeout corresponds to approx 6 meters max)
  unsigned long duration = pulseIn(PIN_US_ECHO, HIGH, 35000UL);

  if (duration == 0) {
    return 0; // Timeout / No Echo detected
  }

  // Speed of sound = ~343 m/s -> Distance in mm = (duration * 0.343) / 2
  // Simplified integer math: duration * 10 / 58
  uint16_t distanceMM = (uint16_t)((duration * 10UL) / 58UL);
  return distanceMM;
}

void setup() {
  // Initialize Hardware USB Serial for PC debugging
  Serial.begin(115200);
  while (!Serial && millis() < 2000); // Allow USB to connect

  // Initialize RS485 SoftwareSerial
  rs485.begin(9600);

  // Pin Configurations
  pinMode(PIN_US_TRIG, OUTPUT);
  pinMode(PIN_US_ECHO, INPUT);
  pinMode(PIN_LED, OUTPUT);

  digitalWrite(PIN_US_TRIG, LOW);
  digitalWrite(PIN_LED, LOW);

  Serial.println(F("========================================"));
  Serial.println(F("  RO MONITOR - NANO TANK NODE TEST     "));
  Serial.println(F("========================================"));
  Serial.print(F("Assigned Node ID: 0x0"));
  Serial.println(MY_NODE_ID, HEX);
  Serial.println(F("Listening for ESP32 Hub polls on RS485..."));
}

void loop() {
  // 1. Periodic ultrasonic distance measurement (every 250ms)
  if (millis() - lastMeasureTime >= 250) {
    lastMeasureTime = millis();
    currentDistanceMM = measureDistanceMM();

    Serial.print(F("[JSN-SR04T] Distance: "));
    if (currentDistanceMM > 0) {
      Serial.print(currentDistanceMM);
      Serial.print(F(" mm  ("));
      Serial.print(currentDistanceMM / 10.0, 1);
      Serial.println(F(" cm)"));
    } else {
      Serial.println(F("Out of Range / No Echo"));
    }
  }

  // 2. Check for incoming RS485 request from ESP32 Hub
  // Frame structure: [0xAA] [0x55] [NODE_ID] [CMD]
  if (rs485.available() >= 4) {
    if (rs485.read() == 0xAA) {
      if (rs485.peek() == 0x55) {
        rs485.read(); // Consume 0x55
        uint8_t targetNode = rs485.read();
        uint8_t command    = rs485.read();

        // Check if message is for this Node
        if (targetNode == MY_NODE_ID) {
          // Blink Onboard LED to indicate communication
          digitalWrite(PIN_LED, HIGH);

          Serial.println(F(">>> [RS485] Poll received from ESP32 Hub! Sending reply..."));

          // Prepare Response Frame:
          // [0xAA] [0x55] [NODE_ID] [CMD | 0x80] [DIST_HIGH] [DIST_LOW] [CHECKSUM]
          uint8_t distHigh = (currentDistanceMM >> 8) & 0xFF;
          uint8_t distLow  = currentDistanceMM & 0xFF;
          uint8_t checksum = MY_NODE_ID ^ (command | 0x80) ^ distHigh ^ distLow;

          rs485.write(0xAA);
          rs485.write(0x55);
          rs485.write(MY_NODE_ID);
          rs485.write((uint8_t)(command | 0x80));
          rs485.write(distHigh);
          rs485.write(distLow);
          rs485.write(checksum);
          rs485.flush();

          delay(20);
          digitalWrite(PIN_LED, LOW);
        }
      }
    }
  }
}
