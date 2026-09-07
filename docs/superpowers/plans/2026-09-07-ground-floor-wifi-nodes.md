# Ground-floor Wi-Fi Nodes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The terrace hub polls two ground-floor ESP32 nodes over the LAN, at IPs typed on `/cal`, and shows sump level, borewell and sump-motor state, utility-room climate and the RWT float on the dashboard and in RainMaker.

**Architecture:** Nodes are stateless HTTP servers that answer `GET /api/telemetry` with raw readings. A new hub task (`app_gf.c`) polls them on a 5 s cadence with a 3-miss offline latch and a 30 s re-probe, and keeps its own snapshot; the existing 2 s poll task copies that snapshot in each cycle, applies the hub's calibration, and everything downstream (telemetry JSON, history, events, cloud, alerts) sees ordinary `hub_state_t` fields. The node firmware is one ESP-IDF project with a menuconfig role switch and `POST /ota` with rollback.

**Tech Stack:** ESP-IDF 5.4.4 (hub and nodes), `esp_http_client`, `cJSON` (IDF `json` component), `esp_http_server`, `app_update` (OTA), Python 3 host checks compiled with mingw `gcc` (already used by `docs/check_frame.py`), vanilla JS dashboard.

**Spec:** `docs/superpowers/specs/2026-09-07-ground-floor-wifi-nodes-design.md`

## Global Constraints

- ESP-IDF **5.4.4**. On this machine the environment comes from the PowerShell profile `C:\Espressif\tools\Microsoft.v5.4.4.PowerShell_profile.ps1`; every `idf.py` step below assumes that shell.
- Hub build: `cd firmware/hub_prod; idf.py build`. Must finish with no warnings from `main/`. **Do not flash the hub** unless Vamsi asks; the bench check is the fake node in Task 8.
- Nodes report **raw readings only**. All calibration constants live in hub NVS and are set from `/cal`.
- Poll period 5 s online, offline after **3** consecutive misses, re-probe every **30 s** while offline, HTTP timeout **2 s**. Empty IP = not fitted: no polling, no alert.
- Node ids `0x05` Sump, `0x06` Utility. JSON keys exactly as in spec §4 and repeated in Task 3.
- Firmware version on both hub and nodes is `git describe --always --tags --dirty` via `PROJECT_VER`.
- A missing clamp or probe reports `null`, never 0. Tank percent `-1` means "no level".
- Every hub telemetry change must keep `python docs/check_telemetry.py` and `python docs/check_alerts.py` passing.
- Commit after every task with the trailer:
  ```
  Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01HaYFMVZdVLLYoSimHcKgd1
  ```

---

## File structure

**Hub (`firmware/hub_prod/main/`)**

| File | Change | Responsibility |
| :--- | :--- | :--- |
| `app_priv.h` | modify | node addresses, GF cadence constants, new state structs, new RainMaker param names |
| `app_cal.h` / `app_cal.c` | modify | `CAL_TANK_SUMP`, `CAL_CT_BORE`, `CAL_CT_SUMP`, `press_range_mm`, `run_deci_amps`, node IP strings, pure maths (`gfLoopDistanceMM`, `gfPhaseDeciAmps`, `gfImbalancePct`) |
| `app_gf.h` / `app_gf.c` | **create** | polling task, HTTP GET, JSON parse, offline latch, snapshot |
| `app_main.c` | modify | `gf_apply()` into `hub_state_t`, events, history, cloud params, node-lost alert |
| `app_web.c` | modify | telemetry JSON, history rows, `/cal` fieldsets, `POST /api/cal/gf` |
| `CMakeLists.txt` | modify | add `app_gf.c`, `PRIV_REQUIRES esp_http_client json` |

**Dashboard** `firmware/hub/data/dashboard.html`: utility room card, borewell phases, node ip/fw, trend columns, demo data.

**Host checks** `docs/check_gf.py` (**create**), `docs/check_telemetry.py` (modify), `docs/fake_gf_node.py` (**create**).

**Node (`firmware/gf_node/`)** — all **create**

| File | Responsibility |
| :--- | :--- |
| `CMakeLists.txt`, `partitions.csv`, `sdkconfig.defaults`, `README.md` | project, two OTA slots, rollback, docs |
| `main/CMakeLists.txt`, `main/Kconfig.projbuild` | sources, role/Wi-Fi/IP menuconfig |
| `main/gf.h` | shared constants, GPIO map, the three module APIs |
| `main/main.c` | boot, OTA validity, sample loop |
| `main/net.c` | Wi-Fi station with static IP, reconnect, `net_up()`, `net_rssi()` |
| `main/web.c` | httpd: `GET /`, `GET /api/telemetry`, `POST /ota` |
| `main/ota.c` | streamed write into the passive slot, reboot |
| `main/sensors_sump.c` | ultrasonic or 4-20 mA loop, `J-PRESS` |
| `main/sensors_util.c` | six CT RMS channels, `PUMP ON`, RWT floaty, SHT30 |

**Docs**: `RS485_PROTOCOL.md` §5, `WIRING.md` §11, `DASHBOARD_AND_RAINMAKER.md`, `firmware/hub_prod/README.md`.

---

### Task 1: Hub calibration — sump tank, two remote clamp sets, node IPs, pure maths

**Files:**
- Modify: `firmware/hub_prod/main/app_priv.h`
- Modify: `firmware/hub_prod/main/app_cal.h`
- Modify: `firmware/hub_prod/main/app_cal.c`
- Modify: `firmware/hub_prod/main/app_web.c:837-862` (`cal_ct_post`, the only `cal_set_ct` caller)
- Create: `docs/check_gf.py`

**Interfaces:**
- Produces:
  ```c
  typedef enum { CAL_TANK_RWT=0, CAL_TANK_TWT=1, CAL_TANK_DOS=2, CAL_TANK_SUMP=3, CAL_TANK_COUNT } cal_tank_t;
  typedef enum { CAL_CT_HPP=0, CAL_CT_RWP=1, CAL_CT_BORE=2, CAL_CT_SUMP=3, CAL_CT_COUNT } cal_ct_t;
  typedef enum { CAL_GF_SUMP=0, CAL_GF_UTIL=1, CAL_GF_COUNT } cal_gf_t;
  /* cal_tank_cfg_t gains */ uint16_t press_range_mm;   /* 0 = ultrasonic */
  /* cal_ct_cfg_t gains   */ uint16_t run_deci_amps;    /* "running" threshold */
  esp_err_t cal_set_press_range(cal_tank_t t, uint16_t range_mm);            /* 0 or 500..10000 */
  esp_err_t cal_set_ct(cal_ct_t c, uint16_t apv_x100, uint8_t turns, uint16_t oc_deci_amps, uint16_t run_deci_amps);
  const char *cal_gf_ip(cal_gf_t n);                    /* "" when not configured; may carry ":port" */
  esp_err_t cal_set_gf_ip(cal_gf_t n, const char *ip); /* "" clears */
  const char *cal_gf_key(cal_gf_t n);                   /* "sump", "util" */
  /* pure, host-testable */
  uint16_t gfLoopDistanceMM(uint32_t loop_ua, uint16_t range_mm);   /* 0 = no usable reading */
  int16_t  gfPhaseDeciAmps(uint16_t rms_mv, uint16_t apv_x100, uint8_t turns); /* -1 = no clamp */
  uint8_t  gfImbalancePct(const int16_t da[3]);                     /* 0 when <2 phases */
  ```
- Constants in `app_priv.h`: `CT_NOISE_FLOOR_MV 15`, `OC_BORE_DECI_A_DEFAULT 150`, `OC_SUMP_DECI_A_DEFAULT 100`, `RUN_DECI_A_DEFAULT 10`, `PRESS_MIN_UA 3500`, `PRESS_MAX_UA 21000`, `NODE_ADDR_SUMP 0x05`, `NODE_ADDR_UTILITY 0x06`.

- [ ] **Step 1: Write the failing host check**

Create `docs/check_gf.py`. It lifts the block between `/* GF_PURE_BEGIN */` and `/* GF_PURE_END */` out of `app_cal.c`, compiles it with gcc and pins the endpoints.

```python
"""Pin the ground-floor pure maths in app_cal.c: loop current to distance,
clamp millivolts to deci-amps, and phase imbalance. Same approach as
check_frame.py - the code is lifted out of the firmware and built with gcc,
so what is tested is what ships.

    python docs/check_gf.py     # from the repo root
"""
import io, os, subprocess, sys, tempfile

CAL = 'firmware/hub_prod/main/app_cal.c'
BEGIN, END = '/* GF_PURE_BEGIN */', '/* GF_PURE_END */'

HARNESS = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#define CT_NOISE_FLOOR_MV 15
#define PRESS_MIN_UA 3500
#define PRESS_MAX_UA 21000
%s
static int fails = 0;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL %%s\n", #expr); fails++; } } while (0)
int main(void) {
    /* loop: 4 mA = full range (empty tank), 20 mA = 0 mm head... distance-alike */
    CHECK(gfLoopDistanceMM(4000, 2000) == 2000);
    CHECK(gfLoopDistanceMM(20000, 2000) == 1);      /* clamped to range-1 head */
    CHECK(gfLoopDistanceMM(12000, 2000) == 1000);
    CHECK(gfLoopDistanceMM(3400, 2000) == 0);       /* open loop */
    CHECK(gfLoopDistanceMM(21500, 2000) == 0);      /* shorted */
    CHECK(gfLoopDistanceMM(12000, 0) == 0);         /* ultrasonic tank, no range */
    /* clamp: 30 A/V, 1 turn: 100 mV rms = 3.0 A */
    CHECK(gfPhaseDeciAmps(100, 3000, 1) == 30);
    CHECK(gfPhaseDeciAmps(100, 3000, 2) == 15);
    CHECK(gfPhaseDeciAmps(10, 3000, 1) == -1);      /* under the noise floor: no clamp */
    CHECK(gfPhaseDeciAmps(15, 3000, 1) == 4);       /* exactly the floor counts */
    int16_t a[3] = { 40, 40, 40 }; CHECK(gfImbalancePct(a) == 0);
    int16_t b[3] = { 40, 30, -1 }; CHECK(gfImbalancePct(b) == 25);
    int16_t c[3] = { 40, -1, -1 }; CHECK(gfImbalancePct(c) == 0);
    int16_t d[3] = { -1, -1, -1 }; CHECK(gfImbalancePct(d) == 0);
    printf(fails ? "check_gf: %%d FAILED\n" : "check_gf: OK\n", fails);
    return fails ? 1 : 0;
}
'''

def main():
    src = io.open(CAL, encoding='utf-8').read()
    if BEGIN not in src or END not in src:
        print('markers %s / %s not found in %s' % (BEGIN, END, CAL)); return 1
    block = src[src.index(BEGIN) + len(BEGIN):src.index(END)]
    d = tempfile.mkdtemp()
    c_path, exe = os.path.join(d, 'gf.c'), os.path.join(d, 'gf.exe')
    io.open(c_path, 'w', encoding='utf-8').write(HARNESS % block)
    if subprocess.call(['gcc', '-Wall', '-Wextra', '-Werror', '-o', exe, c_path]) != 0:
        print('gcc refused the extracted code -- that is the finding.'); return 1
    return subprocess.call([exe])

if __name__ == '__main__':
    sys.exit(main())
```

- [ ] **Step 2: Run it, expect failure**

Run: `python docs/check_gf.py`
Expected: `markers /* GF_PURE_BEGIN */ ... not found`.

- [ ] **Step 3: Constants in `app_priv.h`**

After `#define NODE_ADDR_BATTERY 0x04` (line 145) add:

```c
#define NODE_ADDR_SUMP          0x05    /* ground floor, Wi-Fi, polled by app_gf.c */
#define NODE_ADDR_UTILITY       0x06
```

After `#define CT_RMS_INTERVAL_US 500` (line 242) add:

```c
/* Remote clamps report raw RMS mV; a channel with no clamp sits on its bias
 * pedestal and reads a few mV of noise. Under this it is "no clamp", reported
 * as null - never 0 A, which is what an idle motor reads. */
#define CT_NOISE_FLOOR_MV       15
/* 4-20 mA loop sanity band, from ro_node.ino: under it the loop is open or
 * unpowered, over it shorted or miswired. Neither is a level. */
#define PRESS_MIN_UA            3500
#define PRESS_MAX_UA            21000
#define PRESS_RANGE_MIN_MM      500
#define PRESS_RANGE_MAX_MM      10000
```

After `#define OC_RWP_DECI_A_DEFAULT 90` (line 345) add:

```c
#define OC_BORE_DECI_A_DEFAULT  150     /* 15.0 A - no nameplate; set from the overload dial (WIRING.md 11.3.1) */
#define OC_SUMP_DECI_A_DEFAULT  100     /* 10.0 A - 2 HP three-phase submersible */
#define RUN_DECI_A_DEFAULT      10      /*  1.0 A - highest phase above this = running */
```

- [ ] **Step 4: Header changes in `app_cal.h`**

Replace the two enums and two structs (lines 20-54) so they read:

```c
typedef enum {
    CAL_TANK_RWT = 0,
    CAL_TANK_TWT = 1,
    CAL_TANK_DOS = 2,
    CAL_TANK_SUMP = 3,     /* ground floor, node 0x05 over Wi-Fi */
    CAL_TANK_COUNT,
} cal_tank_t;

typedef enum {
    CAL_CT_HPP = 0,
    CAL_CT_RWP = 1,
    CAL_CT_BORE = 2,       /* node 0x06: one calibration for all three phases */
    CAL_CT_SUMP = 3,       /* node 0x06: one calibration for all three channels */
    CAL_CT_COUNT,
} cal_ct_t;

typedef enum {
    CAL_GF_SUMP = 0,
    CAL_GF_UTIL = 1,
    CAL_GF_COUNT,
} cal_gf_t;

typedef struct {
    uint16_t full_mm;
    uint16_t empty_mm;
    uint16_t tds_k_x100;
    uint8_t  tds_min_pct;
    /* Sump only. Full-scale head of a 4-20 mA submersible transducer, in mm.
     * 0 = no transducer, the ultrasonic is the source. The node reports raw
     * microamps and gfLoopDistanceMM() turns them into the same distance-alike
     * figure the ultrasonic gives, so full/empty above apply unchanged
     * (WIRING.md 9.4.1). */
    uint16_t press_range_mm;
} cal_tank_cfg_t;

typedef struct {
    uint16_t amps_per_volt_x100;
    uint8_t  turns;
    uint16_t oc_deci_amps;
    /* Highest phase at or above this = motor running. Only the remote motors
     * use it (the hub's own pumps have contactor optos); 1.0 A default. */
    uint16_t run_deci_amps;
} cal_ct_cfg_t;
```

Keep the existing comments on the fields you did not change. Then change the `cal_set_ct` prototype and add the new ones after `cal_set_fan`:

```c
esp_err_t cal_set_ct(cal_ct_t c, uint16_t amps_per_volt_x100, uint8_t turns,
                     uint16_t oc_deci_amps, uint16_t run_deci_amps);
esp_err_t cal_set_press_range(cal_tank_t t, uint16_t range_mm);

/* Ground-floor node addresses. "" = not fitted: never polled, never alerted.
 * Dotted quad, optionally ":port" - the port is for the bench fake node. */
const char *cal_gf_ip(cal_gf_t n);
const char *cal_gf_key(cal_gf_t n);
esp_err_t   cal_set_gf_ip(cal_gf_t n, const char *ip);

/* Pure ground-floor maths, host-tested by docs/check_gf.py. */
uint16_t gfLoopDistanceMM(uint32_t loop_ua, uint16_t range_mm);
int16_t  gfPhaseDeciAmps(uint16_t rms_mv, uint16_t amps_per_volt_x100, uint8_t turns);
uint8_t  gfImbalancePct(const int16_t deci_amps[3]);
```

- [ ] **Step 5: Implementation in `app_cal.c`**

Defaults (lines 27-36) become:

```c
static cal_tank_cfg_t s_tanks[CAL_TANK_COUNT] = {
    [CAL_TANK_RWT]  = { .full_mm = 300, .empty_mm = 1500, .tds_k_x100 = 100, .tds_min_pct = 90 },
    [CAL_TANK_TWT]  = { .full_mm = 300, .empty_mm = 1500, .tds_k_x100 = 100, .tds_min_pct = 90 },
    [CAL_TANK_DOS]  = { .full_mm = 250, .empty_mm = 540,  .tds_k_x100 = 100, .tds_min_pct = 0 },
    /* 3.5 m shaft, WIRING.md 9 (Sump). Uncalibrated until someone measures it. */
    [CAL_TANK_SUMP] = { .full_mm = 300, .empty_mm = 3500, .tds_k_x100 = 100, .tds_min_pct = 0, .press_range_mm = 0 },
};

static cal_ct_cfg_t s_cts[CAL_CT_COUNT] = {
    [CAL_CT_HPP]  = { .amps_per_volt_x100 = 3000, .turns = 1, .oc_deci_amps = OC_HPP_DECI_A_DEFAULT,  .run_deci_amps = RUN_DECI_A_DEFAULT },
    [CAL_CT_RWP]  = { .amps_per_volt_x100 = 3000, .turns = 1, .oc_deci_amps = OC_RWP_DECI_A_DEFAULT,  .run_deci_amps = RUN_DECI_A_DEFAULT },
    [CAL_CT_BORE] = { .amps_per_volt_x100 = 3000, .turns = 1, .oc_deci_amps = OC_BORE_DECI_A_DEFAULT, .run_deci_amps = RUN_DECI_A_DEFAULT },
    [CAL_CT_SUMP] = { .amps_per_volt_x100 = 3000, .turns = 1, .oc_deci_amps = OC_SUMP_DECI_A_DEFAULT, .run_deci_amps = RUN_DECI_A_DEFAULT },
};
```

