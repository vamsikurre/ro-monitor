/*
 * app_cal.c — NVS-backed calibration
 *
 * One namespace, short keys, everything typed. Defaults come from app_priv.h and
 * from the geometry notes in WIRING.md §9.3 / §13, so a hub that has never been
 * calibrated still produces sane-looking readings rather than zeros — while
 * levelPercent() refuses to report a level it cannot justify.
 */

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_cal.h"
#include "app_priv.h"

static const char *TAG = "cal";
static const char *NVS_NS = "ro_cal";

/* Defaults match the hub sketch that was on the bench, so replacing the firmware
 * does not silently move a calibrated tank. WIRING.md §13 explains the dosing
 * figures: the sensor sits on a bracket 250-300 mm above the barrel mouth. */
static cal_tank_cfg_t s_tanks[CAL_TANK_COUNT] = {
    [CAL_TANK_RWT] = { .full_mm = 300, .empty_mm = 1500, .tds_k_x100 = 100 },
    [CAL_TANK_TWT] = { .full_mm = 300, .empty_mm = 1500, .tds_k_x100 = 100 },
    [CAL_TANK_DOS] = { .full_mm = 250, .empty_mm = 900,  .tds_k_x100 = 100 },
};

static cal_ct_cfg_t s_cts[CAL_CT_COUNT] = {
    [CAL_CT_HPP] = { .amps_per_volt_x100 = 3000, .turns = 1, .oc_deci_amps = OC_HPP_DECI_A_DEFAULT },
    [CAL_CT_RWP] = { .amps_per_volt_x100 = 3000, .turns = 1, .oc_deci_amps = OC_RWP_DECI_A_DEFAULT },
};

static uint16_t s_fan_on_deci_c  = FAN_ON_DECI_C_DEFAULT;
static uint16_t s_fan_off_deci_c = FAN_OFF_DECI_C_DEFAULT;
static char     s_cal_pass[33]   = CAL_PASS_DEFAULT;

static const char *s_tank_keys[CAL_TANK_COUNT]   = { "rwt", "twt", "dos" };
static const char *s_tank_labels[CAL_TANK_COUNT] = { "Raw Water", "Treated Water", "Dosing" };
static const char *s_ct_keys[CAL_CT_COUNT]       = { "hpp", "rwp" };
static const char *s_ct_labels[CAL_CT_COUNT]     = { "HPP", "RWP" };

const char *cal_tank_key(cal_tank_t t)   { return (t < CAL_TANK_COUNT) ? s_tank_keys[t] : "?"; }
const char *cal_tank_label(cal_tank_t t) { return (t < CAL_TANK_COUNT) ? s_tank_labels[t] : "?"; }
const char *cal_ct_key(cal_ct_t c)       { return (c < CAL_CT_COUNT) ? s_ct_keys[c] : "?"; }
const char *cal_ct_label(cal_ct_t c)     { return (c < CAL_CT_COUNT) ? s_ct_labels[c] : "?"; }

const cal_tank_cfg_t *cal_tank(cal_tank_t t) { return &s_tanks[t < CAL_TANK_COUNT ? t : 0]; }
const cal_ct_cfg_t   *cal_ct(cal_ct_t c)     { return &s_cts[c < CAL_CT_COUNT ? c : 0]; }
uint16_t cal_fan_on_deci_c(void)             { return s_fan_on_deci_c; }
uint16_t cal_fan_off_deci_c(void)            { return s_fan_off_deci_c; }

/* NVS keys are capped at 15 characters, which is why these are abbreviated
 * rather than spelled out. "rwt_f" is tank RWT's full distance. */
static void key_for(char *out, size_t len, const char *base, const char *suffix)
{
    snprintf(out, len, "%s_%s", base, suffix);
}

static esp_err_t store_u16(const char *base, const char *suffix, uint16_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char key[16];
    key_for(key, sizeof(key), base, suffix);
    err = nvs_set_u16(h, key, v);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void load_u16(nvs_handle_t h, const char *base, const char *suffix, uint16_t *dst)
{
    char key[16];
    key_for(key, sizeof(key), base, suffix);
    uint16_t v;
    if (nvs_get_u16(h, key, &v) == ESP_OK) {
        *dst = v;
    }
}

esp_err_t cal_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored calibration — running on defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    for (int i = 0; i < CAL_TANK_COUNT; i++) {
        load_u16(h, s_tank_keys[i], "f", &s_tanks[i].full_mm);
        load_u16(h, s_tank_keys[i], "e", &s_tanks[i].empty_mm);
        load_u16(h, s_tank_keys[i], "k", &s_tanks[i].tds_k_x100);
    }
    for (int i = 0; i < CAL_CT_COUNT; i++) {
        load_u16(h, s_ct_keys[i], "s", &s_cts[i].amps_per_volt_x100);
        load_u16(h, s_ct_keys[i], "o", &s_cts[i].oc_deci_amps);
        uint16_t turns = s_cts[i].turns;
        load_u16(h, s_ct_keys[i], "t", &turns);
        s_cts[i].turns = (turns >= 1 && turns <= 10) ? (uint8_t)turns : 1;
    }
    load_u16(h, "fan", "on", &s_fan_on_deci_c);
    load_u16(h, "fan", "off", &s_fan_off_deci_c);

    size_t plen = sizeof(s_cal_pass);
    nvs_get_str(h, "cal_pass", s_cal_pass, &plen);

    nvs_close(h);

    ESP_LOGI(TAG, "loaded: rwt %u/%u  twt %u/%u  dos %u/%u  fan %u.%u/%u.%u C",
             s_tanks[0].full_mm, s_tanks[0].empty_mm,
             s_tanks[1].full_mm, s_tanks[1].empty_mm,
             s_tanks[2].full_mm, s_tanks[2].empty_mm,
             s_fan_on_deci_c / 10, s_fan_on_deci_c % 10,
             s_fan_off_deci_c / 10, s_fan_off_deci_c % 10);
    return ESP_OK;
}

