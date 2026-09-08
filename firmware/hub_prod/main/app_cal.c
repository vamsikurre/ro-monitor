/*
 * app_cal.c — NVS-backed calibration
 *
 * One namespace, short keys, everything typed. Defaults come from app_priv.h and
 * from the geometry notes in WIRING.md §9.3 / §13, so a hub that has never been
 * calibrated still produces sane-looking readings rather than zeros — while
 * levelPercent() refuses to report a level it cannot justify.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_cal.h"
#include "app_priv.h"

static const char *TAG = "cal";
static const char *NVS_NS = "ro_cal";

/* Defaults match the hub sketch that was on the bench, so replacing the firmware
 * does not silently move a calibrated tank. Dosing: the drum is 530 mm deep and
 * the sensor is IN THE LID (WIRING.md §13), so the floor sits ~540 mm from the
 * face. The old 900 mm "empty" assumed a bracket that was never fitted and could
 * never read below ~55 % - the dosing-low alert was unreachable. */
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
    [CAL_CT_BORE] = { .amps_per_volt_x100 = 3000, .turns = 1, .oc_deci_amps = OC_BORE_DECI_A_DEFAULT, .run_deci_amps = RUN_DECI_A_DEFAULT,
                      .dry_deci_amps = BORE_DRY_DECI_A_DEFAULT },
    [CAL_CT_SUMP] = { .amps_per_volt_x100 = 3000, .turns = 1, .oc_deci_amps = OC_SUMP_DECI_A_DEFAULT, .run_deci_amps = RUN_DECI_A_DEFAULT },
};

static uint16_t s_fan_on_deci_c  = FAN_ON_DECI_C_DEFAULT;
static uint16_t s_fan_off_deci_c = FAN_OFF_DECI_C_DEFAULT;
static char     s_cal_pass[33]   = CAL_PASS_DEFAULT;

static const char *s_evt_keys[CAL_EVT_COUNT]     = { "t_hpp", "t_rwp", "t_twtf", "t_fan" };
static uint32_t    s_evt[CAL_EVT_COUNT];

static const char *s_tank_keys[CAL_TANK_COUNT]   = { "rwt", "twt", "dos", "sump" };
static const char *s_tank_labels[CAL_TANK_COUNT] = { "Raw Water", "Treated Water", "Dosing", "Sump" };
static const char *s_ct_keys[CAL_CT_COUNT]       = { "hpp", "rwp", "bore", "smot" };
static uint16_t s_plant_lph = PLANT_LPH_DEFAULT;
static uint32_t s_runtime_s[CAL_CT_COUNT];
static cal_day_t s_days[CAL_DAYS];
static uint16_t  s_day_n;
static const char *s_ct_labels[CAL_CT_COUNT]     = { "HPP", "RWP", "Borewell", "Sump motor" };
static const char *s_gf_keys[CAL_GF_COUNT]       = { "sump", "util" };
static const char *s_gf_nvs[CAL_GF_COUNT]        = { "gf_sump", "gf_util" };
static char        s_gf_ip[CAL_GF_COUNT][24];    /* "255.255.255.255:65535" fits */

