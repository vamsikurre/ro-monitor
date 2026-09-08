/*
 * sensors_util.c - the utility node's half of the shared sensors_* interface.
 *
 * Sits in a motor starter panel and watches two three-phase motors that
 * expose two different kinds of signal: the borewell pump has no dry
 * contact at all, only its phase currents, so all three of its channels are
 * CT clamps; the sump motor's controller exposes a PUMP ON contact, so only
 * two of its three CT channels are fitted today and the third socket waits
 * on a clamp. It also reads the room's SHT30 and, once an optocoupler is
 * fitted, a roof-tank float loop.
 *
 * The hub owns every unit conversion (spec 4.1, gf.h): A/V, turns ratio, the
 * running threshold, the imbalance figure. This file reports raw millivolts
 * and raw contact/sensor state, nothing derived - any of that arithmetic
 * appearing here is a defect.
 *
 * Pedestal reasoning, carried over from firmware/hub_prod/main/app_sensors.c's
 * ct_read_deci_amps(): that function refuses to report a current at all when
 * the bias pedestal is not centred where a healthy divider should sit,
 * because a genuinely floating pin swings widely on mains coupling and
 * produces a large RMS that reads exactly like a running motor - the worst
 * failure for a dry-run detector. This file's rms_mv() mirrors that check
 * (min/max midpoint against a plausible band, PEDESTAL_MIN_MV/MAX_MV below)
 * rather than skipping it, even though the wire contract (spec 4.2) gives it
 * no null to report through: bore_mv/sump_mv must be plain integers
 * 0-65535, and the hub rejects the whole payload otherwise. It reports 0 mV
 * instead - not "0 A running," but the same "not measurable" the hub's own
 * noise floor already reads as "no clamp fitted" for an ordinary empty
 * socket (WIRING.md 11.3: an empty socket floats near 0 mV too, since the
 * bias rail reaches the pin only through the plugged clamp's own winding).
 * A false "borewell running" is not cosmetic here - it feeds the run-hour
 * ledger and would hide a borewell that is actually dead, which is worse
 * than reporting nothing.
 */
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

/* All six channels share one bias rail (WIRING.md 11.3): 10k/10k off 3V3,
 * same values and same 1650 mV nominal centre as the hub's own two CT
 * channels (app_priv.h's CT_PEDESTAL_MIN/MAX_MV) - it is the identical
 * divider, just fanned out to six sockets instead of two. +-100 mV covers
 * resistor tolerance and each channel's own ~1060 ohm series path (1k plus
 * the clamp winding's DC resistance, WIRING.md's bench figure); the outer
 * +-300 mV is the hub's own extra slack for whatever the bench hasn't
 * caught, carried over unchanged since nothing about fanning the same rail
 * out further tightens the tolerance. */
#define PEDESTAL_MIN_MV 1250    /* 1650 - 100 - 300 */
#define PEDESTAL_MAX_MV 2050    /* 1650 + 100 + 300 */

/* Channel order = header order = spec 4.3. Six channels at 200 ms each is
 * 1.2 s of sensors_sample() - inside the 2 s SAMPLE_PERIOD_MS (gf.h) with
 * 0.8 s left for the SHT30 (~20 ms) and the two GPIO reads, and well inside
 * the hub's 5 s poll either way. sensors_sample() runs in main.c's own
 * sample_task, a separate FreeRTOS task from the one serving httpd, both at
 * the same priority (5): the busy-wait in rms_mv() below never disables
 * interrupts or the scheduler, so even in the worst case of both tasks
 * landing on the same core the tick-driven round robin still lets a pending
 * /api/telemetry request run between esp_rom_delay_us() calls rather than
 * queuing behind the whole 1.2 s. */
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

