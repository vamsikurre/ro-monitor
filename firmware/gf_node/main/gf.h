/*
 * gf.h - the one shared header for the ground-floor node firmware.
 *
 * One binary, two roles (CONFIG_GF_ROLE_SUMP / CONFIG_GF_ROLE_UTILITY, picked
 * at menuconfig time), so this is where the role settles into a node id, a
 * name for logs, and which GPIOs matter. Everything else - net.c, web.c,
 * ota.c, main.c, and the sensors_*.c pair - talks to each other only through
 * the functions declared here.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "sdkconfig.h"
#include "ctl.h"

#if CONFIG_GF_ROLE_SUMP
#define GF_NODE_ID          5
#define GF_ROLE_NAME        "sump"
#else
#define GF_NODE_ID          6
#define GF_ROLE_NAME        "utility"
#endif

/* GPIO, spec 4.3. Sump node. */
#define GPIO_US_TRIG        5
#define GPIO_US_ECHO        18      /* through 1 k / 2 k divider */
#define GPIO_LOOP_SENSE     34      /* ADC1_CH6, J-LOOP pin 2 */
#define GPIO_PRESS_FIT      25      /* J-PRESS shunt to GND = pressure */
/* Utility node. */
#define GPIO_BORE_CT_L1     32      /* ADC1_CH4 */
#define GPIO_BORE_CT_L2     33      /* ADC1_CH5 */
#define GPIO_BORE_CT_L3     34      /* ADC1_CH6 */
/* The sump motor is single-phase 3-wire, so these are windings, not phases
 * (WIRING.md 11.3): L1 = Common (panel B), L2 = Start, L3 = Run. */
#define GPIO_SUMP_CT_L1     35      /* ADC1_CH7 - Common */
#define GPIO_SUMP_CT_L2     36      /* ADC1_CH0 - Start */
#define GPIO_SUMP_CT_L3     39      /* ADC1_CH3 - Run */
#define GPIO_PUMP_ON        25      /* Astero PUMP ON C/NO, closed = running */
#define GPIO_SUMP_FLOAT     26      /* PC817 output, low = opto on = float loop closed */
#define GPIO_RLY_BORE_START 16      /* high-trigger, 10 k pull-down; 1 s pulse */
#define GPIO_RLY_BORE_STOP  17
#define GPIO_RLY_SUMP_STOP  18
#define GPIO_I2C_SDA        21
#define GPIO_I2C_SCL        22

#define SAMPLE_PERIOD_MS    2000
#define OTA_CONFIRM_MS      120000  /* must be valid before this or roll back */

esp_err_t net_start(void);
bool      net_up(void);
int       net_rssi(void);

esp_err_t web_start(void);
bool      web_served_once(void);      /* a /api/telemetry has been answered */

esp_err_t ota_handle(httpd_req_t *req);

void sensors_init(void);
void sensors_sample(void);            /* one cycle; called from main every SAMPLE_PERIOD_MS */
int  sensors_json(char *buf, size_t len);   /* the whole telemetry object */

/* Utility node control (relay.c, ctl_glue.c). */
void relay_init(void);                       /* first thing in app_main, before NVS */
void relay_apply(const ctl_out_t *o);        /* levels + 1 s START pulse on a rising bore_start */
void ctl_glue_init(void);                    /* NVS load (namespace "ctl"), ctl_init; needs NVS up */
void ctl_glue_step(const ctl_in_t *in);      /* fills now_ms and float_low itself, then ctl_step + relays */
ctl_result_t ctl_glue_command(ctl_motor_t m, ctl_cmd_t c, ctl_src_t src);
void ctl_glue_cfg_set(const ctl_cfg_t *c);   /* replaces and saves the config */
void ctl_glue_cfg_get(ctl_cfg_t *c);
int  ctl_glue_json(char *buf, size_t len);   /* ",\"float_low\":..." fields spliced into sensors_json() */
