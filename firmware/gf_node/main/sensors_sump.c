/*
 * sensors_sump.c - the sump node's half of the shared sensors_* interface.
 *
 * Looks down a 3.5 m concrete manhole shaft: a ladder, a riser pipe and rough
 * walls all return an ultrasonic echo, and the borewell inflow foams the
 * surface while it runs. That geometry is exactly what the J-PRESS shunt
 * (gf.h GPIO_PRESS_FIT) exists to route around - read once at boot, never
 * polled again, because a shunt on a bench-tested board does not fall off.
 * Fitted, the node reads a 4-20 mA submersible transducer across the 100R
 * J-LOOP sense resistor instead of pinging.
 *
 * This is the ESP32 rebuild of firmware/ro_node/ro_node.ino's tank-level
 * code (its pingOnce/sampleTank and readPressureOnce/pressureMM). Same
 * rolling median, same blind-zone discard, same agreement-based quality,
 * same dead-cycle fault latch, same 4-20 mA live-zero reasoning - carried
 * over deliberately rather than re-derived, because every one of those
 * numbers came from a field trace on a real echo (ro_node.ino's US_DEBUG
 * history). Deviations from that file, and why, are called out inline.
 *
 * The hub owns calibration - a raw distance or raw microamp reading in,
 * a percentage out. This file reports neither a percentage nor a converted
 * head; that arithmetic does not belong on the node (spec 4.1, gf.h).
 */
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

#define BLIND_ZONE_MM   200      /* AJ-SR04M datasheet minimum; below it, an echo is the ladder or riser, never water */
#define US_TIMEOUT_US   35000    /* ~6 m round trip - ample for a 3.5 m shaft */
#define WINDOW          5        /* rolling median depth, one sample per cycle - matches ro_node.ino */
#define AGREE_MM        25       /* samples this close to the median count as good */
#define DEAD_CYCLES     10       /* consecutive no-echo cycles before NO_ECHO */
#define LOOP_SENSE_OHMS 100
#define LOOP_SAMPLES    8
#define PRESS_MIN_UA    3500     /* below 4 mA live-zero: a cut cable or dead supply, not an empty sump */
#define PRESS_MAX_UA    21000    /* above 20 mA: a shorted loop or miswire */

static bool     s_pressure;                  /* J-PRESS shunt read once at boot */
static uint16_t s_win[WINDOW]; static uint8_t s_n, s_next;
static uint16_t s_median_mm; static uint8_t s_quality; static uint8_t s_dead;
static uint32_t s_loop_ua;
static const char *s_status = "NO_ECHO";
static adc_oneshot_unit_handle_t s_adc; static adc_cali_handle_t s_cali; static bool s_cali_ok;

/* One ping, one credible echo or 0. Polled with esp_timer rather than
 * pulseIn() - the ESP32 has no Arduino pulseIn, and esp_timer_get_time() is a
 * free-running 64-bit microsecond counter available at task context, which is
 * everything the busy-wait loops below need. Unlike ro_node.ino's node this
 * one has no RS485 bus to go deaf for, so there is no equivalent of its
 * US_MAX_DISCARD/rs485.stopListening() dance - one pulse read is enough.
 *
 * Sub-blind-zone pulses are discarded here, not passed through: ro_node.ino's
 * hard-won lesson (see its BLIND_ZONE_MM comment) is that in this geometry
 * the first echo is very often the ladder or the riser pipe, not the water,
 * and a short reading that outvotes real samples in the median window is
 * worse than a missed cycle. */
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
    return mm >= BLIND_ZONE_MM ? mm : 0;   /* sub-blind-zone pulses are artefacts, not water */
}

/* Rolling median over the last WINDOW raw samples (0 = no echo that cycle,
 * excluded). A median rides out the single wild outlier these sensors throw
 * (foaming borewell inflow, a stray reflection); an average would not. */
static uint16_t median_push(uint16_t v)
{
    if (v) { s_win[s_next] = v; s_next = (s_next + 1) % WINDOW; if (s_n < WINDOW) s_n++; }
    if (s_n == 0) return 0;
    uint16_t t[WINDOW]; memcpy(t, s_win, sizeof t);
    for (uint8_t i = 1; i < s_n; i++) { uint16_t k = t[i]; int j = i - 1; while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; } t[j + 1] = k; }
    return t[s_n / 2];
}