/* RMS about the measured mean, so the bias pedestal cancels whatever its
 * exact value is - the divider's real centre depends on resistor tolerance
 * and on the clamp's own loading, same reasoning as the hub's
 * ct_read_deci_amps(). But that mean is only trustworthy to average about if
 * the pedestal is actually there: min/max are tracked in the same pass (no
 * second sampling window - six channels already spend 1.2 s of the 2 s
 * budget, see the comment below), and if their midpoint falls outside
 * PEDESTAL_MIN_MV/MAX_MV the channel is reported as 0 mV without ever
 * computing an RMS from it - see the header comment for why a wrong answer
 * here is worse than none. */
static uint16_t rms_mv(adc_channel_t ch)
{
    static int32_t s[RMS_SAMPLES];
    int64_t sum = 0;
    uint32_t lo = 5000, hi = 0;
    for (int i = 0; i < RMS_SAMPLES; i++) {
        uint32_t v = read_mv(ch);
        s[i] = (int32_t)v;
        sum += v;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        esp_rom_delay_us(RMS_INTERVAL_US);
    }
    uint32_t mid = (lo + hi) / 2;
    if (mid < PEDESTAL_MIN_MV || mid > PEDESTAL_MAX_MV) {
        return 0;
    }
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

/* Dead sensor (bad wiring, bad CRC, a NAK) reports sht_ok:false and leaves
 * s_t/s_rh at their last good value rather than zeroing them - 0.0 C / 0 %RH
 * both look like plausible readings, so the separate ok flag is what the hub
 * must gate on, not the numbers. */
static bool sht30_read(int16_t *t, uint16_t *rh)
{
    const uint8_t cmd[2] = { 0x2C, 0x06 };
    if (i2c_master_transmit(s_sht, cmd, 2, 100) != ESP_OK) return false;
    /* 40 ms, not 20. vTaskDelay() rounds DOWN to whole ticks and the first one
     * is already in progress, so 20 ms at the default 100 Hz tick is two ticks
     * that can elapse in as little as ~10 ms - against a high-repeatability
     * conversion the datasheet allows 15.5 ms for. The short read NAKs, the
     * poll reports sht_ok:false, and the hub hatches the climate card for one
     * cycle for no reason. Four ticks cannot finish early enough to matter. */
    vTaskDelay(pdMS_TO_TICKS(40));
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

    /* CONFIG_GF_RWT_FLOTY_WIRED is a Kconfig bool: OFF means the macro is
     * undefined, not zero, so it cannot appear in an ordinary C ternary here
     * (that would be an undeclared identifier in the default config) the way
     * it can inside a preprocessor #if. #ifdef for the log line, same as the
     * read in sensors_sample() below. */
#ifdef CONFIG_GF_RWT_FLOTY_WIRED
    ESP_LOGI(TAG, "6 ADC1 clamp channels, PUMP ON on %d, floaty wired, SHT30 0x44", GPIO_PUMP_ON);
#else
    ESP_LOGI(TAG, "6 ADC1 clamp channels, PUMP ON on %d, floaty not wired (null), SHT30 0x44", GPIO_PUMP_ON);
#endif
}

void sensors_sample(void)
{
    for (int i = 0; i < 6; i++) s_mv[i] = rms_mv(s_ch[i]);
    s_pump_on = gpio_get_level(GPIO_PUMP_ON) == 0;          /* contact closed pulls low */
#ifdef CONFIG_GF_RWT_FLOTY_WIRED
    s_floty = gpio_get_level(GPIO_RWT_FLOTY) == 0 ? 1 : 0;  /* opto on = float closed */
#else
    s_floty = -1;   /* optocoupler not fitted: cannot see the float at all, not "open" */
#endif
    s_sht_ok = sht30_read(&s_t, &s_rh);
}

int sensors_json(char *buf, size_t len)
{
    /* -1 (opto not fitted) prints null - "we cannot see it" - which is a
     * different fact from false ("we can see it and it is open"). Whichever
     * literal lands here comes from the branch above, never hardcoded, so
     * the checker's splice assertion (docs/check_gf_node.py) can hold this
     * to the same standard as sensors_sump.c's nullable fields. */
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
