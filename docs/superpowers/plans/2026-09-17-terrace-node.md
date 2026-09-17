# Terrace Node Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A firmware for the existing terrace ESP32 board that keeps every wire it owns today, serves one JSON of interpreted readings over the LAN, takes fan-mode and relay-test commands, and updates over HTTP — with no RainMaker, BLE, dashboard, history or alerts.

**Architecture:** `firmware/terrace_node` is carved from `firmware/hub_prod` at commit `4e5aedf`. Four files are copied verbatim (`app_priv.h`, `app_rs485.[ch]`, `app_sensors.[ch]`, `app_cal.[ch]`); `main.c` is the hub's poll loop with only its sensor half; `web.c` is the hub's web server with the dashboard, history, events and RainMaker-only fieldsets cut out and a new telemetry format; `net.c` and `ota.c` follow `firmware/gf_node`. The hub-facing contract is the JSON in Task 5, guarded by `docs/check_terrace_contract.py`.

**Tech Stack:** ESP-IDF v5.4.4 (`C:\Espressif\tools\Microsoft.v5.4.4.PowerShell_profile.ps1` sets up the shell; `idf.py` is then an alias), target `esp32`, esp_http_server, esp_wifi STA, esp_ota_ops with app rollback. Python 3 for the checker.

**Spec:** `docs/superpowers/specs/2026-09-17-hub-split-design.md` — sections 3, 7 and 8 (steps 1 and 6). This plan is the first of three; the S3 hub and the SD storage are planned after this one lands, against the contract this plan makes real.

**Spec correction carried by this plan:** §3 says Wi-Fi credentials are "set once over serial (the gf_node pattern)". The gf_node actually compiles them in via Kconfig, and the hub board already holds working credentials in NVS from BLE provisioning — the same `nvs` partition this firmware keeps. So: the terrace node uses the stored credentials when present, falls back to Kconfig values when NVS has none, and keeps the hub's `/cal` Wi-Fi scan-and-join fieldset for changing them. Task 8 fixes the spec text.

## Global Constraints

- Target `esp32`, 4 MB flash, partition table identical to `firmware/hub_prod/partitions.csv` (same offsets, `fctry` kept) so the board's NVS — calibration under namespace `ro_cal`, Wi-Fi credentials under `nvs.net80211` — survives the reflash untouched.
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`; a new image is on trial until `/api/telemetry` has been served once, then marked valid, or rolled back after `OTA_CONFIRM_MS` = 120000 ms.
- Firmware version is `git describe --always --tags --dirty`, stamped at configure time. **Commit before `idf.py reconfigure build`** for any image that will be flashed.
- Missing readings use the sentinels the hub uses: `-1` for percent and deci-amps, `null` for a TDS reading that is not live, never a plausible zero.
- Poll cadence `POLL_CYCLE_MS` = 2000 ms (from `app_priv.h`).
- Node id in the `link` block is `7`. `5` is the sump node, `6` was the utility node, `0x01` is a retired RS485 address.
- No new dependencies: no managed components, no mDNS, no cJSON. The JSON is one `snprintf` format string, like the hub's.
- Build command for every task that touches C: from `firmware/terrace_node`, `idf.py build`. Expected last line: `Project build complete.` Warnings about unused static functions are acceptable in Tasks 4 and 5 only while the carve is in progress; the final build in Task 7 must be warning-free in `main/`.
- Commit messages follow the repo's style (a plain sentence as the title, a short body saying why) and end with:
  ```
  Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Q26A318TTPtPdfD1GbXwbc
  ```

---

## File Structure

```
firmware/terrace_node/
  CMakeLists.txt            project(terrace_node); git-describe version, copied from hub_prod
  partitions.csv            byte-for-byte copy of hub_prod/partitions.csv
  sdkconfig.defaults        hub_prod's minus RainMaker/BLE/mDNS/SoftAP lines, plus nothing new
  main/
    CMakeLists.txt          SRCS main.c net.c web.c ota.c app_rs485.c app_sensors.c app_cal.c
    Kconfig.projbuild       TN_WIFI_SSID / TN_WIFI_PASS fallbacks (menu "Terrace node")
    app_priv.h              verbatim copy (constants, hub_state_t, pin map)
    app_rs485.c / .h        verbatim copy
    app_sensors.c / .h      verbatim copy
    app_cal.c / .h          verbatim copy
    node.h                  the six cross-file declarations this firmware adds
    main.c                  state + lock, relay test, fan policy, RS485 readers, poll loop, app_main
    net.c                   Wi-Fi STA: stored credentials, Kconfig fallback, reconnect
    web.c                   basic-auth gate, /api/telemetry, /api/fan, /api/relay, /cal, /logs, /ota
    ota.c                   raw-image OTA handler + trial confirmation, from gf_node
docs/check_terrace_contract.py   lifts the terrace format string, fills it, parses, checks keys
docs/superpowers/specs/2026-09-17-hub-split-design.md   §3 Wi-Fi wording corrected
docs/DASHBOARD_AND_RAINMAKER.md  §4.10 gains the terrace OTA procedure
architecture.txt                 terrace box relabelled as a node
```

Responsibilities: `main.c` owns `hub_state_t` and everything that changes it; `web.c` only reads state under the lock and forwards commands through `node.h`; `net.c` and `ota.c` know nothing about sensors.

---

### Task 1: Scaffold the project and copy the four untouched modules

**Files:**
- Create: `firmware/terrace_node/CMakeLists.txt`, `firmware/terrace_node/partitions.csv`, `firmware/terrace_node/sdkconfig.defaults`, `firmware/terrace_node/main/CMakeLists.txt`, `firmware/terrace_node/main/Kconfig.projbuild`, `firmware/terrace_node/main/node.h`, `firmware/terrace_node/main/main.c` (stub)
- Copy: `firmware/hub_prod/main/{app_priv.h,app_rs485.c,app_rs485.h,app_sensors.c,app_sensors.h,app_cal.c,app_cal.h}` → `firmware/terrace_node/main/`

**Interfaces:**
- Produces: `node.h` declarations used by every later task:
  ```c
  esp_err_t net_start(void);
  bool      net_up(void);
  int       net_rssi(void);
  esp_err_t web_start(void);
  bool      web_served_once(void);
  esp_err_t ota_handle(httpd_req_t *req);
  bool      relay_test_start(int n);          /* 1..RELAY_HUB_COUNT+1; 5 = fan */
  void      fan_mode_request(fan_mode_t m);   /* from POST /api/fan */
  ```

- [ ] **Step 1: Create the directory and copy the untouched modules**

From the repo root (Git Bash):

```bash
mkdir -p firmware/terrace_node/main
cp firmware/hub_prod/partitions.csv firmware/terrace_node/partitions.csv
for f in app_priv.h app_rs485.c app_rs485.h app_sensors.c app_sensors.h app_cal.c app_cal.h; do
  cp firmware/hub_prod/main/$f firmware/terrace_node/main/$f
done
```

- [ ] **Step 2: Write the top-level CMakeLists.txt**

`firmware/terrace_node/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)

