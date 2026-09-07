/*
 * app_cal.h — calibration that survives a reboot, and the maths that uses it
 *
 * Nothing here is compiled into a threshold. Tanks get re-calibrated, clamps get
 * re-seated, and climbing to a roof with a laptop to edit two numbers does not
 * scale — so every figure lives in NVS and is set from /cal over the network.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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
    uint16_t full_mm;    /* distance at 100 % */
    uint16_t empty_mm;   /* distance at 0 %   */
    /* TDS probe scale, x100. 100 = 1.00, the uncalibrated default. Set it by
     * reading a known solution and scaling: a 707 ppm sachet reading 640 wants
     * k = 707/640 = 1.10, so 110. Per tank, because two probes of the same part
     * number do not agree out of the bag. */
    uint16_t tds_k_x100;
    /* Level at or above which the TDS/temperature probe is under water. Below
     * it the hub keeps the last good reading and shows its age instead of
     * trusting a probe that is measuring air. 90 until the leads are extended
     * to the tank's low mark, then whatever level the tip actually sits at. */
    uint8_t  tds_min_pct;
    /* Sump only. Full-scale head of a 4-20 mA submersible transducer, in mm.
     * 0 = no transducer, the ultrasonic is the source. The node reports raw
     * microamps and gfLoopDistanceMM() turns them into the same distance-alike
     * figure the ultrasonic gives, so full/empty above apply unchanged
     * (WIRING.md 9.4.1). */
    uint16_t press_range_mm;
} cal_tank_cfg_t;

typedef struct {
    /* Amps per volt of clamp output, x100. An SCT-013-030 is nominally 30 A per
     * 1 V, so 3000 — but it is a trend instrument and the honest figure comes
     * from a two-point calibration against a clamp meter. */
    uint16_t amps_per_volt_x100;
    uint8_t  turns;              /* conductor passes through the jaws; divides the result */
    uint16_t oc_deci_amps;       /* over-current alert threshold */
    /* Highest phase at or above this = motor running. Only the remote motors
     * use it (the hub's own pumps have contactor optos); 1.0 A default. */
    uint16_t run_deci_amps;
} cal_ct_cfg_t;

esp_err_t cal_init(void);

const cal_tank_cfg_t *cal_tank(cal_tank_t t);
const cal_ct_cfg_t   *cal_ct(cal_ct_t c);

/* Each setter validates before it stores. A typo from a phone must not be able
 * to produce a calibration that reads plausibly and is wrong. */
esp_err_t cal_set_tank(cal_tank_t t, uint16_t full_mm, uint16_t empty_mm);
esp_err_t cal_set_tds(cal_tank_t t, uint16_t k_x100, uint8_t min_pct);
esp_err_t cal_set_ct(cal_ct_t c, uint16_t amps_per_volt_x100, uint8_t turns,
                     uint16_t oc_deci_amps, uint16_t run_deci_amps);
esp_err_t cal_set_fan(uint16_t on_deci_c, uint16_t off_deci_c);
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

/*
 * "Last seen" wall-clock stamps, in the same NVS namespace because it is already
 * open and these have the same lifetime as a calibration: they must survive a
 * power cut, or the first thing a returning hub reports is that the plant has
 * never run.
 *
 * Epoch seconds; 0 means never observed. Written only on a transition - a few
 * times a day - so NVS wear is not a consideration.
 */
typedef enum {
    CAL_EVT_HPP_ON = 0,
    CAL_EVT_RWP_ON,
    CAL_EVT_TWT_FULL,
    CAL_EVT_FAN_ON,
    CAL_EVT_COUNT,
} cal_event_t;

uint32_t  cal_event_get(cal_event_t e);
esp_err_t cal_event_set(cal_event_t e, uint32_t epoch);

uint16_t cal_fan_on_deci_c(void);
uint16_t cal_fan_off_deci_c(void);

/* Rated permeate output, litres per hour, for "litres produced today" = HPP run
 * hours x this. Nameplate is 1200; the skid's analogue meter reads ~900 and that
 * is the honest figure, so it is a knob, not a constant. */
uint16_t  cal_plant_lph(void);
esp_err_t cal_set_plant_lph(uint16_t lph);

/* Cumulative pump run seconds, written on each stop - a handful of NVS writes a
 * day. This is the figure that survives a power cut. */
uint32_t  cal_runtime_get(cal_ct_t c);
esp_err_t cal_runtime_set(cal_ct_t c, uint32_t seconds);

/* Per-day run minutes for the last CAL_DAYS days, oldest first, one NVS blob.
 * Weekly and monthly production are sums over this; "today" is restored from it
 * at boot. Written on each pump stop and every few minutes while running. */
#define CAL_DAYS 35
typedef struct {
    uint32_t midnight;             /* local midnight, epoch seconds */
    uint16_t hpp_min, rwp_min;
} cal_day_t;
uint16_t  cal_days(const cal_day_t **out);
esp_err_t cal_day_set(uint32_t midnight, uint16_t hpp_min, uint16_t rwp_min);

/* Calibration page credentials. The password is stored so a site can change it
 * without a reflash; the default is in app_priv.h and is not a secret. */
bool      cal_password_matches(const char *user, const char *pass);
esp_err_t cal_set_password(const char *pass);

const char *cal_tank_key(cal_tank_t t);
const char *cal_tank_label(cal_tank_t t);
const char *cal_ct_key(cal_ct_t c);
const char *cal_ct_label(cal_ct_t c);

/*
 * Distance to percentage, with the calibration passed in rather than read, so
 * the function is pure and docs/check_frame.py can compile it standalone.
 *
 * Returns 255 for "no level", never a plausible-looking number: no echo, an
 * uncalibrated tank, an inverted or zero-span calibration, and a reading inside
 * the blind zone are all cases where the honest answer is that we do not know.
 * The caller turns 255 into an OFFLINE/SENSOR_ERROR state; it must never reach a
 * gauge as a value.
 */
uint8_t levelPercent(uint16_t distanceMM, uint16_t fullMM, uint16_t emptyMM);

/*
 * TDS in ppm from what a node actually reports: probe millivolts and water
 * temperature. Pure, calibration passed in, so docs/check_frame.py compiles it
 * standalone - the same treatment levelPercent() gets, and for the same reason.
 *
 * The node deliberately does not do this: it is a floating-point cubic, and an
 * ATmega with no float anywhere else is the wrong place for it. Keeping it here
 * also means the k factor is editable over Wi-Fi instead of by reflashing a
 * board on a roof.
 *
 * Returns TDS_INVALID, never a plausible-looking number, when the probe is
 * unpowered or absent, when the temperature is outside anything water does, or
 * when the tank is uncalibrated. The caller shows a fault; it must never reach
 * a gauge as a value.
 */
#define TDS_INVALID  0xFFFF
uint16_t tdsPPM(uint16_t mv, int16_t water_temp_deci_c, uint16_t k_x100);

/*
 * Salt rejection, the number that actually says whether the membrane is healthy.
 *
 * Permeate TDS rising is not a fault on its own - it rises when the FEED rises,
 * which is a source-water fact, not a plant one. The ratio removes that, and it
 * also cancels most of the temperature error, since both probes sit in the same
 * weather. Returns -1 when either reading is invalid, or when the feed is too
 * dilute for the ratio to mean anything.
 */
int16_t rejectionPercent(uint16_t feed_ppm, uint16_t permeate_ppm);

#ifdef __cplusplus
}
#endif
