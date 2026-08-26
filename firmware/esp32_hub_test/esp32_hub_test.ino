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
 *
 * Tank calibration lives HERE, not in the nodes. The hub knows every distance in
 * the plant - the two tank nodes report millimetres, the dosing sensor is wired
 * direct - so it is the one place that can turn them into percentages, and the one
 * place you can reach without a USB cable. Join the hub's Wi-Fi AP and open
 * http://192.168.4.1/ to set full/empty per tank; values persist in NVS.
 */

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

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

// Dosing tank AJ-SR04M, wired straight to the hub (WIRING.md 13). ECHO is 5V logic
// and reaches GPIO 4 through a 1k/2k divider - the ESP32 pads are not 5V tolerant.
#define US_TRIG_DOS      5
#define US_ECHO_DOS      4
// Tuning knob, not a magic number: some AJ-SR04M batches are unreliable at the
// nominal 10 us and want 20 us (WIRING.md 9.0). Widen this before suspecting the head.
#define US_TRIG_WIDTH_US 10

#define DOS_LOW_PCT      20    // dashboard raises "Dosing Chemical Low" here
#define BLIND_ZONE_MM    200   // AJ-SR04M sees nothing closer than this

// Calibration AP. Open network would let anyone on the roof change tank scaling.
#define CAL_AP_SSID      "RO-HUB"
#define CAL_AP_PASSWORD  "ro-monitor"   // >= 8 chars, change it

// Per-tank calibration. These are first-boot defaults only: whatever you set over
// Wi-Fi is stored in NVS and wins from then on. Both values are transducer face to
// liquid surface in mm - FULL at the working full mark, EMPTY at the tank floor.
struct TankCal {
  const char *key;      // NVS key prefix and the ?tank= name
  const char *label;
  uint16_t fullMM;
  uint16_t emptyMM;
  uint16_t lastMM;      // last distance seen, so "set full=now" can capture it
};

TankCal tanks[] = {
  { "rwt", "Raw Water",     300, 1500, 0 },
  { "twt", "Treated Water", 300, 1500, 0 },
  { "dos", "Dosing",        250,  900, 0 },
};
#define TANK_RWT 0
#define TANK_TWT 1
#define TANK_DOS 2
#define TANK_COUNT (sizeof(tanks) / sizeof(tanks[0]))

Preferences prefs;
WebServer server(80);

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

// Dosing tank distance, read locally - no RS485 node involved (WIRING.md 13).
// Returns 0 on timeout / no echo. Blocks up to 35 ms, so call it AFTER the last
// slave response in the cycle, never between a poll and its reply.
uint16_t measureDosingMM() {
  digitalWrite(US_TRIG_DOS, LOW);
  delayMicroseconds(4);
  digitalWrite(US_TRIG_DOS, HIGH);
  delayMicroseconds(US_TRIG_WIDTH_US);
  digitalWrite(US_TRIG_DOS, LOW);

  unsigned long duration = pulseIn(US_ECHO_DOS, HIGH, 35000UL); // ~6 m of range
  if (duration == 0) return 0;

  return (uint16_t)((duration * 10UL) / 58UL); // mm, speed of sound ~343 m/s
}

// Distance -> level %. Far reading = empty tank, near reading = full one.
// Returns 255 for "invalid", the sentinel RS485_PROTOCOL.md 4.2 defines.
// The nodes deliberately do NOT do this: they measure, the hub scales. Tanks get
// re-calibrated; reflashing a board on a roof to change two numbers does not scale.
uint8_t levelPercent(uint16_t distanceMM, uint16_t fullMM, uint16_t emptyMM) {
  if (distanceMM == 0) return 255;                 // no echo
  if (emptyMM <= fullMM) return 255;               // calibration not set, or inverted
  if (distanceMM < fullMM) return 255;             // blind zone, or over-full
  if (distanceMM >= emptyMM) return 0;

  long span = (long)emptyMM - (long)fullMM;
  return (uint8_t)(((long)emptyMM - (long)distanceMM) * 100L / span);
}

// ------------------------------------------------ calibration store + web
void loadCal() {
  prefs.begin("rocal", true);
  for (uint8_t i = 0; i < TANK_COUNT; i++) {
    char k[12];
    snprintf(k, sizeof(k), "%s_f", tanks[i].key);
    tanks[i].fullMM = prefs.getUShort(k, tanks[i].fullMM);
    snprintf(k, sizeof(k), "%s_e", tanks[i].key);
    tanks[i].emptyMM = prefs.getUShort(k, tanks[i].emptyMM);
  }
  prefs.end();
}

