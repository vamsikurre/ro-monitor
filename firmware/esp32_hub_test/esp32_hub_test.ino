/*
 * RO Monitor - ESP32 Central Hub Comprehensive Self-Test Sketch
 * 
 * Hardware Setup:
 * - SHT30 Temp/Humidity: SDA -> GPIO 21, SCL -> GPIO 22, VCC -> 3.3V, GND -> GND
 * - XY-485 Module: RXD -> GPIO 16, TXD -> GPIO 17, VCC -> 3.3V, GND -> GND
 * - 4-Ch Relay Module (Physical 1..4 Mapping):
 *     Relay 1 (TWT Float)  -> GPIO 27
 *     Relay 2 (RWT Float)  -> GPIO 23
 *     Relay 3 (Dos Level)  -> GPIO 18
 *     Relay 4 (Aux/Spare)  -> GPIO 19
 * - Opto DC Inputs: GPIO 32 (TWT Float), GPIO 26 (RL1), GPIO 25 (RL2)
 * - Opto AC Inputs: GPIO 34 (HPP AC), GPIO 35 (RWP AC) [Input-only with HW pullup]
 * - Onboard LED: GPIO 2
 */

#include <Wire.h>

// Pin Definitions
#define I2C_SDA          21
#define I2C_SCL          22
#define SHT30_I2C_ADDR   0x44

#define RS485_RX_PIN     16
#define RS485_TX_PIN     17

// Corrected Relay Pin Mapping
#define RELAY1_PIN       27   // Physical Relay 1 (TWT Float Emulation)
#define RELAY2_PIN       23   // Physical Relay 2 (RWT Float Emulation)
#define RELAY3_PIN       18   // Physical Relay 3 (Dosing Level Emulation)
#define RELAY4_PIN       19   // Physical Relay 4 (Auxiliary / Spare)

#define OPTO_TWT_FLOT    32
#define OPTO_RL1_STAT    26
#define OPTO_RL2_STAT    25
#define OPTO_HPP_AC      34
#define OPTO_RWP_AC      35

#define LED_STATUS       2

// SHT30 Read Function
bool readSHT30(float &temperature, float &humidity) {
  Wire.beginTransmission(SHT30_I2C_ADDR);
  Wire.write(0x2C);
  Wire.write(0x06);
  if (Wire.endTransmission() != 0) {
    return false;
  }

  delay(20);

  Wire.requestFrom((uint8_t)SHT30_I2C_ADDR, (uint8_t)6);
  if (Wire.available() == 6) {
    uint8_t data[6];
    for (int i = 0; i < 6; i++) {
      data[i] = Wire.read();
    }

    uint16_t rawTemp = (data[0] << 8) | data[1];
    temperature = -45.0 + (175.0 * ((float)rawTemp / 65535.0));

    uint16_t rawHum = (data[3] << 8) | data[4];
    humidity = 100.0 * ((float)rawHum / 65535.0);

    return true;
  }
  return false;
}

// Poll Arduino Nano Tank Node over RS485
bool pollNanoNode(uint8_t targetNodeId, uint16_t &distanceMM) {
  while (Serial2.available()) Serial2.read();

  Serial2.write(0xAA);
  Serial2.write(0x55);
  Serial2.write(targetNodeId);
  Serial2.write(0x02);
  Serial2.flush();

  unsigned long start = millis();
  while (millis() - start < 150) {
    if (Serial2.available() >= 7) {
      if (Serial2.read() == 0xAA && Serial2.read() == 0x55) {
        uint8_t node = Serial2.read();
        uint8_t cmd  = Serial2.read();
        uint8_t dH   = Serial2.read();
        uint8_t dL   = Serial2.read();
        uint8_t cs   = Serial2.read();

        uint8_t calculatedCS = node ^ cmd ^ dH ^ dL;
        if (node == targetNodeId && cs == calculatedCS) {
          distanceMM = ((uint16_t)dH << 8) | dL;
          return true;
        }
      }
    }
  }
  return false;
}