esp_err_t cal_set_tank(cal_tank_t t, uint16_t full_mm, uint16_t empty_mm)
{
    if (t >= CAL_TANK_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Same three refusals the sketch made, for the same reason: each of these
     * produces a reading that looks like a level and is not one. */
    if (empty_mm <= full_mm) {
        return ESP_ERR_INVALID_ARG;   /* empty must be the longer distance */
    }
    if (full_mm < BLIND_ZONE_MM) {
        return ESP_ERR_INVALID_ARG;   /* full is inside the sensor's blind zone */
    }

    s_tanks[t].full_mm = full_mm;
    s_tanks[t].empty_mm = empty_mm;

    esp_err_t err = store_u16(s_tank_keys[t], "f", full_mm);
    if (err == ESP_OK) {
        err = store_u16(s_tank_keys[t], "e", empty_mm);
    }
    ESP_LOGI(TAG, "%s: full %u mm, empty %u mm", s_tank_keys[t], full_mm, empty_mm);
    return err;
}

esp_err_t cal_set_tds(cal_tank_t t, uint16_t k_x100)
{
    if (t >= CAL_TANK_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    /* A probe needing more than a 2x correction is not calibrated, it is broken
     * or in the wrong solution - and accepting the number would bake that in. */
    if (k_x100 < 50 || k_x100 > 200) {
        return ESP_ERR_INVALID_ARG;
    }
    s_tanks[t].tds_k_x100 = k_x100;
    ESP_LOGI(TAG, "%s TDS k: %u.%02u", s_tank_keys[t], k_x100 / 100, k_x100 % 100);
    return store_u16(s_tank_keys[t], "k", k_x100);
}

esp_err_t cal_set_ct(cal_ct_t c, uint16_t amps_per_volt_x100, uint8_t turns, uint16_t oc_deci_amps)
{
    if (c >= CAL_CT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (amps_per_volt_x100 < 100 || amps_per_volt_x100 > 20000) {
        return ESP_ERR_INVALID_ARG;
    }
    if (turns < 1 || turns > 10) {
        return ESP_ERR_INVALID_ARG;
    }
    if (oc_deci_amps < OC_LIMIT_LOW_DECI || oc_deci_amps > OC_LIMIT_HIGH_DECI) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cts[c].amps_per_volt_x100 = amps_per_volt_x100;
    s_cts[c].turns = turns;
    s_cts[c].oc_deci_amps = oc_deci_amps;

    esp_err_t err = store_u16(s_ct_keys[c], "s", amps_per_volt_x100);
    if (err == ESP_OK) {
        err = store_u16(s_ct_keys[c], "t", turns);
    }
    if (err == ESP_OK) {
        err = store_u16(s_ct_keys[c], "o", oc_deci_amps);
    }
    ESP_LOGI(TAG, "%s CT: %u.%02u A/V, %u turns, OC at %u.%u A",
             s_ct_keys[c], amps_per_volt_x100 / 100, amps_per_volt_x100 % 100,
             turns, oc_deci_amps / 10, oc_deci_amps % 10);
    return err;
}

esp_err_t cal_set_fan(uint16_t on_deci_c, uint16_t off_deci_c)
{
    /* Clamped, not trusted. A typo on a phone must not be able to disable
     * ventilation in a battery room, so the band is narrow and the hysteresis is
     * enforced rather than assumed. */
    if (on_deci_c < FAN_LIMIT_LOW_DECI || on_deci_c > FAN_LIMIT_HIGH_DECI ||
        off_deci_c < FAN_LIMIT_LOW_DECI || off_deci_c > FAN_LIMIT_HIGH_DECI) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((int)on_deci_c - (int)off_deci_c < FAN_MIN_HYST_DECI) {
        return ESP_ERR_INVALID_ARG;
    }

    s_fan_on_deci_c = on_deci_c;
    s_fan_off_deci_c = off_deci_c;

    esp_err_t err = store_u16("fan", "on", on_deci_c);
    if (err == ESP_OK) {
        err = store_u16("fan", "off", off_deci_c);
    }
    return err;
}

bool cal_password_matches(const char *user, const char *pass)
{
    if (user == NULL || pass == NULL) {
        return false;
    }
    if (strcmp(user, CAL_USER) != 0) {
        return false;
    }
    /* Length-independent compare, so a wrong password does not leak its length
     * through timing. Cheap here and the habit is worth keeping. */
    size_t a = strlen(pass), b = strlen(s_cal_pass);
    uint8_t diff = (a == b) ? 0 : 1;
    for (size_t i = 0; i < a && i < b; i++) {
        diff |= (uint8_t)(pass[i] ^ s_cal_pass[i]);
    }
    return diff == 0;
}

esp_err_t cal_set_password(const char *pass)
{
    if (pass == NULL || strlen(pass) < 8 || strlen(pass) > 32) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "cal_pass", pass);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        strncpy(s_cal_pass, pass, sizeof(s_cal_pass) - 1);
        s_cal_pass[sizeof(s_cal_pass) - 1] = '\0';
        ESP_LOGI(TAG, "calibration password changed");
    }
    return err;
}

uint8_t levelPercent(uint16_t distanceMM, uint16_t fullMM, uint16_t emptyMM)
{
    if (distanceMM == 0) return 255;                 /* no echo */
    if (emptyMM <= fullMM) return 255;               /* uncalibrated, or inverted */

    /* Genuinely blind: below the transducer's minimum range there is nothing to
     * trust. It is also where FOULING shows up - crystallised anti-scalant on the
     * face of a dosing sensor, condensation, splash, or the sensor knocked round
     * to look at a wall 170 mm away. Reporting "full" here would let a fouled
     * sensor suppress the replenish alert, which is the one failure direction
     * that costs membranes. So: no level, and the caller shows a fault. */
    if (distanceMM < BLIND_ZONE_MM) return 255;

    /* Over-full, and a perfectly good measurement. Closer than the calibrated
     * full mark but still inside the sensor's range means the vessel is fuller
     * than whoever calibrated it expected - a topped-up dosing barrel, or a tank
     * above its float. That is 100%, not an error. These two cases used to share
     * one branch, which is why an over-filled barrel read as no-level. */
    if (distanceMM < fullMM) return 100;

    if (distanceMM >= emptyMM) return 0;

    long span = (long)emptyMM - (long)fullMM;
    return (uint8_t)(((long)emptyMM - (long)distanceMM) * 100L / span);
}

uint16_t tdsPPM(uint16_t mv, int16_t water_temp_deci_c, uint16_t k_x100)
{
    /* 0 mV is an unpowered or absent probe, not perfectly pure water. The node
     * only ever sends a real reading with its status byte clear, but this must
     * hold on its own - it is the function a checker exercises. */
    if (mv == 0 || mv > TDS_MV_MAX) {
        return TDS_INVALID;
    }
    if (k_x100 == 0) {
        return TDS_INVALID;
    }
    /* Liquid water in a roof tank. Outside this the DS18B20 is faulty, not the
     * weather - 85.0 C in particular is its power-on default, which the node
     * already rejects, and this is the second net under it. */
    if (water_temp_deci_c < 0 || water_temp_deci_c > 600) {
        return TDS_INVALID;
    }

    /* Standard EC compensation: conductivity moves about 2 % per degree, and
     * everything is quoted at 25 C. Without this a tank reads ~40 % apart
     * between a January morning and a May afternoon with nothing having changed
     * in the water, which is precisely the kind of confident wrongness the level
     * maths refuses elsewhere. */
    float coeff = 1.0f + 0.02f * ((water_temp_deci_c / 10.0f) - 25.0f);
    if (coeff < 0.1f) {
        return TDS_INVALID;
    }

    float v = (mv / 1000.0f) / coeff;
    float ppm = (133.42f * v * v * v - 255.86f * v * v + 857.39f * v) * 0.5f;
    ppm = ppm * ((float)k_x100 / 100.0f);

    if (ppm < 0.0f) {
        return 0;
    }
    /* Past this the cubic is extrapolating well beyond anything the probe was
     * fitted to measure, so the number would be arithmetic rather than a
     * measurement. Note that readings between the probe's rated 1000 ppm and
     * this ceiling ARE reported: a brackish source is a fact worth seeing, just
     * a less accurate one. */
    if (ppm > (float)TDS_MAX_PPM) {
        return TDS_INVALID;
    }
    return (uint16_t)(ppm + 0.5f);
}

int16_t rejectionPercent(uint16_t feed_ppm, uint16_t permeate_ppm)
{
    if (feed_ppm == TDS_INVALID || permeate_ppm == TDS_INVALID) {
        return -1;
    }
    /* Below this the ratio is mostly probe noise: a 20 ppm feed and a 4 ppm
     * permeate is arithmetically 80 % rejection and means nothing. */
    if (feed_ppm < TDS_MIN_FEED_PPM) {
        return -1;
    }
    if (permeate_ppm >= feed_ppm) {
        return 0;               /* no rejection at all, or the probes are swapped */
    }
    return (int16_t)(((uint32_t)(feed_ppm - permeate_ppm) * 100U) / feed_ppm);
}
