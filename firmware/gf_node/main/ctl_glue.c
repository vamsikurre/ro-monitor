/*
 * ctl_glue.c - ties the pure controller (ctl.c) to the node: a mutex, NVS,
 * the float input, the relays and the telemetry fields.
 *
 * sensors_sample() (sample task) calls ctl_glue_step(); the command path
 * (httpd task, Task 6) calls ctl_glue_command(). Both take s_mx.
 */
#include "gf.h"

#if CONFIG_GF_ROLE_UTILITY
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "ctl.h"

static const char *TAG = "ctl";
#define CFG_VER 1

static ctl_state_t s_st;
static ctl_cfg_t s_cfg;
static ctl_out_t s_out;
static SemaphoreHandle_t s_mx;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ponytail: one blob key and a version byte; a size/version mismatch just falls back to defaults. */
typedef struct { uint8_t ver; ctl_cfg_t cfg; } cfg_blob_t;

static const ctl_cfg_t CFG_DEFAULT = { .bore_apv_x100 = 3000, .sump_apv_x100 = 3000, .bore_turns = 1,
                                       .sump_turns = 1, .bore_run_da = 10 };

static void save_persist(void)
{
    ctl_persist_t p;  ctl_persist(&s_st, &p);
    nvs_handle_t h;
    if (nvs_open("ctl", NVS_READWRITE, &h) != ESP_OK) { ESP_LOGE(TAG, "nvs open"); return; }
    nvs_set_u8(h, "ustop_bore", p.user_stop_bore);
    nvs_set_u8(h, "ustop_sump", p.user_stop_sump);
    nvs_set_u8(h, "single_ph", p.single_phase);
    if (nvs_commit(h) != ESP_OK) ESP_LOGE(TAG, "nvs commit");
    nvs_close(h);
}

static void save_cfg(void)
{
    cfg_blob_t b = { .ver = CFG_VER, .cfg = s_cfg };
    nvs_handle_t h;
    if (nvs_open("ctl", NVS_READWRITE, &h) != ESP_OK) { ESP_LOGE(TAG, "nvs open"); return; }
    nvs_set_blob(h, "cfg", &b, sizeof b);
    if (nvs_commit(h) != ESP_OK) ESP_LOGE(TAG, "nvs commit");
    nvs_close(h);
}

void ctl_glue_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    configASSERT(s_mx);
    gpio_config_t io = { .pin_bit_mask = 1ULL << GPIO_SUMP_FLOAT, .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE };   /* external 10 k is fitted too */
    gpio_config(&io);

    ctl_persist_t p = {0};
    s_cfg = CFG_DEFAULT;
    nvs_handle_t h;
    if (nvs_open("ctl", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "ustop_bore", &v) == ESP_OK) p.user_stop_bore = v;
        if (nvs_get_u8(h, "ustop_sump", &v) == ESP_OK) p.user_stop_sump = v;
        if (nvs_get_u8(h, "single_ph", &v) == ESP_OK)  p.single_phase = v;
        cfg_blob_t b;  size_t n = sizeof b;
        if (nvs_get_blob(h, "cfg", &b, &n) == ESP_OK && n == sizeof b && b.ver == CFG_VER) s_cfg = b.cfg;
        nvs_close(h);
    }
    ctl_init(&s_st, &p, now_ms());
    ESP_LOGI(TAG, "ctrl_node=%d bore_act=%d sump_act=%d", s_cfg.ctrl_node, s_cfg.bore_act, s_cfg.sump_act);
}

/* The caller (sensors_sample) supplies the readings; the time and the raw
 * float are filled in here, where the config says whether the float is read. */
void ctl_glue_step(const ctl_in_t *in)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    ctl_in_t i = *in;
    i.now_ms = now_ms();
    i.float_low = !s_cfg.ctrl_node ? -1 : (gpio_get_level(GPIO_SUMP_FLOAT) == CONFIG_GF_FLOAT_LOW_LEVEL ? 1 : 0);
    ctl_step(&s_st, &s_cfg, &i, &s_out);
    relay_apply(&s_out);
    if (ctl_persist_changed(&s_st)) save_persist();
    xSemaphoreGive(s_mx);
}

ctl_result_t ctl_glue_command(ctl_motor_t m, ctl_cmd_t c, ctl_src_t src)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    ctl_result_t r = ctl_command(&s_st, &s_cfg, m, c, src, now_ms());
    if (ctl_persist_changed(&s_st)) save_persist();
    xSemaphoreGive(s_mx);
    return r;
}

void ctl_glue_cfg_set(const ctl_cfg_t *c)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_cfg = *c;
    save_cfg();
    xSemaphoreGive(s_mx);
}

void ctl_glue_cfg_get(ctl_cfg_t *c)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    *c = s_cfg;
    xSemaphoreGive(s_mx);
}

/* Starts with a comma: it is spliced after the last field of sensors_json(),
 * and docs/check_gf_node.py splices this format string the same way. */
int ctl_glue_json(char *buf, size_t len)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);     /* snapshot under the lock, format outside it */
    int8_t fl = ctl_float_low(&s_st);
    ctl_cfg_t c = s_cfg;  ctl_out_t o = s_out;
    unsigned flags = ctl_flags(&s_st);
    char last[sizeof s_st.last_cmd];
    strlcpy(last, ctl_last_cmd(&s_st), sizeof last);
    xSemaphoreGive(s_mx);
    return snprintf(buf, len,
        ",\"float_low\":%s,\"ctrl_node\":%s,\"bore_act\":%s,\"sump_act\":%s,\"flags\":%u,"
        "\"relays\":[%s,%s,%s],\"last_cmd\":\"%s\"",
        fl < 0 ? "null" : (fl ? "true" : "false"),
        c.ctrl_node ? "true" : "false", c.bore_act ? "true" : "false", c.sump_act ? "true" : "false", flags,
        o.bore_stop ? "true" : "false", o.bore_start ? "true" : "false", o.sump_stop ? "true" : "false", last);
}
#endif