void saveCal(uint8_t i) {
  char k[12];
  prefs.begin("rocal", false);
  snprintf(k, sizeof(k), "%s_f", tanks[i].key);
  prefs.putUShort(k, tanks[i].fullMM);
  snprintf(k, sizeof(k), "%s_e", tanks[i].key);
  prefs.putUShort(k, tanks[i].emptyMM);
  prefs.end();
}

int tankIndex(const String &key) {
  for (uint8_t i = 0; i < TANK_COUNT; i++) {
    if (key == tanks[i].key) return i;
  }
  return -1;
}

void handleCalJson() {
  String j = "{";
  for (uint8_t i = 0; i < TANK_COUNT; i++) {
    if (i) j += ",";
    j += "\"" + String(tanks[i].key) + "\":{"
         "\"full_mm\":"  + String(tanks[i].fullMM) +
         ",\"empty_mm\":" + String(tanks[i].emptyMM) +
         ",\"last_mm\":"  + String(tanks[i].lastMM) +
         ",\"level_pct\":" +
         String(levelPercent(tanks[i].lastMM, tanks[i].fullMM, tanks[i].emptyMM)) + "}";
  }
  j += "}";
  server.send(200, "application/json", j);
}

// full/empty accept a number in mm, or "now" to capture what the sensor reads this
// second - fill the tank, tap Set full. That is how these actually get calibrated.
void handleCalSet() {
  int i = tankIndex(server.arg("tank"));
  if (i < 0) { server.send(400, "text/plain", "unknown tank"); return; }

  uint16_t full = tanks[i].fullMM, empty = tanks[i].emptyMM;
  if (server.hasArg("full"))  full  = (server.arg("full") == "now")  ? tanks[i].lastMM : (uint16_t)server.arg("full").toInt();
  if (server.hasArg("empty")) empty = (server.arg("empty") == "now") ? tanks[i].lastMM : (uint16_t)server.arg("empty").toInt();

  if (full == 0 || empty == 0) { server.send(400, "text/plain", "no reading to capture"); return; }
  if (empty <= full) { server.send(400, "text/plain", "empty must be a longer distance than full"); return; }
  if (full < BLIND_ZONE_MM) { server.send(400, "text/plain", "full is inside the sensor blind zone"); return; }

  tanks[i].fullMM = full;
  tanks[i].emptyMM = empty;
  saveCal((uint8_t)i);
  Serial.printf("[Cal] %s: full %d mm, empty %d mm (saved)\n", tanks[i].key, full, empty);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleRoot() {
  String h = "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
             "<h2>RO Hub - tank calibration</h2>"
             "<p>Distances are transducer face to liquid surface. Fill or empty the "
             "tank, then use <b>now</b> to capture the live reading.</p>";

  for (uint8_t i = 0; i < TANK_COUNT; i++) {
    uint8_t pct = levelPercent(tanks[i].lastMM, tanks[i].fullMM, tanks[i].emptyMM);
    String key = String(tanks[i].key);

    h += "<h3>" + String(tanks[i].label) + "</h3>";
    h += "<p>now " + String(tanks[i].lastMM) + " mm &rarr; " +
         (pct == 255 ? String("--") : String(pct)) + " %</p>";
    h += "<form action='/api/cal/set'><input type=hidden name=tank value='" + key + "'>"
         "full <input name=full size=6 value='" + String(tanks[i].fullMM) + "'> "
         "empty <input name=empty size=6 value='" + String(tanks[i].emptyMM) + "'> "
         "<button>Save</button></form>";
    h += "<form action='/api/cal/set'><input type=hidden name=tank value='" + key + "'>"
         "<button name=full value=now>Set full = now</button> "
         "<button name=empty value=now>Set empty = now</button></form>";
  }
  h += "<p><a href='/api/cal'>JSON</a></p>";
  server.send(200, "text/html", h);
}

// The web server needs servicing during the long waits in the cycle, or the page
// hangs for seconds at a time while the relays click.
void idle(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    server.handleClient();
    delay(5);
  }
}