# Version = git description, same reasoning as hub_prod/CMakeLists.txt: the
# hub shows it on the dashboard's node list, and "which code is on the
# terrace board" must be answerable without a serial cable.
execute_process(
    COMMAND git describe --always --tags --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE _git_ver
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _git_rc)
if(_git_rc EQUAL 0 AND _git_ver)
    set(PROJECT_VER "${_git_ver}")
else()
    message(WARNING "git describe failed - firmware version will not identify the code on the device")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(terrace_node)
```

- [ ] **Step 3: Write main/CMakeLists.txt**

`firmware/terrace_node/main/CMakeLists.txt`:

```cmake
# No PRIV_REQUIRES: naming any component list on main cancels the default
# that gives main the whole component graph (see gf_node/main/CMakeLists.txt).
idf_component_register(SRCS "main.c" "net.c" "web.c" "ota.c"
                            "app_rs485.c" "app_sensors.c" "app_cal.c"
                       INCLUDE_DIRS ".")
```

- [ ] **Step 4: Write sdkconfig.defaults**

`firmware/terrace_node/sdkconfig.defaults`:

```
# Terrace node: the hub board without the cloud. Everything RainMaker, BLE,
# SoftAP and mDNS from hub_prod/sdkconfig.defaults is deliberately absent.
CONFIG_IDF_TARGET="esp32"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# New image boots pending-verify; main.c marks it valid once one
# /api/telemetry has been served, else it reboots into the old slot.
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y

CONFIG_COMPILER_OPTIMIZATION_SIZE=y
CONFIG_ESP_TASK_WDT_INIT=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=30
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
CONFIG_HTTPD_MAX_URI_LEN=512
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_ESP_COREDUMP_ENABLE_TO_UART=y

# Same pool as the hub: httpd is configured for 7 clients + 3 of its own.
CONFIG_LWIP_MAX_SOCKETS=16
```

- [ ] **Step 5: Write main/Kconfig.projbuild**

```
menu "Terrace node"

    config TN_WIFI_SSID
        string "Wi-Fi SSID used only when NVS holds no credentials"
        default "changeme"
    config TN_WIFI_PASS
        string "Wi-Fi password used only when NVS holds no credentials"
        default "changeme"

endmenu
```

- [ ] **Step 6: Write node.h**

`firmware/terrace_node/main/node.h`:

```c
/*
 * node.h - the declarations that cross file boundaries in the terrace node.
 * main.c owns hub_state_t (declared in app_priv.h) and everything that
 * changes it; web.c reads it under hub_state_lock() and forwards commands
 * through the two functions at the bottom.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "app_priv.h"

#define TN_NODE_ID        7        /* 5 = sump node, 6 was utility; 0x01 is a retired RS485 address */
#define OTA_CONFIRM_MS    120000   /* trial image must serve one telemetry before this */

esp_err_t net_start(void);
bool      net_up(void);
int       net_rssi(void);

esp_err_t web_start(void);
bool      web_served_once(void);      /* one /api/telemetry has been answered */

esp_err_t ota_handle(httpd_req_t *req);

bool relay_test_start(int n);          /* 1..RELAY_HUB_COUNT = hub relays, RELAY_HUB_COUNT+1 = fan */
void fan_mode_request(fan_mode_t m);   /* POST /api/fan; expires to Auto after FAN_FORCE_MS */
```

- [ ] **Step 7: Write a stub main.c so the project configures and builds**

`firmware/terrace_node/main/main.c` (replaced entirely in Task 2):

```c
#include "esp_log.h"
#include "node.h"
static const char *TAG = "tn";
void app_main(void) { ESP_LOGI(TAG, "terrace node scaffold"); }
```

Also create empty placeholders so CMake finds every SRCS file: `net.c`, `web.c`, `ota.c` each containing only `#include "node.h"`.

- [ ] **Step 8: Build**

PowerShell:
```powershell
. C:\Espressif\tools\Microsoft.v5.4.4.PowerShell_profile.ps1 *> $null
Set-Location C:\orbit\git_repos\ro-monitor\firmware\terrace_node
idf.py set-target esp32; idf.py build 2>&1 | Select-Object -Last 5
```
Expected: `Project build complete.` The copied modules compile because they depend only on IDF and each other. If `app_cal.c` or `app_sensors.c` fails on a missing symbol, the symbol is in `app_main.c` — note it for Task 2, do not add it here.

- [ ] **Step 9: Commit**

```bash
git add firmware/terrace_node
git commit -F - <<'EOF'
Terrace node: the hub board's sensor modules, and nothing above them yet

Scaffold for the firmware that will replace hub_prod on the terrace ESP32:
same partition table so NVS survives, same four sensor/bus/calibration
modules copied untouched, rollback on, no RainMaker or BLE. Builds with a
stub app_main; the poll loop and web server follow.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Q26A318TTPtPdfD1GbXwbc
EOF
```

---

### Task 2: main.c — state, relay test, fan policy, RS485 readers

**Files:**
- Modify: `firmware/terrace_node/main/main.c` (replace the stub)
- Source to carve from: `firmware/hub_prod/main/app_main.c` at `4e5aedf`

**Interfaces:**
- Produces: `hub_state_lock()`, `hub_state_unlock()`, `hub_state()` (declared in `app_priv.h` already), `relay_test_start()`, `fan_mode_request()` (declared in `node.h`).
- Consumes: `app_sensors.h`, `app_rs485.h`, `app_cal.h` as copied.

- [ ] **Step 1: Copy the pure functions out of app_main.c by function boundary**

Open `firmware/hub_prod/main/app_main.c`. Copy these into a new `main.c`, in this order, **verbatim**, each from its opening comment to its closing brace. Line numbers are as of `4e5aedf` and are a guide; cut at function boundaries.

| Function(s) | Approx. lines | Change while copying |
|---|---|---|
| file-scope `static hub_state_t s_state; static SemaphoreHandle_t s_state_mux;` and `hub_state_lock/unlock/hub_state` | 67–72 | none |
| `s_relay_test_until_us`, `s_fan_test_until_us`, `relay_test_start()`, `relay_test_expire()` | 912–960 | none |
| `s_fan_mode_set_us`, `fan_mode_str()`, `command_fan()` | 961–1041 | delete `report_fan_mode()` and its two calls inside `command_fan()` (the `report_fan_mode(s->fan_mode);` lines); keep the `ESP_LOG` lines beside them |
| `status_word()`, `wq_word()`, `fmt_tank()`, `log_summary()` | 1042–1177 | none |
| `log_node_firmware()`, `read_tank_node()`, `read_water_quality()`, `read_climate_node()` | 1178–1370 | none (these only log; grep confirms no `event_push` or `esp_rmaker_` inside) |

Then add these includes at the top, replacing the hub's include block:

```c
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "app_cal.h"
#include "app_priv.h"
#include "app_rs485.h"
#include "app_sensors.h"
#include "node.h"

static const char *TAG = "tn";
```