Key and label tables (lines 45-52):

```c
static const char *s_tank_keys[CAL_TANK_COUNT]   = { "rwt", "twt", "dos", "sump" };
static const char *s_tank_labels[CAL_TANK_COUNT] = { "Raw Water", "Treated Water", "Dosing", "Sump" };
static const char *s_ct_keys[CAL_CT_COUNT]       = { "hpp", "rwp", "bore", "smot" };
static const char *s_ct_labels[CAL_CT_COUNT]     = { "HPP", "RWP", "Borewell", "Sump motor" };
static const char *s_gf_keys[CAL_GF_COUNT]       = { "sump", "util" };
static const char *s_gf_nvs[CAL_GF_COUNT]        = { "gf_sump", "gf_util" };
static char        s_gf_ip[CAL_GF_COUNT][24];    /* "255.255.255.255:65535" fits */
```

Accessors next to the existing ones (line 54-62):

```c
const char *cal_gf_key(cal_gf_t n) { return (n < CAL_GF_COUNT) ? s_gf_keys[n] : "?"; }
const char *cal_gf_ip(cal_gf_t n)  { return (n < CAL_GF_COUNT) ? s_gf_ip[n] : ""; }
```

In `cal_init()` extend the tank loop and the CT loop, and load the IPs:

```c
    for (int i = 0; i < CAL_TANK_COUNT; i++) {
        load_u16(h, s_tank_keys[i], "f", &s_tanks[i].full_mm);
        load_u16(h, s_tank_keys[i], "e", &s_tanks[i].empty_mm);
        load_u16(h, s_tank_keys[i], "k", &s_tanks[i].tds_k_x100);
        load_u16(h, s_tank_keys[i], "r", &s_tanks[i].press_range_mm);
        uint16_t mp = s_tanks[i].tds_min_pct;
        load_u16(h, s_tank_keys[i], "m", &mp);
        s_tanks[i].tds_min_pct = (mp <= 100) ? (uint8_t)mp : 90;
    }
    for (int i = 0; i < CAL_CT_COUNT; i++) {
        load_u16(h, s_ct_keys[i], "s", &s_cts[i].amps_per_volt_x100);
        load_u16(h, s_ct_keys[i], "o", &s_cts[i].oc_deci_amps);
        load_u16(h, s_ct_keys[i], "n", &s_cts[i].run_deci_amps);
        uint16_t turns = s_cts[i].turns;
        load_u16(h, s_ct_keys[i], "t", &turns);
        s_cts[i].turns = (turns >= 1 && turns <= 10) ? (uint8_t)turns : 1;
    }
    for (int i = 0; i < CAL_GF_COUNT; i++) {
        size_t len = sizeof(s_gf_ip[i]);
        if (nvs_get_str(h, s_gf_nvs[i], s_gf_ip[i], &len) != ESP_OK) {
            s_gf_ip[i][0] = '\0';
        }
    }
```

`cal_set_ct` gains the parameter and stores it:

```c
esp_err_t cal_set_ct(cal_ct_t c, uint16_t amps_per_volt_x100, uint8_t turns,
                     uint16_t oc_deci_amps, uint16_t run_deci_amps)
{
    if (c >= CAL_CT_COUNT) return ESP_ERR_INVALID_ARG;
    if (amps_per_volt_x100 < 100 || amps_per_volt_x100 > 20000) return ESP_ERR_INVALID_ARG;
    if (turns < 1 || turns > 10) return ESP_ERR_INVALID_ARG;
    if (oc_deci_amps < OC_LIMIT_LOW_DECI || oc_deci_amps > OC_LIMIT_HIGH_DECI) return ESP_ERR_INVALID_ARG;
    /* Running threshold must sit below the trip, or a motor could trip before it
     * is ever "running", and 0 would make a floating channel a running motor. */
    if (run_deci_amps < 1 || run_deci_amps >= oc_deci_amps) return ESP_ERR_INVALID_ARG;

    s_cts[c].amps_per_volt_x100 = amps_per_volt_x100;
    s_cts[c].turns = turns;
    s_cts[c].oc_deci_amps = oc_deci_amps;
    s_cts[c].run_deci_amps = run_deci_amps;

    esp_err_t err = store_u16(s_ct_keys[c], "s", amps_per_volt_x100);
    if (err == ESP_OK) err = store_u16(s_ct_keys[c], "t", turns);
    if (err == ESP_OK) err = store_u16(s_ct_keys[c], "o", oc_deci_amps);
    if (err == ESP_OK) err = store_u16(s_ct_keys[c], "n", run_deci_amps);
    ESP_LOGI(TAG, "%s CT: %u.%02u A/V, %u turns, run at %u.%u A, OC at %u.%u A",
             s_ct_keys[c], amps_per_volt_x100 / 100, amps_per_volt_x100 % 100, turns,
             run_deci_amps / 10, run_deci_amps % 10, oc_deci_amps / 10, oc_deci_amps % 10);
    return err;
}
```

New setters, after `cal_set_fan`:

```c
esp_err_t cal_set_press_range(cal_tank_t t, uint16_t range_mm)
{
    if (t >= CAL_TANK_COUNT) return ESP_ERR_INVALID_ARG;
    if (range_mm != 0 && (range_mm < PRESS_RANGE_MIN_MM || range_mm > PRESS_RANGE_MAX_MM)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_tanks[t].press_range_mm = range_mm;
    ESP_LOGI(TAG, "%s transducer range: %u mm%s", s_tank_keys[t], range_mm,
             range_mm ? "" : " (ultrasonic)");
    return store_u16(s_tank_keys[t], "r", range_mm);
}

/* a.b.c.d or a.b.c.d:port, nothing else. "" clears. */
static bool gf_ip_valid(const char *s)
{
    if (s[0] == '\0') return true;
    unsigned a, b, c, d, port = 80; char tail[2] = "";
    int n = sscanf(s, "%u.%u.%u.%u:%u%1s", &a, &b, &c, &d, &port, tail);
    if (n < 4 || tail[0] != '\0') return false;
    if (n == 4 && strchr(s, ':') != NULL) return false;
    return a < 256 && b < 256 && c < 256 && d < 256 && port >= 1 && port <= 65535;
}

esp_err_t cal_set_gf_ip(cal_gf_t n, const char *ip)
{
    if (n >= CAL_GF_COUNT || ip == NULL || strlen(ip) >= sizeof(s_gf_ip[n]) || !gf_ip_valid(ip)) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_gf_ip[n], ip, sizeof(s_gf_ip[n]) - 1);
    s_gf_ip[n][sizeof(s_gf_ip[n]) - 1] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = ip[0] ? nvs_set_str(h, s_gf_nvs[n], ip) : nvs_erase_key(h, s_gf_nvs[n]);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;      /* clearing an unset key */
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "ground-floor %s node: %s", s_gf_keys[n], ip[0] ? ip : "not fitted");
    return err;
}
```

Pure block, placed after `tdsPPM()` and before `cal_plant_lph()`:

```c
/* GF_PURE_BEGIN */
/* 4-20 mA loop to a distance-alike figure. The node reports microamps; the
 * transducer's full scale is a /cal number. head grows with the water, so
 * range - head SHRINKS as the tank fills - the same direction an ultrasonic
 * reads - and levelPercent() with the usual full/empty applies unchanged
 * (WIRING.md 9.4.1). 0 = no usable reading: loop open, shorted or no range. */
uint16_t gfLoopDistanceMM(uint32_t loop_ua, uint16_t range_mm)
{
    if (range_mm == 0) return 0;
    if (loop_ua < PRESS_MIN_UA || loop_ua > PRESS_MAX_UA) return 0;
    int32_t head = ((int32_t)loop_ua - 4000L) * (int32_t)range_mm / 16000L;
    if (head < 0) head = 0;
    if (head > (int32_t)range_mm - 1) head = (int32_t)range_mm - 1;
    return (uint16_t)((int32_t)range_mm - head);
}

/* Raw RMS millivolts from a remote clamp to deci-amps, or -1 for "no clamp"
 * when the channel is under the noise floor. amps = V * (A/V) / turns. */
int16_t gfPhaseDeciAmps(uint16_t rms_mv, uint16_t amps_per_volt_x100, uint8_t turns)
{
    if (rms_mv < CT_NOISE_FLOOR_MV) return -1;
    if (turns == 0) turns = 1;
    uint32_t deci = ((uint32_t)rms_mv * amps_per_volt_x100) / (100UL * 100UL * turns);
    /* mV/1000 V * (x100/100) A/V * 10 for deci = mv * x100 / 10000 */
    if (deci > 9990) deci = 9990;
    return (int16_t)deci;
}

/* (max - min) / max over the phases that have a clamp. Two or more needed. */
uint8_t gfImbalancePct(const int16_t deci_amps[3])
{
    int16_t hi = -1, lo = 32767; int n = 0;
    for (int i = 0; i < 3; i++) {
        if (deci_amps[i] < 0) continue;
        n++;
        if (deci_amps[i] > hi) hi = deci_amps[i];
        if (deci_amps[i] < lo) lo = deci_amps[i];
    }
    if (n < 2 || hi <= 0) return 0;
    return (uint8_t)(((int32_t)(hi - lo) * 100) / hi);
}
/* GF_PURE_END */
```

`gfPhaseDeciAmps(100, 3000, 1)`: 100 × 3000 / 10000 = 30 ✓. `(15, 3000, 1)` = 4 ✓.

Add `#include <string.h>` and `#include <stdio.h>` at the top of `app_cal.c` if not present.

- [ ] **Step 6: Update the one caller in `app_web.c`**

In `cal_ct_post` (line 837) read a `run` field and pass it:

```c
    char apv[16], turns[8], oc[16], run[16];
    if (!form_field(body, "apv", apv, sizeof(apv)) ||
        !form_field(body, "turns", turns, sizeof(turns)) ||
        !form_field(body, "oc", oc, sizeof(oc)) ||
        !form_field(body, "run", run, sizeof(run))) {
        return bad(req, "need apv, turns, oc and run");
    }

    uint16_t apv_x100 = 0, oc_deci = 0, run_deci = 0;
    if (!parse_x100(apv, &apv_x100)) return bad(req, "amps per volt not a number");
    if (!parse_deci(oc, &oc_deci))   return bad(req, "trip current not a number");
    if (!parse_deci(run, &run_deci)) return bad(req, "run current not a number");
    unsigned t = (unsigned)strtoul(turns, NULL, 10);

    if (cal_set_ct((cal_ct_t)idx, apv_x100, (uint8_t)t, oc_deci, run_deci) != ESP_OK) {
        return bad(req, "rejected: check A/V (1-200), turns (1-10), run (>= 0.1 A) and trip (1.0-30.0 A, above run)");
    }
```

And in `cal_get`'s clamp loop (line 575-593) add the input so the form still submits: after `"trip A <input name=oc size=5 value='%u.%u'> "` insert `"run A <input name=run size=5 value='%u.%u'> "` and the matching arguments `c->run_deci_amps / 10, c->run_deci_amps % 10` after the `oc` pair. The live-value arrays in that function (`live[]`, `live_pct[]`, `ct_lo[]`, `ct_hi[]`, `ct_a[]`) are sized by the enums and now need four entries each; until Task 4 fills them from the new state, initialise the extra entries to 0 / -1:

```c
    uint16_t live[CAL_TANK_COUNT]     = { s->rwt.distance_mm, s->twt.distance_mm, s->dosing.distance_mm, 0 };
    int16_t  live_pct[CAL_TANK_COUNT] = { s->rwt.pct, s->twt.pct, s->dosing.pct, -1 };
    uint32_t ct_lo[CAL_CT_COUNT] = { s->hpp.mv_lo, s->rwp.mv_lo, 0, 0 };
    uint32_t ct_hi[CAL_CT_COUNT] = { s->hpp.mv_hi, s->rwp.mv_hi, 0, 0 };
    int16_t  ct_a[CAL_CT_COUNT]  = { s->hpp.deci_amps, s->rwp.deci_amps, -1, -1 };
```

Also check `app_main.c` for any array indexed by `CAL_CT_COUNT` or `CAL_TANK_COUNT` that lists initialisers per entry — `ct_read_deci_amps` in `app_sensors.c` uses `static median_u16_t win[CAL_CT_COUNT]`, which grows harmlessly. `cal_wq_post` rejects `idx >= CAL_TANK_COUNT - 1`; change that to `idx > 1` so the sump does not become a TDS candidate.

- [ ] **Step 7: Run the host check and build**

Run: `python docs/check_gf.py`
Expected: `check_gf: OK`

Run: `cd firmware/hub_prod; idf.py build`
Expected: builds, no warnings from `main/`.

- [ ] **Step 8: Commit**

```bash
git add docs/check_gf.py firmware/hub_prod/main/app_priv.h firmware/hub_prod/main/app_cal.h firmware/hub_prod/main/app_cal.c firmware/hub_prod/main/app_web.c
git commit -m "Hub cal: sump tank, borewell and sump-motor clamps, node IPs, loop and phase maths"
```

---

### Task 2: Hub state and the ground-floor client module (parse + offline latch)

**Files:**
- Modify: `firmware/hub_prod/main/app_priv.h` (structs, constants)
- Create: `firmware/hub_prod/main/app_gf.h`
- Create: `firmware/hub_prod/main/app_gf.c`
- Modify: `firmware/hub_prod/main/CMakeLists.txt`
- Modify: `docs/check_gf.py`

**Interfaces:**
- Consumes: `NODE_ADDR_SUMP/UTILITY`, `cal_gf_ip()`, `cal_gf_t`.
- Produces (`app_gf.h`):
  ```c
  typedef struct {
      bool     valid;        /* at least one good reply since boot */
      bool     online;
      uint8_t  misses;
      int64_t  last_ok_us;
      int64_t  next_poll_us;
      char     fw[16];
      int8_t   rssi;
      uint32_t uptime_s;
  } gf_link_t;

  typedef struct {
      gf_link_t       link;
      bool            pressure;      /* "source":"pressure" */
      uint16_t        distance_mm;   /* ultrasonic; 0 = none */
      uint8_t         quality;
      sensor_status_t sensor;
      uint32_t        loop_ua;       /* pressure; 0 = none */
  } gf_sump_t;

  typedef struct {
      gf_link_t link;
      uint16_t  bore_mv[3];
      uint16_t  sump_mv[3];
      bool      sump_on;
      int8_t    rwt_floty;           /* -1 null, 0 open, 1 closed */
      int16_t   temp_deci_c;
      uint16_t  hum_deci_pct;
      bool      sht_ok;
  } gf_util_t;

  esp_err_t gf_init(void);                          /* starts the task (Task 3) */
  void      gf_snapshot(gf_sump_t *s, gf_util_t *u); /* copy under gf lock */
  /* pure */
  bool gf_parse_sump(const char *json, gf_sump_t *out);   /* link.* untouched */
  bool gf_parse_util(const char *json, gf_util_t *out);
  void gf_link_result(gf_link_t *l, bool ok, int64_t now_us);
  ```
