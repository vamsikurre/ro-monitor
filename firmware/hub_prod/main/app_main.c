/*
 * app_main.c — RO Monitor production hub
 *
 * Structure follows the gate-controller in this workspace: ESP-IDF, RainMaker as
 * a managed component, two OTA slots, BLE provisioning with a static PoP, and
 * remote firmware updates pushed from the RainMaker dashboard. If you can flash
 * the gates over the air, you can flash this the same way.
 *
 * What is different here: this node is almost entirely a *sensor*. Nothing in
 * this firmware moves water. The only thing it actuates is the battery room
 * exhaust fan, over RS485, and even that has a hotter backstop in the node for
 * when this hub goes quiet. Float emulation relays exist on the board and are
 * driven to de-energised at boot and then left alone — that is Phase C.
 *
 * Three consumers of the same state:
 *   - the local dashboard, once a second, over HTTP          (app_web.c)
 *   - the RainMaker cloud and phone app, on change           (here)
 *   - RainMaker push notifications, on threshold crossings   (here)
 *
 * The poll task is the only writer. Readers take a mutex; see hub_state_lock().
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"

#include <app_network.h>
#include <esp_rmaker_common_events.h>
#include <esp_rmaker_core.h>
#include <esp_rmaker_ota.h>
#include <esp_rmaker_scenes.h>
#include <esp_rmaker_schedule.h>
#include <esp_rmaker_standard_params.h>
#include <esp_rmaker_standard_types.h>
#include <esp_rmaker_utils.h>

#include "app_cal.h"
#include "app_priv.h"
#include "app_rs485.h"
#include "app_sensors.h"
#include "app_web.h"

static const char *TAG = "app_main";

/* ------------------------------------------------------------ shared state */

static hub_state_t s_state;
static SemaphoreHandle_t s_state_mux;

void hub_state_lock(void)   { xSemaphoreTake(s_state_mux, portMAX_DELAY); }
void hub_state_unlock(void) { xSemaphoreGive(s_state_mux); }
hub_state_t *hub_state(void) { return &s_state; }

/* --------------------------------------------------------- RainMaker handles */

static esp_rmaker_device_t *s_dev_tanks;
static esp_rmaker_device_t *s_dev_pumps;
static esp_rmaker_device_t *s_dev_climate;
static esp_rmaker_device_t *s_dev_vent;
static esp_rmaker_device_t *s_dev_plant;

static bool s_cloud_up = false;

/* Report to the cloud only when a value actually moves. A 2 s poll cycle times
 * fourteen parameters is a lot of MQTT for readings that mostly do not change,
 * and RainMaker charges nothing but the radio still costs power and airtime. */
static void report_int(esp_rmaker_device_t *dev, const char *name, int val, int *last, int deadband)
{
    if (!s_cloud_up || dev == NULL) {
        return;
    }
    if (*last != INT32_MIN && abs(val - *last) < deadband) {
        return;
    }
    esp_rmaker_param_t *p = esp_rmaker_device_get_param_by_name(dev, name);
    if (p) {
        esp_rmaker_param_update_and_report(p, esp_rmaker_int(val));
        *last = val;
    }
}

static void report_float(esp_rmaker_device_t *dev, const char *name, float val, float *last, float deadband)
{
    if (!s_cloud_up || dev == NULL) {
        return;
    }
    if (*last > -1000.0f && fabsf(val - *last) < deadband) {
        return;
    }
    esp_rmaker_param_t *p = esp_rmaker_device_get_param_by_name(dev, name);
    if (p) {
        esp_rmaker_param_update_and_report(p, esp_rmaker_float(val));
        *last = val;
    }
}

static void report_bool(esp_rmaker_device_t *dev, const char *name, bool val, int *last)
{
    if (!s_cloud_up || dev == NULL) {
        return;
    }
    if (*last == (val ? 1 : 0)) {
        return;
    }
    esp_rmaker_param_t *p = esp_rmaker_device_get_param_by_name(dev, name);
    if (p) {
        esp_rmaker_param_update_and_report(p, esp_rmaker_bool(val));
        *last = val ? 1 : 0;
    }
}

/* Every other report_* here has a deadband and this one had none, so the status
 * string was published on every 2 s cycle whether or not it had changed - about
 * 43,000 MQTT messages a day saying the same thing. RainMaker budgets messages
 * (the node logs "MQTT Budgeting initialised. Default: 100, Max: 1024" at boot),
 * so that is not merely wasteful, it is a node that eventually gets throttled.
 * Observed on the first cloud-connected boot, 2026-08-28. */
static void report_str(esp_rmaker_device_t *dev, const char *name, const char *val,
                       char *last, size_t last_len)
{
    if (!s_cloud_up || dev == NULL) {
        return;
    }
    if (strncmp(last, val, last_len) == 0) {
        return;                                  /* unchanged: say nothing */
    }
    esp_rmaker_param_t *p = esp_rmaker_device_get_param_by_name(dev, name);
    if (p) {
        esp_rmaker_param_update_and_report(p, esp_rmaker_str(val));
        strncpy(last, val, last_len - 1);
        last[last_len - 1] = '\0';
    }
}