// ------------------------------------------------ RS485 master (production frame)
// 0xAA 0x55 ADDR CMD LEN payload CRC_L CRC_H, CRC-16/Modbus over every byte before
// the CRC, payload fields big-endian. Same framing the nodes run (firmware/ro_node).
#define PREAMBLE_1        0xAA
#define PREAMBLE_2        0x55
#define RESPONSE_BIT      0x80
#define CMD_PING          0x01
#define CMD_READ_LEVEL    0x02
#define CMD_READ_CLIMATE  0x06
#define CMD_SET_FAN_RELAY 0x07
#define MAX_PAYLOAD       32
#define REPLY_TIMEOUT_MS  100   // RS485_PROTOCOL.md 7.1
#define POLL_ATTEMPTS     3     // RS485_PROTOCOL.md 7.2: one try plus two retries

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

// Send one request, wait for the matching reply. Returns payload length, or -1 if
// the node never answered a readable frame in POLL_ATTEMPTS tries.
int pollNode(uint8_t addr, uint8_t cmd, const uint8_t *req, uint8_t reqLen, uint8_t *out) {
  uint8_t frame[7 + MAX_PAYLOAD];
  frame[0] = PREAMBLE_1;
  frame[1] = PREAMBLE_2;
  frame[2] = addr;
  frame[3] = cmd;
  frame[4] = reqLen;
  for (uint8_t i = 0; i < reqLen; i++) frame[5 + i] = req[i];
  uint16_t reqCrc = crc16(frame, (uint8_t)(5 + reqLen));
  frame[5 + reqLen] = (uint8_t)(reqCrc & 0xFF);
  frame[6 + reqLen] = (uint8_t)(reqCrc >> 8);

  for (uint8_t attempt = 0; attempt < POLL_ATTEMPTS; attempt++) {
    while (Serial2.available()) Serial2.read();
    Serial2.write(frame, (size_t)(7 + reqLen));
    Serial2.flush();

    uint8_t buf[7 + MAX_PAYLOAD];
    uint8_t n = 0;
    unsigned long start = millis();
    while (millis() - start < REPLY_TIMEOUT_MS) {
      if (!Serial2.available()) continue;
      uint8_t b = (uint8_t)Serial2.read();

      if (n == 0 && b != PREAMBLE_1) continue;            // hunt for sync
      if (n == 1 && b != PREAMBLE_2) { n = 0; continue; }
      if (n == 4 && b > MAX_PAYLOAD) { n = 0; continue; }
      buf[n++] = b;

      if (n >= 5) {
        uint8_t len = buf[4];
        if (n == (uint8_t)(7 + len)) {
          uint16_t want = (uint16_t)buf[5 + len] | ((uint16_t)buf[6 + len] << 8);
          n = 0;
          if (crc16(buf, (uint8_t)(5 + len)) != want) continue;          // corrupt
          if (buf[2] != addr) continue;                                  // wrong node
          if (buf[3] != (uint8_t)(cmd | RESPONSE_BIT)) continue;         // wrong reply
          for (uint8_t i = 0; i < len; i++) out[i] = buf[5 + i];
          return (int)len;
        }
      }
    }
  }
  return -1;
}

const char *sensorStatusText(uint8_t status) {
  switch (status) {
    case 0:  return "OK";
    case 1:  return "BLIND ZONE";
    case 2:  return "ECHO TIMEOUT";
    default: return "HW FAULT";
  }
}

// One tank node: median distance, raw, level, echo quality, status, uptime.
void reportTankNode(uint8_t addr, const char *label) {
  uint8_t p[MAX_PAYLOAD];
  int len = pollNode(addr, CMD_READ_LEVEL, NULL, 0, p);
  if (len != 10) {
    Serial.printf("[Node 0x%02X - %-13s] %s\n", addr, label,
                  len < 0 ? "TIMEOUT / NO RESPONSE" : "BAD PAYLOAD LENGTH");
    return;
  }

  uint16_t median = ((uint16_t)p[0] << 8) | p[1];
  uint16_t raw    = ((uint16_t)p[2] << 8) | p[3];
  uint16_t uptime = ((uint16_t)p[8] << 8) | p[9];

  Serial.printf("[Node 0x%02X - %-13s] ", addr, label);
  if (p[4] == 255) Serial.print("--- %");
  else             Serial.printf("%3d %%", p[4]);
  Serial.printf("  %4d mm (raw %4d)  q=%3d %%  %-12s  up %u s\n",
                median, raw, p[5], sensorStatusText(p[6]), uptime);
}