- Constants in `app_priv.h`: `GF_POLL_MS 5000`, `GF_REPROBE_MS 30000`, `GF_OFFLINE_MISSES 3`, `GF_HTTP_TIMEOUT_MS 2000`, `GF_REPLY_MAX 512`.
- New `hub_state_t` members (Task 3 fills them; declared now so Task 4's telemetry can compile against them):
  ```c
  typedef struct {
      bool     running;
      int16_t  deci_amps;      /* highest phase, -1 none */
      int16_t  phase_da[3];    /* -1 = no clamp */
      uint8_t  imbalance_pct;
  } gf_motor_state_t;
  /* in hub_state_t: */
  tank_state_t     sump;
  bool             sump_pressure;
  gf_motor_state_t borewell, sump_motor;
  climate_state_t  utility_room;
  int8_t           rwt_floty;      /* -1 unknown */
  bool             sump_configured, utility_configured;
  bool             sump_online, utility_online;
  int64_t          sump_last_us, utility_last_us;
  char             sump_fw[16], utility_fw[16];
  ```
  `hist_rec_t` gains `int8_t sump; int16_t bore_da, smot_da, util_t;`.

- [ ] **Step 1: Extend the host check for the parser and the latch**

`gf_parse_*` uses cJSON, which lives in the IDF tree. Add a second harness to `docs/check_gf.py` that compiles `app_gf.c`'s pure block with cJSON from `IDF_PATH`. Append to the file:

```python
GF = 'firmware/hub_prod/main/app_gf.c'
GF_BEGIN, GF_END = '/* GF_PARSE_BEGIN */', '/* GF_PARSE_END */'

PARSE_HARNESS = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "cJSON.h"
typedef enum { SENSOR_OK=0, SENSOR_BLIND=1, SENSOR_NO_ECHO=2, SENSOR_HW_FAULT=3 } sensor_status_t;
#define GF_POLL_MS 5000
#define GF_REPROBE_MS 30000
#define GF_OFFLINE_MISSES 3
typedef struct { bool valid; bool online; uint8_t misses; int64_t last_ok_us; int64_t next_poll_us;
                 char fw[16]; int8_t rssi; uint32_t uptime_s; } gf_link_t;
typedef struct { gf_link_t link; bool pressure; uint16_t distance_mm; uint8_t quality;
                 sensor_status_t sensor; uint32_t loop_ua; } gf_sump_t;
typedef struct { gf_link_t link; uint16_t bore_mv[3]; uint16_t sump_mv[3]; bool sump_on;
                 int8_t rwt_floty; int16_t temp_deci_c; uint16_t hum_deci_pct; bool sht_ok; } gf_util_t;
%s
static int fails = 0;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL %%s\n", #expr); fails++; } } while (0)
int main(void) {
    gf_sump_t s = {0};
    CHECK(gf_parse_sump("{\"id\":5,\"fw\":\"a3cb57d\",\"uptime_s\":3840,\"rssi\":-64,"
                        "\"source\":\"ultrasonic\",\"distance_mm\":1750,\"quality\":95,\"status\":\"OK\","
                        "\"loop_ua\":null}", &s));
    CHECK(!s.pressure && s.distance_mm == 1750 && s.quality == 95 && s.sensor == SENSOR_OK && s.loop_ua == 0);
    CHECK(strcmp(s.link.fw, "a3cb57d") == 0 && s.link.rssi == -64 && s.link.uptime_s == 3840);
    CHECK(gf_parse_sump("{\"id\":5,\"fw\":\"x\",\"uptime_s\":1,\"rssi\":-70,\"source\":\"pressure\","
                        "\"distance_mm\":null,\"quality\":null,\"status\":\"HW_FAULT\",\"loop_ua\":2100}", &s));
    CHECK(s.pressure && s.distance_mm == 0 && s.loop_ua == 2100 && s.sensor == SENSOR_HW_FAULT);
    CHECK(!gf_parse_sump("{\"id\":6,\"fw\":\"x\"}", &s));          /* wrong node */
    CHECK(!gf_parse_sump("{\"id\":5,\"fw\":\"x\"", &s));            /* truncated */
    gf_util_t u = {0};
    CHECK(gf_parse_util("{\"id\":6,\"fw\":\"b\",\"uptime_s\":9,\"rssi\":-68,\"bore_mv\":[412,405,398],"
                        "\"sump_mv\":[398,0,0],\"sump_on\":true,\"rwt_floty\":null,"
                        "\"t_deci_c\":312,\"rh_deci_pct\":548,\"sht_ok\":true}", &u));
    CHECK(u.bore_mv[0] == 412 && u.bore_mv[2] == 398 && u.sump_mv[0] == 398 && u.sump_mv[2] == 0);
    CHECK(u.sump_on && u.rwt_floty == -1 && u.temp_deci_c == 312 && u.hum_deci_pct == 548 && u.sht_ok);
    CHECK(gf_parse_util("{\"id\":6,\"fw\":\"b\",\"uptime_s\":9,\"rssi\":-68,\"bore_mv\":[0,0,0],"
                        "\"sump_mv\":[0,0,0],\"sump_on\":false,\"rwt_floty\":false,"
                        "\"t_deci_c\":0,\"rh_deci_pct\":0,\"sht_ok\":false}", &u));
    CHECK(u.rwt_floty == 0 && !u.sht_ok);
    CHECK(!gf_parse_util("{\"id\":6,\"fw\":\"b\",\"bore_mv\":[1,2]}", &u));   /* short array */
    /* latch: 3 misses to offline, reprobe slower, first reply restores */
    gf_link_t l = {0}; int64_t t = 1000000;
    gf_link_result(&l, true, t);  CHECK(l.online && l.valid && l.misses == 0 && l.next_poll_us == t + 5000000LL);
    gf_link_result(&l, false, t); CHECK(l.online && l.misses == 1 && l.next_poll_us == t + 5000000LL);
    gf_link_result(&l, false, t); CHECK(l.online && l.misses == 2);
    gf_link_result(&l, false, t); CHECK(!l.online && l.misses == 3 && l.next_poll_us == t + 30000000LL);
    gf_link_result(&l, false, t); CHECK(!l.online && l.misses == 4 && l.next_poll_us == t + 30000000LL);
    gf_link_result(&l, true, t + 7); CHECK(l.online && l.misses == 0 && l.last_ok_us == t + 7 && l.next_poll_us == t + 7 + 5000000LL);
    printf(fails ? "check_gf parse: %%d FAILED\n" : "check_gf parse: OK\n", fails);
    return fails ? 1 : 0;
}
'''

def idf_cjson():
    idf = os.environ.get('IDF_PATH') or r'C:\Espressif\frameworks\esp-idf-v5.4.4'
    d = os.path.join(idf, 'components', 'json', 'cJSON')
    if not os.path.isfile(os.path.join(d, 'cJSON.c')):
        # esp-idf-v5.4.4 may live one level deeper; look for any cJSON.c under C:\Espressif
        for root, _, files in os.walk(r'C:\Espressif'):
            if 'cJSON.c' in files and root.endswith('cJSON'):
                return root
        return None
    return d

def main_parse():
    src = io.open(GF, encoding='utf-8').read()
    if GF_BEGIN not in src or GF_END not in src:
        print('markers %s / %s not found in %s' % (GF_BEGIN, GF_END, GF)); return 1
    cj = idf_cjson()
    if cj is None:
        print('cJSON.c not found - set IDF_PATH'); return 1
    block = src[src.index(GF_BEGIN) + len(GF_BEGIN):src.index(GF_END)]
    d = tempfile.mkdtemp()
    c_path, exe = os.path.join(d, 'gfp.c'), os.path.join(d, 'gfp.exe')
    io.open(c_path, 'w', encoding='utf-8').write(PARSE_HARNESS % block)
    if subprocess.call(['gcc', '-Wall', '-Wextra', '-Werror', '-I', cj, '-o', exe, c_path,
                        os.path.join(cj, 'cJSON.c')]) != 0:
        print('gcc refused the extracted parser -- that is the finding.'); return 1
    return subprocess.call([exe])
```

and change the `__main__` block to `sys.exit(main() or main_parse())`.

Find the real cJSON directory once and fix the default in `idf_cjson()`: run `Get-ChildItem C:\Espressif -Recurse -Filter cJSON.c | Select-Object -First 1 FullName` in PowerShell and paste the containing directory into the code as the default. `dir C:\Espressif\frameworks` earlier did not exist, so the install root differs; the walk fallback covers it but the explicit default is what should be committed.

- [ ] **Step 2: Run it, expect failure**

Run: `python docs/check_gf.py`
Expected: first harness OK, then `markers /* GF_PARSE_BEGIN */ ... not found in firmware/hub_prod/main/app_gf.c`.

- [ ] **Step 3: Constants and state structs in `app_priv.h`**

After `#define POLL_CYCLE_MS 2000` add:

```c
/* Ground-floor Wi-Fi nodes, polled by app_gf.c. Online: every 5 s. After
 * GF_OFFLINE_MISSES consecutive misses the node is OFFLINE and probed every
 * 30 s instead, so a dead node costs nothing; the first reply restores 5 s. */
#define GF_POLL_MS              5000
#define GF_REPROBE_MS           30000
#define GF_OFFLINE_MISSES       3
#define GF_HTTP_TIMEOUT_MS      2000
#define GF_REPLY_MAX            512
```

After `motor_state_t` (line 495) add:

```c
/* A remote motor read by clamps on the utility node: per-phase, so the
 * imbalance figure exists, and "running" derived from the highest phase. */
typedef struct {
    bool     running;
    int16_t  deci_amps;            /* highest phase, -1 = no clamp anywhere */
    int16_t  phase_da[3];          /* -1 = no clamp on that channel */
    uint8_t  imbalance_pct;        /* (max-min)/max over fitted phases, 0 if < 2 */
} gf_motor_state_t;
```

Inside `hub_state_t`, after `motor_state_t hpp, rwp;`:

```c
    /* Ground floor, from nodes 0x05 and 0x06 over Wi-Fi (app_gf.c). configured
     * = an IP is set on /cal; online = it answered within the latch. */
    tank_state_t     sump;
    bool             sump_pressure;         /* node reports the 4-20 mA loop, not the ultrasonic */
    gf_motor_state_t borewell, sump_motor;
    climate_state_t  utility_room;
    int8_t           rwt_floty;             /* -1 unknown, 0 open, 1 closed */
    bool             sump_configured, utility_configured;
    bool             sump_online, utility_online;
    int64_t          sump_last_us, utility_last_us;
    char             sump_fw[16], utility_fw[16];
```

`hist_rec_t` gains, after `int16_t ro_t, bat_t;`:

```c
    int8_t   sump;                 /* -1 = no level */
    int16_t  bore_da, smot_da;     /* -1 = no clamp */
    int16_t  util_t;               /* INT16_MIN = no reading */
```

Update the comment above `hist_rec_t` to list the new columns.

- [ ] **Step 4: `app_gf.h`**

```c
/*
 * app_gf.h — the ground-floor Wi-Fi nodes
 *
 * Two ESP32s on the house LAN, polled over plain HTTP at addresses typed on
 * /cal. This module owns the poll task and a snapshot of what each node last
 * said; the 2 s poll task in app_main.c copies the snapshot in each cycle and
 * applies the hub's calibration. Nodes report raw readings only.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     valid;                /* at least one good reply since boot */
    bool     online;
    uint8_t  misses;               /* consecutive failed polls */
    int64_t  last_ok_us;
    int64_t  next_poll_us;
    char     fw[16];
    int8_t   rssi;
    uint32_t uptime_s;
} gf_link_t;

typedef struct {
    gf_link_t       link;
    bool            pressure;      /* "source":"pressure" */
    uint16_t        distance_mm;   /* ultrasonic; 0 = none */
    uint8_t         quality;
    sensor_status_t sensor;
    uint32_t        loop_ua;       /* 4-20 mA loop; 0 = none */
} gf_sump_t;

typedef struct {
    gf_link_t link;
    uint16_t  bore_mv[3];          /* raw RMS mV per clamp channel */
    uint16_t  sump_mv[3];
    bool      sump_on;             /* Astero PUMP ON contact */
    int8_t    rwt_floty;           /* -1 null (not wired), 0 open, 1 closed */
    int16_t   temp_deci_c;
    uint16_t  hum_deci_pct;
    bool      sht_ok;
} gf_util_t;

esp_err_t gf_init(void);
void      gf_snapshot(gf_sump_t *sump, gf_util_t *util);

/* Pure; host-tested by docs/check_gf.py. Parsers leave out->link untouched
 * except fw, rssi and uptime_s, and return false on anything malformed. */
bool gf_parse_sump(const char *json, gf_sump_t *out);
bool gf_parse_util(const char *json, gf_util_t *out);
void gf_link_result(gf_link_t *l, bool ok, int64_t now_us);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 5: `app_gf.c` — parser and latch only (task comes in Task 3)**

```c
/*
 * app_gf.c — poll the ground-floor Wi-Fi nodes
 *
 * See app_gf.h. The pure part sits between GF_PARSE_BEGIN/END so
 * docs/check_gf.py can build it on the host against cJSON.
 */
#include <string.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_cal.h"
#include "app_gf.h"

static const char *TAG = "gf";

static SemaphoreHandle_t s_mux;
static gf_sump_t s_sump;
static gf_util_t s_util;

/* GF_PARSE_BEGIN */
static bool jint(const cJSON *o, const char *k, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(v)) return false;
    *out = v->valueint;
    return true;
}

/* Number or null. Returns false only if the key is missing or another type. */
static bool jint_or_null(const cJSON *o, const char *k, int *out, int if_null)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNull(v)) { *out = if_null; return true; }
    if (!cJSON_IsNumber(v)) return false;
    *out = v->valueint;
    return true;
}

static bool jlink(const cJSON *o, int want_id, gf_link_t *l)
{
    int id, up, rssi;
    if (!jint(o, "id", &id) || id != want_id) return false;
    const cJSON *fw = cJSON_GetObjectItemCaseSensitive(o, "fw");
    if (!cJSON_IsString(fw)) return false;
    if (!jint(o, "uptime_s", &up) || !jint(o, "rssi", &rssi)) return false;
    strncpy(l->fw, fw->valuestring, sizeof(l->fw) - 1);
    l->fw[sizeof(l->fw) - 1] = '\0';
    l->uptime_s = (uint32_t)up;
    l->rssi = (int8_t)rssi;
    return true;
}

static bool jmv3(const cJSON *o, const char *k, uint16_t out[3])
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != 3) return false;
    for (int i = 0; i < 3; i++) {
        const cJSON *v = cJSON_GetArrayItem(a, i);
        if (!cJSON_IsNumber(v) || v->valueint < 0 || v->valueint > 65535) return false;
        out[i] = (uint16_t)v->valueint;
    }
    return true;
}

bool gf_parse_sump(const char *json, gf_sump_t *out)
{
    cJSON *o = cJSON_Parse(json);
    if (o == NULL) return false;
    bool ok = false;
    gf_sump_t s = *out;
    int dist, q, ua;
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(o, "source");
    const cJSON *st  = cJSON_GetObjectItemCaseSensitive(o, "status");
    if (!jlink(o, 5, &s.link) || !cJSON_IsString(src) || !cJSON_IsString(st)) goto done;
    if (!jint_or_null(o, "distance_mm", &dist, 0) || !jint_or_null(o, "quality", &q, 0) ||
        !jint_or_null(o, "loop_ua", &ua, 0)) goto done;
    s.pressure    = strcmp(src->valuestring, "pressure") == 0;
    s.distance_mm = (uint16_t)(dist < 0 ? 0 : dist);
    s.quality     = (uint8_t)(q < 0 ? 0 : (q > 100 ? 100 : q));
    s.loop_ua     = (uint32_t)(ua < 0 ? 0 : ua);
    if      (strcmp(st->valuestring, "OK") == 0)       s.sensor = SENSOR_OK;
    else if (strcmp(st->valuestring, "BLIND") == 0)    s.sensor = SENSOR_BLIND;
    else if (strcmp(st->valuestring, "NO_ECHO") == 0)  s.sensor = SENSOR_NO_ECHO;
    else if (strcmp(st->valuestring, "HW_FAULT") == 0) s.sensor = SENSOR_HW_FAULT;
    else goto done;
    *out = s;
    ok = true;
done:
    cJSON_Delete(o);
    return ok;
}

bool gf_parse_util(const char *json, gf_util_t *out)
{
    cJSON *o = cJSON_Parse(json);
    if (o == NULL) return false;
    bool ok = false;
    gf_util_t u = *out;
    int t, rh;
    const cJSON *on  = cJSON_GetObjectItemCaseSensitive(o, "sump_on");
    const cJSON *fl  = cJSON_GetObjectItemCaseSensitive(o, "rwt_floty");
    const cJSON *sht = cJSON_GetObjectItemCaseSensitive(o, "sht_ok");
    if (!jlink(o, 6, &u.link)) goto done;
    if (!jmv3(o, "bore_mv", u.bore_mv) || !jmv3(o, "sump_mv", u.sump_mv)) goto done;
    if (!cJSON_IsBool(on) || !cJSON_IsBool(sht)) goto done;
    if (cJSON_IsNull(fl)) u.rwt_floty = -1;
    else if (cJSON_IsBool(fl)) u.rwt_floty = cJSON_IsTrue(fl) ? 1 : 0;
    else goto done;
    if (!jint(o, "t_deci_c", &t) || !jint(o, "rh_deci_pct", &rh)) goto done;
    u.sump_on      = cJSON_IsTrue(on);
    u.sht_ok       = cJSON_IsTrue(sht);
    u.temp_deci_c  = (int16_t)t;
    u.hum_deci_pct = (uint16_t)(rh < 0 ? 0 : rh);
    *out = u;
    ok = true;
done:
    cJSON_Delete(o);
    return ok;
}

/* The latch. Same shape as the RS485 side's give-up-and-reprobe: a node is
 * not offline on one miss, and an offline node is still asked - just slowly. */
void gf_link_result(gf_link_t *l, bool ok, int64_t now_us)
{
    if (ok) {
        l->misses = 0;
        l->online = true;
        l->valid = true;
        l->last_ok_us = now_us;
    } else {
        if (l->misses < 255) l->misses++;
        if (l->misses >= GF_OFFLINE_MISSES) l->online = false;
    }
    l->next_poll_us = now_us + (int64_t)(l->online ? GF_POLL_MS : GF_REPROBE_MS) * 1000;
}
/* GF_PARSE_END */

