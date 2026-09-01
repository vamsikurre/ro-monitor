/*
 * app_sensors.c — hub-local sensing
 *
 * ADC notes that matter on this board:
 *   - Every analog pin here is ADC1. ADC2 is unusable while Wi-Fi is up, which
 *     is why GPIO 4 (the dosing ECHO) is digital-only and why GPIO 36/39 were
 *     the last two pins available for the clamps.
 *   - 12 dB attenuation for the full 0-3.3 V span. The constant was ADC_ATTEN_DB_11
 *     before IDF 5.x; DB_12 is the same setting under the corrected name.
 *   - Readings go through the calibration scheme, so they are millivolts rather
 *     than raw counts. The ESP32 ADC is nonlinear enough that counts are not
 *     worth trusting across boards.
 */

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_sensors.h"

static const char *TAG = "sensors";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_sht30 = NULL;

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static bool s_cali_ok = false;

/* ------------------------------------------------------------------ ADC setup */

static adc_channel_t channel_for(int gpio)
{
    switch (gpio) {
        case 36: return ADC_CHANNEL_0;
        case 39: return ADC_CHANNEL_3;
        case 34: return ADC_CHANNEL_6;
        case 35: return ADC_CHANNEL_7;
        default: return ADC_CHANNEL_0;
    }
}

static uint32_t read_mv(int gpio)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc, channel_for(gpio), &raw) != ESP_OK) {
        return 0;
    }
    int mv = 0;
    if (s_cali_ok && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
        return (uint32_t)mv;
    }
    /* No calibration scheme burned into this chip: fall back to the nominal
     * full-scale ratio. Good enough for the threshold decisions here, and the
     * CT scale factor is measured against a clamp meter anyway. */
    return (uint32_t)((raw * 3300) / 4095);
}

/* Min and max millivolts across one full mains cycle. Both the AC opto channels
 * and the CT inputs ask the same question — not what level the pin sits at, but
 * how far it moves — so the sampling is shared and only the verdicts differ. */
static void sample_min_max_mv(int gpio, uint32_t *lo, uint32_t *hi)
{
    *lo = 5000;
    *hi = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)PROBE_WINDOW_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        uint32_t mv = read_mv(gpio);
        if (mv < *lo) *lo = mv;
        if (mv > *hi) *hi = mv;
    }
}

/* --------------------------------------------------------------------- init */

esp_err_t sensors_init(void)
{
    /* --- dry-contact optos: active LOW, pulled up internally --- */
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << GPIO_IN_TWT_FLOT) | (1ULL << GPIO_IN_RL1_STAT) |
                        (1ULL << GPIO_IN_RL2_STAT) | (1ULL << GPIO_IN_LPS),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));

    /* ALARM comes straight off the Aster's relay contact with no optocoupler and
     * no external parts, so it rides the internal pull-up like the opto channels
     * do — see the note in app_priv.h for what that costs and why it is accepted.
     * It is configured separately from them only to keep that reasoning attached
     * to the one input it applies to. If an external pull-up is ever fitted
     * (§6.6), this stays as it is: the two in parallel simply wet the contact
     * harder, which is the direction that helps. */
    gpio_config_t alarm_cfg = {
        .pin_bit_mask = 1ULL << GPIO_IN_ALARM,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&alarm_cfg));

    /* --- relays: de-energised before the pin becomes an output, or every reset
     *     clicks the whole bank. Float emulation is Phase C; nothing drives
     *     these yet, and "de-energised" is the safe state by design. --- */
    const int relays[] = { GPIO_RLY_TWT, GPIO_RLY_RWT, GPIO_RLY_DOS, GPIO_RLY_AUX };
    for (size_t i = 0; i < sizeof(relays) / sizeof(relays[0]); i++) {
        gpio_set_level(relays[i], RELAY_OFF);
        gpio_config_t r = {
            .pin_bit_mask = 1ULL << relays[i],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&r));
        gpio_set_level(relays[i], RELAY_OFF);
    }

    /* --- dosing ultrasonic --- */
    gpio_config_t trig = {
        .pin_bit_mask = 1ULL << GPIO_US_TRIG_DOS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&trig));
    gpio_set_level(GPIO_US_TRIG_DOS, 0);

    gpio_config_t echo = {
        .pin_bit_mask = 1ULL << GPIO_US_ECHO_DOS,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&echo));

    gpio_config_t led = {
        .pin_bit_mask = 1ULL << GPIO_LED_STATUS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led));

    /* --- I2C for the RO room SHT30 --- */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_I2C_SDA,
        .scl_io_num = GPIO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT30_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_sht30));

    /* --- ADC1 for the two AC opto channels and the two clamps --- */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    const int analog[] = { GPIO_IN_HPP_AC, GPIO_IN_RWP_AC, GPIO_IN_HPP_CT, GPIO_IN_RWP_CT };
    for (size_t i = 0; i < sizeof(analog) / sizeof(analog[0]); i++) {
        ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, channel_for(analog[i]), &chan_cfg));
    }

    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali) == ESP_OK) {
        s_cali_ok = true;
    } else {
        ESP_LOGW(TAG, "no ADC calibration scheme — falling back to the nominal ratio");
    }

    ESP_LOGI(TAG, "GPIO, I2C and ADC1 up");
    return ESP_OK;
}

