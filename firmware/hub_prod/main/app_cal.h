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
    CAL_TANK_COUNT,
} cal_tank_t;

typedef enum {
    CAL_CT_HPP = 0,
    CAL_CT_RWP = 1,
    CAL_CT_COUNT,
} cal_ct_t;

typedef struct {
    uint16_t full_mm;    /* distance at 100 % */
    uint16_t empty_mm;   /* distance at 0 %   */
} cal_tank_cfg_t;

typedef struct {
    /* Amps per volt of clamp output, x100. An SCT-013-030 is nominally 30 A per
     * 1 V, so 3000 — but it is a trend instrument and the honest figure comes
     * from a two-point calibration against a clamp meter. */
    uint16_t amps_per_volt_x100;
    uint8_t  turns;              /* conductor passes through the jaws; divides the result */
    uint16_t oc_deci_amps;       /* over-current alert threshold */
} cal_ct_cfg_t;

esp_err_t cal_init(void);

const cal_tank_cfg_t *cal_tank(cal_tank_t t);
const cal_ct_cfg_t   *cal_ct(cal_ct_t c);

/* Each setter validates before it stores. A typo from a phone must not be able
 * to produce a calibration that reads plausibly and is wrong. */
esp_err_t cal_set_tank(cal_tank_t t, uint16_t full_mm, uint16_t empty_mm);
esp_err_t cal_set_ct(cal_ct_t c, uint16_t amps_per_volt_x100, uint8_t turns, uint16_t oc_deci_amps);
esp_err_t cal_set_fan(uint16_t on_deci_c, uint16_t off_deci_c);

uint16_t cal_fan_on_deci_c(void);
uint16_t cal_fan_off_deci_c(void);

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

#ifdef __cplusplus
}
#endif