void gf_snapshot(gf_sump_t *sump, gf_util_t *util)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (sump) *sump = s_sump;
    if (util) *util = s_util;
    xSemaphoreGive(s_mux);
}

esp_err_t gf_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    if (s_mux == NULL) return ESP_FAIL;
    memset(&s_sump, 0, sizeof(s_sump));
    memset(&s_util, 0, sizeof(s_util));
    s_util.rwt_floty = -1;
    ESP_LOGI(TAG, "ground-floor nodes: sump %s, utility %s",
             cal_gf_ip(CAL_GF_SUMP)[0] ? cal_gf_ip(CAL_GF_SUMP) : "not fitted",
             cal_gf_ip(CAL_GF_UTIL)[0] ? cal_gf_ip(CAL_GF_UTIL) : "not fitted");
    return ESP_OK;   /* the task is started in Task 3 */
}
```

- [ ] **Step 6: Register the source and components**

`firmware/hub_prod/main/CMakeLists.txt` line 6 becomes:

```cmake
idf_component_register(SRCS "app_main.c" "app_rs485.c" "app_sensors.c" "app_cal.c" "app_web.c" "app_gf.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_http_client json)
```

If the build then complains that other components (`esp_http_server`, `esp_adc`, `driver`, `nvs_flash`, `esp_wifi`, `app_update`, `esp_timer`, `mdns`, `esp_rainmaker`, `rmaker_app_network`) are no longer found, that is because naming any `PRIV_REQUIRES` turns off the implicit common set for that list only — add the ones the compiler names to `PRIV_REQUIRES` until it builds.

- [ ] **Step 7: Init the module from `app_main`**

In `app_main()` after `ESP_ERROR_CHECK(rs485_init());` add `ESP_ERROR_CHECK(gf_init());` and `#include "app_gf.h"` at the top. In the state initialisation block after `s_state.ro_room.fault = s_state.battery_room.fault = true;` add:

```c
    s_state.sump.pct = -1;
    s_state.borewell.deci_amps = s_state.sump_motor.deci_amps = -1;
    for (int i = 0; i < 3; i++) s_state.borewell.phase_da[i] = s_state.sump_motor.phase_da[i] = -1;
    s_state.utility_room.fault = true;
    s_state.rwt_floty = -1;
```

- [ ] **Step 8: Host check and build**

Run: `python docs/check_gf.py`
Expected: both harnesses OK.

Run: `cd firmware/hub_prod; idf.py build`
Expected: builds clean.

- [ ] **Step 9: Commit**

```bash
git add docs/check_gf.py firmware/hub_prod/main/app_priv.h firmware/hub_prod/main/app_gf.h firmware/hub_prod/main/app_gf.c firmware/hub_prod/main/app_main.c firmware/hub_prod/main/CMakeLists.txt
git commit -m "Hub: ground-floor node module - JSON parsers, offline latch, state fields"
```

---

### Task 3: Hub polling task and applying the snapshot into `hub_state_t`

**Files:**
- Modify: `firmware/hub_prod/main/app_gf.c` (task + HTTP)
- Modify: `firmware/hub_prod/main/app_main.c` (`gf_apply`, `diff_events`, `history_push`)
- Modify: `firmware/hub_prod/main/app_web.c` (history rows)

**Interfaces:**
- Consumes: `gf_snapshot`, `gf_parse_*`, `gf_link_result`, `cal_gf_ip`, `cal_tank`, `cal_ct`, `levelPercent`, `gfLoopDistanceMM`, `gfPhaseDeciAmps`, `gfImbalancePct`.
- Produces: `static void gf_apply(hub_state_t *s)` in `app_main.c`, called each cycle after `read_climate_node(&local)`. `/api/history` rows gain four trailing columns `[..., sump, bore_da, smot_da, util_t]` (indices 9-12).

- [ ] **Step 1: The HTTP poll in `app_gf.c`**

Add includes `#include "esp_http_client.h"` and, below `gf_snapshot`, the fetch and the task:

```c
/* One GET, body into buf. Returns bytes read, or -1. Plain HTTP on the LAN;
 * a 2 s timeout because a node that takes longer is not answering. */
static int gf_fetch(const char *ip_port, char *buf, size_t len)
{
    char url[64];
    snprintf(url, sizeof(url), "http://%s/api/telemetry", ip_port);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = GF_HTTP_TIMEOUT_MS,
        .method = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return -1;
    int got = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        int64_t clen = esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status == 200 && clen != 0) {
            int n = 0;
            while (n < (int)len - 1) {
                int r = esp_http_client_read(c, buf + n, (int)len - 1 - n);
                if (r <= 0) break;
                n += r;
            }
            buf[n] = '\0';
            got = n;
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return got;
}

static void gf_poll_one(cal_gf_t which, int64_t now_us)
{
    const char *ip = cal_gf_ip(which);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    gf_link_t *l = (which == CAL_GF_SUMP) ? &s_sump.link : &s_util.link;
    if (ip[0] == '\0') {
        /* Not fitted: forget everything, so a node that is later removed from
         * /cal does not keep showing its last reading. */
        bool was = l->online;
        memset(l, 0, sizeof(*l));
        if (was) ESP_LOGI(TAG, "%s node removed from /cal", cal_gf_key(which));
        xSemaphoreGive(s_mux);
        return;
    }
    bool due = now_us >= l->next_poll_us;
    xSemaphoreGive(s_mux);
    if (!due) return;

    static char body[GF_REPLY_MAX];
    int n = gf_fetch(ip, body, sizeof(body));

    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool was_online = l->online, ok = false;
    if (n > 0) {
        ok = (which == CAL_GF_SUMP) ? gf_parse_sump(body, &s_sump) : gf_parse_util(body, &s_util);
        if (!ok) ESP_LOGW(TAG, "%s node %s: unparseable reply (%d bytes): %.80s", cal_gf_key(which), ip, n, body);
    }
    gf_link_result(l, ok, now_us);
    if (ok && !was_online)  ESP_LOGI(TAG, "%s node %s answering (fw %s, rssi %d)", cal_gf_key(which), ip, l->fw, l->rssi);
    if (!ok && was_online && !l->online) ESP_LOGW(TAG, "%s node %s offline after %d misses - probing every %d s",
                                                   cal_gf_key(which), ip, l->misses, GF_REPROBE_MS / 1000);
    xSemaphoreGive(s_mux);
}

static void gf_task(void *arg)
{
    (void)arg;
    while (true) {
        int64_t now = esp_timer_get_time();
        gf_poll_one(CAL_GF_SUMP, now);
        gf_poll_one(CAL_GF_UTIL, now);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

and at the end of `gf_init()` replace the `return ESP_OK;` line with:

```c
    if (xTaskCreate(gf_task, "gf", 6144, NULL, 4, NULL) != pdPASS) return ESP_FAIL;
    return ESP_OK;
```

Note: `gf_parse_*` writes into `s_sump`/`s_util` directly under the mutex, and on a failed parse leaves them untouched (the parsers copy-in/copy-out). Priority 4, below the poll task's 5, so a slow HTTP read never delays RS485.

- [ ] **Step 2: `gf_apply()` in `app_main.c`**

Add `#include "app_gf.h"` (Task 2 did). Above `poll_task`, after `read_climate_node`:

```c
/*
 * Ground floor. The gf task keeps its own snapshot; this copies it into the
 * cycle's state and applies calibration, so everything downstream (telemetry,
 * history, events, cloud, alerts) sees ordinary hub_state_t fields.
 */
static void gf_apply(hub_state_t *s)
{
    gf_sump_t gs; gf_util_t gu;
    gf_snapshot(&gs, &gu);

    /* ---- sump 0x05 ---- */
    s->sump_configured = cal_gf_ip(CAL_GF_SUMP)[0] != '\0';
    s->sump_online     = s->sump_configured && gs.link.online;
    s->sump_last_us    = gs.link.last_ok_us;
    strncpy(s->sump_fw, gs.link.fw, sizeof(s->sump_fw) - 1);
    if (s->sump_configured && gs.link.valid) {
        const cal_tank_cfg_t *c = cal_tank(CAL_TANK_SUMP);
        s->sump_pressure = gs.pressure;
        uint16_t dist = gs.pressure ? gfLoopDistanceMM(gs.loop_ua, c->press_range_mm) : gs.distance_mm;
        s->sump.distance_mm = dist;
        s->sump.raw_mm      = dist;
        s->sump.quality     = gs.pressure ? 100 : gs.quality;
        s->sump.sensor      = gs.sensor;
        s->sump.last_ok_us  = gs.link.last_ok_us;
        uint8_t pct = (dist == 0) ? 255 : levelPercent(dist, c->full_mm, c->empty_mm);
        if (pct != 255 && !gs.pressure && gs.quality < MIN_LEVEL_QUALITY) pct = 255;
        s->sump.pct = (pct == 255) ? -1 : (int16_t)pct;
    } else {
        s->sump.pct = -1;
        s->sump.distance_mm = 0;
    }

    /* ---- utility 0x06 ---- */
    s->utility_configured = cal_gf_ip(CAL_GF_UTIL)[0] != '\0';
    s->utility_online     = s->utility_configured && gu.link.online;
    s->utility_last_us    = gu.link.last_ok_us;
    strncpy(s->utility_fw, gu.link.fw, sizeof(s->utility_fw) - 1);

    gf_motor_state_t *bm = &s->borewell, *sm = &s->sump_motor;
    if (s->utility_configured && gu.link.valid) {
        const cal_ct_cfg_t *bc = cal_ct(CAL_CT_BORE), *sc = cal_ct(CAL_CT_SUMP);
        bm->deci_amps = sm->deci_amps = -1;
        for (int i = 0; i < 3; i++) {
            bm->phase_da[i] = gfPhaseDeciAmps(gu.bore_mv[i], bc->amps_per_volt_x100, bc->turns);
            sm->phase_da[i] = gfPhaseDeciAmps(gu.sump_mv[i], sc->amps_per_volt_x100, sc->turns);
            if (bm->phase_da[i] > bm->deci_amps) bm->deci_amps = bm->phase_da[i];
            if (sm->phase_da[i] > sm->deci_amps) sm->deci_amps = sm->phase_da[i];
        }
        bm->imbalance_pct = gfImbalancePct(bm->phase_da);
        sm->imbalance_pct = gfImbalancePct(sm->phase_da);
        /* Borewell has no contact of its own: running = drawing current. The
         * sump motor has the Astero PUMP ON contact, which cannot be fooled by
         * a floating channel, so the clamps there are for amps only. */
        bm->running = s->utility_online && bm->deci_amps >= (int16_t)bc->run_deci_amps;
        sm->running = s->utility_online && gu.sump_on;

        s->utility_room.fault        = !gu.sht_ok;
        s->utility_room.temp_deci_c  = gu.temp_deci_c;
        s->utility_room.hum_deci_pct = gu.hum_deci_pct;
        s->utility_room.last_ok_us   = gu.link.last_ok_us;
        s->rwt_floty = gu.rwt_floty;
    } else {
        bm->running = sm->running = false;
        bm->deci_amps = sm->deci_amps = -1;
        for (int i = 0; i < 3; i++) bm->phase_da[i] = sm->phase_da[i] = -1;
        bm->imbalance_pct = sm->imbalance_pct = 0;
        s->utility_room.fault = true;
        s->rwt_floty = -1;
    }
}
```

In `poll_task`, directly after `read_climate_node(&local);` add `gf_apply(&local);`.

- [ ] **Step 3: Events and history**

In `diff_events()` add after the `battery_online` line:

```c
    EDGE(sump_online,    EVT_NODE_ON,  EVT_NODE_OFF,  NODE_ADDR_SUMP);
    EDGE(utility_online, EVT_NODE_ON,  EVT_NODE_OFF,  NODE_ADDR_UTILITY);
```

In `history_push()` after `r->bat_t = ...;`:

```c
    r->sump    = (int8_t)(s->sump_online ? s->sump.pct : -1);
    r->bore_da = s->utility_online ? s->borewell.deci_amps : -1;
    r->smot_da = s->utility_online ? s->sump_motor.deci_amps : -1;
    r->util_t  = (!s->utility_online || s->utility_room.fault) ? INT16_MIN : s->utility_room.temp_deci_c;
```

In `app_web.c` `history_get()` extend the row:

```c
        char ro[8], bat[8], ut[8];
        if (r->ro_t == INT16_MIN) snprintf(ro, sizeof ro, "null"); else snprintf(ro, sizeof ro, "%d", r->ro_t);
        if (r->bat_t == INT16_MIN) snprintf(bat, sizeof bat, "null"); else snprintf(bat, sizeof bat, "%d", r->bat_t);
        if (r->util_t == INT16_MIN) snprintf(ut, sizeof ut, "null"); else snprintf(ut, sizeof ut, "%d", r->util_t);
        n += snprintf(buf + n, sizeof(buf) - n, "%s[%lu,%d,%d,%d,%u,%d,%d,%s,%s,%d,%d,%d,%s]",
                      i ? "," : "", (unsigned long)r->t, r->rwt, r->twt, r->dos, r->flags,
                      r->hpp_da, r->rwp_da, ro, bat, r->sump, r->bore_da, r->smot_da, ut);
```

and change the chunk threshold `if (n > (int)sizeof(buf) - 80)` to `- 100` since rows are longer. Update the comment block in `dashboard.html` at line 1891 that documents the row layout (Task 6 does the JS).

- [ ] **Step 4: Build**

Run: `cd firmware/hub_prod; idf.py build`
Expected: clean.

- [ ] **Step 5: Commit**

```bash
git add firmware/hub_prod/main/app_gf.c firmware/hub_prod/main/app_main.c firmware/hub_prod/main/app_web.c
git commit -m "Hub: poll the ground-floor nodes, apply calibration, log node edges, extend history"
```

---

### Task 4: Telemetry JSON and the `/cal` page

**Files:**
- Modify: `firmware/hub_prod/main/app_web.c` (`telemetry_get`, `cal_get`, new `cal_gf_post`, `cal_tank_post`, routes)
- Modify: `docs/check_telemetry.py`

**Interfaces:**
- Produces telemetry keys: `tanks.sump.{pct,distance_mm,state,sensor,source}`, `pumps.borewell.{on,state}`, `pumps.sump_motor.{on,state}`, `aster.rwt_floty` (bool), `env.utility_room.{t,rh,state,src,age_s}`, `motors.borewell.{amps,phases,imbalance_pct,running}`, `motors.sump_motor.{amps,phases,imbalance_pct,running}`, `nodes[i].ip`, `nodes[i].fw` for 0x05/0x06.
- Route `POST /api/cal/gf` with fields `node=sump|util`, `ip=<dotted quad[:port] or empty>`.
- `POST /api/cal/tank` accepts an optional `range` field (mm) for any tank; only the sump form sends it.

- [ ] **Step 1: Failing check — add the keys**

In `docs/check_telemetry.py` `REQUIRED` add:

```python
    'tanks.sump.source',
    'motors.borewell.amps', 'motors.borewell.phases', 'motors.borewell.imbalance_pct', 'motors.borewell.running',
    'motors.sump_motor.amps', 'motors.sump_motor.phases', 'motors.sump_motor.running',
    'env.utility_room.t', 'env.utility_room.rh', 'env.utility_room.state', 'env.utility_room.src', 'env.utility_room.age_s',
```

Run: `python docs/check_telemetry.py` — Expected: FAIL listing those keys.

- [ ] **Step 2: Helpers in `app_web.c`**

Next to `wq_json()` add:

```c
/* "4.1" or null, the same rule as the pump amps. */
static void da_json(char *out, size_t n, int16_t da)
{
    if (da < 0) snprintf(out, n, "null");
    else        snprintf(out, n, "%d.%d", da / 10, da % 10);
}

/* [4.1,4.0,null] */
static void phases_json(char *out, size_t n, const int16_t da[3])
{
    char a[12], b[12], c[12];
    da_json(a, sizeof a, da[0]); da_json(b, sizeof b, da[1]); da_json(c, sizeof c, da[2]);
    snprintf(out, n, "[%s,%s,%s]", a, b, c);
}

static const char *gf_node_state(bool configured, bool online)
{
    return !configured ? "OFFLINE" : (online ? "ONLINE" : "OFFLINE");
}
```

- [ ] **Step 3: Rewrite the placeholder fields in `telemetry_get`**

Grow the buffer: `static char json[3100];` → `static char json[4200];`. Before the big `snprintf`, build:

```c
    char bore_amps[12], smot_amps[12], bore_ph[40], smot_ph[40];
    da_json(bore_amps, sizeof bore_amps, s->borewell.deci_amps);
    da_json(smot_amps, sizeof smot_amps, s->sump_motor.deci_amps);
    phases_json(bore_ph, sizeof bore_ph, s->borewell.phase_da);
    phases_json(smot_ph, sizeof smot_ph, s->sump_motor.phase_da);
    const char *sump_sensor = !s->sump_configured ? "Not configured"
                             : (!s->sump_online ? "Offline" : sensor_word(s->sump.sensor));
```

Then change the format string pieces:

```c
        "\"tanks\":{"
          "\"sump\":{\"pct\":%d,\"distance_mm\":%u,\"state\":\"%s\",\"sensor\":\"%s\",\"source\":\"%s\"},"
```

```c
        "\"pumps\":{"
          "\"borewell\":{\"on\":%s,\"state\":\"%s\"},"
          "\"sump_motor\":{\"on\":%s,\"state\":\"%s\"},"
```

```c
        "\"aster\":{\"twt_floty\":%s,\"rwt_floty\":%s,\"sump_floty\":false,"
```

```c
        "\"env\":{"
          "\"ro_room\":{...unchanged...},"
          "\"battery_room\":{...unchanged...},"
          "\"utility_room\":{\"t\":%d.%d,\"rh\":%d.%d,\"state\":\"%s\",\"src\":\"SHT30 . Node 0x06\",\"age_s\":%d}"
        "},"
        "\"motors\":{"
          "\"hpp\":{\"amps\":%s,\"mv_lo\":%lu,\"mv_hi\":%lu},"
          "\"rwp\":{\"amps\":%s,\"mv_lo\":%lu,\"mv_hi\":%lu},"
          "\"borewell\":{\"amps\":%s,\"phases\":%s,\"imbalance_pct\":%u,\"running\":%s},"
          "\"sump_motor\":{\"amps\":%s,\"phases\":%s,\"imbalance_pct\":%u,\"running\":%s},"
          "\"overcurrent\":%s,\"no_production\":%s"
        "},"
```

```c
          "{\"id\":\"0x05\",\"role\":\"Sump\",\"link\":\"Wi-Fi\",\"state\":\"%s\",\"age_s\":%d,\"ip\":\"%s\",\"fw\":\"%s\"},"
          "{\"id\":\"0x06\",\"role\":\"Utility\",\"link\":\"Wi-Fi\",\"state\":\"%s\",\"age_s\":%d,\"ip\":\"%s\",\"fw\":\"%s\"}"
```

And the arguments, in the same positions:

```c
        /* tanks.sump */
        s->sump.pct < 0 ? 0 : s->sump.pct, s->sump.distance_mm,
        gf_node_state(s->sump_configured, s->sump_online), sump_sensor,
        s->sump_pressure ? "pressure" : "ultrasonic",
        /* pumps.borewell, pumps.sump_motor */
        s->borewell.running ? "true" : "false",   gf_node_state(s->utility_configured, s->utility_online),
        s->sump_motor.running ? "true" : "false", gf_node_state(s->utility_configured, s->utility_online),
        /* aster.rwt_floty */
        s->rwt_floty == 1 ? "true" : "false",
        /* env.utility_room */
        s->utility_room.temp_deci_c / 10, abs(s->utility_room.temp_deci_c % 10),
        s->utility_room.hum_deci_pct / 10, s->utility_room.hum_deci_pct % 10,
        !s->utility_online ? "OFFLINE" : (s->utility_room.fault ? "SENSOR_ERROR" : "ONLINE"),
        age_s(s->utility_room.last_ok_us),
        /* motors.borewell, motors.sump_motor */
        bore_amps, bore_ph, (unsigned)s->borewell.imbalance_pct,   s->borewell.running ? "true" : "false",
        smot_amps, smot_ph, (unsigned)s->sump_motor.imbalance_pct, s->sump_motor.running ? "true" : "false",
        /* nodes 0x05, 0x06 */
        gf_node_state(s->sump_configured, s->sump_online),       age_s(s->sump_last_us),    cal_gf_ip(CAL_GF_SUMP), s->sump_fw,
        gf_node_state(s->utility_configured, s->utility_online), age_s(s->utility_last_us), cal_gf_ip(CAL_GF_UTIL), s->utility_fw
```

The `rs485.total` stays 3: it counts the bus. The dashboard header shows `online/total` for RS485 only.

`check_telemetry.py`'s filler turns bare `%s` into `null`, so `"phases":%s` becomes `"phases":null` — valid JSON, and the key check passes. Fine.

- [ ] **Step 4: `/cal` — Ground floor nodes fieldset, sump range, remote clamp rows**

Grow the page buffer: `static char page[10240];` → `static char page[14336];`.

Live arrays now come from state:

```c
    uint16_t live[CAL_TANK_COUNT]     = { s->rwt.distance_mm, s->twt.distance_mm, s->dosing.distance_mm, s->sump.distance_mm };
    int16_t  live_pct[CAL_TANK_COUNT] = { s->rwt.pct, s->twt.pct, s->dosing.pct, s->sump.pct };
    uint32_t ct_lo[CAL_CT_COUNT] = { s->hpp.mv_lo, s->rwp.mv_lo, 0, 0 };
    uint32_t ct_hi[CAL_CT_COUNT] = { s->hpp.mv_hi, s->rwp.mv_hi, 0, 0 };
    int16_t  ct_a[CAL_CT_COUNT]  = { s->hpp.deci_amps, s->rwp.deci_amps, s->borewell.deci_amps, s->sump_motor.deci_amps };
    int16_t  ct_ph[CAL_CT_COUNT][3];
    memcpy(ct_ph[CAL_CT_BORE], s->borewell.phase_da, sizeof(ct_ph[0]));
    memcpy(ct_ph[CAL_CT_SUMP], s->sump_motor.phase_da, sizeof(ct_ph[0]));
    bool sump_pressure = s->sump_pressure, sump_online = s->sump_online, util_online = s->utility_online;
```

Inside the tank loop, after the `empty` input and before `<button>Save</button>`, add for the sump only:

```c
        if (i == CAL_TANK_SUMP) {
            n += snprintf(page + n, sizeof(page) - n,
                "transducer range mm <input name=range size=6 value='%u'> ", c->press_range_mm);
        }
```

and change the live text for the sump so it says which source the node reports: before the `snprintf` that prints `<h3>%s</h3><p>live <b>%u mm</b>`, compute a `src` string:

```c
        const char *src = "";
        if (i == CAL_TANK_SUMP) {
            src = !sump_online ? " (node offline)" : (sump_pressure ? " (4-20 mA loop)" : " (ultrasonic)");
        }
```

and append `%s` after the `<b>%s</b>` pct with `src` as its argument. Add to the trailing `<small>` of the tank fieldset: `"Sump: transducer range is the 4-20 mA sensor's full scale in mm, 0 when the ultrasonic is fitted; the node's J-PRESS shunt decides which one it reads."`

In the clamp loop, the `reading` text for the remote rows shows phases. Replace the single `snprintf` with:

```c
        char reading[64];
        if (i >= CAL_CT_BORE) {
            char p[40];
            /* per-phase deci-amps, "--" for a socket with no clamp */
            char a[3][8];
            for (int k = 0; k < 3; k++) {
                if (ct_ph[i][k] < 0) snprintf(a[k], sizeof a[k], "--");
                else snprintf(a[k], sizeof a[k], "%d.%d", ct_ph[i][k] / 10, ct_ph[i][k] % 10);
            }
            snprintf(p, sizeof p, "%s / %s / %s A", a[0], a[1], a[2]);
            snprintf(reading, sizeof reading, "%s%s", util_online ? "phases " : "node offline; last ", p);
        } else {
            snprintf(reading, sizeof reading, "pedestal %lu-%lu mV, reading %s",
                     (unsigned long)ct_lo[i], (unsigned long)ct_hi[i],
                     ct_a[i] < 0 ? "none (no clamp or no pedestal)" : "live");
        }
        n += snprintf(page + n, sizeof(page) - n,
            "<h3>%s</h3><p>%s</p>"
            "<form method=post action='/api/cal/ct'>"
            "<input type=hidden name=ct value='%s'>"
            "A per V <input name=apv size=6 value='%u.%02u'> "
            "turns <input name=turns size=3 value='%u'> "
            "run A <input name=run size=5 value='%u.%u'> "
            "trip A <input name=oc size=5 value='%u.%u'> "
            "<button>Save</button></form>",
            cal_ct_label(i), reading, cal_ct_key(i),
            c->amps_per_volt_x100 / 100, c->amps_per_volt_x100 % 100, c->turns,
            c->run_deci_amps / 10, c->run_deci_amps % 10,
            c->oc_deci_amps / 10, c->oc_deci_amps % 10);
```

Add to that fieldset's `<small>`: `"<b>Run A</b>: the borewell has no contact of its own, so it is running when its highest phase is above this. Borewell and sump motor share one calibration across their three channels."`

New fieldset, placed before the "Relay test" one:

```c
    n += snprintf(page + n, sizeof(page) - n,
        "<fieldset id=gf><legend>Ground floor nodes</legend>"
        "<p><small>Wi-Fi nodes the hub polls every %d s. Give each a fixed address on the "
        "router (or in its firmware) and type it here. Empty = not fitted: nothing is polled "
        "and nothing alerts. <code>a.b.c.d</code>, or <code>a.b.c.d:port</code> for a bench "
        "node.</small></p>", GF_POLL_MS / 1000);
    static const char *gf_label[CAL_GF_COUNT] = { "Sump level node 0x05", "Utility room node 0x06" };
    bool gf_on[CAL_GF_COUNT] = { sump_online, util_online };
    const char *gf_fw[CAL_GF_COUNT] = { s_sump_fw, s_util_fw };
    for (int i = 0; i < CAL_GF_COUNT; i++) {
        const char *ip = cal_gf_ip(i);
        n += snprintf(page + n, sizeof(page) - n,
            "<h3>%s</h3><p>%s</p>"
            "<form method=post action='/api/cal/gf'>"
            "<input type=hidden name=node value='%s'>"
            "IP <input name=ip size=21 value='%s' placeholder='192.168.1.50'> "
            "<button>Save</button></form>",
            gf_label[i],
            ip[0] == '\0' ? "not fitted" : (gf_on[i] ? "online" : "configured, not answering"),
            cal_gf_key(i), ip);
        if (ip[0] && gf_on[i] && gf_fw[i][0]) {
            n += snprintf(page + n, sizeof(page) - n, "<p><small>fw %s</small></p>", gf_fw[i]);
        }
    }
    n += snprintf(page + n, sizeof(page) - n, "</fieldset>");
```

where `s_sump_fw` / `s_util_fw` are local copies taken under the lock at the top: `char s_sump_fw[16], s_util_fw[16]; strncpy(...)` from `s->sump_fw` / `s->utility_fw`.

- [ ] **Step 5: POST handlers**

`cal_tank_post`: after the existing `cal_set_tank` call succeeds, read the optional field:

```c
    char rs[8];
    if (form_field(body, "range", rs, sizeof(rs))) {
        unsigned r = (unsigned)strtoul(rs, NULL, 10);
        if (cal_set_press_range((cal_tank_t)idx, (uint16_t)r) != ESP_OK) {
            return bad(req, "rejected: transducer range must be 0 (ultrasonic) or 500-10000 mm");
        }
    }
```

Increase that handler's `body` buffer to 192 if it is smaller.

New handler after `cal_wq_post`:

```c
static const char *gf_key_i(int i) { return cal_gf_key((cal_gf_t)i); }

static esp_err_t cal_gf_post(httpd_req_t *req)
{
    char body[128], f[8], ip[24];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return bad(req, "body too long");
    if (!form_field(body, "node", f, sizeof(f))) return bad(req, "need node");
    int idx = key_index(f, gf_key_i, CAL_GF_COUNT);
    if (idx < 0) return bad(req, "unknown node");
    if (!form_field(body, "ip", ip, sizeof(ip))) ip[0] = '\0';   /* empty field = clear */
    /* trim spaces a phone keyboard adds */
    char *p = ip; while (*p == ' ') p++;
    size_t l = strlen(p); while (l && p[l - 1] == ' ') p[--l] = '\0';
    if (cal_set_gf_ip((cal_gf_t)idx, p) != ESP_OK) {
        return bad(req, "rejected: a.b.c.d or a.b.c.d:port, or empty to remove");
    }
    return redirect_to(req, "/cal#gf");
}
```

Check `form_field()` URL-decodes `%3A` to `:`; if it only handles `+` and plain text, the colon arrives as `%3A` and `gf_ip_valid` rejects it. Read `form_field` (line 694) and, if it lacks percent-decoding, add a two-hex-digit decode inside its copy loop.

Routes table: add `{ "/api/cal/gf", HTTP_POST, cal_gf_post, false },` after the `wq` line. `max_uri_handlers` is derived, so nothing else changes.

- [ ] **Step 6: Checks and build**

Run: `python docs/check_telemetry.py` — Expected: `OK` (or its existing success line).
Run: `cd firmware/hub_prod; idf.py build` — Expected: clean.

- [ ] **Step 7: Commit**

```bash
git add firmware/hub_prod/main/app_web.c docs/check_telemetry.py
git commit -m "Hub web: ground-floor telemetry keys, node IPs and sump transducer range on /cal"
```

---

### Task 5: Cloud parameters and the node-lost alert

**Files:**
- Modify: `firmware/hub_prod/main/app_priv.h` (param names)
- Modify: `firmware/hub_prod/main/app_main.c` (`build_node`, `poll_task` reporting, `evaluate_alerts`)

**Interfaces:**
- Produces RainMaker device `"Ground Floor"` (`DEV_GROUND`) with params `Borewell Running`, `Sump Motor Running`, `Borewell Current`, `Sump Motor Current`, `Utility Room Temp`, `Utility Room Humidity`; and on `Water Tanks`: `Sump Level`, `RWT Float Full`.

- [ ] **Step 1: Names in `app_priv.h`**

After `#define DEV_TANKS "Water Tanks"`:

```c
#define DEV_GROUND              "Ground Floor"
#define PARAM_SUMP_PCT          "Sump Level"
#define PARAM_RWT_FLOAT         "RWT Float Full"
#define PARAM_BORE_ON           "Borewell Running"
#define PARAM_SMOT_ON           "Sump Motor Running"
#define PARAM_BORE_AMPS         "Borewell Current"
#define PARAM_SMOT_AMPS         "Sump Motor Current"
#define PARAM_UTIL_TEMP         "Utility Room Temp"
#define PARAM_UTIL_HUM          "Utility Room Humidity"
```

- [ ] **Step 2: Device and params in `build_node()`**

Add `static esp_rmaker_device_t *s_dev_ground;` next to the other device statics. In the WATER TANKS block, after `PARAM_TWT_FLOAT`:

```c
    esp_rmaker_device_add_param(s_dev_tanks, ro_param(PARAM_SUMP_PCT, "esp.param.water-level",
                                                      esp_rmaker_int(VAL_NO_READING_INT), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_tanks, ro_param(PARAM_RWT_FLOAT, "esp.param.toggle",
                                                      esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE));
```

After `esp_rmaker_node_add_device(node, s_dev_tanks);`:

```c
    /* ============================== GROUND FLOOR ==============================
     * Node 0x06 in the starter panel: what the two motors are doing and the
     * air around them. The sump level itself sits with the other tanks. */
    s_dev_ground = esp_rmaker_device_create(DEV_GROUND, "esp.device.other", NULL);
    esp_rmaker_param_t *bo = ro_param(PARAM_BORE_ON, ESP_RMAKER_PARAM_POWER,
                                      esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE);
    esp_rmaker_device_add_param(s_dev_ground, bo);
    esp_rmaker_device_assign_primary_param(s_dev_ground, bo);
    esp_rmaker_device_add_param(s_dev_ground, ro_param(PARAM_SMOT_ON, ESP_RMAKER_PARAM_POWER,
                                                       esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE));
    esp_rmaker_device_add_param(s_dev_ground, ro_param(PARAM_BORE_AMPS, "esp.param.current",
                                                       esp_rmaker_float(VAL_NO_READING_FLOAT), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_ground, ro_param(PARAM_SMOT_AMPS, "esp.param.current",
                                                       esp_rmaker_float(VAL_NO_READING_FLOAT), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_ground, ro_param(PARAM_UTIL_TEMP, ESP_RMAKER_PARAM_TEMPERATURE,
                                                       esp_rmaker_float(VAL_NO_READING_FLOAT), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_ground, ro_param(PARAM_UTIL_HUM, "esp.param.humidity",
                                                       esp_rmaker_float(VAL_NO_READING_FLOAT), ESP_RMAKER_UI_TEXT));
    esp_rmaker_node_add_device(node, s_dev_ground);
```

- [ ] **Step 3: Reporting in `poll_task`**

Add to the deadband locals: `int last_sump = INT32_MIN, last_bore_on = -1, last_smot_on = -1, last_rwt_float = -1; float last_bore_a = -9999, last_smot_a = -9999, last_util_t = -9999, last_util_h = -9999;`

After the three tank `report_int` lines:

```c
        if (local.sump_online && local.sump.pct >= 0) report_int(s_dev_tanks, PARAM_SUMP_PCT, local.sump.pct, &last_sump, 3);
        if (local.utility_online && local.rwt_floty >= 0) report_bool(s_dev_tanks, PARAM_RWT_FLOAT, local.rwt_floty == 1, &last_rwt_float);
        if (local.utility_online) {
            report_bool(s_dev_ground, PARAM_BORE_ON, local.borewell.running, &last_bore_on);
            report_bool(s_dev_ground, PARAM_SMOT_ON, local.sump_motor.running, &last_smot_on);
            if (local.borewell.deci_amps >= 0)
                report_float(s_dev_ground, PARAM_BORE_AMPS, local.borewell.deci_amps / 10.0f, &last_bore_a, 0.3f);
            if (local.sump_motor.deci_amps >= 0)
                report_float(s_dev_ground, PARAM_SMOT_AMPS, local.sump_motor.deci_amps / 10.0f, &last_smot_a, 0.3f);
            if (!local.utility_room.fault) {
                report_float(s_dev_ground, PARAM_UTIL_TEMP, local.utility_room.temp_deci_c / 10.0f, &last_util_t, 0.3f);
                report_float(s_dev_ground, PARAM_UTIL_HUM,  local.utility_room.hum_deci_pct / 10.0f, &last_util_h, 1.0f);
            }
        }
```

- [ ] **Step 4: Node-lost alert**

In `evaluate_alerts()` replace the `any_lost` block:

```c
    bool any_lost = !s->rwt_online || !s->twt_online || !s->battery_online ||
                    (s->sump_configured && !s->sump_online) ||
                    (s->utility_configured && !s->utility_online);
    snprintf(msg, sizeof(msg), "Node offline: %s%s%s%s%s. Check bus, terminators, or the LAN.",
             s->rwt_online ? "" : "0x02 RWT ",
             s->twt_online ? "" : "0x03 TWT ",
             s->battery_online ? "" : "0x04 Battery ",
             (s->sump_configured && !s->sump_online) ? "0x05 Sump " : "",
             (s->utility_configured && !s->utility_online) ? "0x06 Utility " : "");
    alert_eval(&s_al_node_lost, any_lost, msg, 0, 1);
```

- [ ] **Step 5: Checks and build**

Run: `python docs/check_alerts.py` — Expected: passes (the message got shorter at the front to make room for two more nodes; if the checker says it is over the cap, shorten the tail to `"Check bus or LAN."`).
Run: `cd firmware/hub_prod; idf.py build` — Expected: clean.

- [ ] **Step 6: Commit**

```bash
git add firmware/hub_prod/main/app_priv.h firmware/hub_prod/main/app_main.c
git commit -m "Hub cloud: Ground Floor device, sump level and RWT float params, node-lost covers 0x05/0x06"
```

---

### Task 6: Dashboard

**Files:**
- Modify: `firmware/hub/data/dashboard.html`

**Interfaces:**
- Consumes the telemetry keys from Task 4 and history columns 9-12 from Task 3.

- [ ] **Step 1: Utility room card**

After the Battery Room card (line 476-480) add:

```html
  <div class="card">
    <span class="legend">Utility Room · Node 0x06</span>
    <div class="climate" id="cUtil"></div>
  </div>
```

In the render function where `climate($("#cBatt"), s.env.battery_room);` is called (line 1627), add:

```js
  climate($("#cUtil"), (s.env && s.env.utility_room) || { state: "OFFLINE" });
```

- [ ] **Step 2: Borewell and sump-motor amps and phases**

In `buildPlant` (lines 1185-1186) pass the clamps:

```js
    smotor: () => machineUnit(P.sump_motor, "Sump Motor", "smotor", 26, subPumpGlyph, s.motors && s.motors.sump_motor),
    bore:   () => machineUnit(P.borewell, "Borewell Pump", "bore", 26, boreGlyph, s.motors && s.motors.borewell),
```

In `machineUnit`, after the `if (ct) { ... }` block, add the phase line:

```js
  if (ct && Array.isArray(ct.phases) && ct.phases.some(p => p !== null && p !== undefined)) {
    const ph = node("amps");
    ph.dataset.phases = id;
    ph.innerHTML = phasesText(ct.phases, ct.imbalance_pct);
    u.appendChild(ph);
  }
```

and next to `ampsText`:

```js
/* "L1 4.1 · L2 4.0 · L3 — · imb 5%" - a socket with no clamp draws a dash, never 0 */
function phasesText(ph, imb) {
  const one = (v, i) => `L${i + 1} ${v === null || v === undefined ? "\u2014" : v.toFixed(1)}`;
  const s = ph.map(one).join(" \u00b7 ");
  return (imb > 0 ? `${s} \u00b7 imb ${imb}%` : s);
}
```

Where the page updates amps on each poll (search for `data-amps` / `[data-amps=`), add the matching update for `[data-phases="bore"]` and `[data-phases="smotor"]` using `phasesText`. Also check the fixed sump-motor holder in the sunk sump cell (line 1347) still passes `s.motors.sump_motor` if it builds its own `machineUnit`; if it does, add the sixth argument there too.

- [ ] **Step 3: Node list and event names**

`rows($("#cNodes"), ...)` (line 1687): note becomes

```js
    const extra = (n.ip ? ` ${n.ip}` : "") + (n.fw ? ` · fw ${n.fw}` : "");
    return [`${n.id}  ${n.role}`, label, kind, `${n.link}${extra} · ${ago(n.age_s)}`];
```

`nodeName` (line 2026):

```js
const nodeName = a => ({ 2: "0x02 Raw Water", 3: "0x03 Treated Water", 4: "0x04 Battery Room", 5: "0x05 Sump", 6: "0x06 Utility" })[a] || ("0x0" + a);
```

- [ ] **Step 4: Trend**

CSS line 389-390: add `.trend .ln.sump{stroke:var(--raw); stroke-dasharray:2 3} .trend .ln.util{stroke:var(--ink-2); stroke-dasharray:1 3}`.

In `drawTrend`, after `endLabel(line(3, "dos", Yt), "DOS", v => v + "%");` add `endLabel(line(9, "sump", Yt), "SUMP", v => v + "%");`. The temps line (1955) becomes `rows.flatMap(r => [r[7], r[8], r[12]])`, and wherever the RO and battery temperature lines are drawn (`line(7, "ro", Yc)`, `line(8, "bat", Yc)`) add `endLabel(line(12, "util", Yc), "UTIL", v => (v / 10).toFixed(1) + "°")` in the same style as the existing two. Update the row-layout comment at line 1891 to `[t, rwt, twt, dos, flags, hpp_da, rwp_da, ro_t, bat_t, sump, bore_da, smot_da, util_t]`. `demoHistory()` (search for it) must emit 13-column rows: append `, r[1] - 10, 41, 33, r[7] + 5` style plausible values so the demo strip still draws.

- [ ] **Step 5: Demo data**

In `demo` add `env.utility_room: { t: 33.8, rh: 55, state: "ONLINE", src: "SHT30 · Node 0x06", age_s: 3 }`, `motors.borewell: { amps: 4.1, phases: [4.1, 4.0, 3.9], imbalance_pct: 5, running: true }`, `motors.sump_motor: { amps: 3.3, phases: [3.3, 3.2, null], imbalance_pct: 3, running: true }`, `tanks.sump.source: "ultrasonic"`, and `ip: "192.168.1.51", fw: "a3cb57d"` on the 0x05 node and `ip: "192.168.1.52", fw: "a3cb57d"` on 0x06.

- [ ] **Step 6: Verify**

Run: `node -e "const fs=require('fs');const h=fs.readFileSync('firmware/hub/data/dashboard.html','utf8');const m=h.match(/<script>([\s\S]*)<\/script>/);new Function(m[1]);console.log('syntax ok')"` — Expected: `syntax ok`. (If `node` is not installed, open the file in a browser with the hub unreachable: the demo must render a Utility Room card, phases under the borewell, and a SUMP line on the trend.)

Run: `python docs/check_telemetry.py` — Expected: OK (it reads the dashboard for consumed keys; nothing new should break).

Run: `cd firmware/hub_prod; idf.py build` — Expected: clean; the page gz is regenerated.

- [ ] **Step 7: Commit**

```bash
git add firmware/hub/data/dashboard.html
git commit -m "Dashboard: utility room card, borewell phases, node IP and fw, sump and utility trend lines"
```

---

### Task 7: Fake node and bench procedure

**Files:**
- Create: `docs/fake_gf_node.py`
- Modify: `firmware/hub_prod/README.md` (bench section)

**Interfaces:**
- Produces a script: `python docs/fake_gf_node.py --role sump --port 8085` / `--role util --port 8086`, with `GET /set?distance_mm=1200&quality=95`, `GET /set?bore_mv=400,395,390&sump_on=1`, `GET /silent?on=1` (drops requests until `on=0`).

- [ ] **Step 1: Write the script**

```python
"""A ground-floor node on a PC, so the hub's polling, offline latch, re-probe
and /cal maths are proven before either board exists.

    python docs/fake_gf_node.py --role sump --port 8085
    python docs/fake_gf_node.py --role util --port 8086

Then on the hub's /cal page: Sump node IP = <this PC>:8085, Utility = <PC>:8086.

Knobs, from a browser or curl on the same port:
    /set?distance_mm=1200&quality=95&status=OK       (sump, ultrasonic)
    /set?source=pressure&loop_ua=11200               (sump, loop)
    /set?bore_mv=400,395,390&sump_mv=380,0,0&sump_on=1&rwt_floty=1&t_deci_c=318
    /silent?on=1      answers nothing until /silent?on=0 - the hub must go OFFLINE
                      after 3 misses and come back on the first reply
"""
import argparse, json, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

STATE = {
    'sump': {'id': 5, 'fw': 'fake', 'source': 'ultrasonic', 'distance_mm': 1750, 'quality': 95,
             'status': 'OK', 'loop_ua': None},
    'util': {'id': 6, 'fw': 'fake', 'bore_mv': [412, 405, 398], 'sump_mv': [398, 0, 0], 'sump_on': False,
             'rwt_floty': None, 't_deci_c': 312, 'rh_deci_pct': 548, 'sht_ok': True},
}
SILENT = {'on': False}
T0 = time.time()

def coerce(role, k, v):
    if k in ('bore_mv', 'sump_mv'):
        return [int(x) for x in v.split(',')]
    if k in ('sump_on', 'sht_ok'):
        return v not in ('0', 'false', '')
    if k == 'rwt_floty':
        return None if v in ('null', '') else v not in ('0', 'false')
    if k in ('source', 'status'):
        return v
    if k == 'loop_ua' and v in ('null', ''):
        return None
    return int(v)

class H(BaseHTTPRequestHandler):
    role = 'sump'
    def log_message(self, *a): pass
    def _send(self, code, body):
        data = body.encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == '/set':
            for k, v in q.items():
                if k in STATE[self.role]:
                    STATE[self.role][k] = coerce(self.role, k, v)
            return self._send(200, json.dumps(STATE[self.role]))
        if u.path == '/silent':
            SILENT['on'] = q.get('on', '1') not in ('0', 'false')
            return self._send(200, json.dumps(SILENT))
        if u.path == '/api/telemetry':
            if SILENT['on']:
                time.sleep(5)          # longer than the hub's 2 s timeout
                return
            body = dict(STATE[self.role])
            body['uptime_s'] = int(time.time() - T0)
            body['rssi'] = -60
            if self.role == 'sump' and body['source'] == 'pressure':
                body['distance_mm'] = None; body['quality'] = None
            return self._send(200, json.dumps(body))
        self._send(404, '{}')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--role', choices=['sump', 'util'], required=True)
    ap.add_argument('--port', type=int, required=True)
    a = ap.parse_args()
    H.role = a.role
    print('fake %s node on port %d - /api/telemetry, /set?..., /silent?on=1' % (a.role, a.port))
    ThreadingHTTPServer(('0.0.0.0', a.port), H).serve_forever()

if __name__ == '__main__':
    main()
```

- [ ] **Step 2: Prove the script against the parsers**

Run: `python docs/fake_gf_node.py --role sump --port 8085` in one terminal, then `curl -s http://127.0.0.1:8085/api/telemetry` — Expected: JSON with `"id":5`. `curl -s "http://127.0.0.1:8085/set?source=pressure&loop_ua=11200"` then fetch again — Expected: `"distance_mm":null` and `"loop_ua":11200`.

Add a third harness to `docs/check_gf.py` that starts the fake node in a thread, fetches both roles' JSON and feeds them through the compiled parsers (write the JSON to a temp file, extend `PARSE_HARNESS` main to accept `argv[1]`/`argv[2]` paths and assert both parse). Simplest: extend `PARSE_HARNESS` with

```c
    if (argc >= 3) {
        FILE *f = fopen(argv[1], "rb"); static char b1[600]; size_t n1 = fread(b1, 1, 599, f); fclose(f); b1[n1] = 0;
        f = fopen(argv[2], "rb"); static char b2[600]; size_t n2 = fread(b2, 1, 599, f); fclose(f); b2[n2] = 0;
        gf_sump_t fs = {0}; gf_util_t fu = {0};
        CHECK(gf_parse_sump(b1, &fs)); CHECK(gf_parse_util(b2, &fu));
    }
```

(change `int main(void)` to `int main(int argc, char **argv)`) and in Python, import `fake_gf_node` from `docs/`, build both bodies via its `STATE` plus `uptime_s`/`rssi`, write them to temp files and pass the paths when running the exe.

Run: `python docs/check_gf.py` — Expected: all three OK.

- [ ] **Step 3: Bench procedure in the hub README**

Add a section after "Reaching it afterwards":

````markdown
## Ground-floor nodes on the bench

No node hardware needed to prove the hub side:

```
python docs/fake_gf_node.py --role sump --port 8085
python docs/fake_gf_node.py --role util --port 8086
```

On `/cal` → Ground floor nodes, enter `<PC IP>:8085` and `<PC IP>:8086`. Within
5 s the dashboard's sump, borewell and sump-motor cards un-hatch and the node
list shows both as Online with `fw fake`.

Then: `curl "http://<PC>:8085/silent?on=1"` — after ~15 s the sump card hatches,
the event log shows `Node offline 0x05 Sump`, the console says it is probing
every 30 s. `curl "http://<PC>:8085/silent?on=0"` — within 30 s it is back.

Clear the IP field and save: the node goes to "not fitted", cards hatch, no
alert. The IPs are the only thing this stores; nothing else on the node side
survives a node reflash because nothing else lives there.
````

- [ ] **Step 4: Commit**

```bash
git add docs/fake_gf_node.py docs/check_gf.py firmware/hub_prod/README.md
git commit -m "Bench: fake ground-floor node, parser check against its output, procedure"
```

---

### Task 8: Node firmware skeleton — project, Wi-Fi, HTTP server, version

**Files:**
- Create: `firmware/gf_node/CMakeLists.txt`, `firmware/gf_node/partitions.csv`, `firmware/gf_node/sdkconfig.defaults`
- Create: `firmware/gf_node/main/CMakeLists.txt`, `firmware/gf_node/main/Kconfig.projbuild`
- Create: `firmware/gf_node/main/gf.h`, `firmware/gf_node/main/main.c`, `firmware/gf_node/main/net.c`, `firmware/gf_node/main/web.c`
- Create: `firmware/gf_node/main/sensors_sump.c`, `firmware/gf_node/main/sensors_util.c` (stubs that report fixed values; real sensing in Tasks 10 and 11)
- Create: `firmware/gf_node/.gitignore` with `build/`, `sdkconfig`, `sdkconfig.old`, `dependencies.lock`

**Interfaces (`gf.h`):**
```c
#define GF_NODE_ID   (CONFIG_GF_ROLE_SUMP ? 5 : 6)
/* net.c */  esp_err_t net_start(void); bool net_up(void); int net_rssi(void);
/* web.c */  esp_err_t web_start(void); bool web_served_once(void);
/* ota.c */  esp_err_t ota_handle(httpd_req_t *req);           /* Task 9 */
/* sensors_*.c */ void sensors_init(void); void sensors_sample(void); int sensors_json(char *buf, size_t len);
```
`sensors_json` writes the **whole** telemetry object including `id`, `fw`, `uptime_s`, `rssi`.

- [ ] **Step 1: Project files**

`firmware/gf_node/CMakeLists.txt` — copy the hub's `git describe` block verbatim (lines 3-40 of `firmware/hub_prod/CMakeLists.txt`), then `include($ENV{IDF_PATH}/tools/cmake/project.cmake)` and `project(gf_node)`.

`firmware/gf_node/partitions.csv`:

```
# Name,     Type, SubType, Offset,   Size
nvs,        data, nvs,     0x9000,   0x6000,
otadata,    data, ota,     0xf000,   0x2000,
phy_init,   data, phy,     0x11000,  0x1000,
ota_0,      app,  ota_0,   0x20000,  0x1E0000,
ota_1,      app,  ota_1,   0x200000, 0x1E0000,
```

`firmware/gf_node/sdkconfig.defaults`:

```
CONFIG_IDF_TARGET="esp32"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
# New image boots pending-verify; main.c marks it valid once Wi-Fi is up and
# one telemetry request has been served, else it reboots into the old slot.
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
CONFIG_ESP_TASK_WDT_INIT=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=30
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=6144
```

`firmware/gf_node/main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.c" "net.c" "web.c" "ota.c" "sensors_sump.c" "sensors_util.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_netif nvs_flash esp_http_server app_update
                                     esp_adc driver esp_timer)
```

(`ota.c` is created in Task 9; for this task create it with just the include and an `ota_handle` that returns `httpd_resp_send_500(req)`, so the build passes.)

`firmware/gf_node/main/Kconfig.projbuild`:

```
menu "Ground-floor node"

    choice GF_ROLE
        prompt "Role"
        default GF_ROLE_SUMP
        config GF_ROLE_SUMP
            bool "Sump level node 0x05"
        config GF_ROLE_UTILITY
            bool "Utility room node 0x06"
    endchoice

    config GF_WIFI_SSID
        string "Wi-Fi SSID"
        default "changeme"
    config GF_WIFI_PASS
        string "Wi-Fi password"
        default "changeme"
    config GF_STATIC_IP
        string "Static IP (what you type on the hub's /cal)"
        default "192.168.1.51"
    config GF_GATEWAY
        string "Gateway"
        default "192.168.1.1"
    config GF_NETMASK
        string "Netmask"
        default "255.255.255.0"
    config GF_RWT_FLOTY_WIRED
        bool "RWT floaty opto is wired to GPIO 26 (utility node)"
        default n
        help
            Off until the Astero's TWT FLOTY loop voltage has been metered and
            an optocoupler fitted. While off the node reports rwt_floty:null.
endmenu
```

- [ ] **Step 2: `gf.h`**

```c
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "sdkconfig.h"

#if CONFIG_GF_ROLE_SUMP
#define GF_NODE_ID          5
#define GF_ROLE_NAME        "sump"
#else
#define GF_NODE_ID          6
#define GF_ROLE_NAME        "utility"
#endif

/* GPIO, spec 4.3. Sump node. */
#define GPIO_US_TRIG        5
#define GPIO_US_ECHO        18      /* through 1 k / 2 k divider */
#define GPIO_LOOP_SENSE     34      /* ADC1_CH6, J-LOOP pin 2 */
#define GPIO_PRESS_FIT      25      /* J-PRESS shunt to GND = pressure */
/* Utility node. */
#define GPIO_BORE_CT_L1     32      /* ADC1_CH4 */
#define GPIO_BORE_CT_L2     33      /* ADC1_CH5 */
#define GPIO_BORE_CT_L3     34      /* ADC1_CH6 */
#define GPIO_SUMP_CT_L1     35      /* ADC1_CH7 */
#define GPIO_SUMP_CT_L2     36      /* ADC1_CH0 */
#define GPIO_SUMP_CT_L3     39      /* ADC1_CH3 */
#define GPIO_PUMP_ON        25      /* Astero PUMP ON C/NO, closed = running */
#define GPIO_RWT_FLOTY      26      /* opto across the float loop, when wired */
#define GPIO_I2C_SDA        21
#define GPIO_I2C_SCL        22

#define SAMPLE_PERIOD_MS    2000
#define OTA_CONFIRM_MS      120000  /* must be valid before this or roll back */

esp_err_t net_start(void);
bool      net_up(void);
int       net_rssi(void);

esp_err_t web_start(void);
bool      web_served_once(void);      /* a /api/telemetry has been answered */

esp_err_t ota_handle(httpd_req_t *req);

void sensors_init(void);
void sensors_sample(void);            /* one cycle; called from main every SAMPLE_PERIOD_MS */
int  sensors_json(char *buf, size_t len);   /* the whole telemetry object */
```

- [ ] **Step 3: `net.c`**

```c
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "gf.h"

static const char *TAG = "net";
static bool s_up = false;
static esp_netif_t *s_netif;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_up = false;
        ESP_LOGW(TAG, "disconnected - retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_up = true;
        ESP_LOGI(TAG, "up at " CONFIG_GF_STATIC_IP);
    }
}

esp_err_t net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    /* Static, because the hub polls this address and a DHCP lease that moves is
     * a node that vanishes. */
    esp_netif_dhcpc_stop(s_netif);
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr      = esp_ip4addr_aton(CONFIG_GF_STATIC_IP);
    ip.gw.addr      = esp_ip4addr_aton(CONFIG_GF_GATEWAY);
    ip.netmask.addr = esp_ip4addr_aton(CONFIG_GF_NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &ip));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, CONFIG_GF_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, CONFIG_GF_WIFI_PASS, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));   /* the hub polls; do not doze */
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

- [ ] **Step 4: `web.c`**

```c
#include "esp_app_desc.h"
#include "esp_log.h"
#include "gf.h"

static const char *TAG = "web";
static bool s_served = false;

static esp_err_t root_get(httpd_req_t *req)
{
    char b[96];
    int n = snprintf(b, sizeof b, "gf_node %s id %d fw %s\nGET /api/telemetry  POST /ota\n",
                     GF_ROLE_NAME, GF_NODE_ID, esp_app_get_description()->version);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, b, n);
}

