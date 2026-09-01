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
#include <time.h>

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
#include <esp_app_desc.h>
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

static esp_rmaker_device_t *s_dev_ro_room, *s_dev_battery, *s_dev_tanks;
static esp_rmaker_device_t *s_dev_ro_room;
static esp_rmaker_device_t *s_dev_ro_room;
static esp_rmaker_device_t *s_dev_battery;
static esp_rmaker_device_t *s_dev_ro_room;

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
static alert_t s_al_fault, s_al_overcurrent, s_al_node_lost, s_al_idle;

/* repeat_ms == 0 keeps the original behaviour: one notification per trip, ever.
 * A non-zero value re-notifies on that interval for as long as the condition
 * holds, and is for the small number of faults that stay true until a person
 * physically intervenes. Everything else passes 0 on purpose. */
static void alert_eval(alert_t *a, bool tripped, const char *msg, uint32_t repeat_ms)
{
    int64_t now = esp_timer_get_time();

    if (!tripped) {
        a->latched = false;
        return;
    }
    /* One notification per trip, unless this alert asked for reminders.
     * Re-notifying about a fault somebody already knows about is how people
     * learn to mute the app, and the dashboard and the app's own parameter both
     * still show the live state - so the default is silence after the first.
     *
     * ALERT_REARM_MS is NOT the repeat interval; it is the floor between
     * separate TRIPS. repeat_ms is the reminder interval within one trip. */
    if (a->latched) {
        if (repeat_ms == 0) {
            return;
        }
        if ((now - a->last_fired_us) < (int64_t)repeat_ms * 1000) {
            return;
        }
        /* Due for a reminder: fall through and fire again. The rearm floor below
         * is skipped deliberately - it guards against a flapping sensor raising
         * a NEW trip too soon, which is not what this is. */
        ESP_LOGW(TAG, "ALERT (still active): %s", msg);
        if (!s_cloud_up) {
            return;
        }
        a->last_fired_us = now;
        esp_rmaker_raise_alert(msg);
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
        alert_eval(&s_al_twt_full, full, msg, 0);
    }

    if (s->rwt.pct >= 0 && s->rwt_online) {
        bool full = s->rwt.pct > ALERT_TANK_FULL_PCT;
        if (s->rwt.pct < ALERT_TANK_FULL_PCT - ALERT_HYST_PCT) full = false;
        snprintf(msg, sizeof(msg), "Raw water tank full (%d%%). Check the float and the sump motor.", s->rwt.pct);
        alert_eval(&s_al_rwt_full, full, msg, 0);
    }

    if (s->dosing.pct >= 0) {
        bool low = s->dosing.pct < ALERT_DOSING_LOW_PCT;
        if (s->dosing.pct > ALERT_DOSING_LOW_PCT + ALERT_HYST_PCT) low = false;
        snprintf(msg, sizeof(msg), "Dosing chemical low (%d%%). Replenish the anti-scalant.", s->dosing.pct);
        alert_eval(&s_al_dos_low, low, msg, 0);
    }

    if (!s->battery_room.fault && s->battery_online) {
        bool hot = s->battery_room.temp_deci_c >= (int16_t)cal_fan_on_deci_c();
        snprintf(msg, sizeof(msg), "Battery room %d.%d C. Exhaust fan %s.",
                 s->battery_room.temp_deci_c / 10, abs(s->battery_room.temp_deci_c % 10),
                 s->fan_on ? "ON" : "not running");
        alert_eval(&s_al_bat_hot, hot, msg, 0);
    }

    /* The Aster multiplexes every fault condition onto the one AUX OP contact, so
     * on its own this can say something tripped but never which — WIRING.md §6.3.
     * LPS is tapped separately for exactly that reason: closed at the same time,
     * low feed pressure IS the cause and the alert can name it instead of sending
     * someone to read the panel. That is what the channel was spent on. */
    /* The one alert that reminds. A tripped Aster stops making water and stays
     * stopped until somebody walks over and resets it, so silence after the
     * first notification is silence about a plant that is down. Every other
     * alert here clears itself when the condition does; this one does not. */
    if (s->alarm_active && s->lps_active) {
        alert_eval(&s_al_fault, true,
                   "RO controller fault: LOW FEED PRESSURE. Check the feed pump, "
                   "the filters and the LPS setting.", ALERT_REPEAT_MS);
    } else {
        alert_eval(&s_al_fault, s->alarm_active,
                   "RO controller fault, cause unknown. Check panel: dosing, "
                   "pump overload, interlocks.", ALERT_REPEAT_MS);
    }

    if (s->overcurrent) {
        snprintf(msg, sizeof(msg), "Motor over-current. HPP %d.%d A, RWP %d.%d A. Check for a seized pump or lost phase.",
                 s->hpp.deci_amps < 0 ? 0 : s->hpp.deci_amps / 10,
                 s->hpp.deci_amps < 0 ? 0 : s->hpp.deci_amps % 10,
                 s->rwp.deci_amps < 0 ? 0 : s->rwp.deci_amps / 10,
                 s->rwp.deci_amps < 0 ? 0 : s->rwp.deci_amps % 10);
        alert_eval(&s_al_overcurrent, true, msg, 0);
    } else {
        alert_eval(&s_al_overcurrent, false, NULL, 0);
    }

    /* The plant is idle while the treated water tank still has room for water.
     * TWT FLOTY is a C/NC contact: CLOSED is the healthy "not full" state
     * (WIRING.md 6.1), so closed-and-not-producing means the plant should be
     * making water and is not. The tank being full opens the contact and this
     * goes quiet, which is why it does not fire every night in standby.
     *
     * Two guards, both earned:
     *   - IDLE_HOLD_MS, because the Aster flushes and backwashes and the HPP
     *     drops out legitimately for a few minutes at a time. Alerting on the
     *     instant it stops would notify on every normal cycle.
     *   - ac_floating, because a broken wire at the opto module reads exactly
     *     like a stopped pump, and that has already happened once on this
     *     build. Without this, one loose wire produces an hourly notification
     *     about a plant that is running perfectly.
     *
     * The message names MANUAL mode specifically because that is the observed
     * cause: maintenance leaves the Aster in manual and forgets to put it back,
     * and the plant then sits there making nothing until somebody notices the
     * taps running dry. The hub cannot see the AUTO/MANUAL switch - it is not
     * wired anywhere - so the alert names the likeliest reason rather than
     * asking for a general look at the panel. The nagging is the feature. */
    static int64_t hpp_last_seen_running_us = 0;
    int64_t now_us = esp_timer_get_time();
    /* Seed from boot, not from the first time the pump is seen running. A hub
     * that reboots while the plant is already down would otherwise stay silent
     * forever - and a reboot during a fault is exactly when it must not. */
    if (hpp_last_seen_running_us == 0 || s->hpp.running) {
        hpp_last_seen_running_us = now_us;
    }
    bool idle_long_enough =
        (now_us - hpp_last_seen_running_us) >= (int64_t)ALERT_IDLE_HOLD_MS * 1000;
    bool should_be_producing = !s->hpp.running && !s->hpp.ac_floating &&
                               s->twt_float_closed && idle_long_enough;
    alert_eval(&s_al_idle, should_be_producing,
               "RO idle 15 min, treated water tank has room. "
               "Is the Aster still in MANUAL?", ALERT_REPEAT_MS);

    bool any_lost = !s->rwt_online || !s->twt_online || !s->battery_online;
    snprintf(msg, sizeof(msg), "RS485 node offline: %s%s%s. Check the bus and the terminators.",
             s->rwt_online ? "" : "0x02 RWT ",
             s->twt_online ? "" : "0x03 TWT ",
             s->battery_online ? "" : "0x04 Battery ");
    alert_eval(&s_al_node_lost, any_lost, msg, 0);
}

