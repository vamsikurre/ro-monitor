# Local Web Dashboard & ESP RainMaker Integration Guide

**Document Version:** 1.0  
**Date:** 2026-08-24  
**Scope:** Local Network Web UI + AWS IoT-backed ESP RainMaker Cloud & Mobile Application

---

## 1. System Telemetry & Process Flow

The system telemetry directly visualizes the complete fluid balance and environmental status of the plant:

```
[Borewell Pump] ──> [Ground Sump (3.5m)] ──(Sump Motor)──> [RWT (Roof)] ──> [RO Plant Skid: RWP + Dosing + HPP] ──> [TWT (Roof)]
                                                                               │
                                                                               └── [Battery Room: SHT30 + Exhaust Fan]
```

---

## 2. Local Web Dashboard (Local Network Access)

The ESP32 Central Hub hosts a zero-dependency, ultra-responsive HTML5/CSS/JavaScript web server accessible by any PC, tablet, or smartphone connected to the local Wi-Fi router.

### 2.1. Access Endpoints
* **mDNS URL:** `http://ro-hub.local`
* **Direct IP URL:** `http://<ESP32_HUB_IP>` (e.g. `http://192.168.1.150`)
* **REST API Polling Endpoint:** `GET /api/telemetry` (Returns complete JSON state every 1000ms)
* **Control Endpoints:**
  * `POST /api/fan/toggle` (Overrides Battery Room Exhaust Fan `ON` / `OFF` / `AUTO`)
  * `POST /api/interlock/override` (Manual override for Aster float emulation relays)

### 2.2. Visual Component Layout & Process Flow Mirroring
1. **Borewell Pump Card:** Shows 240V AC power status (`RUNNING` / `IDLE`), run duration counter, and emergency cutoff toggle.
2. **Ground Floor Sump Tank (3.5m Depth):** Animated liquid SVG cylinder with percentage indicator, raw millimeter depth, and low-level warning indicator.
3. **Sump Motor Card:** Shows 240V AC status when pumping water from ground floor to roof top RWT.
4. **Raw Water Tank (RWT - Roof Top):** Real-time percentage, capacity volume (liters), and float status indicator.
5. **RO Plant Skid (Dashed Enclosure):**
   * **Raw Water Pump (RWP):** 240V status indicator.
   * **Chemical Dosing Tank:** Level gauge with low-reagent warning.
   * **High Pressure Pump (HPP):** 240V contactor active indicator.
   * **Astero Controller Indicators:** Real-time LED status for `TWT FLOTY`, `RL1`, `RL2`, `ALARM`, and RO Room Temperature/Humidity.
6. **Treated Water Tank (TWT - Roof Top):** Real-time percentage and full-tank cutoff indicator.
7. **Battery Room Climate & Ventilation Zone:**
   * Temperature & Relative Humidity gauge from Node `0x04`.
   * Animated exhaust fan icon showing rotation when active.
   * Auto-ventilation toggle (turns ON when Temp > 38°C or RH > 75%).

---

## 3. ESP RainMaker Cloud & Mobile Application Integration

ESP RainMaker provides AWS IoT cloud synchronization, remote out-of-home telemetry, and instant iOS/Android mobile alerts with **zero cloud subscription fees**.

### 3.1. ESP RainMaker Node Hierarchy

```
ESP32 Central Hub (RainMaker Node: "RO Plant & Sump Monitor")
├── Device 1: "Ground Sump" [Type: Water Tank Sensor]
│   ├── Param: level_percent (Integer, Read-Only, 0..100 %)
│   ├── Param: water_depth_cm (Integer, Read-Only, 0..350 cm)
│   └── Param: low_level_alert (Boolean, Read-Only)
├── Device 2: "Rooftop Tanks" [Type: Multi-Tank Sensor]
│   ├── Param: rwt_level_percent (Integer, Read-Only, 0..100 %)
│   ├── Param: twt_level_percent (Integer, Read-Only, 0..100 %)
│   └── Param: dosing_level_percent (Integer, Read-Only, 0..100 %)
├── Device 3: "Pumps & Motors" [Type: Motor Controller]
│   ├── Param: borewell_motor (Boolean, Read-Only, ON/OFF)
│   ├── Param: sump_motor (Boolean, Read-Only, ON/OFF)
│   ├── Param: rwp_pump (Boolean, Read-Only, ON/OFF)
│   ├── Param: hpp_pump (Boolean, Read-Only, ON/OFF)
│   └── Param: dry_run_cutoff (Boolean, Read-Write)
├── Device 4: "Environment & Climate" [Type: Climate Monitor]
│   ├── Param: ro_room_temp (Float, Read-Only, °C)
│   ├── Param: ro_room_humidity (Float, Read-Only, %)
│   ├── Param: battery_room_temp (Float, Read-Only, °C)
│   └── Param: battery_room_humidity (Float, Read-Only, %)
└── Device 5: "Ventilation" [Type: Fan Controller]
    ├── Param: exhaust_fan_power (Boolean, Read-Write, ON/OFF)
    └── Param: auto_temp_threshold (Integer, Read-Write, 25..50 °C)
```

---

## 4. Push Notification & Alert Rules Matrix

ESP RainMaker automatically dispatches native push notifications to all paired mobile devices under the following conditions:

| Alert Trigger | Severity | Mobile Push Notification Message | Automated Interlock Action |
| :--- | :---: | :--- | :--- |
| **Sump Level < 15%** | `CRITICAL` | 🚨 **Ground Sump Low!** Level is at {X}%. Sump pump shut off to prevent dry-run damage. | Sump Motor Float Relay opened; RWP paused on Astero. |
| **Sump Level > 95%** | `WARNING` | 💧 **Ground Sump Full!** Level is at {X}%. Borewell pump cutoff activated. | Borewell Motor Float Relay opened to stop overflow. |
| **TWT Level > 95%** | `INFO` | ✅ **Treated Water Tank Full!** RO Plant entering standby flush cycle. | Astero TWT Float Relay opened. |
| **Dosing Level < 20%** | `WARNING` | ⚠️ **Dosing Chemical Low!** Replenish anti-scalant / dosing reagent tank. | Astero Dosing Relay opened; Alarm flag set. |
| **Battery Room Temp > 38°C**| `ALERT` | 🌡️ **High Battery Room Temperature!** Room temp is {X}°C. Exhaust fan turned ON. | Node `0x04` Exhaust Fan Relay automatically energized. |
| **Astero Controller Trip** | `CRITICAL` | ⚠️ **RO Controller Trip!** Aster Alarm contact active. Check LPS/HPS pressure switches. | System status set to FAULT; Alarm flagged in UI. |
| **Wi-Fi Node Disconnect** | `WARNING` | 📡 **Ground Floor Node Offline!** No telemetry received for > 10 seconds. | Plant interlocks revert to safe default state. |

---

## 5. Provisioning & Pairing Procedure

1. **BLE / SoftAP Pairing:** On initial startup (or when BOOT button is held for 5 seconds), the ESP32 Central Hub enters BLE pairing mode with a flashing Blue Status LED.
2. **ESP RainMaker App:** Open the ESP RainMaker app (available on Google Play Store & Apple App Store).
3. **Scan QR Code / BLE Device:** Select "Add Device" $\to$ Scan the provisioning QR code displayed on the serial console or printed on the Hub enclosure.
4. **Wi-Fi Configuration:** Select the home/facility Wi-Fi network and enter the credentials. The Hub connects, authenticates with AWS IoT, and registers all devices automatically.