const char *cal_tank_key(cal_tank_t t)   { return (t < CAL_TANK_COUNT) ? s_tank_keys[t] : "?"; }
const char *cal_tank_label(cal_tank_t t) { return (t < CAL_TANK_COUNT) ? s_tank_labels[t] : "?"; }
const char *cal_ct_key(cal_ct_t c)       { return (c < CAL_CT_COUNT) ? s_ct_keys[c] : "?"; }
const char *cal_ct_label(cal_ct_t c)     { return (c < CAL_CT_COUNT) ? s_ct_labels[c] : "?"; }
const char *cal_gf_key(cal_gf_t n)       { return (n < CAL_GF_COUNT) ? s_gf_keys[n] : "?"; }
const char *cal_gf_ip(cal_gf_t n)        { return (n < CAL_GF_COUNT) ? s_gf_ip[n] : ""; }

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
        load_u16(h, s_tank_keys[i], "r", &s_tanks[i].press_range_mm);
        uint16_t mp = s_tanks[i].tds_min_pct;
        load_u16(h, s_tank_keys[i], "m", &mp);
        s_tanks[i].tds_min_pct = (mp <= 100) ? (uint8_t)mp : 90;
    }
    for (int i = 0; i < CAL_CT_COUNT; i++) {
        load_u16(h, s_ct_keys[i], "s", &s_cts[i].amps_per_volt_x100);
        load_u16(h, s_ct_keys[i], "o", &s_cts[i].oc_deci_amps);
        load_u16(h, s_ct_keys[i], "n", &s_cts[i].run_deci_amps);
        /* "d" is newer than the other four. load_u16 leaves the compiled
         * default when a key is absent, so a hub calibrated before this
         * existed reads 0 here and the dry detector stays off - no migration,
         * because these are separate u16 keys and not one blob. */
        load_u16(h, s_ct_keys[i], "d", &s_cts[i].dry_deci_amps);
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
    for (int i = 0; i < CAL_EVT_COUNT; i++) {
        nvs_get_u32(h, s_evt_keys[i], &s_evt[i]);   /* absent leaves it 0 = never */
    }
    load_u16(h, "fan", "on", &s_fan_on_deci_c);
    load_u16(h, "fan", "off", &s_fan_off_deci_c);
    load_u16(h, "plant", "lph", &s_plant_lph);
    for (int i = 0; i < CAL_CT_COUNT; i++) {
        char key[16];
        key_for(key, sizeof(key), s_ct_keys[i], "rt");
        nvs_get_u32(h, key, &s_runtime_s[i]);       /* absent leaves 0 */
    }
    /* "days2", not "days": cal_day_t grew two fields for the ground-floor
     * motors, so a blob written by an older build is a different record size.
     * Read under the old key it would divide into a plausible row count and
     * come back as garbage midnights and impossible minutes. A new key leaves
     * the old blob unread, which is what is wanted - nothing is deployed, and
     * a lost bench ledger costs nothing next to a misparsed one. */
    size_t dlen = sizeof(s_days);
    if (nvs_get_blob(h, "days2", s_days, &dlen) == ESP_OK) {
        s_day_n = (uint16_t)(dlen / sizeof(cal_day_t));
        if (s_day_n > CAL_DAYS) s_day_n = CAL_DAYS;
    }

    size_t plen = sizeof(s_cal_pass);
    nvs_get_str(h, "cal_pass", s_cal_pass, &plen);

    nvs_close(h);

    /* Retire the pre-"days2" blob: nothing will ever read it again, and it is
     * 280 bytes of a small partition. Its own handle because ours above is
     * read-only, and not found is the ordinary case on a hub that never wrote
     * one. */
    nvs_handle_t wh;
    if (nvs_open(NVS_NS, NVS_READWRITE, &wh) == ESP_OK) {
        esp_err_t derr = nvs_erase_key(wh, "days");
        if (derr == ESP_OK) {
            nvs_commit(wh);
            ESP_LOGI(TAG, "erased orphaned \"days\" ledger blob");
        } else if (derr != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "could not erase \"days\": %s", esp_err_to_name(derr));
        }
        nvs_close(wh);
    }

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

esp_err_t cal_set_tds(cal_tank_t t, uint16_t k_x100, uint8_t min_pct)
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
    if (min_pct > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    s_tanks[t].tds_min_pct = min_pct;
    esp_err_t err = store_u16(s_tank_keys[t], "k", k_x100);
    if (err == ESP_OK) {
        err = store_u16(s_tank_keys[t], "m", min_pct);
    }
    return err;
}

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
    /* The dry threshold has to sit between the two figures just written, and
     * this call can move either of them out from under it. Switch it off
     * rather than keep a threshold that can no longer mean anything - a
     * detector that quietly never fires is worse than one plainly off, and the
     * log says so where somebody recalibrating will see it. */
    if (s_cts[c].dry_deci_amps != 0 &&
        (s_cts[c].dry_deci_amps <= run_deci_amps || s_cts[c].dry_deci_amps >= oc_deci_amps)) {
        ESP_LOGW(TAG, "%s CT: dry threshold %u.%u A no longer between run and OC - switched off",
                 s_ct_keys[c], s_cts[c].dry_deci_amps / 10, s_cts[c].dry_deci_amps % 10);
        s_cts[c].dry_deci_amps = 0;
        if (err == ESP_OK) err = store_u16(s_ct_keys[c], "d", 0);
    }
    ESP_LOGI(TAG, "%s CT: %u.%02u A/V, %u turns, run at %u.%u A, OC at %u.%u A",
             s_ct_keys[c], amps_per_volt_x100 / 100, amps_per_volt_x100 % 100, turns,
             run_deci_amps / 10, run_deci_amps % 10, oc_deci_amps / 10, oc_deci_amps % 10);
    return err;
}

