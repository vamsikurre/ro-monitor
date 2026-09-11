# Credentials

Every password, key and default that is compiled into this repo, in one place.
Nothing here is a secret worth defending — this is a plant on a house LAN, not
an internet-facing service — but it is worth knowing exactly what the defaults
are and which of them a site can change without a reflash.

## 1. The list

| What | User | Value | Where it is set | Changeable without reflash? |
|------|------|-------|-----------------|-----------------------------|
| **Calibration / control pages** (`/cal` and every POST) | `admin` | **`ro-calibrate`** | `firmware/hub_prod/main/app_priv.h` — `CAL_USER`, `CAL_PASS_DEFAULT` | **Yes** — `/cal` → *This page's password*. Stored in NVS key `cal_pass`, which wins over the compiled default from then on. |
| **RainMaker pairing** (proof of possession) | — | **`rohub1234`** | `firmware/hub_prod/main/app_main.c` — `app_network_set_custom_pop()` | No |
| **Hub fallback access point** | SSID `RO-HUB` | **`ro-monitor`** | `app_priv.h` — `AP_SSID`, `AP_PASS` | No. **Currently dead**: `AP_MODE_ENABLED 0` since 2026-09-01, so the AP is not started at all. |
| **Ground-floor node Wi-Fi** (nodes 0x05, 0x06) | SSID from menuconfig | **`changeme`** (placeholder) | `firmware/gf_node/main/Kconfig.projbuild` — `GF_WIFI_SSID`, `GF_WIFI_PASS` | No — it is a build-time Kconfig value. The real house SSID/passphrase is set per build via `idf.py menuconfig` and lands in the **untracked** `sdkconfig`; the tracked `sdkconfig.defaults` / `sdkconfig.util` carry no credentials. |

The hub itself has **no stored Wi-Fi password in the source tree** — its station
credentials arrive over BLE during RainMaker provisioning and live in NVS.

## 2. What is protected and what is not

* **The dashboard `/` is open on purpose.** Read-only, no password: tank levels
  and pump states for anyone already on the house LAN.
* **Everything that changes something is behind HTTP Basic** with the
  credentials in row 1 — the `/cal` page, relay commands, calibration writes and
  `POST /api/cal/pass` itself. `app_web.c` defaults every route to protected;
  "open" is a flag someone has to write deliberately.
* **Basic auth is base64, not encryption**, and this is plain HTTP. Anyone
  sniffing the LAN sees the password. That is accepted for a house network.
* **The ground-floor nodes have no authentication at all.** Their HTTP endpoints
  (`/id`, `/telemetry`, `/ota`) answer anyone on the LAN. They report readings
  and accept firmware — the OTA path is protected only by ESP-IDF's rollback,
  not by a password.

## 3. Changing the calibration password

`/cal` → *This page's password* → new password → **Change**. Rules enforced by
`cal_set_password()`: **8–32 characters**, anything shorter or longer is
rejected with "rejected: 8-32 characters". The browser keeps sending the old
credentials afterwards, so close it or clear saved logins before reopening
`/cal`.

To get back to `ro-calibrate`, erase NVS (`idf.py erase-flash`, which also drops
provisioning and runtime totals) or set it back through the same form.