- [ ] **Step 2: Add fan_mode_request()**

Directly after `command_fan()`:

```c
/* The hub's write_cb used to do this from the RainMaker app; now the hub
 * forwards the app's choice as POST /api/fan. Same expiry to Auto. */
void fan_mode_request(fan_mode_t m)
{
    hub_state_lock();
    s_state.fan_mode = m;
    hub_state_unlock();
    s_fan_mode_set_us = esp_timer_get_time();
    ESP_LOGI(TAG, "fan mode -> %s (expires to Auto in %lu min)",
             fan_mode_str(m), (unsigned long)(FAN_FORCE_MS / 60000));
}
```

- [ ] **Step 3: Write the poll loop — the hub's sensor half only**

Append to `main.c`. This is `poll_task()` from `4e5aedf` lines 1483–1663 with `gf_apply()`, the run-accounting block, `history_push`, `diff_events`, every `report_*`, `evaluate_alerts` and `cloud_watchdog` removed:

```c
static void poll_task(void *arg)
{
    (void)arg;
    static median_u16_t s_dosing_win;
    int  ct_turn = 0;              /* round-robin: one clamp per cycle */
    int  wq_turn = WQ_POLL_CYCLES; /* poll water quality on the first cycle, then every Nth */
    int  oc_streak = 0;

    while (true) {
        int64_t cycle_start = esp_timer_get_time();
        gpio_set_level(GPIO_LED_STATUS, 1);

        hub_state_t local;
        hub_state_lock();
        local = s_state;
        hub_state_unlock();

        read_tank_node(NODE_ADDR_RWT, &local.rwt, &local.rwt_online, CAL_TANK_RWT);
        read_tank_node(NODE_ADDR_TWT, &local.twt, &local.twt_online, CAL_TANK_TWT);
        read_climate_node(&local);

        if (++wq_turn >= WQ_POLL_CYCLES) {
            wq_turn = 0;
            #define WQ_SUBMERGED(t, c) ((t).pct >= 0 && (t).pct >= (int16_t)cal_tank(c)->tds_min_pct)
            if (local.rwt_online && WQ_SUBMERGED(local.rwt, CAL_TANK_RWT)) {
                read_water_quality(NODE_ADDR_RWT, &local.rwt_wq, CAL_TANK_RWT);
                local.rwt_wq.live = local.rwt_wq.fitted;
            } else {
                local.rwt_wq.live = false;
            }
            if (local.twt_online && WQ_SUBMERGED(local.twt, CAL_TANK_TWT)) {
                read_water_quality(NODE_ADDR_TWT, &local.twt_wq, CAL_TANK_TWT);
                local.twt_wq.live = local.twt_wq.fitted;
            } else {
                local.twt_wq.live = false;
            }
            #undef WQ_SUBMERGED
            local.rejection_pct = rejectionPercent(local.rwt_wq.ppm, local.twt_wq.ppm);
        }

        relay_test_expire();
        command_fan(&local);

        int16_t t = 0;
        uint16_t h = 0;
        if (sht30_read(&t, &h)) {
            local.ro_room.temp_deci_c = t;
            local.ro_room.hum_deci_pct = h;
            local.ro_room.fault = false;
            local.ro_room.last_ok_us = esp_timer_get_time();
        } else {
            local.ro_room.fault = true;
        }

        local.dosing.raw_mm = dosing_read_mm();
        local.dosing.distance_mm = median_u16_push(&s_dosing_win, local.dosing.raw_mm);
        if (local.dosing.distance_mm == 0) {
            local.dosing.sensor = SENSOR_NO_ECHO;
            local.dosing.pct = -1;
        } else {
            local.dosing.sensor = (local.dosing.distance_mm < BLIND_ZONE_MM) ? SENSOR_BLIND : SENSOR_OK;
            local.dosing.last_ok_us = esp_timer_get_time();
            const cal_tank_cfg_t *c = cal_tank(CAL_TANK_DOS);
            uint8_t pct = levelPercent(local.dosing.distance_mm, c->full_mm, c->empty_mm);
            local.dosing.pct = (pct == 255) ? -1 : (int16_t)pct;
        }

        local.twt_float_closed = opto_twt_float_closed();
        local.rl1_active = opto_rl1_active();
        local.rl2_active = opto_rl2_active();
        local.alarm_active = alarm_active();
        local.lps_active = opto_lps_active();

        ac_probe(GPIO_IN_HPP_AC, &local.hpp.running, &local.hpp.ac_floating,
                 &local.hpp.mv_lo, &local.hpp.mv_hi);
        if (local.hpp.ac_floating) {
            ESP_LOGW(TAG, "HPP AC channel floating (%lu-%lu mV) - check VCC and OUT at the module",
                     (unsigned long)local.hpp.mv_lo, (unsigned long)local.hpp.mv_hi);
        }
        ac_probe(GPIO_IN_RWP_AC, &local.rwp.running, &local.rwp.ac_floating,
                 &local.rwp.mv_lo, &local.rwp.mv_hi);
        if (local.rwp.ac_floating) {
            ESP_LOGW(TAG, "RWP AC channel floating (%lu-%lu mV) - check VCC and OUT at the module",
                     (unsigned long)local.rwp.mv_lo, (unsigned long)local.rwp.mv_hi);
        }

        if (ct_turn == 0) {
            local.hpp.deci_amps = ct_read_deci_amps(GPIO_IN_HPP_CT, CAL_CT_HPP, &local.hpp.ct_mid_mv);
        } else {
            local.rwp.deci_amps = ct_read_deci_amps(GPIO_IN_RWP_CT, CAL_CT_RWP, &local.rwp.ct_mid_mv);
        }
        ct_turn ^= 1;

        /* Over-current is decided here, not on the hub: the thresholds are in
         * this board's /cal, next to the clamps they describe. */
        bool oc_now =
            (local.hpp.running && local.hpp.deci_amps > 0 &&
             local.hpp.deci_amps > (int16_t)cal_ct(CAL_CT_HPP)->oc_deci_amps) ||
            (local.rwp.running && local.rwp.deci_amps > 0 &&
             local.rwp.deci_amps > (int16_t)cal_ct(CAL_CT_RWP)->oc_deci_amps);
        oc_streak = oc_now ? (oc_streak + 1) : 0;
        local.overcurrent = (oc_streak >= OC_CONFIRM_CYCLES);

        local.rs485_errors = rs485_error_count();
        local.last_cycle_ms = (uint32_t)((esp_timer_get_time() - cycle_start) / 1000);

        hub_state_lock();
        s_state = local;
        hub_state_unlock();

        static int64_t last_summary_us = 0;
        int64_t now_us = esp_timer_get_time();
        if (last_summary_us == 0 || now_us - last_summary_us >= (int64_t)LOG_SUMMARY_MS * 1000) {
            last_summary_us = now_us;
            log_summary(&local);
            static uint32_t last_err_total = 0;
            uint32_t err_total = rs485_error_count();
            if (err_total != last_err_total) {
                last_err_total = err_total;
                char report[160];
                if (rs485_error_report(report, sizeof(report)) > 0) {
                    ESP_LOGW(TAG, "rs485 failures by node/command: %s", report);
                }
            }
        }

        gpio_set_level(GPIO_LED_STATUS, 0);
        int64_t elapsed_ms = (esp_timer_get_time() - cycle_start) / 1000;
        int64_t remain = POLL_CYCLE_MS - elapsed_ms;
        vTaskDelay(pdMS_TO_TICKS(remain > 50 ? remain : 50));
    }
}
```