// Battery room. The fan is that node's own decision - the hub only reports it.
void reportClimateNode() {
  uint8_t p[MAX_PAYLOAD];
  int len = pollNode(0x04, CMD_READ_CLIMATE, NULL, 0, p);
  if (len != 6) {
    Serial.printf("[Node 0x04 - %-13s] %s\n", "Battery Room",
                  len < 0 ? "TIMEOUT / NO RESPONSE" : "BAD PAYLOAD LENGTH");
    return;
  }

  int16_t t = (int16_t)(((uint16_t)p[0] << 8) | p[1]);
  uint16_t rh = ((uint16_t)p[2] << 8) | p[3];
  Serial.printf("[Node 0x04 - %-13s] %.1f C  %.1f %%RH  Fan: %-3s%s\n", "Battery Room", t / 10.0, rh / 10.0, p[4] ? "ON" : "OFF",
                p[5] ? "  (SHT30 FAULT)" : "");
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

  pinMode(US_TRIG_DOS, OUTPUT);
  pinMode(US_ECHO_DOS, INPUT);
  digitalWrite(US_TRIG_DOS, LOW);

  pinMode(LED_STATUS, OUTPUT);
  digitalWrite(LED_STATUS, LOW);

  loadCal();

  // AP rather than joining a network: the roof has no Wi-Fi worth relying on, and
  // calibration must work standing next to the tank with a phone.
  WiFi.mode(WIFI_AP);
  WiFi.softAP(CAL_AP_SSID, CAL_AP_PASSWORD);
  // Wi-Fi transmit peaks are what brown out a hub fed through a thin USB cable:
  // the radio pulls a few hundred mA in bursts the AMS1117 on a dev board cannot
  // follow. 11 dBm still covers a phone standing at the panel. If the board still
  // resets with "Brownout detector was triggered", the supply is the fault - fix
  // that rather than disabling the detector, which just moves the crash later.
  WiFi.setTxPower(WIFI_POWER_11dBm);
  server.on("/", handleRoot);
  server.on("/api/cal", handleCalJson);
  server.on("/api/cal/set", handleCalSet);
  server.begin();

  Serial.println(F("\n=================================================="));
  Serial.println(F("    RO MONITOR - ESP32 CENTRAL HUB SELF-TEST     "));
  Serial.println(F("=================================================="));
  Serial.printf("Calibration AP: %s  ->  http://%s/\n",
                CAL_AP_SSID, WiFi.softAPIP().toString().c_str());
  for (uint8_t i = 0; i < TANK_COUNT; i++) {
    Serial.printf("  %-14s full %4d mm  empty %4d mm\n",
                  tanks[i].label, tanks[i].fullMM, tanks[i].emptyMM);
  }
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

  // 2. RS485 nodes, polled by address (RS485_PROTOCOL.md 1.2) - not in cable order.
  //    0x01 is retired: the dosing sensor is wired straight to the hub.
  reportTankNode(0x02, TANK_RWT);
  idle(40);                        // short gap between polls, web stays alive
  reportTankNode(0x03, TANK_TWT);
  idle(40);
  reportClimateNode();
  idle(40);

  // 2b. Dosing tank, read here because the bus is now idle for the rest of the cycle
  uint16_t dosingMM = measureDosingMM();
  tanks[TANK_DOS].lastMM = dosingMM;
  uint8_t dosingPct = levelPercent(dosingMM, tanks[TANK_DOS].fullMM, tanks[TANK_DOS].emptyMM);
  if (dosingMM == 0) {
    Serial.println(F("[Direct    - Dosing Tank  ] NO ECHO (wiring, power, or blind zone)"));
  } else if (dosingPct == 255) {
    Serial.printf("[Direct    - Dosing Tank  ] %4d mm - OUT OF RANGE, calibrate at http://%s/\n",
                  dosingMM, WiFi.softAPIP().toString().c_str());
  } else {
    Serial.printf("[Direct    - Dosing Tank  ] %3d %%  %4d mm (%.1f cm)%s\n",
                  dosingPct, dosingMM, dosingMM / 10.0,
                  dosingPct < DOS_LOW_PCT ? "  << LOW, REPLENISH" : "");
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
  idle(300);
  digitalWrite(RELAY1_PIN, HIGH);

  digitalWrite(RELAY2_PIN, LOW);
  idle(300);
  digitalWrite(RELAY2_PIN, HIGH);

  digitalWrite(RELAY3_PIN, LOW);
  idle(300);
  digitalWrite(RELAY3_PIN, HIGH);

  digitalWrite(RELAY4_PIN, LOW);
  idle(300);
  digitalWrite(RELAY4_PIN, HIGH);

  digitalWrite(LED_STATUS, LOW);
  idle(2000);
}
