/*
 * app_sensors.h — everything wired straight to the hub
 *
 * The RS485 nodes are app_rs485.c's problem. This is the hub's own I2C climate
 * sensor, the dosing ultrasonic, the six optoisolator channels and the two
 * current clamps.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#include "app_cal.h"
#include "app_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sensors_init(void);

/* RO room SHT30 on the hub's own I2C. Returns false and leaves the outputs
 * untouched if either CRC-8 fails — a corrupt reading here raises alarms. */
bool sht30_read(int16_t *temp_deci_c, uint16_t *hum_deci_pct);

/* Dosing tank, direct to the hub. 0 means no echo. Blocks for up to 35 ms, so
 * call it in the idle part of the cycle, never between a poll and its reply. */
uint16_t dosing_read_mm(void);

/* The four PC817 dry-contact channels. All active LOW through INPUT_PULLUP. */
bool opto_twt_float_closed(void);
bool opto_rl1_active(void);
bool opto_rl2_active(void);
bool opto_lps_active(void);

/* ALARM is the exception: read straight off the Aster's volt-free relay contact,
 * with no optocoupler and no external parts. Active LOW on the internal pull-up,
 * and DEBOUNCED - it is the highest-impedance input on the board, so a single
 * sample is not trustworthy. See app_priv.h and WIRING.md §6.6. */
bool alarm_active(void);

/*
 * The two 240 V channels, measured rather than sampled.
 *
 * These pins are ADC1, and measuring them separates three states a digitalRead
 * collapses into one "OFF": mains present, idle with the module's 47k pull-up
 * alive, and a pin floating because VCC or OUT lost continuity. The last of those
 * has happened once on this build, and it is the reading most likely to be filed
 * as a pass.
 *
 * `running` is taken from the MINIMUM across a full mains cycle, so it latches
 * correctly whether the module's 100 uF holds the output steady or a future batch
 * without it lets the output pulse. That is phase-B spec §5.4's prescribed fix,
 * obtained by measuring instead of by widening a digital filter.
 */
void ac_probe(int gpio, bool *running, bool *floating, uint32_t *mv_lo, uint32_t *mv_hi);

/*
 * True RMS current on one clamp, in tenths of an amp.
 *
 * Returns -1 when there is nothing trustworthy to report: the pedestal is absent
 * (no breakout fitted, or the divider is wrong), which would otherwise produce a
 * large and entirely fictional current. Blocks for ~200 ms.
 */
int16_t ct_read_deci_amps(int gpio, cal_ct_t which);

/* Raw pedestal figures for the calibration page, so the breakout can be proven
 * before any clamp is fitted — ~1650 mV steady is correct. */
void ct_probe(int gpio, uint32_t *mv_lo, uint32_t *mv_hi);

#ifdef __cplusplus
}
#endif