- [ ] **Step 4: Write ota_watch() and app_main()**

Append:

```c
/* From gf_node/main/main.c: a new image is on trial until it has served
 * the hub once. Returns immediately on a normal boot. */
static void ota_watch(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) return;
    int64_t t0 = esp_timer_get_time();
    while (true) {
        if (net_up() && web_served_once()) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "OTA image confirmed valid");
            return;
        }
        if (esp_timer_get_time() - t0 > (int64_t)OTA_CONFIRM_MS * 1000) {
            ESP_LOGE(TAG, "new image never served the hub - rolling back");
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "RO Monitor - Terrace Node  fw %s", esp_app_get_description()->version);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS full or outdated - erasing and re-initialising");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_state_mux = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_state_mux ? ESP_OK : ESP_FAIL);
    memset(&s_state, 0, sizeof(s_state));
    s_state.rwt.pct = s_state.twt.pct = s_state.dosing.pct = -1;
    s_state.hpp.deci_amps = s_state.rwp.deci_amps = -1;
    s_state.ro_room.fault = s_state.battery_room.fault = true;
    s_state.rwt_wq.ppm = s_state.twt_wq.ppm = TDS_INVALID;
    s_state.rejection_pct = -1;

    ESP_ERROR_CHECK(cal_init());
    ESP_ERROR_CHECK(sensors_init());
    ESP_ERROR_CHECK(rs485_init());
    ESP_ERROR_CHECK(net_start());
    if (web_start() != ESP_OK) {
        ESP_LOGE(TAG, "web server failed to start - the hub cannot poll this node");
    }
    xTaskCreate(poll_task, "poll", 6144, NULL, 5, NULL);
    ota_watch();
}
```

Check `wq_state_t` in `app_priv.h` for the field that holds ppm; if it is not `ppm`, use the real name. Check `TDS_INVALID` is visible (it is, from `app_cal.h`).

- [ ] **Step 5: Build**

Same command as Task 1 step 8. Expected: linker errors for `net_start`, `net_up`, `web_start`, `web_served_once` only (Tasks 3–5 provide them). No other error. If a hub-only symbol slipped into a copied function (`event_push`, `esp_rmaker_*`, `s_dev_*`, `history_*`), remove that call — it belongs to the hub.

- [ ] **Step 6: Commit**

```bash
git add firmware/terrace_node/main/main.c
git commit -F - <<'EOF'
Terrace node: the poll loop keeps the wires and drops the cloud

The hub's poll_task with its sensor half only: RS485 tanks and climate,
water quality, dosing ultrasonic, Aster contacts, AC optos and clamps,
over-current, fan policy, relay test. Run accounting, history, events,
RainMaker reports and alerts stay on the hub, which now learns all of this
from /api/telemetry.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Q26A318TTPtPdfD1GbXwbc
EOF
```

---

### Task 3: net.c — Wi-Fi from stored credentials, Kconfig fallback

**Files:**
- Modify: `firmware/terrace_node/main/net.c`

**Interfaces:**
- Produces: `net_start()`, `net_up()`, `net_rssi()` per `node.h`.

- [ ] **Step 1: Write net.c**

```c
/*
 * net.c - join the house LAN as a station, DHCP, and stay joined.
 *
 * Credentials: whatever esp_wifi already holds in NVS. On the terrace board
 * that is the SSID and passphrase BLE provisioning wrote when this board was
 * the hub - the nvs partition is at the same offset in this firmware's table,
 * so a reflash keeps them. Only if NVS holds no SSID (a fresh board) do the
 * Kconfig values apply. /cal's Wi-Fi fieldset (web.c) can change them later;
 * esp_wifi_set_config() writes NVS, so a join survives reboot.
 */
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "node.h"

static const char *TAG = "net";
static bool s_up = false;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_up = false;
        ESP_LOGW(TAG, "disconnected - retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_up = true;
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "up at " IPSTR " - put this in the hub's /cal terrace slot", IP2STR(&e->ip_info.ip));
    }
}

esp_err_t net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wc = { 0 };
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0] != '\0') {
        ESP_LOGI(TAG, "using stored credentials for \"%s\"", (const char *)wc.sta.ssid);
    } else {
        ESP_LOGW(TAG, "NVS holds no Wi-Fi credentials - using the Kconfig fallback \"%s\"", CONFIG_TN_WIFI_SSID);
        memset(&wc, 0, sizeof(wc));
        strncpy((char *)wc.sta.ssid, CONFIG_TN_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
        strncpy((char *)wc.sta.password, CONFIG_TN_WIFI_PASS, sizeof(wc.sta.password) - 1);
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    }
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));   /* the hub polls every 2 s; do not doze */
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

bool net_up(void) { return s_up; }

int net_rssi(void)
{
    wifi_ap_record_t ap;
    return (s_up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}
```

- [ ] **Step 2: Build**

Expected: remaining link errors are `web_start` and `web_served_once` only.

- [ ] **Step 3: Commit**

```bash
git add firmware/terrace_node/main/net.c
git commit -F - <<'EOF'
Terrace node joins the LAN with the credentials the hub already stored

esp_wifi keeps its config in the nvs partition this firmware leaves where
it was, so the reflash from hub to node needs no re-provisioning. Kconfig
values only for a board that has never been joined to anything.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Q26A318TTPtPdfD1GbXwbc
EOF
```

---

### Task 4: ota.c — raw image over HTTP

**Files:**
- Modify: `firmware/terrace_node/main/ota.c`
- Source: `firmware/gf_node/main/ota.c` (117 lines)

**Interfaces:**
- Produces: `ota_handle(httpd_req_t *)` per `node.h`.

- [ ] **Step 1: Copy gf_node's handler**

```bash
cp firmware/gf_node/main/ota.c firmware/terrace_node/main/ota.c
```

Then in the copy: replace `#include "gf.h"` with `#include "node.h"`, and change the TAG string to `"ota"` if it says `"gf"`. The function is already `esp_err_t ota_handle(httpd_req_t *req)`. Keep its trial-image guard (`esp_ota_begin` refused while the running image is still pending verify) and its comments — they explain why.

- [ ] **Step 2: Build**

Expected: only `web_start` / `web_served_once` unresolved.

- [ ] **Step 3: Commit**

```bash
git add firmware/terrace_node/main/ota.c
git commit -F - <<'EOF'
Terrace node takes a raw image on POST /ota, the ground-floor way

Same handler as gf_node, same guard: a board still on a trial image will not
accept another, because the slot it would write is the one it would roll
back into.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Q26A318TTPtPdfD1GbXwbc
EOF
```