/* How many of the window agree with the median, as a percent: the hub gates
 * a level under q60 (MIN_LEVEL_QUALITY), same rule as the RS485 tank nodes.
 * A single good reading among four timeouts scores low even though it agrees
 * with itself - agreement alone would call that q100, and it is not. */
static uint8_t quality_of(uint16_t med)
{
    if (s_n == 0) return 0;
    uint8_t agree = 0;
    for (uint8_t i = 0; i < s_n; i++) if ((s_win[i] > med ? s_win[i] - med : med - s_win[i]) <= AGREE_MM) agree++;
    return (uint8_t)((agree * 100) / WINDOW);
}

/* Averaged raw microamps across the 100R sense resistor. No live-zero check
 * here - read_loop_ua() only measures; sensors_sample() below is what turns
 * an out-of-range reading into HW_FAULT, same split ro_node.ino's
 * readPressureOnce()/pressureMM() draw between "read" and "judge". */
static uint32_t read_loop_ua(void)
{
    uint32_t sum = 0;
    for (int i = 0; i < LOOP_SAMPLES; i++) {
        int raw = 0, mv = 0;
        adc_oneshot_read(s_adc, ADC_CHANNEL_6, &raw);
        if (s_cali_ok && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) sum += (uint32_t)mv;
        else sum += (uint32_t)raw * 3300 / 4095;   /* uncalibrated fallback: 12-bit over a 3.3 V atten range */
        esp_rom_delay_us(500);
    }
    uint32_t mv = sum / LOOP_SAMPLES;
    return (mv * 1000UL) / LOOP_SENSE_OHMS;   /* 100R: 400 mV = 4000 uA, 2000 mV = 20000 uA */
}

void sensors_init(void)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << GPIO_US_TRIG, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    io.pin_bit_mask = 1ULL << GPIO_US_ECHO; io.mode = GPIO_MODE_INPUT; gpio_config(&io);
    io.pin_bit_mask = 1ULL << GPIO_PRESS_FIT; io.mode = GPIO_MODE_INPUT; io.pull_up_en = GPIO_PULLUP_ENABLE; gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(10));   /* let the pullup settle before trusting the level */
    s_pressure = gpio_get_level(GPIO_PRESS_FIT) == 0;

    /* The loop ADC channel is configured on every boot, fitted or not - same
     * as ro_node.ino leaves PIN_PRESS_SENSE unread rather than unconfigured.
     * Costs nothing on an ultrasonic-only board and means a J-PRESS shunt
     * added later needs no firmware change, only a reboot. */
    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &s_adc));
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_6, &c));   /* GPIO_LOOP_SENSE = GPIO 34 */
    adc_cali_line_fitting_config_t cc = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    s_cali_ok = adc_cali_create_scheme_line_fitting(&cc, &s_cali) == ESP_OK;
    ESP_LOGI(TAG, "source: %s", s_pressure ? "4-20 mA loop on GPIO 34 (J-PRESS shunted)" : "AJ-SR04M ultrasonic");
}

void sensors_sample(void)
{
    if (s_pressure) {
        s_loop_ua = read_loop_ua();
        /* Live-zero, same reasoning as ro_node.ino's pressureMM(): a 4-20 mA
         * loop cannot legitimately read outside 4-20 mA, so anything outside
         * that (with a little slack for sense-resistor tolerance) is a wiring
         * fault - a cut cable or dead supply reads near 0 uA, a short or
         * miswire reads high - never mistaken for a real, if extreme, level. */
        s_status = (s_loop_ua < PRESS_MIN_UA || s_loop_ua > PRESS_MAX_UA) ? "HW_FAULT" : "OK";
        return;
    }
    uint16_t raw = ping_once();
    s_dead = raw ? 0 : (s_dead < 255 ? s_dead + 1 : 255);
    s_median_mm = median_push(raw);
    s_quality = quality_of(s_median_mm);
    if (s_dead >= DEAD_CYCLES || s_n == 0) s_status = "NO_ECHO";   /* hardware fault: nothing credible in ten cycles */
    else if (s_median_mm < BLIND_ZONE_MM)  s_status = "BLIND";     /* echoing, but too close to trust - ladder/riser range */
    else                                    s_status = "OK";
}

int sensors_json(char *buf, size_t len)
{
    /* distance_mm/quality and loop_ua are mutually exclusive per the hub
     * contract (spec 4.1): whichever sensor is NOT fitted reports null, not
     * a stale or zero reading a percentage could be computed from by mistake. */
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