/* ----------------------------------------------------------------- alerting */

/*
 * One latch per alert, per DASHBOARD_AND_RAINMAKER.md §4.
 *
 * Two guards, both learned from alarms that get ignored: an alert cannot re-fire
 * until the value has come back past a margin (so a reading sitting exactly on
 * the threshold does not notify every cycle), and there is a hard floor between
 * repeats. An alert that cries wolf teaches somebody to mute the app, and a
 * muted app protects nothing.
 */
typedef struct {
    bool    latched;
    int64_t last_fired_us;
} alert_t;

static alert_t s_al_twt_full, s_al_rwt_full, s_al_dos_low, s_al_bat_hot;
static alert_t s_al_fault, s_al_overcurrent, s_al_node_lost;

static void alert_eval(alert_t *a, bool tripped, const char *msg)
{
    int64_t now = esp_timer_get_time();

    if (!tripped) {
        a->latched = false;
        return;
    }
    /* One notification per trip. While the condition stays true this returns
     * here and never fires again - deliberately. Re-notifying about a fault
     * somebody already knows about is how people learn to mute the app, and the
     * dashboard and the app's own parameter both still show the live state.
     *
     * ALERT_REARM_MS is NOT a repeat interval - there are no repeats. */
    if (a->latched) {
        return;
    }

    /* A floor between separate TRIPS, which is what ALERT_REARM_MS is actually
     * for. The latch alone cannot damp a flapping sensor: the condition going
     * false clears the latch, so the next trip notifies immediately. Observed
     * 2026-08-28 - an unmounted dosing sensor bouncing 0 -> 86 -> 0 raised the
     * same "chemical low" twice inside ninety seconds. Latch it here without
     * notifying, so the flap is remembered and silent. */
    if (a->last_fired_us != 0 &&
        (now - a->last_fired_us) < (int64_t)ALERT_REARM_MS * 1000) {
        a->latched = true;
        return;
    }

    ESP_LOGW(TAG, "ALERT: %s", msg);

    /* Only latch if it could actually be delivered. Latching an alert raised
     * before MQTT is up means the cloud never hears about it and the latch
     * suppresses every retry - so a fault already present at boot, which is the
     * likeliest kind, would be logged to a serial port nobody is watching and
     * never pushed. Seen on the first cloud boot: the dosing-low alert fired at
     * 1.5 s, well before MQTT connected at 7.4 s. Leaving it unlatched costs one
     * repeated log line per cycle until the link comes up, which is the right
     * trade against a silently dropped alert. */
    if (!s_cloud_up) {
        return;
    }
    a->latched = true;
    a->last_fired_us = now;
    esp_rmaker_raise_alert(msg);
}

static void evaluate_alerts(const hub_state_t *s)
{
    char msg[128];

    if (s->twt.pct >= 0 && s->twt_online) {
        bool full = s->twt.pct > ALERT_TANK_FULL_PCT;
        if (s->twt.pct < ALERT_TANK_FULL_PCT - ALERT_HYST_PCT) full = false;
        snprintf(msg, sizeof(msg), "Treated water tank full (%d%%). RO plant entering standby.", s->twt.pct);
        alert_eval(&s_al_twt_full, full, msg);
    }

    if (s->rwt.pct >= 0 && s->rwt_online) {
        bool full = s->rwt.pct > ALERT_TANK_FULL_PCT;
        if (s->rwt.pct < ALERT_TANK_FULL_PCT - ALERT_HYST_PCT) full = false;
        snprintf(msg, sizeof(msg), "Raw water tank full (%d%%). Check the float and the sump motor.", s->rwt.pct);
        alert_eval(&s_al_rwt_full, full, msg);
    }

    if (s->dosing.pct >= 0) {
        bool low = s->dosing.pct < ALERT_DOSING_LOW_PCT;
        if (s->dosing.pct > ALERT_DOSING_LOW_PCT + ALERT_HYST_PCT) low = false;
        snprintf(msg, sizeof(msg), "Dosing chemical low (%d%%). Replenish the anti-scalant.", s->dosing.pct);
        alert_eval(&s_al_dos_low, low, msg);
    }

    if (!s->battery_room.fault && s->battery_online) {
        bool hot = s->battery_room.temp_deci_c >= (int16_t)cal_fan_on_deci_c();
        snprintf(msg, sizeof(msg), "Battery room %d.%d C. Exhaust fan %s.",
                 s->battery_room.temp_deci_c / 10, abs(s->battery_room.temp_deci_c % 10),
                 s->fan_on ? "ON" : "not running");
        alert_eval(&s_al_bat_hot, hot, msg);
    }

    /* The Aster multiplexes every fault condition onto the one AUX OP contact, so
     * on its own this can say something tripped but never which — WIRING.md §6.3.
     * LPS is tapped separately for exactly that reason: closed at the same time,
     * low feed pressure IS the cause and the alert can name it instead of sending
     * someone to read the panel. That is what the channel was spent on. */
    if (s->alarm_active && s->lps_active) {
        alert_eval(&s_al_fault, true,
                   "RO controller fault: LOW FEED PRESSURE. Check the feed pump, "
                   "the filters and the LPS setting.");
    } else {
        alert_eval(&s_al_fault, s->alarm_active,
                   "RO controller fault, cause not identified. Check the panel: "
                   "dosing level, pump overload, level interlocks.");
    }

    if (s->overcurrent) {
        snprintf(msg, sizeof(msg), "Motor over-current. HPP %d.%d A, RWP %d.%d A. Check for a seized pump or lost phase.",
                 s->hpp.deci_amps < 0 ? 0 : s->hpp.deci_amps / 10,
                 s->hpp.deci_amps < 0 ? 0 : s->hpp.deci_amps % 10,
                 s->rwp.deci_amps < 0 ? 0 : s->rwp.deci_amps / 10,
                 s->rwp.deci_amps < 0 ? 0 : s->rwp.deci_amps % 10);
        alert_eval(&s_al_overcurrent, true, msg);
    } else {
        alert_eval(&s_al_overcurrent, false, NULL);
    }

    bool any_lost = !s->rwt_online || !s->twt_online || !s->battery_online;
    snprintf(msg, sizeof(msg), "RS485 node offline: %s%s%s. Check the bus and the terminators.",
             s->rwt_online ? "" : "0x02 RWT ",
             s->twt_online ? "" : "0x03 TWT ",
             s->battery_online ? "" : "0x04 Battery ");
    alert_eval(&s_al_node_lost, any_lost, msg);
}