static esp_err_t telemetry_get(httpd_req_t *req)
{
    static char json[512];
    int n = sensors_json(json, sizeof json);
    if (n <= 0 || n >= (int)sizeof json) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, json, n);
    if (e == ESP_OK) s_served = true;
    return e;
}

static esp_err_t ota_post(httpd_req_t *req) { return ota_handle(req); }

bool web_served_once(void) { return s_served; }

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 3;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    httpd_handle_t srv = NULL;
    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err)); return err; }
    httpd_uri_t r = { .uri = "/",              .method = HTTP_GET,  .handler = root_get };
    httpd_uri_t t = { .uri = "/api/telemetry", .method = HTTP_GET,  .handler = telemetry_get };
    httpd_uri_t o = { .uri = "/ota",           .method = HTTP_POST, .handler = ota_post };
    httpd_register_uri_handler(srv, &r);
    httpd_register_uri_handler(srv, &t);
    httpd_register_uri_handler(srv, &o);
    ESP_LOGI(TAG, "http://" CONFIG_GF_STATIC_IP "/api/telemetry");
    return ESP_OK;
}
```

- [ ] **Step 5: `main.c`**

```c
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "gf.h"

static const char *TAG = "gf";

/* Rollback. With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE a freshly OTA'd image
 * boots as pending-verify. It becomes permanent only when the node is on the
 * network AND has answered the hub once - the two things an update can break.
 * If that has not happened within OTA_CONFIRM_MS the node reboots into the
 * previous slot on its own. A non-OTA boot has nothing pending and this is a
 * no-op. */
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