esp_err_t cal_set_ct_dry(cal_ct_t c, uint16_t dry_deci_amps)
{
    if (c >= CAL_CT_COUNT) return ESP_ERR_INVALID_ARG;
    /* 0 switches the detector off. Any other value has to sit strictly between
     * "the motor is energised" and "the motor is in trouble": at or below
     * run_deci_amps it could never fire while running, and at or above
     * oc_deci_amps a healthy loaded pump would read dry the whole time it ran.
     * Both are silent failures, so they are refused rather than clamped. */
    if (dry_deci_amps != 0 &&
        (dry_deci_amps <= s_cts[c].run_deci_amps || dry_deci_amps >= s_cts[c].oc_deci_amps)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cts[c].dry_deci_amps = dry_deci_amps;
    esp_err_t err = store_u16(s_ct_keys[c], "d", dry_deci_amps);
    if (dry_deci_amps == 0) {
        ESP_LOGI(TAG, "%s CT: dry-run detection off", s_ct_keys[c]);
    } else {
        ESP_LOGI(TAG, "%s CT: dry below %u.%u A for %d s",
                 s_ct_keys[c], dry_deci_amps / 10, dry_deci_amps % 10, BORE_DRY_DEBOUNCE_S);
    }
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

uint16_t cal_plant_lph(void)
{
    return s_plant_lph;
}

esp_err_t cal_set_plant_lph(uint16_t lph)
{
    if (lph < PLANT_LPH_MIN || lph > PLANT_LPH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    s_plant_lph = lph;
    return store_u16("plant", "lph", lph);
}

uint32_t cal_runtime_get(cal_ct_t c)
{
    return (c < CAL_CT_COUNT) ? s_runtime_s[c] : 0;
}

esp_err_t cal_runtime_set(cal_ct_t c, uint32_t seconds)
{
    if (c >= CAL_CT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    s_runtime_s[c] = seconds;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char key[16];
    key_for(key, sizeof(key), s_ct_keys[c], "rt");
    err = nvs_set_u32(h, key, seconds);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

uint16_t cal_days(const cal_day_t **out)
{
    if (out) *out = s_days;
    return s_day_n;
}

esp_err_t cal_day_set(uint32_t midnight, uint16_t hpp_min, uint16_t rwp_min,
                      uint16_t bore_min, uint16_t smot_min)
{
    if (midnight == 0) {
        return ESP_ERR_INVALID_ARG;                 /* no clock, no day to book it to */
    }
    if (s_day_n > 0 && s_days[s_day_n - 1].midnight == midnight) {
        if (s_days[s_day_n - 1].hpp_min == hpp_min && s_days[s_day_n - 1].rwp_min == rwp_min &&
            s_days[s_day_n - 1].bore_min == bore_min && s_days[s_day_n - 1].smot_min == smot_min) {
            return ESP_OK;                          /* nothing moved, spare the flash */
        }
    } else {
        if (s_day_n == CAL_DAYS) {
            memmove(&s_days[0], &s_days[1], sizeof(cal_day_t) * (CAL_DAYS - 1));
            s_day_n--;
        }
        s_day_n++;
        s_days[s_day_n - 1].midnight = midnight;
    }
    s_days[s_day_n - 1].hpp_min  = hpp_min;
    s_days[s_day_n - 1].rwp_min  = rwp_min;
    s_days[s_day_n - 1].bore_min = bore_min;
    s_days[s_day_n - 1].smot_min = smot_min;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, "days2", s_days, sizeof(cal_day_t) * s_day_n);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

uint32_t cal_event_get(cal_event_t e)
{
    return (e < CAL_EVT_COUNT) ? s_evt[e] : 0;
}

esp_err_t cal_event_set(cal_event_t e, uint32_t epoch)
{
    if (e >= CAL_EVT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Callers pass 0 when the clock has not synchronised yet. Storing that would
     * overwrite a good stamp with "never", which is worse than not recording the
     * event at all - so refuse it here rather than trusting every caller. */
    if (epoch == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_evt[e] = epoch;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(h, s_evt_keys[e], epoch);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