---

### Task 5: web.c — telemetry contract, commands, /cal, /logs

**Files:**
- Modify: `firmware/terrace_node/main/web.c`
- Source to carve from: `firmware/hub_prod/main/app_web.c` at `4e5aedf`
- Also copy `log_tap()` and its ring from `app_main.c` lines 2347–2411 into `main.c`, and call `s_log_next = esp_log_set_vprintf(log_tap);` as the first line of `app_main()` — `/logs` needs it.

**Interfaces:**
- Produces: `web_start()`, `web_served_once()`; routes `GET /api/telemetry`, `POST /api/fan`, `POST /api/relay`, `POST /ota`, `GET /cal` + its POSTs, `GET /logs`.
- Consumes: `hub_state_lock()`, `hub_state()`, `relay_test_start()`, `fan_mode_request()`, `ota_handle()`, `net_rssi()`.

- [ ] **Step 1: Carve the web server**

Copy from `app_web.c`, by function boundary:

| Keep | Approx. lines | Note |
|---|---|---|
| includes, `TAG`, helper statics: `wifi_rssi`, `reset_word`, `link_word`, `age_s`, `sensor_word`, `tank_state_word`, `wq_json`, `fw_word`, the scratch buffer, `deny()`, `gate()` | 1–226 | delete `da_json`, `phases_json`, `sump_state_word`, `gf_node_state` (ground floor only); delete the `dashboard.html.gz` `extern` symbols |
| `logs_get()` and the Wi-Fi scan helpers that follow it | 741–954 | none |
| `cal_get()` | 955–1458 | delete the `plant` fieldset (`"<fieldset id=plant>"` … its closing `</fieldset>",` and the `cal_plant_lph()…` argument line) and the `gf` fieldset (`"<fieldset id=gf>"` … through its `</fieldset>` and argument lines). Keep `wifi`, `tanks`, `clamps`, `fan`, `wq`, `relays`, password |
| `read_body`, `redirect_to`, `bad`, `parse_deci`, `form_field` | 1459–1551 | none |
| `cal_tank_post`, `cal_ct_post`, `cal_ct_dry_post`, `cal_fan_post`, `cal_wq_post`, `cal_relay_post`, `cal_scan_post`, `cal_wifi_post`, `cal_pass_post` | 1552–1910 | drop `cal_gf_post`, `cal_plant_post`, `cal_ap_pass_post` |

Do **not** copy `dashboard_get`, `favicon_get`, `history_get`, `events_get`, `telemetry_get`, or the old `web_start`.

Replace `#include "app_web.h"` with `#include "node.h"`; delete the `bool relay_test_start(int n);` forward declaration (it is in `node.h` now).

- [ ] **Step 2: Write the telemetry handler — this is the contract**

Add after the helpers:

```c
/*
 * The contract with the hub. Interpreted values, because the calibration
 * that makes them lives on this board. Keys are checked by
 * docs/check_terrace_contract.py; change both together.
 *
 * Sentinels: pct -1 = no level; deci_amps -1 = no clamp; quality ppm/t null
 * unless the probe is live. Never a plausible zero.
 */
static bool s_served = false;
bool web_served_once(void) { return s_served; }

static const char *fan_mode_word(fan_mode_t m)
{
    return (m == FAN_MODE_ON) ? "on" : (m == FAN_MODE_OFF) ? "off" : "auto";
}

static esp_err_t telemetry_get(httpd_req_t *req)
{
    static char json[1536];
    hub_state_lock();
    const hub_state_t *s = hub_state();

    char rwt_ppm[12], twt_ppm[12], rwt_wt[12], twt_wt[12];
    wq_json(rwt_ppm, sizeof(rwt_ppm), rwt_wt, sizeof(rwt_wt), &s->rwt_wq);
    wq_json(twt_ppm, sizeof(twt_ppm), twt_wt, sizeof(twt_wt), &s->twt_wq);
    char rs485_failures[160];
    rs485_error_report(rs485_failures, sizeof(rs485_failures));

    int n = snprintf(json, sizeof(json),
        "{"
        "\"link\":{\"id\":7,\"fw\":\"%s\",\"uptime_s\":%lld,\"rssi\":%d,\"heap_free\":%u,\"cycle_ms\":%lu},"
        "\"tanks\":{"
          "\"rwt\":{\"pct\":%d,\"distance_mm\":%u,\"sensor\":\"%s\",\"online\":%s},"
          "\"twt\":{\"pct\":%d,\"distance_mm\":%u,\"sensor\":\"%s\",\"online\":%s},"
          "\"dos\":{\"pct\":%d,\"distance_mm\":%u,\"sensor\":\"%s\",\"online\":true}"
        "},"
        "\"quality\":{"
          "\"rwt\":{\"ppm\":%s,\"t\":%s,\"fitted\":%s,\"live\":%s,\"age_s\":%d},"
          "\"twt\":{\"ppm\":%s,\"t\":%s,\"fitted\":%s,\"live\":%s,\"age_s\":%d}"
        "},"
        "\"motors\":{"
          "\"hpp\":{\"running\":%s,\"ac_floating\":%s,\"deci_amps\":%d,\"mv_lo\":%lu,\"mv_hi\":%lu},"
          "\"rwp\":{\"running\":%s,\"ac_floating\":%s,\"deci_amps\":%d,\"mv_lo\":%lu,\"mv_hi\":%lu},"
          "\"overcurrent\":%s"
        "},"
        "\"contacts\":{\"twt_float_closed\":%s,\"rl1_active\":%s,\"rl2_active\":%s,"
                     "\"lps_active\":%s,\"alarm_active\":%s},"
        "\"climate\":{"
          "\"ro_room\":{\"temp_deci_c\":%d,\"hum_deci_pct\":%u,\"fault\":%s},"
          "\"battery_room\":{\"temp_deci_c\":%d,\"hum_deci_pct\":%u,\"fault\":%s,\"online\":%s}"
        "},"
        "\"fan\":{\"on\":%s,\"mode\":\"%s\"},"
        "\"rs485\":{\"errors\":%lu,\"last_poll_ms\":%lu,\"failures\":\"%s\"}"
        "}",
        esp_app_get_description()->version,
        esp_timer_get_time() / 1000000, net_rssi(),
        (unsigned)esp_get_free_heap_size(), (unsigned long)s->last_cycle_ms,

        s->rwt.pct, s->rwt.distance_mm, sensor_word(s->rwt.sensor), s->rwt_online ? "true" : "false",
        s->twt.pct, s->twt.distance_mm, sensor_word(s->twt.sensor), s->twt_online ? "true" : "false",
        s->dosing.pct, s->dosing.distance_mm, sensor_word(s->dosing.sensor),

        rwt_ppm, rwt_wt, s->rwt_wq.fitted ? "true" : "false",
        s->rwt_wq.live ? "true" : "false", age_s(s->rwt_wq.last_ok_us),
        twt_ppm, twt_wt, s->twt_wq.fitted ? "true" : "false",
        s->twt_wq.live ? "true" : "false", age_s(s->twt_wq.last_ok_us),

        s->hpp.running ? "true" : "false", s->hpp.ac_floating ? "true" : "false",
        s->hpp.deci_amps, (unsigned long)s->hpp.mv_lo, (unsigned long)s->hpp.mv_hi,
        s->rwp.running ? "true" : "false", s->rwp.ac_floating ? "true" : "false",
        s->rwp.deci_amps, (unsigned long)s->rwp.mv_lo, (unsigned long)s->rwp.mv_hi,
        s->overcurrent ? "true" : "false",

        s->twt_float_closed ? "true" : "false", s->rl1_active ? "true" : "false",
        s->rl2_active ? "true" : "false", s->lps_active ? "true" : "false",
        s->alarm_active ? "true" : "false",

        s->ro_room.temp_deci_c, (unsigned)s->ro_room.hum_deci_pct, s->ro_room.fault ? "true" : "false",
        s->battery_room.temp_deci_c, (unsigned)s->battery_room.hum_deci_pct,
        s->battery_room.fault ? "true" : "false", s->battery_online ? "true" : "false",

        s->fan_on ? "true" : "false", fan_mode_word(s->fan_mode),
        (unsigned long)s->rs485_errors, (unsigned long)s->last_cycle_ms, rs485_failures);
    hub_state_unlock();

    if (n <= 0 || n >= (int)sizeof(json)) {
        ESP_LOGE(TAG, "telemetry did not fit (%d of %u)", n, (unsigned)sizeof(json));
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, json, n);
    if (e == ESP_OK) s_served = true;
    return e;
}
```