static void sample_task(void *arg)
{
    (void)arg;
    while (true) {
        sensors_sample();
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "gf_node %s (id 0x%02X) fw %s", GF_ROLE_NAME, GF_NODE_ID, esp_app_get_description()->version);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    sensors_init();
    ESP_ERROR_CHECK(net_start());
    ESP_ERROR_CHECK(web_start());
    xTaskCreate(sample_task, "sample", 4096, NULL, 5, NULL);
    ota_watch();   /* returns immediately on a normal boot */
}
```

- [ ] **Step 6: Stub sensors so both roles build**

`sensors_sump.c`:

```c
#include <stdio.h>
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "gf.h"
#if CONFIG_GF_ROLE_SUMP
void sensors_init(void) {}
void sensors_sample(void) {}
int sensors_json(char *buf, size_t len)
{
    return snprintf(buf, len,
        "{\"id\":5,\"fw\":\"%s\",\"uptime_s\":%lld,\"rssi\":%d,"
        "\"source\":\"ultrasonic\",\"distance_mm\":null,\"quality\":null,\"status\":\"NO_ECHO\",\"loop_ua\":null}",
        esp_app_get_description()->version, esp_timer_get_time() / 1000000, net_rssi());
}
#endif
```

`sensors_util.c`:

```c
#include <stdio.h>
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "gf.h"
#if CONFIG_GF_ROLE_UTILITY
void sensors_init(void) {}
void sensors_sample(void) {}
int sensors_json(char *buf, size_t len)
{
    return snprintf(buf, len,
        "{\"id\":6,\"fw\":\"%s\",\"uptime_s\":%lld,\"rssi\":%d,"
        "\"bore_mv\":[0,0,0],\"sump_mv\":[0,0,0],\"sump_on\":false,\"rwt_floty\":null,"
        "\"t_deci_c\":0,\"rh_deci_pct\":0,\"sht_ok\":false}",
        esp_app_get_description()->version, esp_timer_get_time() / 1000000, net_rssi());
}
#endif
```

- [ ] **Step 7: Build both roles**

Run (IDF shell): `cd firmware/gf_node; idf.py set-target esp32; idf.py build` — Expected: clean, sump role (default).
Run: `idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.util" build` after creating `firmware/gf_node/sdkconfig.util` containing `CONFIG_GF_ROLE_UTILITY=y` and `CONFIG_GF_STATIC_IP="192.168.1.52"` — Expected: clean. (Delete `sdkconfig` between role builds, or use `-B build_util` for a second build dir; document whichever you used in the README in Task 9.)