/* -------------------------------------------------------------------- SHT30 */

static uint8_t sht30_crc8(uint8_t hi, uint8_t lo)
{
    uint8_t data[2] = { hi, lo };
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < 2; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

bool sht30_read(int16_t *temp_deci_c, uint16_t *hum_deci_pct)
{
    /* Single shot, high repeatability, clock stretching disabled. */
    const uint8_t cmd[2] = { 0x2C, 0x06 };
    if (i2c_master_transmit(s_sht30, cmd, sizeof(cmd), 100) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));   /* 15 ms conversion plus margin */

    uint8_t d[6];
    if (i2c_master_receive(s_sht30, d, sizeof(d), 100) != ESP_OK) {
        return false;
    }

    /* The sensor sends a CRC-8 per word, and this is the one place a corrupt
     * reading would switch a fan and raise an alarm. Check both. */
    if (sht30_crc8(d[0], d[1]) != d[2]) return false;
    if (sht30_crc8(d[3], d[4]) != d[5]) return false;

    uint16_t raw_t = ((uint16_t)d[0] << 8) | d[1];
    uint16_t raw_h = ((uint16_t)d[3] << 8) | d[4];

    /* T = -45 + 175 * raw / 65535, RH = 100 * raw / 65535, both in tenths.
     * Integer only — no float needed for two one-decimal values. */
    *temp_deci_c  = (int16_t)(-450 + (int16_t)(((int32_t)1750 * raw_t) / 65535L));
    *hum_deci_pct = (uint16_t)(((uint32_t)1000 * raw_h) / 65535UL);
    return true;
}

/* --------------------------------------------------------------- ultrasonic */

uint16_t median_u16_push(median_u16_t *m, uint16_t sample)
{
    m->v[m->next] = sample;
    m->next = (uint8_t)((m->next + 1) % SENSOR_WINDOW);
    if (m->n < SENSOR_WINDOW) {
        m->n++;
    }

    /* Zeros are "no reading", not "zero distance". Copy out only real samples. */
    uint16_t valid[SENSOR_WINDOW];
    uint8_t k = 0;
    for (uint8_t i = 0; i < m->n; i++) {
        if (m->v[i] > 0) {
            valid[k++] = m->v[i];
        }
    }
    if (k == 0) {
        return 0;
    }

    for (uint8_t i = 1; i < k; i++) {          /* insertion sort, k <= 5 */
        uint16_t x = valid[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && valid[j] > x) {
            valid[j + 1] = valid[j];
            j--;
        }
        valid[j + 1] = x;
    }
    return valid[k / 2];
}

uint16_t dosing_read_median_mm(void)
{
    static median_u16_t win;
    return median_u16_push(&win, dosing_read_mm());
}

uint16_t dosing_read_mm(void)
{
    gpio_set_level(GPIO_US_TRIG_DOS, 0);
    esp_rom_delay_us(4);
    gpio_set_level(GPIO_US_TRIG_DOS, 1);
    esp_rom_delay_us(US_TRIG_WIDTH_US);
    gpio_set_level(GPIO_US_TRIG_DOS, 0);

    /* Wait for the echo to rise, then time how long it stays high. Busy-wait
     * rather than an interrupt: the whole thing is bounded at 35 ms, it runs in
     * the idle part of the cycle, and an ISR here would buy nothing. */
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(GPIO_US_ECHO_DOS) == 0) {
        if (esp_timer_get_time() - start > US_TIMEOUT_US) {
            return 0;                      /* never rose: no echo */
        }
    }

    int64_t rise = esp_timer_get_time();
    while (gpio_get_level(GPIO_US_ECHO_DOS) == 1) {
        if (esp_timer_get_time() - rise > US_TIMEOUT_US) {
            return 0;                      /* stuck high: treat as no echo */
        }
    }
    int64_t width = esp_timer_get_time() - rise;

    return (uint16_t)((width * 10) / 58);  /* mm at ~343 m/s */
}

/* --------------------------------------------------------- dry-contact optos */