Check `wq_json()`'s signature in the copied helpers matches `(char *ppm, size_t, char *t, size_t, const wq_state_t *)`; it is called that way in the hub's `telemetry_get`.

- [ ] **Step 3: Write the two command handlers and /ota**

```c
/* POST /api/fan  body: mode=auto|on|off  (from the hub, which got it from the app) */
static esp_err_t fan_post(httpd_req_t *req)
{
    char body[64], mode[8];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return bad(req, "body too long");
    if (!form_field(body, "mode", mode, sizeof(mode))) return bad(req, "need mode");
    fan_mode_t m;
    if      (strcmp(mode, "auto") == 0) m = FAN_MODE_AUTO;
    else if (strcmp(mode, "on")   == 0) m = FAN_MODE_ON;
    else if (strcmp(mode, "off")  == 0) m = FAN_MODE_OFF;
    else return bad(req, "mode must be auto, on or off");
    fan_mode_request(m);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, mode, HTTPD_RESP_USE_STRLEN);
}

/* POST /api/relay  body: n=1..5  - one pulse, released by the poll loop */
static esp_err_t relay_post(httpd_req_t *req)
{
    char body[64], nbuf[8];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return bad(req, "body too long");
    if (!form_field(body, "n", nbuf, sizeof(nbuf))) return bad(req, "need n");
    if (!relay_test_start(atoi(nbuf))) return bad(req, "relay must be 1-5");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "pulsed", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_post(httpd_req_t *req) { return ota_handle(req); }

static esp_err_t root_get(httpd_req_t *req)
{
    char b[160];
    int n = snprintf(b, sizeof b,
        "terrace node id %d fw %s\nGET /api/telemetry  POST /api/fan  POST /api/relay  POST /ota  GET /cal  GET /logs\n",
        TN_NODE_ID, esp_app_get_description()->version);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, b, n);
}
```

- [ ] **Step 4: Write web_start() with the route table**

Reuse the hub's `route_t` type and the `gate()` wrapper exactly as `app_web.c` does (its `web_start` registers each route through a wrapper that calls `gate()` when `open` is false). Copy that registration loop and replace the table:

```c
    static const route_t routes[] = {
        { "/",              HTTP_GET,  root_get,      true  },
        { "/api/telemetry", HTTP_GET,  telemetry_get, true  },  /* the hub polls this */
        { "/api/fan",       HTTP_POST, fan_post,      true  },  /* the hub forwards the app */
        { "/api/relay",     HTTP_POST, relay_post,    true  },
        { "/ota",           HTTP_POST, ota_post,      false },  /* password: same as /cal */
        { "/logs",          HTTP_GET,  logs_get,      false },
        { "/cal",           HTTP_GET,  cal_get,       false },
        { "/api/cal/tank",  HTTP_POST, cal_tank_post, false },
        { "/api/cal/ct",    HTTP_POST, cal_ct_post,   false },
        { "/api/cal/ct_dry", HTTP_POST, cal_ct_dry_post, false },
        { "/api/cal/fan",   HTTP_POST, cal_fan_post,  false },
        { "/api/cal/wq",    HTTP_POST, cal_wq_post,   false },
        { "/api/cal/relay", HTTP_POST, cal_relay_post, false },
        { "/api/cal/scan",  HTTP_POST, cal_scan_post, false },
        { "/api/cal/wifi",  HTTP_POST, cal_wifi_post, false },
        { "/api/cal/pass",  HTTP_POST, cal_pass_post, false },
    };
```