/* -------------------------------------------------------------- fan policy */

/*
 * The hub owns the thresholds so they can be changed from a phone instead of
 * with a programmer on a ladder. The node keeps a deliberately hotter backstop
 * for when this hub stops talking, so the two never fight: this is policy, that
 * is a floor under it.
 *
 * Re-assert periodically even when nothing changes. Silence is not the same as
 * "leave it as is" — after five quiet minutes the node stops believing us and
 * falls back to its own thresholds.
 */
static void command_fan(hub_state_t *s)
{
    static int64_t last_cmd_us = 0;

    if (!s->battery_online || s->battery_room.fault) {
        return;   /* no trustworthy temperature; the node's own fail-safe covers this */
    }

    bool want = s->fan_on;
    if (!s->fan_on && s->battery_room.temp_deci_c >= (int16_t)cal_fan_on_deci_c())  want = true;
    if (s->fan_on  && s->battery_room.temp_deci_c <= (int16_t)cal_fan_off_deci_c()) want = false;

    bool changed = (want != s->fan_on);
    int64_t now = esp_timer_get_time();
    if (!changed && (now - last_cmd_us) < (int64_t)FAN_REFRESH_MS * 1000) {
        return;
    }

    uint8_t req = want ? 1 : 0, ack[RS485_MAX_PAYLOAD];
    int len = rs485_poll(NODE_ADDR_BATTERY, CMD_SET_FAN_RELAY, &req, 1, ack);
    last_cmd_us = now;

    if (len != 1) {
        ESP_LOGW(TAG, "fan command not acknowledged — node keeps its own backstop");
        return;
    }
    if (changed) {
        ESP_LOGI(TAG, "fan: %d.%d C -> %s (on %u.%u / off %u.%u)",
                 s->battery_room.temp_deci_c / 10, abs(s->battery_room.temp_deci_c % 10),
                 ack[0] ? "ON" : "OFF",
                 cal_fan_on_deci_c() / 10, cal_fan_on_deci_c() % 10,
                 cal_fan_off_deci_c() / 10, cal_fan_off_deci_c() % 10);
    }
    s->fan_on = (ack[0] != 0);
}

/* ------------------------------------------------------------- node reading */

/*
 * Log a node's state only when it CHANGES. The bench sketch printed a line per
 * node per cycle, which was right for a thing you sit and watch; this runs for
 * months and a per-cycle line is noise that hides everything else. But logging
 * nothing was worse: a node that stops answering produced total silence, and the
 * only symptom was a parameter that quietly stopped being reported. That is how
 * "why am I not getting RWT readings" became a question with no answer in the log
 * (2026-08-28).
 */