/* ------------------------------------------------------------- event stamps */

/*
 * Wall clock, or 0 if we do not have one yet.
 *
 * Recording an event before SNTP has synchronised would stamp it in 1970, and a
 * plant that "last ran in 1970" is worse than one that has never run: the first
 * is a bug someone has to chase, the second is a fact. The threshold is simply a
 * date safely in the past - anything below it is the epoch default, not a time.
 */
static uint32_t now_epoch(void)
{
    time_t now = time(NULL);
    return (now > 1700000000) ? (uint32_t)now : 0;
}

static void stamp_text(char *out, size_t len, uint32_t epoch)
{
    if (epoch == 0) {
        snprintf(out, len, "--");
        return;
    }
    time_t t = (time_t)epoch;
    struct tm tm;
    localtime_r(&t, &tm);
    if (strftime(out, len, "%d %b %H:%M", &tm) == 0) {
        snprintf(out, len, "--");
    }
}

/* Record on the RISING edge only. `prev` is the caller's memory of last cycle. */
static void stamp_on_rise(bool now_true, int *prev, cal_event_t e, uint32_t *dst)
{
    if (now_true && *prev != 1) {
        uint32_t t = now_epoch();
        if (t != 0 && cal_event_set(e, t) == ESP_OK) {
            *dst = t;
        }
    }
    *prev = now_true ? 1 : 0;
}

/* Publishes only when the stamp actually changes - which is a few times a day,
 * not every cycle. `sent` is the caller's memory of what the cloud already has. */
