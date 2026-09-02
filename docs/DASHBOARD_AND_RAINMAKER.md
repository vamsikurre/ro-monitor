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
| **Astero Controller Trip** | `CRITICAL` | ⚠️ **RO Controller Trip!** Aster Alarm contact active. Check feed pressure (LPS), dosing level and pump overload. | System status set to FAULT; Alarm flagged in UI. |
| **Wi-Fi Node Disconnect** | `WARNING` | 📡 **Ground Floor Node Offline!** No telemetry received for > 10 seconds. | Plant interlocks revert to safe default state. |

**Two constraints on the `alarm` field, both from `WIRING.md` Section 6:**
1. ~~The Aster `ALARM` terminal is the configurable `AUX OP`. Until it is confirmed set to `ALARM` (password 678), the signal may actually mean "RWP running" and must not be surfaced as a fault.~~ **Closed 2026-08-26:** observed open in normal running and closed on a plant issue, so it is a genuine fault flag and may be surfaced as one. It does not say *which* fault — the panel multiplexes every condition onto the one contact — so alert text must read "controller fault, check the panel" rather than naming a cause (`WIRING.md` §6.3).
2. `HPS` is unwired on this plant, so over-pressure can never be the cause of a trip and no `hps` state should be published. Low pressure additionally lags the display by up to the `LPS TRIP` time (factory 3 min), so alarm onset and fault onset are separate timestamps.

---

## 4.9. What the App Shows — devices are rooms

RainMaker groups by *device*, so the devices are named after **places**, not
signal types. Someone standing in the RO room wants one screen with everything
in that room on it.

| Device | Primary | Carries |
| :--- | :--- | :--- |
| **RO Room** | `Status` | HPP/RWP running + current, over-current, RL1/RL2, LPS, controller fault, room temp/RH, and **HPP / RWP last run, TWT last full** |
| **Battery Room** | `Battery Room Temp` | temp/RH, **the exhaust fan switch**, fan mode, fan-on threshold, fan last run |
| **Water Tanks** | `Treated Water Level` | RWT/TWT/dosing levels, TWT float, TDS ×2, water temp ×2, salt rejection |

### 4.9.1. Nothing shows a plausible zero before it has been measured

**RainMaker has no null**, and a parameter's initial value is published at boot
whether or not anything has ever measured it. Zero is a *plausible reading* for
every figure here — 0 ppm is distilled water, 0 % rejection is a destroyed
membrane, 0 A is an idle pump, 0 °C is a cold room — so unmeasured parameters
start at **`-1`** (integers) or **`-99`** (temperatures and currents), values
nobody can mistake for data.

The report guards already refuse to *send* a reading that does not exist; these
sentinels are what the app shows until one does. **Seeing `-1` or `-99` means the
sensor is not fitted or its node is offline — it is not a fault in the hub.**

### 4.9.2. The exhaust fan: one opinion at a time

The fan can be switched from the app, and there are three ways to look at it:

| Control | Does |
| :--- | :--- |
| **Exhaust Fan** (toggle) | The switch. On sets Force On, off sets Force Off |
| **Fan Mode** (Auto / On / Off) | The same thing, said explicitly — and it is what tells you *why* the fan is where it is |
| **Fan On Above** (slider) | The Auto threshold, 25–55 °C |

**Both overrides expire back to Auto after 30 minutes** (`FAN_FORCE_MS`). A
battery room must not inherit its ventilation policy from somebody who set a
toggle and went home.

**Force Off is refused above 40.0 °C** (`FAN_FORCE_OFF_CEILING_DECI`) — the hub
reverts to Auto and logs it. That figure matches `BACKSTOP_ON_DECI_C` in
`ro_node.ino` exactly, and the coupling is the point: while the hub is talking,
the node obeys it and stands its own backstop down (§4.4), so a hub commanding
OFF at 45 °C would suppress the very fail-safe that exists for a room making
hydrogen. Forcing the fan **on** is never restricted — ventilating is the safe
direction.

### 4.9.3. "Last run" times

`HPP Last Run`, `RWP Last Run`, `TWT Last Full` and `Fan Last Run` are wall-clock
strings — RainMaker has no timestamp type, and "02 Sep 14:32" is what someone
wants to read. They answer the question a live boolean cannot: a pump that is off
*right now* says nothing about whether the plant has worked today.

- Recorded on the **rising edge**, stored in NVS, so a power cut does not erase
  when the plant last ran.
- **`--` until observed with a synchronised clock.** An event stamped before SNTP
  syncs would read 1970, and a plant that "last ran in 1970" is a bug someone has
  to chase; one that has never run is a fact.
- **TWT full comes from the level, not the Aster float.** `TWT FLOTY` is a C/NC
  contact whose *open* state means full, and an unwired input reads open
  (`WIRING.md` §0.3.2) — so trusting the float would stamp "tank full" at every
  boot of every hub not yet wired to the panel.

---

## 5. Provisioning & Pairing Procedure