static void read_tank_node(uint8_t addr, tank_state_t *t, bool *online, cal_tank_t which)
{
    bool was_online = *online;
    uint8_t p[RS485_MAX_PAYLOAD];
    int len = rs485_poll(addr, CMD_READ_LEVEL, NULL, 0, p);

    if (len != LEN_LEVEL_REPLY) {
        if (was_online) {
            ESP_LOGW(TAG, "node 0x%02X stopped answering (%s) - check the bus, the "
                          "terminators and its power", addr,
                     len < 0 ? "timeout" : "bad payload length");
        }
        *online = false;
        return;
    }
    if (!was_online) {
        ESP_LOGI(TAG, "node 0x%02X answering again", addr);
    }
    *online = true;

    t->distance_mm = ((uint16_t)p[0] << 8) | p[1];
    t->raw_mm      = ((uint16_t)p[2] << 8) | p[3];
    /* p[4] is the node's level byte, always 255: percentage is the hub's job so
     * the geometry can be re-calibrated over Wi-Fi (RS485_PROTOCOL.md §4.2). */
    t->quality     = p[5];
    t->sensor      = (sensor_status_t)p[6];
    t->last_ok_us  = esp_timer_get_time();

    const cal_tank_cfg_t *c = cal_tank(which);
    uint8_t pct = levelPercent(t->distance_mm, c->full_mm, c->empty_mm);
    int16_t was = t->pct;
    t->pct = (pct == 255) ? -1 : (int16_t)pct;

    /* A node that answers but cannot yield a level is the confusing case: the
     * bus is healthy, the parameter simply never appears. Say why, once, on the
     * transition - the distance is the diagnostic, not the percentage. */
    if (t->pct < 0 && was >= 0) {
        ESP_LOGW(TAG, "node 0x%02X answers but gives no level: %u mm is outside "
                      "its calibration (full %u / empty %u), sensor status %d",
                 addr, t->distance_mm, c->full_mm, c->empty_mm, (int)t->sensor);
    } else if (t->pct >= 0 && was < 0) {
        ESP_LOGI(TAG, "node 0x%02X level readable again: %u mm -> %d%%",
                 addr, t->distance_mm, t->pct);
    }
}

static void read_climate_node(hub_state_t *s)
{
    uint8_t p[RS485_MAX_PAYLOAD];
    int len = rs485_poll(NODE_ADDR_BATTERY, CMD_READ_CLIMATE, NULL, 0, p);

    if (len != LEN_CLIMATE_REPLY) {
        s->battery_online = false;
        return;
    }
    s->battery_online = true;
    s->battery_room.temp_deci_c  = (int16_t)(((uint16_t)p[0] << 8) | p[1]);
    s->battery_room.hum_deci_pct = ((uint16_t)p[2] << 8) | p[3];
    s->fan_on                    = (p[4] != 0);
    s->battery_room.fault        = (p[5] != 0);
    if (!s->battery_room.fault) {
        s->battery_room.last_ok_us = esp_timer_get_time();
    }
}

/* ------------------------------------------------------------- the poll task */