Check the JSON the stub emits parses: copy the format string's literal output with plausible values into `python -c "import json; json.loads('...')"`.

- [ ] **Step 8: Commit**

```bash
git add firmware/gf_node
git commit -m "gf_node: ESP-IDF project, role switch, static-IP Wi-Fi, HTTP telemetry skeleton"
```

---

### Task 9: Node OTA with rollback, and the node README

**Files:**
- Modify: `firmware/gf_node/main/ota.c`
- Create: `firmware/gf_node/README.md`

**Interfaces:**
- Consumes: `ota_handle` route from `web.c`, `ota_watch` in `main.c`.
- Produces: `POST /ota` with the raw `.bin` as body → `200 OK, rebooting into ota_N` or `4xx/5xx` with a reason.

- [ ] **Step 1: `ota.c`**

```c
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gf.h"

static const char *TAG = "ota";

/* The whole image arrives as the POST body and is streamed into the slot that
 * is not running. esp_ota_end() validates the image header and checksum, so a
 * text file or a hub build sent here by mistake is refused before it can be
 * booted. Rollback is main.c's job: the new image must prove itself. */
esp_err_t ota_handle(httpd_req_t *req)
{
    int total = req->content_len;
    if (total < 100000) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "body too small to be a firmware image\n");
    }
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next == NULL) {
        return httpd_resp_send_500(req);
    }
    if (total > (int)next->size) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "image larger than the OTA slot\n");
    }
    ESP_LOGI(TAG, "receiving %d bytes into %s", total, next->label);

    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return httpd_resp_send_500(req);
    }

    static char buf[4096];
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf, sizeof buf);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            esp_ota_abort(h);
            ESP_LOGE(TAG, "receive failed at %d/%d", got, total);
            return httpd_resp_send_500(req);
        }
        err = esp_ota_write(h, buf, r);
        if (err != ESP_OK) {
            esp_ota_abort(h);
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            return httpd_resp_send_500(req);
        }
        got += r;
    }

    err = esp_ota_end(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s (not a valid image?)", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "image rejected - not a valid ESP32 app image\n");
    }
    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        return httpd_resp_send_500(req);
    }

    char msg[64];
    snprintf(msg, sizeof msg, "OK %d bytes, rebooting into %s\n", got, next->label);
    httpd_resp_sendstr(req, msg);
    ESP_LOGI(TAG, "%s", msg);
    vTaskDelay(pdMS_TO_TICKS(500));   /* let the response leave */
    esp_restart();
    return ESP_OK;
}
```

- [ ] **Step 2: README**

````markdown
# gf_node — ground-floor Wi-Fi nodes

One ESP-IDF project, two roles. Polled by the terrace hub over the LAN
(`docs/superpowers/specs/2026-09-07-ground-floor-wifi-nodes-design.md`).

| Role | Node | Reads |
| :--- | :--- | :--- |
| Sump | `0x05` | AJ-SR04M ultrasonic, or a 4-20 mA transducer when `J-PRESS` is shunted |
| Utility | `0x06` | 3 borewell CTs, 3 sump-motor CT channels, Astero `PUMP ON`, RWT floaty, SHT30 |

## Build

ESP-IDF 5.4.4 shell (`C:\Espressif\tools\Microsoft.v5.4.4.PowerShell_profile.ps1`).

```
cd firmware/gf_node
idf.py set-target esp32
idf.py menuconfig          # Ground-floor node -> role, SSID, password, static IP
idf.py build
idf.py -p COMx flash monitor
```

The static IP you set here is what you type on the hub's `/cal` page. Give it a
reservation on the router too, or pick one outside the DHCP pool.

Two roles from one tree: keep a build dir per role —
`idf.py -B build_sump build`, `idf.py -B build_util build` — each with its own
`sdkconfig` (`idf.py -B build_util menuconfig`).

## OTA

Two app slots, rollback on. From any PC on the LAN:

```
curl --data-binary @build_sump/gf_node.bin http://192.168.1.51/ota
```

The node writes the image into the idle slot, validates it, and reboots. The
new image marks itself good only after it has Wi-Fi **and** has answered one
`/api/telemetry`; if that does not happen within 120 s it reboots into the old
image by itself. So a build with the wrong SSID costs two minutes, not a ladder.

The hub shows each node's `fw` (the git description) in its node list, which
is how you know the push landed.

## What it answers

`GET /` — one line of identity. `GET /api/telemetry` — the JSON in the spec §4,
raw readings only; every calibration number lives on the hub. `POST /ota` — above.
````

- [ ] **Step 3: Build and bench OTA**

Run: `idf.py -B build_sump build` — Expected: clean.
Flash one ESP32 DevKit over USB with the sump role, watch `monitor` for `up at 192.168.1.51`, `curl http://192.168.1.51/api/telemetry` — Expected: the stub JSON. Then edit nothing, rebuild (version gains nothing, that is fine for a first push) and `curl --data-binary @build_sump/gf_node.bin http://192.168.1.51/ota` — Expected: `OK N bytes, rebooting into ota_1`, monitor shows the reboot, `OTA image confirmed valid` within a few seconds of the next `curl .../api/telemetry`.
Rollback proof: build once with a wrong Wi-Fi password, push it, watch the monitor: `disconnected - retrying` for two minutes, then `rolling back`, then the old image boots and connects.

- [ ] **Step 4: Commit**

```bash
git add firmware/gf_node/main/ota.c firmware/gf_node/README.md
git commit -m "gf_node: POST /ota into the idle slot, rollback unless Wi-Fi and one poll succeed"
```

---

### Task 10: Sump node sensing — ultrasonic or 4-20 mA loop

**Files:**
- Modify: `firmware/gf_node/main/sensors_sump.c`

**Interfaces:**
- Produces the sump JSON from spec §4.1 with live values. `status` follows `ro_node.ino`: `BLIND` when the median is under 200 mm, `NO_ECHO` after 10 consecutive empty cycles, `HW_FAULT` when the loop is outside 3.5–21 mA.

- [ ] **Step 1: Implementation**