1. **BLE pairing — not SoftAP.** A hub that has never been provisioned is already in pairing mode at boot and stays there **30 minutes** (`CONFIG_APP_NETWORK_PROV_TIMEOUT_PERIOD`). To re-arm it afterwards, **hold `BOOT` for ~5 seconds and release** — the action fires on release, and the accepted window is **3 to 9 seconds**:

   | Hold | On release | Notes |
   | :--- | :--- | :--- |
   | tap, < 3 s | **nothing** | Not a pairing gesture. Neither is a power cycle — a provisioned hub just reconnects |
   | **3-9 s** | **Wi-Fi reset → pairing mode** | The one you want. `esp_rmaker_wifi_reset(2, 2)` |
   | **≥ 10 s** | **factory reset** | `esp_rmaker_factory_reset(2, 2)` — also unclaims the node from the RainMaker account, and re-claiming needs the app and internet again. **Let go before ten.** |

   > **Releasing `BOOT` after a flash no longer erases the Wi-Fi credentials.** The
   > flash sequence is *hold `BOOT`, tap `EN`, release `BOOT`*, and `idf.py` hard-resets
   > the board over RTS when it finishes — so a hub could come back up with the button
   > still held, count to three, and treat the release as a deliberate Wi-Fi reset.
   > **This happened on 2026-09-02:** a reflashed hub came back advertising over BLE
   > with its provisioning gone. The button is now armed only after it has been seen
   > *released* at least once since boot, so a press has to have a beginning.
   >
   > **If it does happen** (older firmware, or a genuine long press): only the Wi-Fi
   > credentials are cleared, **not** the node claim. Re-pair with **Add Device →
   > `PROV_xxxxxx`**, PoP `rohub1234`, and the same node comes back with its devices,
   > calibration and schedules intact. `esp_rmaker_factory_reset` is the one that
   > unclaims, and that needs a ten-second hold.

   > **`BOOT` held while the board is reset is NOT pairing mode — it is flash download mode.** `GPIO 0` is a strapping pin. Press and hold `BOOT` on an already-running hub; never combine it with `EN` or a power cycle. Easy to get wrong with the FTDI sitting right there (`WIRING.md` §0.3.1).

   > **The blue LED is not a pairing indicator.** `GPIO 2` is driven only by the poll loop — one blink per 2 s cycle, provisioned or not. It says the poll task is alive and nothing else. Judge pairing from the serial log or from the app, never from the LED. *(This line previously claimed a "flashing Blue Status LED" marked pairing mode. It never did.)*
2. **ESP RainMaker App:** Open the ESP RainMaker app (available on Google Play Store & Apple App Store).
3. **Scan QR Code / BLE Device:** Select "Add Device" $\to$ scan the QR printed on the serial console at boot, or choose "I don't have a QR code" and pick the BLE device named **`PROV_xxxxxx`**. The proof of possession is **`rohub1234`** (`app_network_set_custom_pop()` in `app_main.c`).

   > **The phone needs working internet during pairing.** This hub uses **Assisted Claiming** — the original ESP32-WROOM-32 cannot self-claim, so the app fetches the node certificate from the RainMaker cloud and hands it over during the BLE session. On a roof with no mobile data, pairing fails at the claim step and the error does not name the cause. Pair where there is signal.
4. **Wi-Fi Configuration:** Select the home/facility Wi-Fi network and enter the credentials. The Hub connects, authenticates with AWS IoT, and registers all devices automatically. A wrong password is recoverable without touching the board: after `CONFIG_APP_NETWORK_PROV_MAX_RETRY_CNT` = 3 failed attempts, `CONFIG_APP_NETWORK_RESET_PROV_ON_FAILURE` returns it to pairing mode by itself.

**There is no `RO-HUB` access point any more.** As of 2026-09-01 the hub's own AP is **off** (`AP_MODE_ENABLED 0` in `app_priv.h`) and the dashboard and `/cal` are reachable **over the house LAN only** — `http://ro-hub.local/` or the hub's DHCP address. Provisioning is still BLE and is unaffected.

> **What that costs, so it is not a surprise on a bad day:** if the router dies or the hub drops off the Wi-Fi, there is no local way in at all — no dashboard, no `/cal`, no calibrating a tank on a roof without a working network. RS485 polling, the alerts and the fan policy all continue regardless; the hub keeps running the plant, you just cannot see or configure it until the LAN is back. Bringing the AP back is `AP_MODE_ENABLED 1` and a reflash — deliberately a compile-time switch, because a runtime one has to answer "what happens when somebody turns the AP off while connected to the AP".

### 5.1. Who needs the password

The dashboard is deliberately open — it is read-only, it shows tank levels and pump states, and anyone already on the house LAN can see the plant without hunting for a password. Everything that *changes* something is behind HTTP Basic (`admin` / the password stored in NVS, default `ro-calibrate`, changeable at `/cal`).

| Route | Password | Why |
| :--- | :---: | :--- |
| `/` | no | Read-only view |
| `/api/telemetry` | no | What the dashboard polls |
| `/cal` and every `/api/cal/*` | **yes** | Changes calibration, thresholds, the password itself |
| *anything added later* | **yes, by default** | See below |

**Protected is the default in the code, not a convention.** Every route is registered through a single `gate()` in `app_web.c` that refuses unless the route is explicitly marked `.open = true`. A relay toggle added later is password-protected because somebody would have to go out of their way to make it public. The previous arrangement put the check inside each handler, which protects exactly the handlers somebody remembered to protect.