static void poll_task(void *arg)
{
    /* Deadbands, remembered per parameter so report_*() can skip no-op updates. */
    int   last_rwt = INT32_MIN, last_twt = INT32_MIN, last_dos = INT32_MIN;
    int   last_hpp_on = -1, last_rwp_on = -1, last_fan = -1, last_alarm = -1, last_oc = -1;
    int   last_lps = -1;
    char  last_status[64] = "";
    float last_ro_t = -9999, last_ro_h = -9999, last_bat_t = -9999, last_bat_h = -9999;
    float last_hpp_a = -9999, last_rwp_a = -9999;

    int  ct_turn = 0;              /* round-robin: one clamp per cycle */
    int  oc_streak = 0;

    while (true) {
        int64_t cycle_start = esp_timer_get_time();
        gpio_set_level(GPIO_LED_STATUS, 1);

        hub_state_t local;
        hub_state_lock();
        local = s_state;
        hub_state_unlock();

        /* --- RS485 nodes, by address, not in cable order --- */
        read_tank_node(NODE_ADDR_RWT, &local.rwt, &local.rwt_online, CAL_TANK_RWT);
        read_tank_node(NODE_ADDR_TWT, &local.twt, &local.twt_online, CAL_TANK_TWT);
        read_climate_node(&local);
        command_fan(&local);

        /* --- hub-local sensing, with the bus now idle for the rest of the cycle --- */
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

        local.dosing.distance_mm = dosing_read_mm();
        local.dosing.raw_mm = local.dosing.distance_mm;
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

        bool floating = false;
        ac_probe(GPIO_IN_HPP_AC, &local.hpp.running, &floating, &local.hpp.mv_lo, &local.hpp.mv_hi);
        if (floating) {
            ESP_LOGW(TAG, "HPP AC channel floating (%lu-%lu mV) — check VCC and OUT at the module",
                     (unsigned long)local.hpp.mv_lo, (unsigned long)local.hpp.mv_hi);
        }
        ac_probe(GPIO_IN_RWP_AC, &local.rwp.running, &floating, &local.rwp.mv_lo, &local.rwp.mv_hi);
        if (floating) {
            ESP_LOGW(TAG, "RWP AC channel floating (%lu-%lu mV) — check VCC and OUT at the module",
                     (unsigned long)local.rwp.mv_lo, (unsigned long)local.rwp.mv_hi);
        }

        /* One clamp per cycle: each RMS read blocks ~200 ms, and every channel
         * still refreshes inside 4 s — far faster than any thermal failure
         * develops. Start and stop are caught by the contactor optos, which are
         * instant, so nothing is lost by sampling current slowly (spec §7.3). */
        if (ct_turn == 0) {
            local.hpp.deci_amps = ct_read_deci_amps(GPIO_IN_HPP_CT, CAL_CT_HPP);
        } else {
            local.rwp.deci_amps = ct_read_deci_amps(GPIO_IN_RWP_CT, CAL_CT_RWP);
        }
        ct_turn ^= 1;

        /* Over-current has to persist. A single bad RMS read — a contactor
         * closing mid-window, a loose 3.5 mm plug — is not a fault, and a
         * notification for one is how people learn to ignore them. */
        bool oc_now =
            (local.hpp.deci_amps > 0 && local.hpp.deci_amps > (int16_t)cal_ct(CAL_CT_HPP)->oc_deci_amps) ||
            (local.rwp.deci_amps > 0 && local.rwp.deci_amps > (int16_t)cal_ct(CAL_CT_RWP)->oc_deci_amps);
        oc_streak = oc_now ? (oc_streak + 1) : 0;
        local.overcurrent = (oc_streak >= OC_CONFIRM_CYCLES);

        local.rs485_errors = rs485_error_count();
        local.last_cycle_ms = (uint32_t)((esp_timer_get_time() - cycle_start) / 1000);

        hub_state_lock();
        s_state = local;
        hub_state_unlock();

        /* --- cloud --- */
        if (local.rwt.pct >= 0) report_int(s_dev_tanks, PARAM_RWT_PCT, local.rwt.pct, &last_rwt, 1);
        if (local.twt.pct >= 0) report_int(s_dev_tanks, PARAM_TWT_PCT, local.twt.pct, &last_twt, 1);
        if (local.dosing.pct >= 0) report_int(s_dev_tanks, PARAM_DOS_PCT, local.dosing.pct, &last_dos, 1);

        report_bool(s_dev_pumps, PARAM_HPP_ON, local.hpp.running, &last_hpp_on);
        report_bool(s_dev_pumps, PARAM_RWP_ON, local.rwp.running, &last_rwp_on);
        if (local.hpp.deci_amps >= 0) {
            report_float(s_dev_pumps, PARAM_HPP_AMPS, local.hpp.deci_amps / 10.0f, &last_hpp_a, 0.3f);
        }
        if (local.rwp.deci_amps >= 0) {
            report_float(s_dev_pumps, PARAM_RWP_AMPS, local.rwp.deci_amps / 10.0f, &last_rwp_a, 0.3f);
        }
        report_bool(s_dev_pumps, PARAM_OVERCURRENT, local.overcurrent, &last_oc);

        if (!local.ro_room.fault) {
            report_float(s_dev_climate, PARAM_RO_TEMP, local.ro_room.temp_deci_c / 10.0f, &last_ro_t, 0.3f);
            report_float(s_dev_climate, PARAM_RO_HUM, local.ro_room.hum_deci_pct / 10.0f, &last_ro_h, 1.0f);
        }
        if (local.battery_online && !local.battery_room.fault) {
            report_float(s_dev_climate, PARAM_BAT_TEMP, local.battery_room.temp_deci_c / 10.0f, &last_bat_t, 0.3f);
            report_float(s_dev_climate, PARAM_BAT_HUM, local.battery_room.hum_deci_pct / 10.0f, &last_bat_h, 1.0f);
        }
        report_bool(s_dev_vent, PARAM_FAN_ON, local.fan_on, &last_fan);
        report_bool(s_dev_plant, PARAM_ALARM, local.alarm_active, &last_alarm);
        report_bool(s_dev_plant, PARAM_LPS, local.lps_active, &last_lps);

        /* A one-line summary is what the app shows without opening anything. */
        char status[64];
        if (local.alarm_active) {
            snprintf(status, sizeof(status), local.lps_active ? "Fault - low pressure"
                                                              : "Controller fault");
        } else if (local.overcurrent) {
            snprintf(status, sizeof(status), "Over-current");
        } else if (!local.rwt_online || !local.twt_online || !local.battery_online) {
            snprintf(status, sizeof(status), "Node offline");
        } else {
            /* "0%" and "no reading" are different facts and must not print the
             * same. pct < 0 means levelPercent() refused to compute one - no
             * echo, an uncalibrated tank, or a distance outside the calibration -
             * and rendering that as 0% claims the tank is empty. Same rule as the
             * amps field reporting null rather than 0.0 when no clamp is fitted. */
            char twt[12];
            if (local.twt.pct < 0) {
                snprintf(twt, sizeof(twt), "--");
            } else {
                snprintf(twt, sizeof(twt), "%d%%", local.twt.pct);
            }
            snprintf(status, sizeof(status), "%s - TWT %s",
                     local.hpp.running ? "Producing" : "Idle", twt);
        }
        report_str(s_dev_plant, PARAM_STATUS, status, last_status, sizeof(last_status));

        evaluate_alerts(&local);

        gpio_set_level(GPIO_LED_STATUS, 0);

        int64_t elapsed_ms = (esp_timer_get_time() - cycle_start) / 1000;
        int64_t remain = POLL_CYCLE_MS - elapsed_ms;
        vTaskDelay(pdMS_TO_TICKS(remain > 50 ? remain : 50));
    }
}