static void report_stamp(esp_rmaker_device_t *dev, const char *name,
                         uint32_t epoch, uint32_t *sent)
{
    if (epoch == *sent) {
        return;
    }
    char text[24];
    stamp_text(text, sizeof(text), epoch);
    esp_rmaker_param_t *p = esp_rmaker_device_get_param_by_name(dev, name);
    if (p) {
        esp_rmaker_param_update_and_report(p, esp_rmaker_str(text));
        *sent = epoch;
    }
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
static int64_t s_fan_mode_set_us = 0;

static const char *fan_mode_str(fan_mode_t m)
{
    return (m == FAN_MODE_ON) ? "On" : (m == FAN_MODE_OFF) ? "Off" : "Auto";
}

static void report_fan_mode(fan_mode_t m)
{
    esp_rmaker_param_t *p = esp_rmaker_device_get_param_by_name(s_dev_battery, PARAM_FAN_MODE);
    if (p) {
        esp_rmaker_param_update_and_report(p, esp_rmaker_str(fan_mode_str(m)));
    }
}

static void command_fan(hub_state_t *s)
{
    static int64_t last_cmd_us = 0;

    if (!s->battery_online || s->battery_room.fault) {
        return;   /* no trustworthy temperature; the node's own fail-safe covers this */
    }

    /* A manual mode is a temporary instruction, not a new policy: it expires back
     * to Auto so that a hub left in Force Off by somebody who has gone home does
     * not quietly become the ventilation strategy for a battery room. */
    if (s->fan_mode != FAN_MODE_AUTO &&
        (esp_timer_get_time() - s_fan_mode_set_us) > (int64_t)FAN_FORCE_MS * 1000) {
        ESP_LOGI(TAG, "fan override expired — back to Auto");
        s->fan_mode = FAN_MODE_AUTO;
        report_fan_mode(s->fan_mode);
    }

    bool want = s->fan_on;
    if (s->fan_mode == FAN_MODE_ON) {
        want = true;                       /* always allowed: ventilating is the safe direction */
    } else if (s->fan_mode == FAN_MODE_OFF &&
               s->battery_room.temp_deci_c < FAN_FORCE_OFF_CEILING_DECI) {
        want = false;
    } else {
        /* Auto, or Force Off overridden because the room is too hot for it to be
         * a convenience any more. See FAN_FORCE_OFF_CEILING_DECI. */
        if (s->fan_mode == FAN_MODE_OFF) {
            ESP_LOGW(TAG, "Force Off ignored at %d.%d C — reverting to Auto",
                     s->battery_room.temp_deci_c / 10, abs(s->battery_room.temp_deci_c % 10));
            s->fan_mode = FAN_MODE_AUTO;
            report_fan_mode(s->fan_mode);
        }
        if (!s->fan_on && s->battery_room.temp_deci_c >= (int16_t)cal_fan_on_deci_c())  want = true;
        if (s->fan_on  && s->battery_room.temp_deci_c <= (int16_t)cal_fan_off_deci_c()) want = false;
    }

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

/* ------------------------------------------------------------ console summary */

static const char *status_word(sensor_status_t s)
{
    switch (s) {
        case SENSOR_OK:       return "OK";
        case SENSOR_BLIND:    return "BLIND";
        case SENSOR_NO_ECHO:  return "NO_ECHO";
        case SENSOR_HW_FAULT: return "HW_FAULT";
        default:              return "?";
    }
}

/* One tank as "2304 mm q100 OK -> 0%", or why there is no level. The distance
 * comes first on purpose: it is the measurement, and the percentage is an
 * interpretation of it through a calibration that may well be wrong. */
static void fmt_tank(char *out, size_t n, const tank_state_t *t, bool online, bool has_q)
{
    if (!online) {
        snprintf(out, n, "%-28s", "OFFLINE - not answering");
        return;
    }
    char pct[8];
    if (t->pct < 0) {
        snprintf(pct, sizeof(pct), "--");
    } else {
        snprintf(pct, sizeof(pct), "%d%%", t->pct);
    }
    if (has_q) {
        snprintf(out, n, "%5u mm q%-3u %-8s -> %-4s",
                 t->distance_mm, t->quality, status_word(t->sensor), pct);
    } else {
        snprintf(out, n, "%5u mm      %-8s -> %-4s",
                 t->distance_mm, status_word(t->sensor), pct);
    }
}

static void log_summary(const hub_state_t *s)
{
    char rwt[40], twt[40], dos[40];
    fmt_tank(rwt, sizeof(rwt), &s->rwt, s->rwt_online, true);
    fmt_tank(twt, sizeof(twt), &s->twt, s->twt_online, true);
    fmt_tank(dos, sizeof(dos), &s->dosing, true, false);

    ESP_LOGI(TAG, "tanks   RWT %s | TWT %s | DOS %s", rwt, twt, dos);

    ESP_LOGI(TAG, "climate RO %d.%d C %d.%d %%RH%s | BAT %d.%d C %d.%d %%RH fan %s%s",
             s->ro_room.temp_deci_c / 10, abs(s->ro_room.temp_deci_c % 10),
             s->ro_room.hum_deci_pct / 10, s->ro_room.hum_deci_pct % 10,
             s->ro_room.fault ? " SHT30 FAULT" : "",
             s->battery_room.temp_deci_c / 10, abs(s->battery_room.temp_deci_c % 10),
             s->battery_room.hum_deci_pct / 10, s->battery_room.hum_deci_pct % 10,
             s->fan_on ? "ON " : "OFF",
             !s->battery_online ? " NODE 0x04 OFFLINE" : (s->battery_room.fault ? " SHT30 FAULT" : ""));

    /* Millivolts, not just the booleans they resolve to. On these two channels a
     * floating pin and an idle opto both read "off", and only the spread tells
     * them apart - which is the whole reason ac_probe() measures rather than
     * samples. Same for the clamps: the pedestal proves the breakout exists. */
    ESP_LOGI(TAG, "motors  HPP %s %4lu-%4lu mV %s | RWP %s %4lu-%4lu mV %s%s",
             s->hpp.running ? "RUN " : "idle",
             (unsigned long)s->hpp.mv_lo, (unsigned long)s->hpp.mv_hi,
             s->hpp.deci_amps < 0 ? "CT --" : "CT ok",
             s->rwp.running ? "RUN " : "idle",
             (unsigned long)s->rwp.mv_lo, (unsigned long)s->rwp.mv_hi,
             s->rwp.deci_amps < 0 ? "CT --" : "CT ok",
             s->overcurrent ? "  OVER CURRENT" : "");

    /* Printed only when a probe is actually fitted - every node reads "not
     * fitted" until the hardware goes in, and a permanent "TDS --" line is noise
     * in a log somebody has to read at 3 am. */
    if (s->rwt_wq.fitted || s->twt_wq.fitted) {
        char rej[8];
        if (s->rejection_pct < 0) {
            snprintf(rej, sizeof(rej), "--");
        } else {
            snprintf(rej, sizeof(rej), "%d%%", s->rejection_pct);
        }
        ESP_LOGI(TAG, "quality RWT %u ppm %d.%d C | TWT %u ppm %d.%d C | rejection %s",
                 s->rwt_wq.ppm, s->rwt_wq.temp_deci_c / 10, abs(s->rwt_wq.temp_deci_c % 10),
                 s->twt_wq.ppm, s->twt_wq.temp_deci_c / 10, abs(s->twt_wq.temp_deci_c % 10),
                 rej);
    }

    ESP_LOGI(TAG, "aster   TWT_FLOT %s | RL1 %s | RL2 %s | LPS %s | ALARM %s   rs485 err %lu",
             s->twt_float_closed ? "CLOSED" : "open  ",
             s->rl1_active ? "ACTIVE" : "idle  ",
             s->rl2_active ? "ACTIVE" : "idle  ",
             s->lps_active ? "LOW   " : "normal",
             s->alarm_active ? "FAULT" : "clear",
             (unsigned long)s->rs485_errors);
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

/*
 * TDS and water temperature, polled only from a node that has the pair fitted.
 *
 * Cheap to ask and cheap to ignore: a node with nothing fitted answers with its
 * status byte set and we mark it unfitted, so this costs one extra frame per
 * cycle per tank and never invents a reading. Note that a poll failure here does
 * NOT mark the node offline - the level is what decides that, and a working tank
 * level with a dead TDS probe is a node that is very much alive.
 */
static void read_water_quality(uint8_t addr, wq_state_t *wq, cal_tank_t which)
{
    uint8_t p[RS485_MAX_PAYLOAD];
    int len = rs485_poll(addr, CMD_READ_WQ, NULL, 0, p);

    if (len != LEN_WQ_REPLY) {
        wq->fitted = false;
        wq->ppm = TDS_INVALID;
        return;
    }

    uint8_t status = p[4];
    if (status != 0) {
        /* The node sets both flags together on purpose: TDS without a water
         * temperature is uncompensated and drifts ~2 %/degC, so it declines to
         * report either. See sampleWaterQuality() in ro_node.ino. */
        if (wq->fitted) {
            ESP_LOGW(TAG, "node 0x%02X water quality went unreadable (status 0x%02X) "
                          "- check the DS18B20 pullup and the probe lead", addr, status);
        }
        wq->fitted = false;
        wq->ppm = TDS_INVALID;
        return;
    }

    bool was = wq->fitted;
    wq->tds_mv      = ((uint16_t)p[0] << 8) | p[1];
    wq->temp_deci_c = (int16_t)(((uint16_t)p[2] << 8) | p[3]);
    wq->fitted      = true;
    wq->last_ok_us  = esp_timer_get_time();

    const cal_tank_cfg_t *c = cal_tank(which);
    wq->ppm = tdsPPM(wq->tds_mv, wq->temp_deci_c, c->tds_k_x100);

    if (!was) {
        ESP_LOGI(TAG, "node 0x%02X water quality online: %u mV at %d.%d C -> %u ppm",
                 addr, wq->tds_mv, wq->temp_deci_c / 10, abs(wq->temp_deci_c % 10),
                 wq->ppm);
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
    int   last_twt_float = -1, last_rl1 = -1, last_rl2 = -1;
    int   last_rwt_tds = -1, last_twt_tds = -1, last_rejection = -1;
    /* -1 rather than 0: "not seen yet" must differ from "seen, and it was off",
     * or the first cycle records a rising edge that never happened. */
    int   prev_hpp_run = -1, prev_rwp_run = -1, prev_fan_run = -1, prev_twt_full = -1;
    uint32_t sent_hpp_t = 0, sent_rwp_t = 0, sent_twt_t = 0, sent_fan_t = 0;
    int   last_lps = -1;
    char  last_status[64] = "";
    float last_ro_t = -9999, last_ro_h = -9999, last_bat_t = -9999, last_bat_h = -9999;
    float last_hpp_a = -9999, last_rwp_a = -9999;
    float last_rwt_wt = -9999, last_twt_wt = -9999;

    /* Survives a power cut: without this the first thing a returning hub reports
     * is that the plant has never run. */
    hub_state_lock();
    s_state.hpp_last_on   = cal_event_get(CAL_EVT_HPP_ON);
    s_state.rwp_last_on   = cal_event_get(CAL_EVT_RWP_ON);
    s_state.twt_last_full = cal_event_get(CAL_EVT_TWT_FULL);
    s_state.fan_last_on   = cal_event_get(CAL_EVT_FAN_ON);
    hub_state_unlock();

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

        /* --- RS485 nodes, by address, not in cable order --- */
        read_tank_node(NODE_ADDR_RWT, &local.rwt, &local.rwt_online, CAL_TANK_RWT);
        read_tank_node(NODE_ADDR_TWT, &local.twt, &local.twt_online, CAL_TANK_TWT);
        read_climate_node(&local);

        /* Water quality is two extra frames per tank, and the node only refreshes
         * it every 10 s anyway - so asking every 2 s cycle would spend bus time to
         * re-read the same number. Once every WQ_POLL_CYCLES keeps the chain quiet
         * for the levels, which are what people actually watch. */
        if (++wq_turn >= WQ_POLL_CYCLES) {
            wq_turn = 0;
            /* An offline node has no water quality either. Without this the last
             * good reading sits in the app looking current, which is the failure
             * the tank levels already refuse. */
            if (local.rwt_online) read_water_quality(NODE_ADDR_RWT, &local.rwt_wq, CAL_TANK_RWT);
            else                  { local.rwt_wq.fitted = false; local.rwt_wq.ppm = TDS_INVALID; }
            if (local.twt_online) read_water_quality(NODE_ADDR_TWT, &local.twt_wq, CAL_TANK_TWT);
            else                  { local.twt_wq.fitted = false; local.twt_wq.ppm = TDS_INVALID; }
            /* Feed and permeate of the same plant. The ratio is the membrane's
             * health; either number alone mostly tracks the source water. */
            local.rejection_pct = rejectionPercent(local.rwt_wq.ppm, local.twt_wq.ppm);
        }
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

        ac_probe(GPIO_IN_HPP_AC, &local.hpp.running, &local.hpp.ac_floating,
                 &local.hpp.mv_lo, &local.hpp.mv_hi);
        if (local.hpp.ac_floating) {
            ESP_LOGW(TAG, "HPP AC channel floating (%lu-%lu mV) — check VCC and OUT at the module",
                     (unsigned long)local.hpp.mv_lo, (unsigned long)local.hpp.mv_hi);
        }
        ac_probe(GPIO_IN_RWP_AC, &local.rwp.running, &local.rwp.ac_floating,
                 &local.rwp.mv_lo, &local.rwp.mv_hi);
        if (local.rwp.ac_floating) {
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

        report_bool(s_dev_ro_room, PARAM_HPP_ON, local.hpp.running, &last_hpp_on);
        report_bool(s_dev_ro_room, PARAM_RWP_ON, local.rwp.running, &last_rwp_on);
        if (local.hpp.deci_amps >= 0) {
            report_float(s_dev_ro_room, PARAM_HPP_AMPS, local.hpp.deci_amps / 10.0f, &last_hpp_a, 0.3f);
        }
        if (local.rwp.deci_amps >= 0) {
            report_float(s_dev_ro_room, PARAM_RWP_AMPS, local.rwp.deci_amps / 10.0f, &last_rwp_a, 0.3f);
        }
        report_bool(s_dev_ro_room, PARAM_OVERCURRENT, local.overcurrent, &last_oc);

        if (!local.ro_room.fault) {
            report_float(s_dev_ro_room, PARAM_RO_TEMP, local.ro_room.temp_deci_c / 10.0f, &last_ro_t, 0.3f);
            report_float(s_dev_ro_room, PARAM_RO_HUM, local.ro_room.hum_deci_pct / 10.0f, &last_ro_h, 1.0f);
        }
        if (local.battery_online && !local.battery_room.fault) {
            report_float(s_dev_battery, PARAM_BAT_TEMP, local.battery_room.temp_deci_c / 10.0f, &last_bat_t, 0.3f);
            report_float(s_dev_battery, PARAM_BAT_HUM, local.battery_room.hum_deci_pct / 10.0f, &last_bat_h, 1.0f);
        }
        report_bool(s_dev_battery, PARAM_FAN_ON, local.fan_on, &last_fan);
        report_bool(s_dev_ro_room, PARAM_ALARM, local.alarm_active, &last_alarm);
        report_bool(s_dev_ro_room, PARAM_LPS, local.lps_active, &last_lps);
        report_bool(s_dev_tanks, PARAM_TWT_FLOAT, local.twt_float_closed, &last_twt_float);

        /* Only when there is something to say. An unfitted or faulty probe
         * reports nothing rather than 0 ppm - same rule as the tank levels and
         * the CT currents: a missing sensor must not look like clean water. */
        if (local.rwt_wq.fitted && local.rwt_wq.ppm != TDS_INVALID) {
            report_int(s_dev_tanks, PARAM_RWT_TDS, local.rwt_wq.ppm, &last_rwt_tds, 5);
            report_float(s_dev_tanks, PARAM_RWT_WTEMP, local.rwt_wq.temp_deci_c / 10.0f,
                         &last_rwt_wt, 0.3f);
        }
        if (local.twt_wq.fitted && local.twt_wq.ppm != TDS_INVALID) {
            report_int(s_dev_tanks, PARAM_TWT_TDS, local.twt_wq.ppm, &last_twt_tds, 5);
            report_float(s_dev_tanks, PARAM_TWT_WTEMP, local.twt_wq.temp_deci_c / 10.0f,
                         &last_twt_wt, 0.3f);
        }
        if (local.rejection_pct >= 0) {
            report_int(s_dev_tanks, PARAM_REJECTION, local.rejection_pct, &last_rejection, 1);
        }
        /* --- when things last happened ---
         *
         * Rising edges only, and only once the clock is real. TWT "full" is taken
         * from the LEVEL, not from the Aster float: TWT FLOTY is a C/NC contact
         * whose OPEN state means full, and an unwired input reads open (WIRING.md
         * 0.3.2) - so trusting the float would stamp "tank full" at every boot of
         * every hub that has not been wired to the panel yet. */
        stamp_on_rise(local.hpp.running, &prev_hpp_run, CAL_EVT_HPP_ON, &local.hpp_last_on);
        stamp_on_rise(local.rwp.running, &prev_rwp_run, CAL_EVT_RWP_ON, &local.rwp_last_on);
        stamp_on_rise(local.fan_on,      &prev_fan_run, CAL_EVT_FAN_ON, &local.fan_last_on);
        stamp_on_rise(local.twt.pct >= ALERT_TANK_FULL_PCT, &prev_twt_full,
                      CAL_EVT_TWT_FULL, &local.twt_last_full);

        report_stamp(s_dev_ro_room, PARAM_HPP_LAST_ON,   local.hpp_last_on,   &sent_hpp_t);
        report_stamp(s_dev_ro_room, PARAM_RWP_LAST_ON,   local.rwp_last_on,   &sent_rwp_t);
        report_stamp(s_dev_ro_room, PARAM_TWT_LAST_FULL, local.twt_last_full, &sent_twt_t);
        report_stamp(s_dev_battery, PARAM_FAN_LAST_ON,   local.fan_last_on,   &sent_fan_t);

        report_bool(s_dev_ro_room, PARAM_RL1, local.rl1_active, &last_rl1);
        report_bool(s_dev_ro_room, PARAM_RL2, local.rl2_active, &last_rl2);

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
        report_str(s_dev_ro_room, PARAM_STATUS, status, last_status, sizeof(last_status));

        evaluate_alerts(&local);

        /* Periodic console summary. Deliberately AFTER the state is committed and
         * the cloud updated, so a slow console never delays either. */
        static int64_t last_summary_us = 0;
        int64_t now_us = esp_timer_get_time();
        if (last_summary_us == 0 ||
            now_us - last_summary_us >= (int64_t)LOG_SUMMARY_MS * 1000) {
            last_summary_us = now_us;
            log_summary(&local);
        }

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

    /* Three writable parameters, all of them the fan: the switch, the mode and
     * the threshold. Everything else this hub publishes is a measurement, and a
     * measurement is not a control.
     * Calibration deliberately lives on /cal rather than in the app: it needs a
     * live distance reading next to the tank, which a phone notification cannot
     * give you. */
    /* The toggle is a shortcut into the same mode machinery below - never a
     * fourth opinion about the relay. Note it can be REFUSED: Force Off above
     * the ceiling is not a thing this hub will do (FAN_FORCE_OFF_CEILING_DECI),
     * and command_fan() will revert it on the next cycle and say so. */
    if (strcmp(name, PARAM_FAN_ON) == 0) {
        fan_mode_t m = val.val.b ? FAN_MODE_ON : FAN_MODE_OFF;
        hub_state_lock();
        s_state.fan_mode = m;
        hub_state_unlock();
        s_fan_mode_set_us = esp_timer_get_time();
        ESP_LOGI(TAG, "fan switched %s from the app (expires to Auto in %lu min)",
                 val.val.b ? "ON" : "OFF", (unsigned long)(FAN_FORCE_MS / 60000));
        esp_rmaker_param_update_and_report(param, val);
        report_fan_mode(m);
        return ESP_OK;
    }

    if (strcmp(name, PARAM_FAN_MODE) == 0) {
        const char *v = val.val.s ? val.val.s : "Auto";
        fan_mode_t m = FAN_MODE_AUTO;
        if (strcmp(v, "On") == 0)       m = FAN_MODE_ON;
        else if (strcmp(v, "Off") == 0) m = FAN_MODE_OFF;
        else if (strcmp(v, "Auto") != 0) {
            ESP_LOGW(TAG, "unknown fan mode %s", v);
            return ESP_ERR_INVALID_ARG;
        }
        hub_state_lock();
        s_state.fan_mode = m;
        hub_state_unlock();
        s_fan_mode_set_us = esp_timer_get_time();
        /* Take effect on the next cycle rather than commanding the relay here:
         * the poll task owns the bus, and a write callback runs on the RainMaker
         * work queue. Two tasks driving one RS485 chain is a race, and the cycle
         * is 2 s away. */
        ESP_LOGI(TAG, "fan mode -> %s (expires to Auto in %lu min)",
                 v, (unsigned long)(FAN_FORCE_MS / 60000));
        esp_rmaker_param_update_and_report(param, val);
        return ESP_OK;
    }

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
    /* ================================ RO ROOM ================================
     * Everything in the room on one screen: what the plant is doing, what the
     * Aster contacts say, what the pumps are drawing, and the air around them.
     * Split by signal type it was three screens for one physical place. */
    s_dev_ro_room = esp_rmaker_device_create(DEV_RO_ROOM, "esp.device.other", NULL);

    esp_rmaker_param_t *st = ro_param(PARAM_STATUS, "esp.param.status",
                                      esp_rmaker_str("Starting"), ESP_RMAKER_UI_TEXT);
    esp_rmaker_device_add_param(s_dev_ro_room, st);
    esp_rmaker_device_assign_primary_param(s_dev_ro_room, st);

    esp_rmaker_device_add_param(s_dev_ro_room, ro_param(PARAM_ALARM, "esp.param.alert",
                                                        esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE));
    esp_rmaker_device_add_param(s_dev_ro_room, ro_param(PARAM_LPS, "esp.param.alert",
                                                        esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE));
    /* Not alerts - the multiport valve position, which is how you tell
     * "producing" from "backwashing" without standing in the room. */
    esp_rmaker_device_add_param(s_dev_ro_room, ro_param(PARAM_RL1, "esp.param.toggle",
                                                        esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE));
    esp_rmaker_device_add_param(s_dev_ro_room, ro_param(PARAM_RL2, "esp.param.toggle",
                                                        esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE));

    esp_rmaker_device_add_param(s_dev_ro_room, ro_param(PARAM_HPP_ON, ESP_RMAKER_PARAM_POWER,
                                                        esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE));
    esp_rmaker_device_add_param(s_dev_ro_room, ro_param(PARAM_RWP_ON, ESP_RMAKER_PARAM_POWER,
                                                        esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE));
    /* A pump with no clamp fitted reports nothing, so these show the sentinel
     * until one is: 0.0 A is exactly what an idle pump reads. */
    esp_rmaker_device_add_param(s_dev_ro_room, ro_param(PARAM_HPP_AMPS, "esp.param.current",
                                                        esp_rmaker_float(VAL_NO_READING_FLOAT), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_ro_room, ro_param(PARAM_RWP_AMPS, "esp.param.current",
                                                        esp_rmaker_float(VAL_NO_READING_FLOAT), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_ro_room, ro_param(PARAM_OVERCURRENT, "esp.param.alert",
                                                        esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE));

    esp_rmaker_device_add_param(s_dev_ro_room, ro_param(PARAM_RO_TEMP, ESP_RMAKER_PARAM_TEMPERATURE,
                                                        esp_rmaker_float(VAL_NO_READING_FLOAT), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_ro_room, ro_param(PARAM_RO_HUM, "esp.param.humidity",
                                                        esp_rmaker_float(VAL_NO_READING_FLOAT), ESP_RMAKER_UI_TEXT));

    /* "When did it last run" answers the question people open the app to ask,
     * and a live boolean cannot: a pump that is off right now tells you nothing
     * about whether the plant has been working today. */
    esp_rmaker_device_add_param(s_dev_ro_room, ro_param(PARAM_HPP_LAST_ON, "esp.param.status",
                                                        esp_rmaker_str("--"), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_ro_room, ro_param(PARAM_RWP_LAST_ON, "esp.param.status",
                                                        esp_rmaker_str("--"), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_ro_room, ro_param(PARAM_TWT_LAST_FULL, "esp.param.status",
                                                        esp_rmaker_str("--"), ESP_RMAKER_UI_TEXT));
    esp_rmaker_node_add_device(node, s_dev_ro_room);

    /* ============================= BATTERY ROOM ============================= */
    s_dev_battery = esp_rmaker_device_create(DEV_BATTERY_ROOM, ESP_RMAKER_DEVICE_TEMP_SENSOR, NULL);
    esp_rmaker_device_add_cb(s_dev_battery, write_cb, NULL);

    esp_rmaker_param_t *bt = ro_param(PARAM_BAT_TEMP, ESP_RMAKER_PARAM_TEMPERATURE,
                                      esp_rmaker_float(VAL_NO_READING_FLOAT), ESP_RMAKER_UI_TEXT);
    esp_rmaker_device_add_param(s_dev_battery, bt);
    esp_rmaker_device_assign_primary_param(s_dev_battery, bt);
    esp_rmaker_device_add_param(s_dev_battery, ro_param(PARAM_BAT_HUM, "esp.param.humidity",
                                                        esp_rmaker_float(VAL_NO_READING_FLOAT), ESP_RMAKER_UI_TEXT));
    /* Writable: this is the switch, and tapping it is what people will actually
     * do. It sets the mode underneath - on means Force On, off means Force Off -
     * so there is still only ONE opinion about the relay at any moment, and it
     * still expires back to Auto rather than becoming permanent policy. */
    esp_rmaker_param_t *fan = esp_rmaker_param_create(PARAM_FAN_ON, ESP_RMAKER_PARAM_POWER,
                                                      esp_rmaker_bool(false),
                                                      PROP_FLAG_READ | PROP_FLAG_WRITE);
    esp_rmaker_param_add_ui_type(fan, ESP_RMAKER_UI_TOGGLE);
    esp_rmaker_device_add_param(s_dev_battery, fan);

    /* The mode beside it, so the toggle is never ambiguous: it says whether the
     * fan is where it is because a thermostat put it there or because a person
     * did, and how long that will last. A plain toggle beside a thermostat
     * is two opinions about one relay with no way to say which wins; a mode says
     * outright who is in charge. Auto is the resting state and both overrides
     * expire back to it - FAN_FORCE_MS and FAN_FORCE_OFF_CEILING_DECI. */
    esp_rmaker_param_t *fm = esp_rmaker_param_create(PARAM_FAN_MODE, "esp.param.mode",
                                                     esp_rmaker_str("Auto"),
                                                     PROP_FLAG_READ | PROP_FLAG_WRITE);
    esp_rmaker_param_add_ui_type(fm, ESP_RMAKER_UI_DROPDOWN);
    static const char *fan_modes[] = { "Auto", "On", "Off" };
    esp_rmaker_param_add_valid_str_list(fm, fan_modes, 3);
    esp_rmaker_device_add_param(s_dev_battery, fm);

    esp_rmaker_param_t *thr = esp_rmaker_param_create(PARAM_FAN_THRESHOLD, "esp.param.temperature",
                                                      esp_rmaker_int(cal_fan_on_deci_c() / 10),
                                                      PROP_FLAG_READ | PROP_FLAG_WRITE);
    esp_rmaker_param_add_ui_type(thr, ESP_RMAKER_UI_SLIDER);
    esp_rmaker_param_add_bounds(thr, esp_rmaker_int(FAN_LIMIT_LOW_DECI / 10),
                                esp_rmaker_int(FAN_LIMIT_HIGH_DECI / 10), esp_rmaker_int(1));
    esp_rmaker_device_add_param(s_dev_battery, thr);

    esp_rmaker_device_add_param(s_dev_battery, ro_param(PARAM_FAN_LAST_ON, "esp.param.status",
                                                        esp_rmaker_str("--"), ESP_RMAKER_UI_TEXT));
    esp_rmaker_node_add_device(node, s_dev_battery);

    /* ============================== WATER TANKS ==============================
     * Levels, and the water in them. Every figure starts at the sentinel: 0 %
     * is an empty tank, 0 ppm is distilled water and 0 % rejection is a
     * destroyed membrane, so none of them may be what the app shows before
     * anything has been measured. */
    s_dev_tanks = esp_rmaker_device_create(DEV_TANKS, "esp.device.water-tank", NULL);
    esp_rmaker_param_t *twt = ro_param(PARAM_TWT_PCT, "esp.param.water-level",
                                       esp_rmaker_int(VAL_NO_READING_INT), ESP_RMAKER_UI_TEXT);
    esp_rmaker_device_add_param(s_dev_tanks, twt);
    esp_rmaker_device_assign_primary_param(s_dev_tanks, twt);
    esp_rmaker_device_add_param(s_dev_tanks, ro_param(PARAM_RWT_PCT, "esp.param.water-level",
                                                      esp_rmaker_int(VAL_NO_READING_INT), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_tanks, ro_param(PARAM_DOS_PCT, "esp.param.water-level",
                                                      esp_rmaker_int(VAL_NO_READING_INT), ESP_RMAKER_UI_TEXT));
    /* The Aster own TWT float, next to our ultrasonic reading of the same tank,
     * because the useful thing is the DISAGREEMENT: float closed with the level
     * reading low means one of the two is lying. */
    esp_rmaker_device_add_param(s_dev_tanks, ro_param(PARAM_TWT_FLOAT, "esp.param.toggle",
                                                      esp_rmaker_bool(false), ESP_RMAKER_UI_TOGGLE));
    esp_rmaker_device_add_param(s_dev_tanks, ro_param(PARAM_RWT_TDS, "esp.param.concentration",
                                                      esp_rmaker_int(VAL_NO_READING_INT), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_tanks, ro_param(PARAM_TWT_TDS, "esp.param.concentration",
                                                      esp_rmaker_int(VAL_NO_READING_INT), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_tanks, ro_param(PARAM_RWT_WTEMP, ESP_RMAKER_PARAM_TEMPERATURE,
                                                      esp_rmaker_float(VAL_NO_READING_FLOAT), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_tanks, ro_param(PARAM_TWT_WTEMP, ESP_RMAKER_PARAM_TEMPERATURE,
                                                      esp_rmaker_float(VAL_NO_READING_FLOAT), ESP_RMAKER_UI_TEXT));
    esp_rmaker_device_add_param(s_dev_tanks, ro_param(PARAM_REJECTION, "esp.param.percentage",
                                                      esp_rmaker_int(VAL_NO_READING_INT), ESP_RMAKER_UI_TEXT));
    esp_rmaker_node_add_device(node, s_dev_tanks);
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
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT && AP_MODE_ENABLED
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
/* AP off, by AP_MODE_ENABLED or because SoftAP is not compiled in. Provisioning
 * is BLE so pairing still works; what is lost is reaching the dashboard and /cal
 * without a working router. Logged at boot rather than left silent, because "I
 * cannot find RO-HUB" is otherwise indistinguishable from a broken radio. */
static void start_softap(void)
{
    ESP_LOGW(TAG, "local AP disabled (AP_MODE_ENABLED=%d) - dashboard and /cal are LAN only",
             AP_MODE_ENABLED);
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
    ESP_LOGI(TAG, "  RO Monitor — Central Hub  fw %s", esp_app_get_description()->version);
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
    esp_rmaker_node_add_attribute(node, "Firmware", esp_app_get_description()->version);

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
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT && AP_MODE_ENABLED
    ESP_LOGI(TAG, "  Fallback  : SSID %s -> http://192.168.4.1/", AP_SSID);
#else
    /* Printed, not omitted. "There is no fallback" is the single most useful
     * thing this banner can say to somebody standing in front of a hub whose
     * router is down, and a missing line says nothing at all. */
    ESP_LOGI(TAG, "  Fallback  : none - LAN only, AP_MODE_ENABLED=0");
#endif
    ESP_LOGI(TAG, "  Calibrate : /cal  (user %s)", CAL_USER);
    ESP_LOGI(TAG, "  Pairing   : RainMaker app, PoP rohub1234");
    ESP_LOGI(TAG, "==========================================");
}