// Debounced AC Detection (filters out noise / transient glitches)
bool readACInputFiltered(int pin) {
  int lowCount = 0;
  for (int i = 0; i < 10; i++) {
    if (digitalRead(pin) == LOW) {
      lowCount++;
    }
    delayMicroseconds(500);
  }
  // If at least 8 out of 10 samples are LOW, declare solid AC detection
  return (lowCount >= 8);
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 1500);

  Wire.begin(I2C_SDA, I2C_SCL);
  Serial2.begin(9600, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);

  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(RELAY3_PIN, OUTPUT);
  pinMode(RELAY4_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, HIGH);
  digitalWrite(RELAY2_PIN, HIGH);
  digitalWrite(RELAY3_PIN, HIGH);
  digitalWrite(RELAY4_PIN, HIGH);

  pinMode(OPTO_TWT_FLOT, INPUT_PULLUP);
  pinMode(OPTO_RL1_STAT, INPUT_PULLUP);
  pinMode(OPTO_RL2_STAT, INPUT_PULLUP);
  pinMode(OPTO_HPP_AC,   INPUT);
  pinMode(OPTO_RWP_AC,   INPUT);

  pinMode(LED_STATUS, OUTPUT);
  digitalWrite(LED_STATUS, LOW);

  Serial.println(F("\n=================================================="));
  Serial.println(F("    RO MONITOR - ESP32 CENTRAL HUB SELF-TEST     "));
  Serial.println(F("=================================================="));
}

void loop() {
  digitalWrite(LED_STATUS, HIGH);
  Serial.println(F("\n--- [CYCLE DIAGNOSTICS REPORT] ---"));

  // 1. SHT30 Sensor
  float temp = 0.0, hum = 0.0;
  if (readSHT30(temp, hum)) {
    Serial.printf("[SHT30 Sensor]  Temp: %.1f C  |  Humidity: %.1f %%\n", temp, hum);
  } else {
    Serial.println(F("[SHT30 Sensor]  FAILED / NOT DETECTED"));
  }

  // 2. RS485 Nano Nodes (Poll 0x01 through 0x04)
  const char* nodeLabels[] = { "Dosing Tank", "Raw Water (RWT)", "Treated Water (TWT)", "Node 4 / Aux" };
  for (uint8_t nodeId = 1; nodeId <= 4; nodeId++) {
    uint16_t distanceMM = 0;
    if (pollNanoNode(nodeId, distanceMM)) {
      Serial.printf("[RS485 Node 0x%02X - %-19s] OK! Tank Dist: %4d mm (%.1f cm)\n", 
                    nodeId, nodeLabels[nodeId - 1], distanceMM, distanceMM / 10.0);
    } else {
      Serial.printf("[RS485 Node 0x%02X - %-19s] TIMEOUT / NO RESPONSE\n", 
                    nodeId, nodeLabels[nodeId - 1]);
    }
    delay(40); // Short gap between successive polls on the bus
  }

  // 3. Optocoupler Status Readings (Filtered for AC inputs)
  bool twtClosed = (digitalRead(OPTO_TWT_FLOT) == LOW);
  bool rl1Active = (digitalRead(OPTO_RL1_STAT) == LOW);
  bool rl2Active = (digitalRead(OPTO_RL2_STAT) == LOW);
  bool hppActive = readACInputFiltered(OPTO_HPP_AC);
  bool rwpActive = readACInputFiltered(OPTO_RWP_AC);

  Serial.printf("[Opto Inputs]   TWT Float: %s | RL1: %s | RL2: %s | 240V HPP: %s | 240V RWP: %s\n",
                twtClosed ? "CLOSED" : "OPEN",
                rl1Active ? "ACTIVE" : "INACTIVE",
                rl2Active ? "ACTIVE" : "INACTIVE",
                hppActive ? "240V ON" : "OFF",
                rwpActive ? "240V ON" : "OFF");

  // 4. Relay Sequential Cycling
  Serial.println(F("[Relay Test]    Cycling Relay 1 -> 2 -> 3 -> 4..."));
  
  digitalWrite(RELAY1_PIN, LOW);
  delay(300);
  digitalWrite(RELAY1_PIN, HIGH);

  digitalWrite(RELAY2_PIN, LOW);
  delay(300);
  digitalWrite(RELAY2_PIN, HIGH);

  digitalWrite(RELAY3_PIN, LOW);
  delay(300);
  digitalWrite(RELAY3_PIN, HIGH);

  digitalWrite(RELAY4_PIN, LOW);
  delay(300);
  digitalWrite(RELAY4_PIN, HIGH);

  digitalWrite(LED_STATUS, LOW);
  delay(2000);
}