/* -------------------------------------------------- RainMaker node assembly */

static esp_err_t write_cb(const esp_rmaker_device_t *device, const esp_rmaker_param_t *param,
                          const esp_rmaker_param_val_t val, void *priv, esp_rmaker_write_ctx_t *ctx)
{
    const char *name = esp_rmaker_param_get_name(param);
    ESP_LOGI(TAG, "write %s from %s", name, ctx ? esp_rmaker_device_cb_src_to_str(ctx->src) : "?");

    /* The only writable parameter is the fan-on threshold. Everything else this
     * hub publishes is a measurement, and a measurement is not a control.
     * Calibration deliberately lives on /cal rather than in the app: it needs a
     * live distance reading next to the tank, which a phone notification cannot
     * give you. */
    if (strcmp(name, PARAM_FAN_THRESHOLD) == 0) {
        uint16_t on_deci = (uint16_t)(val.val.i * 10);
        uint16_t off_deci = (on_deci > FAN_MIN_HYST_DECI + 10) ? (uint16_t)(on_deci - 20) : on_deci;
        if (cal_set_fan(on_deci, off_deci) != ESP_OK) {
            ESP_LOGW(TAG, "rejected fan threshold %d C", val.val.i);
            return ESP_ERR_INVALID_ARG;
        }
        esp_rmaker_param_update_and_report(param, val);
        return ESP_OK;
    }
    return ESP_OK;
}

static esp_rmaker_param_t *ro_param(const char *name, const char *type,
                                    esp_rmaker_param_val_t val, const char *ui)
{
    esp_rmaker_param_t *p = esp_rmaker_param_create(name, type, val, PROP_FLAG_READ);
    if (p && ui) {
        esp_rmaker_param_add_ui_type(p, ui);
    }
    return p;
}