```c
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gf.h"

#if CONFIG_GF_ROLE_SUMP
static const char *TAG = "sump";

#define BLIND_ZONE_MM   200
#define US_TIMEOUT_US   35000       /* ~6 m of flight */
#define WINDOW          5
#define AGREE_MM        25
#define DEAD_CYCLES     10
#define LOOP_SENSE_OHMS 100
#define LOOP_SAMPLES    8
#define PRESS_MIN_UA    3500
#define PRESS_MAX_UA    21000

static bool     s_pressure;                  /* J-PRESS shunt read once at boot */
static uint16_t s_win[WINDOW]; static uint8_t s_n, s_next;
static uint16_t s_median_mm; static uint8_t s_quality; static uint8_t s_dead;
static uint32_t s_loop_ua;
static const char *s_status = "NO_ECHO";
static adc_oneshot_unit_handle_t s_adc; static adc_cali_handle_t s_cali; static bool s_cali_ok;

/* One ping. 0 = no credible echo. Polled with esp_timer; +-10 us is +-2 mm. */
static uint16_t ping_once(void)
{
    gpio_set_level(GPIO_US_TRIG, 0); esp_rom_delay_us(4);
    gpio_set_level(GPIO_US_TRIG, 1); esp_rom_delay_us(20);
    gpio_set_level(GPIO_US_TRIG, 0);
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(GPIO_US_ECHO) == 0) {
        if (esp_timer_get_time() - t0 > US_TIMEOUT_US) return 0;
    }
    int64_t rise = esp_timer_get_time();
    while (gpio_get_level(GPIO_US_ECHO) == 1) {
        if (esp_timer_get_time() - rise > US_TIMEOUT_US) return 0;
    }
    uint32_t us = (uint32_t)(esp_timer_get_time() - rise);
    uint16_t mm = (uint16_t)((us * 10UL) / 58UL);
    return mm >= BLIND_ZONE_MM ? mm : 0;   /* sub-blind-zone pulses are artefacts */
}

static uint16_t median_push(uint16_t v)
{
    if (v) { s_win[s_next] = v; s_next = (s_next + 1) % WINDOW; if (s_n < WINDOW) s_n++; }
    if (s_n == 0) return 0;
    uint16_t t[WINDOW]; memcpy(t, s_win, sizeof t);
    for (uint8_t i = 1; i < s_n; i++) { uint16_t k = t[i]; int j = i - 1; while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; } t[j + 1] = k; }
    return t[s_n / 2];
}

/* How many of the window agree with the median, as a percent: the hub gates
 * levels under q60 (MIN_LEVEL_QUALITY), same as the RS485 tank nodes. */
static uint8_t quality_of(uint16_t med)
{
    if (s_n == 0) return 0;
    uint8_t agree = 0;
    for (uint8_t i = 0; i < s_n; i++) if ((s_win[i] > med ? s_win[i] - med : med - s_win[i]) <= AGREE_MM) agree++;
    return (uint8_t)((agree * 100) / s_n);
}

static uint32_t read_loop_ua(void)
{
    uint32_t sum = 0;
    for (int i = 0; i < LOOP_SAMPLES; i++) {
        int raw = 0, mv = 0;
        adc_oneshot_read(s_adc, ADC_CHANNEL_6, &raw);
        if (s_cali_ok && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) sum += (uint32_t)mv;
        else sum += (uint32_t)raw * 3300 / 4095;
        esp_rom_delay_us(500);
    }
    uint32_t mv = sum / LOOP_SAMPLES;
    return (mv * 1000UL) / LOOP_SENSE_OHMS;   /* 100 R: 400 mV = 4000 uA */
}

void sensors_init(void)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << GPIO_US_TRIG, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    io.pin_bit_mask = 1ULL << GPIO_US_ECHO; io.mode = GPIO_MODE_INPUT; gpio_config(&io);
    io.pin_bit_mask = 1ULL << GPIO_PRESS_FIT; io.mode = GPIO_MODE_INPUT; io.pull_up_en = GPIO_PULLUP_ENABLE; gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(10));
    s_pressure = gpio_get_level(GPIO_PRESS_FIT) == 0;

    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &s_adc));
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_6, &c));   /* GPIO 34 */
    adc_cali_line_fitting_config_t cc = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    s_cali_ok = adc_cali_create_scheme_line_fitting(&cc, &s_cali) == ESP_OK;
    ESP_LOGI(TAG, "source: %s", s_pressure ? "4-20 mA loop on GPIO 34 (J-PRESS shunted)" : "AJ-SR04M ultrasonic");
}

void sensors_sample(void)
{
    if (s_pressure) {
        s_loop_ua = read_loop_ua();
        s_status = (s_loop_ua < PRESS_MIN_UA || s_loop_ua > PRESS_MAX_UA) ? "HW_FAULT" : "OK";
        return;
    }
    uint16_t raw = ping_once();
    s_dead = raw ? 0 : (s_dead < 255 ? s_dead + 1 : 255);
    s_median_mm = median_push(raw);
    s_quality = quality_of(s_median_mm);
    if (s_dead >= DEAD_CYCLES || s_n == 0) s_status = "NO_ECHO";
    else if (s_median_mm < BLIND_ZONE_MM)  s_status = "BLIND";
    else                                   s_status = "OK";
}

int sensors_json(char *buf, size_t len)
{
    char dist[8], q[8], ua[12];
    if (s_pressure) { snprintf(dist, sizeof dist, "null"); snprintf(q, sizeof q, "null"); snprintf(ua, sizeof ua, "%lu", (unsigned long)s_loop_ua); }
    else { snprintf(dist, sizeof dist, "%u", s_median_mm); snprintf(q, sizeof q, "%u", s_quality); snprintf(ua, sizeof ua, "null"); }
    return snprintf(buf, len,
        "{\"id\":5,\"fw\":\"%s\",\"uptime_s\":%lld,\"rssi\":%d,"
        "\"source\":\"%s\",\"distance_mm\":%s,\"quality\":%s,\"status\":\"%s\",\"loop_ua\":%s}",
        esp_app_get_description()->version, esp_timer_get_time() / 1000000, net_rssi(),
        s_pressure ? "pressure" : "ultrasonic", dist, q, s_status, ua);
}
#endif
```

- [ ] **Step 2: Build and bench**

Run: `idf.py -B build_sump build` — Expected: clean.
Bench: flash, point an AJ-SR04M at a wall ~1 m away, `curl .../api/telemetry` — Expected: `distance_mm` within 30 mm of a tape measure, `quality` 80-100, `status OK`. Point it at nothing — Expected: after ~20 s `status NO_ECHO`. Shunt `J-PRESS` (GPIO 25 to GND) and reboot — Expected: `source pressure`, `status HW_FAULT`, `loop_ua` near 0 with nothing on the loop. Then the hub, with this IP on `/cal`: sump card un-hatches with a percent once the sump row on `/cal` has full/empty typed.

- [ ] **Step 3: Commit**

```bash
git add firmware/gf_node/main/sensors_sump.c
git commit -m "gf_node sump: ultrasonic median and quality, 4-20 mA loop when J-PRESS is shunted"
```

---

### Task 11: Utility node sensing — six CT channels, PUMP ON, RWT floaty, SHT30

**Files:**
- Modify: `firmware/gf_node/main/sensors_util.c`

**Interfaces:**
- Produces the utility JSON from spec §4.2 with live values. `bore_mv[3]` and `sump_mv[3]` are AC RMS millivolts around the measured mean over 400 samples at 500 µs, one channel at a time (six channels ≈ 1.2 s, inside the 2 s sample period).

- [ ] **Step 1: Implementation**

```c
#include <math.h>
#include <stdio.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gf.h"

#if CONFIG_GF_ROLE_UTILITY
static const char *TAG = "util";

#define RMS_SAMPLES     400     /* 200 ms at 500 us = ten 50 Hz cycles, as the hub */
#define RMS_INTERVAL_US 500

/* Channel order = header order = spec 4.3. */
static const adc_channel_t s_ch[6] = {
    ADC_CHANNEL_4, ADC_CHANNEL_5, ADC_CHANNEL_6,   /* GPIO 32 33 34: bore L1 L2 L3 */
    ADC_CHANNEL_7, ADC_CHANNEL_0, ADC_CHANNEL_3,   /* GPIO 35 36 39: sump L1 L2 L3 */
};
static uint16_t s_mv[6];
static bool s_pump_on; static int8_t s_floty = -1;
static int16_t s_t; static uint16_t s_rh; static bool s_sht_ok;
static adc_oneshot_unit_handle_t s_adc; static adc_cali_handle_t s_cali; static bool s_cali_ok;
static i2c_master_bus_handle_t s_bus; static i2c_master_dev_handle_t s_sht;

static uint32_t read_mv(adc_channel_t ch)
{
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, ch, &raw) != ESP_OK) return 0;
    if (s_cali_ok && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) return (uint32_t)mv;
    return (uint32_t)raw * 3300 / 4095;
}

/* RMS about the measured mean, so the bias pedestal cancels whatever its exact
 * value is. Raw mV: the hub applies A/V, turns and the noise floor. */
static uint16_t rms_mv(adc_channel_t ch)
{
    static int32_t s[RMS_SAMPLES];
    int64_t sum = 0;
    for (int i = 0; i < RMS_SAMPLES; i++) { s[i] = (int32_t)read_mv(ch); sum += s[i]; esp_rom_delay_us(RMS_INTERVAL_US); }
    int32_t mean = (int32_t)(sum / RMS_SAMPLES);
    double acc = 0;
    for (int i = 0; i < RMS_SAMPLES; i++) { double d = (double)(s[i] - mean); acc += d * d; }
    double r = sqrt(acc / RMS_SAMPLES);
    return (uint16_t)(r > 65535 ? 65535 : r + 0.5);
}

static uint8_t sht30_crc8(uint8_t hi, uint8_t lo)
{
    uint8_t d[2] = { hi, lo }, crc = 0xFF;
    for (int i = 0; i < 2; i++) { crc ^= d[i]; for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1); }
    return crc;
}

static bool sht30_read(int16_t *t, uint16_t *rh)
{
    const uint8_t cmd[2] = { 0x2C, 0x06 };
    if (i2c_master_transmit(s_sht, cmd, 2, 100) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t d[6];
    if (i2c_master_receive(s_sht, d, 6, 100) != ESP_OK) return false;
    if (sht30_crc8(d[0], d[1]) != d[2] || sht30_crc8(d[3], d[4]) != d[5]) return false;
    uint16_t rt = ((uint16_t)d[0] << 8) | d[1], rh_raw = ((uint16_t)d[3] << 8) | d[4];
    *t  = (int16_t)(-450 + (int16_t)(((int32_t)1750 * rt) / 65535L));
    *rh = (uint16_t)(((uint32_t)1000 * rh_raw) / 65535UL);
    return true;
}

void sensors_init(void)
{
    gpio_config_t io = { .pin_bit_mask = (1ULL << GPIO_PUMP_ON) | (1ULL << GPIO_RWT_FLOTY),
                         .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);

    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &s_adc));
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    for (int i = 0; i < 6; i++) ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_ch[i], &c));
    adc_cali_line_fitting_config_t cc = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    s_cali_ok = adc_cali_create_scheme_line_fitting(&cc, &s_cali) == ESP_OK;

    i2c_master_bus_config_t bc = { .i2c_port = I2C_NUM_0, .sda_io_num = GPIO_I2C_SDA, .scl_io_num = GPIO_I2C_SCL,
                                   .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &s_bus));
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x44, .scl_speed_hz = 100000 };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dc, &s_sht));
    ESP_LOGI(TAG, "6 ADC1 clamp channels, PUMP ON on %d, floaty %s, SHT30 0x44", GPIO_PUMP_ON,
             CONFIG_GF_RWT_FLOTY_WIRED ? "wired" : "not wired (null)");
}

void sensors_sample(void)
{
    for (int i = 0; i < 6; i++) s_mv[i] = rms_mv(s_ch[i]);
    s_pump_on = gpio_get_level(GPIO_PUMP_ON) == 0;          /* contact closed pulls low */
#if CONFIG_GF_RWT_FLOTY_WIRED
    s_floty = gpio_get_level(GPIO_RWT_FLOTY) == 0 ? 1 : 0;  /* opto on = float closed */
#else
    s_floty = -1;
#endif
    s_sht_ok = sht30_read(&s_t, &s_rh);
}

int sensors_json(char *buf, size_t len)
{
    const char *fl = s_floty < 0 ? "null" : (s_floty ? "true" : "false");
    return snprintf(buf, len,
        "{\"id\":6,\"fw\":\"%s\",\"uptime_s\":%lld,\"rssi\":%d,"
        "\"bore_mv\":[%u,%u,%u],\"sump_mv\":[%u,%u,%u],\"sump_on\":%s,\"rwt_floty\":%s,"
        "\"t_deci_c\":%d,\"rh_deci_pct\":%u,\"sht_ok\":%s}",
        esp_app_get_description()->version, esp_timer_get_time() / 1000000, net_rssi(),
        s_mv[0], s_mv[1], s_mv[2], s_mv[3], s_mv[4], s_mv[5],
        s_pump_on ? "true" : "false", fl, s_t, s_rh, s_sht_ok ? "true" : "false");
}
#endif
```

Add `m` to the link libraries if `sqrt` fails to link (ESP-IDF links libm by default; it should not).

- [ ] **Step 2: Build and bench**

Run: `idf.py -B build_util build` — Expected: clean.
Bench with a DevKit and an SHT30: `curl .../api/telemetry` — Expected: `sht_ok true`, plausible `t_deci_c`; all six `*_mv` near 0-10 with nothing on the bias networks (or noise if pins float; the breakout is what fixes that). Short GPIO 25 to GND — Expected: `sump_on true`. On the hub, with a bias breakout and one clamp on a known load: the borewell row on `/cal` shows the phase, `run A` threshold turns the borewell card Running.

- [ ] **Step 3: Commit**

```bash
git add firmware/gf_node/main/sensors_util.c
git commit -m "gf_node utility: six clamp channels RMS, PUMP ON contact, floaty opto, SHT30"
```

---

### Task 12: Documentation

**Files:**
- Modify: `docs/RS485_PROTOCOL.md` §5 (lines 149-185)
- Modify: `docs/WIRING.md` §11.1-11.3 (from line 1351)
- Modify: `docs/DASHBOARD_AND_RAINMAKER.md` (device/param tables, the "Wi-Fi Node Disconnect" row at line 144)
- Modify: `firmware/hub_prod/README.md` (the `/cal` list, Ground floor nodes)

- [ ] **Step 1: Protocol §5 — pull, not push**

Replace §5 with: heading "Wi-Fi nodes — polled JSON (Ground floor)", one paragraph stating the hub GETs `http://<ip>/api/telemetry` every 5 s, 2 s timeout, 3 misses → OFFLINE, 30 s re-probe, IP set on `/cal`; then the two JSON blocks from the spec §4.1 (both ultrasonic and pressure) and §4.2 verbatim, the field notes, and the GPIO table from spec §4.3. Delete the sentence in §7 item 3 about "emergency pump interlocks engage" and replace it with "status is set to OFFLINE, the cards hatch, and the node-lost alert names it".

- [ ] **Step 2: WIRING §11**

§11.1: power becomes `230 V → 12 V module → buck → 5 V`; add `J-LOOP` / `J-PRESS` with a cross-reference to §9.4.2 for the passives, and the two GPIO lines (34 sense, 25 shunt). §11.2: replace the opto and relay-board bullets with: `PUMP ON (C, NO)` → GPIO 25 with pull-up; RWT floaty opto → GPIO 26 **after** metering the Astero's `TWT FLOTY` loop (state the rule from spec §2); SHT30 on 21/22; **no relay board**. §11.3: table becomes six rows `BORE_CT_L1/L2/L3` 32/33/34 and `SUMP_CT_L1/L2/L3` 35/36/39, with a note that sump L3's socket is built and empty. Add a §11.4 "The two panels, as found 2026-09-07" containing spec §2 verbatim, including the "PUMP CONT. wire to nowhere" finding.

- [ ] **Step 3: Dashboard and RainMaker doc**

Add the `Ground Floor` device and its six params, plus `Sump Level` and `RWT Float Full` under Water Tanks, to the parameter table. Add the Utility Room card to the dashboard section. Change the "Wi-Fi Node Disconnect" row to: `WARNING — Node offline: 0x05 Sump / 0x06 Utility. Fifteen seconds of no reply; re-probed every 30 s.` Remove "Plant interlocks revert to safe default state."

- [ ] **Step 4: Hub README**

In the "Reaching it afterwards" / calibration notes, list the new `/cal` fieldset and the two new clamp rows, and point at `firmware/gf_node/README.md`.

- [ ] **Step 5: Run every check, build the hub, commit, push**

```
python docs/check_gf.py
python docs/check_telemetry.py
python docs/check_alerts.py
python docs/check_pinmap.py
cd firmware/hub_prod; idf.py build
```

All must pass; `check_pinmap.py` compares `app_priv.h` against the docs — if it now flags the new `NODE_ADDR_*` or GPIO mentions, fix the doc, not the checker.

```bash
git add docs/RS485_PROTOCOL.md docs/WIRING.md docs/DASHBOARD_AND_RAINMAKER.md firmware/hub_prod/README.md
git commit -m "Docs: ground-floor nodes are polled; panels as found; node wiring and OTA"
git push origin main
```

---

## Self-review against the spec

- §1 two nodes, no relays — Tasks 8-11; relays absent. ✓
- §2 panel findings — Task 12 §11.4; `PUMP ON` used in Task 11; floaty null until wired via Kconfig. ✓
- §3 topology, cadence, latch, empty IP — Task 2 latch (tested), Task 3 task, Task 4 `/cal`. Events `EVT_NODE_ON/OFF` — Task 3. ✓
- §4.1/4.2 JSON — parsers Task 2 (tested against the exact shapes and the fake node), emitters Tasks 10/11, fake node Task 7. Field names identical throughout: `source, distance_mm, quality, status, loop_ua, bore_mv, sump_mv, sump_on, rwt_floty, t_deci_c, rh_deci_pct, sht_ok`. ✓
- §4.3 GPIO — `gf.h` Task 8. ✓ Pressure loop passives and 12 V supply — Task 12 docs; hub-side conversion `gfLoopDistanceMM` Task 1 (tested). ✓
- §5 hub state — Task 2; derivation Task 3; telemetry keys Task 4; history Task 3. ✓
- §6 cal page — Task 4 (IPs, sump range, remote clamp rows with run threshold). ✓
- §7 cloud + node-lost — Task 5. ✓
- §8 dashboard — Task 6. ✓
- §9 node firmware, OTA, version — Tasks 8-9. ✓
- §10 testing — check_gf.py Tasks 1/2/7, check_telemetry Task 4, fake node Task 7, OTA rollback bench Task 9. ✓
- §11 docs — Task 12. ✓

Type consistency: `cal_set_ct` has five parameters everywhere (Task 1 header, impl, `cal_ct_post`). `gf_link_result(gf_link_t*, bool, int64_t)` same in header, impl, harness. `gfPhaseDeciAmps(uint16_t, uint16_t, uint8_t)` same in header, impl, harness, `gf_apply`. `hist_rec_t` column order `sump, bore_da, smot_da, util_t` matches `history_push`, `history_get` and the dashboard indices 9-12.