`/api/fan` and `/api/relay` are open because the hub calls them without credentials on the LAN, exactly as `/api/telemetry` is open today; a relay pulse is already reachable from the hub's own `/cal` and self-releases. `/ota` is gated: the ground-floor node's is not, and a password on a firmware-write endpoint is the cheapest guard there is. (`gate()` reads the same `cal_password_matches()` the hub used; the board's existing password carries over in NVS.)

Keep `cfg.max_open_sockets = 7;`, `cfg.lru_purge_enable = true;`, `cfg.stack_size = 6144;`, `cfg.server_port = WEB_PORT;`.

- [ ] **Step 5: Build until clean**

Expected: `Project build complete.` Fix in order: missing helpers you deleted but `cal_get` still uses (put them back), `unused function` warnings (delete the function — ground-floor helpers are dead here). No `-Werror` is set, but leave `main/` warning-free.

- [ ] **Step 6: Commit**

```bash
git add firmware/terrace_node/main/web.c firmware/terrace_node/main/main.c
git commit -F - <<'EOF'
Terrace node serves what it measures and takes two commands

/api/telemetry is the contract with the hub: interpreted values with the
hub's sentinels, one snprintf, checked by docs/check_terrace_contract.py.
/api/fan and /api/relay are what the app's two controls become once the
hub is downstairs. /cal keeps the fieldsets for the things this board reads
- tanks, clamps, probes, fan, relays, Wi-Fi - and loses the plant and
ground-floor ones, which now belong to the hub.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Q26A318TTPtPdfD1GbXwbc
EOF
```

---

### Task 6: docs/check_terrace_contract.py

**Files:**
- Create: `docs/check_terrace_contract.py`
- Reuses: `docs/check_telemetry.py` (`format_string`, `fill`, `dig`, `read`)

**Interfaces:**
- Produces: the REQUIRED key list, which Plan 2's hub parser task copies verbatim.

- [ ] **Step 1: Write the failing checker**

```python
"""Fail if the terrace node's /api/telemetry cannot produce valid JSON, or
drops a key the hub's parser will read.

Same method as check_telemetry.py, same reason: a typo in a snprintf format
is invisible on the device and shows up as a hub that hatches the terrace
section for no visible cause. The REQUIRED list below is the contract; the
hub's terrace_apply() (plan 2) is written against it and the same list is
checked there against the parser's keys.

    python docs/check_terrace_contract.py     # from the repo root
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_telemetry as ct  # noqa: E402  (format_string, fill, dig, read)

WEB = 'firmware/terrace_node/main/web.c'

REQUIRED = [
    'link.id', 'link.fw', 'link.uptime_s', 'link.rssi', 'link.heap_free', 'link.cycle_ms',
    'tanks.rwt.pct', 'tanks.rwt.distance_mm', 'tanks.rwt.sensor', 'tanks.rwt.online',
    'tanks.twt.pct', 'tanks.twt.distance_mm', 'tanks.twt.sensor', 'tanks.twt.online',
    'tanks.dos.pct', 'tanks.dos.distance_mm', 'tanks.dos.sensor', 'tanks.dos.online',
    'quality.rwt.ppm', 'quality.rwt.t', 'quality.rwt.fitted', 'quality.rwt.live', 'quality.rwt.age_s',
    'quality.twt.ppm', 'quality.twt.t', 'quality.twt.fitted', 'quality.twt.live', 'quality.twt.age_s',
    'motors.hpp.running', 'motors.hpp.ac_floating', 'motors.hpp.deci_amps', 'motors.hpp.mv_lo', 'motors.hpp.mv_hi',
    'motors.rwp.running', 'motors.rwp.ac_floating', 'motors.rwp.deci_amps', 'motors.rwp.mv_lo', 'motors.rwp.mv_hi',
    'motors.overcurrent',
    'contacts.twt_float_closed', 'contacts.rl1_active', 'contacts.rl2_active',
    'contacts.lps_active', 'contacts.alarm_active',
    'climate.ro_room.temp_deci_c', 'climate.ro_room.hum_deci_pct', 'climate.ro_room.fault',
    'climate.battery_room.temp_deci_c', 'climate.battery_room.hum_deci_pct',
    'climate.battery_room.fault', 'climate.battery_room.online',
    'fan.on', 'fan.mode',
    'rs485.errors', 'rs485.last_poll_ms', 'rs485.failures',
]


def main():
    text = ct.read(WEB)
    fmt = ct.format_string(text)
    filled = ct.fill(fmt)
    try:
        obj = json.loads(filled)
    except ValueError as e:
        print('terrace telemetry is not valid JSON: %s' % e)
        print(filled)
        return 1
    missing = [k for k in REQUIRED if not ct.dig(obj, k)[1]]
    if missing:
        print('terrace telemetry is missing: %s' % ', '.join(missing))
        return 1
    if obj['link']['id'] != 7:
        print('link.id must be 7 (5 = sump, 6 was utility); got %r' % obj['link']['id'])
        return 1
    print('OK: terrace /api/telemetry emits valid JSON with all %d keys the hub reads.' % len(REQUIRED))
    return 0


if __name__ == '__main__':
    sys.exit(main())
```

Note: `ct.format_string()` anchors on the literal `int n = snprintf(json, sizeof(json),` — Task 5's handler uses exactly that line, on purpose. The node id is a literal `7` in the format string rather than a `%d` argument so that this checker can read it; `TN_NODE_ID` in `node.h` serves `root_get` and must stay 7.

- [ ] **Step 2: Run it**

```
python docs/check_terrace_contract.py
```
Expected: `OK: terrace /api/telemetry emits valid JSON with all 57 keys the hub reads.` If it reports a missing key, the format string in Task 5 and this list disagree; fix whichever is wrong against spec §3.

- [ ] **Step 3: Prove it fails when the contract breaks**

Temporarily delete `\"overcurrent\":%s` and its argument from the format in `web.c`, run the checker — expected `terrace telemetry is missing: motors.overcurrent` — then restore it and run again for `OK`.

- [ ] **Step 4: Run every existing checker to prove nothing in the hub tree moved**

```
python docs/check_alerts.py && python docs/check_telemetry.py && python docs/check_offline_view.py && python docs/check_run_hours.py && python docs/check_terrace_contract.py
```
Expected: five `OK` lines.

- [ ] **Step 5: Commit**

```bash
git add docs/check_terrace_contract.py
git commit -F - <<'EOF'
The terrace contract is a list of keys, and a script that fails when one goes

check_terrace_contract.py lifts the node's format string the way
check_telemetry.py lifts the hub's, fills it, parses it, and requires the
57 keys the hub's parser will read. The S3 hub plan copies this list.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Q26A318TTPtPdfD1GbXwbc
EOF
```

---

### Task 7: Bench verification on a spare DevKit

**Files:** none changed unless a step fails. This task produces evidence, recorded in the commit of Task 8.

Prerequisite: a spare 38-pin ESP32 DevKit on USB. No sensors need be attached — every reading will show its sentinel, which is itself a check. If no spare exists, this task runs at cutover on the terrace board (spec §8 step 5) and Task 8 is committed first.

- [ ] **Step 1: Commit is clean, then configure and flash**

```powershell
git status --short          # must print nothing
. C:\Espressif\tools\Microsoft.v5.4.4.PowerShell_profile.ps1 *> $null
Set-Location C:\orbit\git_repos\ro-monitor\firmware\terrace_node
idf.py reconfigure build
idf.py -p COM<n> flash monitor
```
Expected in the monitor: `RO Monitor - Terrace Node  fw <hash>` where `<hash>` is `git rev-parse --short HEAD` with no `-dirty`. On a fresh DevKit: `NVS holds no Wi-Fi credentials - using the Kconfig fallback` (set `TN_WIFI_SSID`/`TN_WIFI_PASS` through `idf.py menuconfig` → Terrace node first, or expect this line and use `/cal` later). Then `up at <ip>`.

- [ ] **Step 2: Telemetry parses and shows sentinels**

From a PC on the same LAN:
```
curl -s http://<ip>/api/telemetry | python -m json.tool
```
Expected: valid JSON; `tanks.rwt.pct` = -1, `tanks.rwt.online` = false, `motors.hpp.deci_amps` = -1, `quality.rwt.ppm` = null, `climate.ro_room.fault` = true, `rs485.errors` climbing, `link.id` = 7, `link.fw` = the hash.

- [ ] **Step 3: Commands**

```
curl -s -d "mode=on"  http://<ip>/api/fan      → on    ; monitor: "fan mode -> On (expires to Auto in 30 min)"
curl -s -d "mode=zzz" http://<ip>/api/fan      → HTTP 400 "mode must be auto, on or off"
curl -s -d "n=3"      http://<ip>/api/relay    → pulsed; monitor: "RELAY TEST: ..." then "... released" 5 s later
curl -s -d "n=9"      http://<ip>/api/relay    → HTTP 400
```
Then `curl -s http://<ip>/api/telemetry | python -c "import sys,json;print(json.load(sys.stdin)['fan']['mode'])"` → `on`.

- [ ] **Step 4: /cal is gated and the terrace fieldsets are present, hub ones absent**

```
curl -s -o /dev/null -w "%{http_code}\n" http://<ip>/cal                 → 401
curl -s -u admin:<password> http://<ip>/cal | grep -o "fieldset id=[a-z]*"
```
Expected ids: `wifi tanks clamps fan wq relays` and no `plant`, no `gf`. (User name and default password are in `docs/CREDENTIALS.md`; on a fresh board the default applies.)

- [ ] **Step 5: OTA and rollback, both directions**

Make a trivial commit (e.g. a comment in `main.c`), `idf.py reconfigure build`, then:
```
curl -s -u admin:<password> --data-binary @build/terrace_node.bin http://<ip>/ota
```
Expected: monitor shows the reboot, the new hash, then within seconds of the first `curl /api/telemetry` from the PC: `OTA image confirmed valid`.

Rollback: repeat with a deliberately broken image is unnecessary — instead, flash the new image and **do not** fetch telemetry for 120 s. Expected: `new image never served the hub - rolling back`, reboot, old hash in the banner. Then fetch telemetry once so the old image stays valid. Revert the trivial commit.

- [ ] **Step 6: Heap**

`link.heap_free` in the telemetry. Record the number. Expected: well above 100 000 with no RainMaker, BLE or dashboard resident. This figure goes in Task 8's doc note.

---

### Task 8: Documentation and the spec correction

**Files:**
- Modify: `docs/superpowers/specs/2026-09-17-hub-split-design.md` (§3 Network paragraph)
- Modify: `docs/DASHBOARD_AND_RAINMAKER.md` (§4.10, after the "Rebooting" paragraph)
- Modify: `architecture.txt` (the ESP32-S HUB box, section 1)
- Create: `firmware/terrace_node/README.md`

- [ ] **Step 1: Correct the spec's Wi-Fi sentence**

In §3 replace:
```
**Network.** Joins the house LAN as a station with credentials in NVS, set
once over serial (the `gf_node` pattern). No AP, no BLE, no mDNS required;
```
with:
```
**Network.** Joins the house LAN as a station with the credentials esp_wifi
already holds in NVS — on the terrace board, the ones BLE provisioning wrote
when it was the hub — falling back to Kconfig values only on a board that has
none. `/cal`'s Wi-Fi fieldset changes them. No AP, no BLE, no mDNS;
```

- [ ] **Step 2: Add the terrace OTA procedure to §4.10**

After the paragraph beginning `**Rebooting:**`, add:

```markdown
**Terrace node OTA (from the hub split onward).** The terrace board no longer
talks to RainMaker; it takes a raw image over the LAN:

    cd firmware/terrace_node
    git status --short            # clean, or the version says -dirty
    idf.py reconfigure build
    curl -u admin:<cal password> --data-binary @build/terrace_node.bin http://<terrace ip>/ota

The node reboots into the new image on trial. It is marked valid the first
time `/api/telemetry` is served — the hub does that within 2 s of the node
being back — or it rolls back after 120 s. A node still on trial refuses a
second `/ota`. Bench-measured free heap on `<hash from Task 7>`: `<figure>`.
```

- [ ] **Step 3: Relabel the terrace box in architecture.txt**

Change the box title line `│                      ESP32-S HUB                        │  (Bus Master, 0x00 - RO Room)` to `│                 ESP32-S TERRACE NODE (id 7)             │  (Bus Master, 0x00 - RO Room)` and replace the line `│  - Local Web Dashboard Server & ESP RainMaker Cloud     │` with `│  - /api/telemetry to the S3 hub (ground floor), /cal    │`. Leave everything else; the RS485 chain below it is unchanged.

- [ ] **Step 4: README for the tree**

`firmware/terrace_node/README.md`:

```markdown
# terrace_node

The terrace ESP32 after the hub split (docs/superpowers/specs/2026-09-17-hub-split-design.md).
It owns the RS485 bus, the dosing ultrasonic, the HPP/RWP optos and clamps,
the Aster contacts, the relays and the fan policy, and serves one JSON to
the S3 hub downstairs. No RainMaker, no BLE, no dashboard.

Build: `idf.py build` from this directory, IDF v5.4.4, target esp32.
Flash the first time over serial; after that `POST /ota` (see
docs/DASHBOARD_AND_RAINMAKER.md 4.10). Commit before building an image you
will flash — the version is the git description.

Endpoints: `GET /api/telemetry` (the contract, checked by
docs/check_terrace_contract.py), `POST /api/fan mode=auto|on|off`,
`POST /api/relay n=1..5`, `POST /ota` (password), `GET /cal` (password),
`GET /logs` (password).

Calibration lives in NVS namespace `ro_cal`, same keys as hub_prod, so a
board that was the hub needs no recalibration.
```

- [ ] **Step 5: Run the checkers once more, then commit**

```
python docs/check_terrace_contract.py && python docs/check_telemetry.py
git add docs architecture.txt firmware/terrace_node/README.md
git commit -F - <<'EOF'
The terrace is a node now; the docs say how to update it and what it holds

Spec corrected on Wi-Fi: the board keeps the credentials provisioning
wrote, no serial step. OTA procedure for the node in 4.10, next to the
hub's. architecture.txt relabels the terrace box.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Q26A318TTPtPdfD1GbXwbc
EOF
```

---

## Self-review against the spec

- §3 Terrace node: modules kept (Task 1), local decisions (Task 2: fan policy, relay test; the Relay 1 interlock is out of scope and nothing here blocks it), telemetry keys (Task 5 + Task 6), POST fan/relay (Task 5), /cal fieldsets (Task 5), /ota with trial guard and 120 s confirm (Tasks 2, 4), 2 s cadence with the JSON being the last completed cycle (Task 2 writes `s_state` once per cycle under the lock; Task 5 reads under the lock). Network wording corrected (Task 8).
- §7 Verification: `check_terrace_contract.py` (Task 6); existing checkers still pass (Task 6 step 4). The fake-node extension and the hub-side parser check belong to Plan 2.
- §8 Steps 1 and 6: Task 7 is step 1; the back-out in step 6 needs nothing from this plan beyond the partition table being identical (Task 1).
- Not in this plan, by design: anything on the S3, the SD card, `gf_node`'s utility role removal, the dashboard.

Type consistency: `fan_mode_request(fan_mode_t)` — declared in `node.h` (Task 1), defined in `main.c` (Task 2), called in `web.c` (Task 5). `relay_test_start(int)` — same. `web_served_once()` — defined in `web.c` (Task 5), used by `ota_watch()` in `main.c` (Task 2). `net_rssi()` — `net.c` (Task 3), used in `web.c` (Task 5). `TN_NODE_ID` — `node.h`; used by `root_get`; the telemetry format carries the id as a literal so the checker can read it.