static void build_node(esp_rmaker_node_t *node)
{
    /* --- tanks --- */
    s_dev_tanks = esp_rmaker_device_create(DEV_TANKS, "esp.device.water-tank", NULL);
    esp_rmaker_device_add_param(s_dev_tanks, ro_param(PARAM_RWT_PCT, "esp.param.water-level",
                                                      esp_rmaker_int(0), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_tanks, ro_param(PARAM_TWT_PCT, "esp.param.water-level",
                                                      esp_rmaker_int(0), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_tanks, ro_param(PARAM_DOS_PCT, "esp.param.water-level",
                                                      esp_rmaker_int(0), ESP_RMAKER_UI_TEXT));
    esp_rmaker_node_add_device(node, s_dev_tanks);

    /* --- pumps and current --- */
    s_dev_pumps = esp_rmaker_device_create(DEV_PUMPS, "esp.device.other", NULL);
    esp_rmaker_device_add_param(s_dev_pumps, ro_param(PARAM_HPP_ON, ESP_RMAKER_PARAM_POWER,
                                                      esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE));
    esp_rmaker_device_add_param(s_dev_pumps, ro_param(PARAM_RWP_ON, ESP_RMAKER_PARAM_POWER,
                                                      esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE));
    esp_rmaker_device_add_param(s_dev_pumps, ro_param(PARAM_HPP_AMPS, "esp.param.current",
                                                      esp_rmaker_float(0), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_pumps, ro_param(PARAM_RWP_AMPS, "esp.param.current",
                                                      esp_rmaker_float(0), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_pumps, ro_param(PARAM_OVERCURRENT, "esp.param.alert",
                                                      esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE));
    esp_rmaker_node_add_device(node, s_dev_pumps);

    /* --- climate --- */
    s_dev_climate = esp_rmaker_device_create(DEV_CLIMATE, ESP_RMAKER_DEVICE_TEMP_SENSOR, NULL);
    esp_rmaker_param_t *ro_t = ro_param(PARAM_RO_TEMP, ESP_RMAKER_PARAM_TEMPERATURE,
                                        esp_rmaker_float(0), ESP_RMAKER_UI_TEXT);
    esp_rmaker_device_add_param(s_dev_climate, ro_t);
    esp_rmaker_device_assign_primary_param(s_dev_climate, ro_t);
    esp_rmaker_device_add_param(s_dev_climate, ro_param(PARAM_RO_HUM, "esp.param.humidity",
                                                        esp_rmaker_float(0), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_climate, ro_param(PARAM_BAT_TEMP, ESP_RMAKER_PARAM_TEMPERATURE,
                                                        esp_rmaker_float(0), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_climate, ro_param(PARAM_BAT_HUM, "esp.param.humidity",
                                                        esp_rmaker_float(0), ESP_RMAKER_UI_TEXT));
    esp_rmaker_node_add_device(node, s_dev_climate);

    /* --- ventilation: the fan state is read-only here because the node owns the
     *     relay and this hub owns the policy. Exposing a manual ON/OFF in the app
     *     would create a third opinion about a fan in a battery room. The
     *     threshold is the honest control. --- */
    s_dev_vent = esp_rmaker_device_create(DEV_VENT, ESP_RMAKER_DEVICE_FAN, NULL);
    esp_rmaker_device_add_cb(s_dev_vent, write_cb, NULL);
    esp_rmaker_device_add_param(s_dev_vent, ro_param(PARAM_FAN_ON, ESP_RMAKER_PARAM_POWER,
                                                     esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE));
    esp_rmaker_param_t *thr = esp_rmaker_param_create(PARAM_FAN_THRESHOLD, "esp.param.temperature",
                                                      esp_rmaker_int(cal_fan_on_deci_c() / 10),
                                                      PROP_FLAG_READ | PROP_FLAG_WRITE);
    esp_rmaker_param_add_ui_type(thr, ESP_RMAKER_UI_SLIDER);
    esp_rmaker_param_add_bounds(thr, esp_rmaker_int(FAN_LIMIT_LOW_DECI / 10),
                                esp_rmaker_int(FAN_LIMIT_HIGH_DECI / 10), esp_rmaker_int(1));
    esp_rmaker_device_add_param(s_dev_vent, thr);
    esp_rmaker_node_add_device(node, s_dev_vent);

    /* --- plant status and the fault flag --- */
    s_dev_plant = esp_rmaker_device_create(DEV_PLANT, "esp.device.other", NULL);
    esp_rmaker_param_t *st = ro_param(PARAM_STATUS, "esp.param.status",
                                      esp_rmaker_str("Starting"), ESP_RMAKER_UI_TEXT);
    esp_rmaker_device_add_param(s_dev_plant, st);
    esp_rmaker_device_assign_primary_param(s_dev_plant, st);
    esp_rmaker_device_add_param(s_dev_plant, ro_param(PARAM_ALARM, "esp.param.alert",
                                                      esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE));
    esp_rmaker_device_add_param(s_dev_plant, ro_param(PARAM_LPS, "esp.param.alert",
                                                      esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE));
    esp_rmaker_node_add_device(node, s_dev_plant);
}

/* ------------------------------------------------------------- connectivity */

static void rmaker_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == RMAKER_COMMON_EVENT && id == RMAKER_MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "RainMaker cloud connected");
        s_cloud_up = true;
    } else if (base == RMAKER_COMMON_EVENT && id == RMAKER_MQTT_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "RainMaker cloud disconnected");
        s_cloud_up = false;
    }
}

/*
 * Bring up the hub's own AP alongside the station.
 *
 * Calibration happens standing next to a tank on a roof, which is exactly where
 * the house Wi-Fi is least reliable — and it is also what you need when the
 * router is the thing that has failed. Provisioning is over BLE, not SoftAP, so
 * this AP does not contend with it.
 */
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
static void start_softap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", AP_SSID);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", AP_PASS);
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    /* Not ESP_ERROR_CHECK. The AP is a convenience - it makes /cal reachable
     * without a router - and it must not be able to abort the firmware. A hub
     * that polls the plant and talks to the cloud but has no local AP is running
     * degraded; a hub that panics on boot is not running at all. */
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_NULL) {
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "could not enter AP+STA (%s) - continuing without the local AP",
                     esp_err_to_name(err));
            return;
        }
    }
    esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (cfg_err != ESP_OK) {
        ESP_LOGW(TAG, "AP config rejected (%s) - continuing without the local AP",
                 esp_err_to_name(cfg_err));
        return;
    }

    ESP_LOGI(TAG, "AP up: SSID %s -> http://192.168.4.1/", AP_SSID);
}
#else
/* SoftAP compiled out. Provisioning is BLE, so pairing still works - what is
 * lost is reaching /cal without a working router. */
static void start_softap(void)
{
    ESP_LOGW(TAG, "SoftAP not compiled in - /cal needs the LAN");
}
#endif

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed — use the IP address");
        return;
    }
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set("RO Monitor Hub");
    mdns_service_add(NULL, "_http", "_tcp", WEB_PORT, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local/", MDNS_HOSTNAME);
}

/* Boot button: hold to re-provision, hold longer for a factory reset. Polled in
 * its own tiny task rather than pulling in the button component for one GPIO. */