bool opto_twt_float_closed(void) { return gpio_get_level(GPIO_IN_TWT_FLOT) == 0; }
bool opto_rl1_active(void)       { return gpio_get_level(GPIO_IN_RL1_STAT) == 0; }
bool opto_rl2_active(void)       { return gpio_get_level(GPIO_IN_RL2_STAT) == 0; }
bool opto_lps_active(void)       { return gpio_get_level(GPIO_IN_LPS) == 0; }

/* ALARM has no optocoupler and sits on a 45k internal pull-up, which makes it the
 * highest-impedance input on the board and the one most exposed to a contactor
 * switching a metre away. Eight reads over ~1.6 ms, six must agree: ample to
 * reject coupled noise, and nowhere near slow enough to miss a real contact that
 * stays closed for minutes. The same 6-of-8 shape the bench sketch used for its
 * AC inputs, for the same reason. */
bool alarm_active(void)
{
    uint8_t low = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (gpio_get_level(GPIO_IN_ALARM) == 0) {
            low++;
        }
        esp_rom_delay_us(200);
    }
    return low >= 6;
}

/* ------------------------------------------------------------ 240 V channels */

void ac_probe(int gpio, bool *running, bool *floating, uint32_t *mv_lo, uint32_t *mv_hi)
{
    uint32_t lo, hi;
    sample_min_max_mv(gpio, &lo, &hi);

    if (mv_lo) *mv_lo = lo;
    if (mv_hi) *mv_hi = hi;

    /* Idle: the module's 47k pull-up holds both ends of the window high. */
    bool idle = (lo > AC_IDLE_ABOVE_MV);
    /* Active: the opto pulled the pin down at some point in the cycle. Using the
     * minimum is what makes this correct for a pulsing output as well as a
     * smoothed one. */
    bool active = (lo < AC_ACTIVE_BELOW_MV);

    if (running)  *running = active;
    /* Neither rail reached: nothing is driving the pin. VCC or OUT is open. */
    if (floating) *floating = (!idle && !active);
}

/* ---------------------------------------------------------- current clamps */

void ct_probe(int gpio, uint32_t *mv_lo, uint32_t *mv_hi)
{
    sample_min_max_mv(gpio, mv_lo, mv_hi);
}

int16_t ct_read_deci_amps(int gpio, cal_ct_t which)
{
    /* Refuse to report a current when the pedestal is not there. A floating pin
     * produces a large RMS figure that looks exactly like a running motor, and
     * that is the worst possible failure for a dry-run detector. */
    uint32_t lo, hi;
    sample_min_max_mv(gpio, &lo, &hi);
    uint32_t mid = (lo + hi) / 2;
    if (mid < CT_PEDESTAL_MIN_MV - 300 || mid > CT_PEDESTAL_MAX_MV + 300) {
        return -1;
    }

    /* Measure the offset rather than assuming 1650 mV: the divider's real centre
     * depends on resistor tolerance and on the clamp's own loading. */
    int64_t sum = 0;
    static int32_t samples[CT_RMS_SAMPLES];
    for (int i = 0; i < CT_RMS_SAMPLES; i++) {
        samples[i] = (int32_t)read_mv(gpio);
        sum += samples[i];
        esp_rom_delay_us(CT_RMS_INTERVAL_US);
    }
    int32_t mean = (int32_t)(sum / CT_RMS_SAMPLES);

    double acc = 0;
    for (int i = 0; i < CT_RMS_SAMPLES; i++) {
        double d = (double)(samples[i] - mean);
        acc += d * d;
    }
    double rms_mv = sqrt(acc / CT_RMS_SAMPLES);

    const cal_ct_cfg_t *cfg = cal_ct(which);
    /* amps = volts * (A/V) / turns. Everything scaled to stay in integers at the
     * boundary: rms_mv/1000 volts, amps_per_volt_x100/100. */
    double amps = (rms_mv / 1000.0) * ((double)cfg->amps_per_volt_x100 / 100.0) / (double)cfg->turns;

    if (amps < 0) amps = 0;
    if (amps > 999.0) amps = 999.0;

    /* A median across calls, on top of the RMS within one. The RMS already
     * rejects noise WITHIN a burst; it cannot reject a whole bad burst - a
     * contactor closing mid-window, a loose 3.5 mm plug, a floating pin drifting
     * through the pedestal band. One window per channel, hence the index. */
    static median_u16_t win[CAL_CT_COUNT];
    uint16_t deci = (uint16_t)(amps * 10.0 + 0.5);
    /* Nudge a genuine zero to 1 so it is not mistaken for "no reading" by the
     * median, which discards zeros. 0.1 A is below anything this measures. */
    if (deci == 0) {
        deci = 1;
    }
    return (int16_t)median_u16_push(&win[which < CAL_CT_COUNT ? which : 0], deci);
}