static void button_task(void *arg)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GPIO_BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    int held = 0;
    while (true) {
        if (gpio_get_level(GPIO_BOOT_BUTTON) == 0) {
            held++;
            if (held == WIFI_RESET_HOLD_SEC) {
                ESP_LOGW(TAG, "BOOT held %ds — Wi-Fi reset queued, release to apply", held);
            }
            if (held >= FACTORY_RESET_HOLD_SEC) {
                ESP_LOGW(TAG, "BOOT held %ds — factory reset", held);
                esp_rmaker_factory_reset(2, 2);
            }
        } else {
            if (held >= WIFI_RESET_HOLD_SEC && held < FACTORY_RESET_HOLD_SEC) {
                ESP_LOGW(TAG, "Wi-Fi reset — re-provision from the RainMaker app");
                esp_rmaker_wifi_reset(2, 2);
            }
            held = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ------------------------------------------------------------------ app_main */

void app_main(void)
{
    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "  RO Monitor — Central Hub  fw %s", FW_VERSION);
    ESP_LOGI(TAG, "==========================================");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS full or outdated — erasing and re-initialising");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_state_mux = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_state_mux ? ESP_OK : ESP_FAIL);

    /* Sensible starting state: nothing is online until something answers, and no
     * level is known until a calibrated reading arrives. -1 rather than 0, so a
     * tank that has never reported does not render as empty. */
    memset(&s_state, 0, sizeof(s_state));
    s_state.rwt.pct = s_state.twt.pct = s_state.dosing.pct = -1;
    s_state.hpp.deci_amps = s_state.rwp.deci_amps = -1;
    s_state.ro_room.fault = s_state.battery_room.fault = true;

    ESP_ERROR_CHECK(cal_init());
    ESP_ERROR_CHECK(sensors_init());
    ESP_ERROR_CHECK(rs485_init());

    app_network_init();

    esp_event_handler_register(RMAKER_COMMON_EVENT, ESP_EVENT_ANY_ID, &rmaker_event_handler, NULL);

    esp_rmaker_config_t rainmaker_cfg = { .enable_time_sync = true };

    /* MAC-suffixed names, the same trick the gate controllers use, so a second
     * hub on the same account is distinguishable without a reflash. */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char node_name[40];
    snprintf(node_name, sizeof(node_name), "RO Plant Monitor - %02X%02X", mac[4], mac[5]);

    esp_rmaker_node_t *node = esp_rmaker_node_init(&rainmaker_cfg, node_name, NODE_TYPE);
    if (node == NULL) {
        ESP_LOGE(TAG, "RainMaker node init failed — aborting");
        abort();
    }
    esp_rmaker_node_add_attribute(node, "Project", NODE_PROJECT);
    esp_rmaker_node_add_attribute(node, "Device Type", NODE_TYPE);
    esp_rmaker_node_add_attribute(node, "Firmware", FW_VERSION);

    build_node(node);

    /* OTA from the RainMaker dashboard, into whichever of the two app slots is
     * not running. Same mechanism, same dashboard, same workflow as the gates. */
    esp_rmaker_ota_enable_default();
    esp_rmaker_schedule_enable();
    esp_rmaker_timezone_service_enable();
    esp_rmaker_scenes_enable();

    ESP_ERROR_CHECK(esp_rmaker_start());

    /* Static PoP so pairing does not need the serial console. */
    app_network_set_custom_pop("rohub1234");
    err = app_network_start(POP_TYPE_CUSTOM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_network_start failed: %s", esp_err_to_name(err));
    }

    /* The AP and the web server do not depend on the station connecting — that is
     * the whole point of running both. Start them regardless. */
    /* These run BEFORE nothing and AFTER app_network_start() only because that
     * call no longer blocks - see CONFIG_APP_NETWORK_ASYNCHRONOUS_CONNECTION in
     * sdkconfig.defaults. Without it, provisioning parked app_main here forever
     * and none of the following ever started. */
    start_softap();
    start_mdns();
    if (web_start() != ESP_OK) {
        /* Same reasoning as the AP: losing the local dashboard is degraded
         * operation, not a reason to stop monitoring the plant. RainMaker
         * telemetry and the alerts are unaffected. */
        ESP_LOGE(TAG, "local web server failed to start - cloud path unaffected");
    }

    xTaskCreate(poll_task, "poll", 6144, NULL, 5, NULL);
    xTaskCreate(button_task, "button", 2560, NULL, 3, NULL);

    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "  Dashboard : http://%s.local/", MDNS_HOSTNAME);
    ESP_LOGI(TAG, "  Fallback  : SSID %s -> http://192.168.4.1/", AP_SSID);
    ESP_LOGI(TAG, "  Calibrate : /cal  (user %s)", CAL_USER);
    ESP_LOGI(TAG, "  Pairing   : RainMaker app, PoP rohub1234");
    ESP_LOGI(TAG, "==========================================");
}
