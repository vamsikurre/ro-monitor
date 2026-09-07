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
#define GPIO_SUMP_CT_L1     35      /* ADC1_CH7 */
#define GPIO_SUMP_CT_L2     36      /* ADC1_CH0 */
#define GPIO_SUMP_CT_L3     39      /* ADC1_CH3 */
#define GPIO_PUMP_ON        25      /* Astero PUMP ON C/NO, closed = running */
#define GPIO_RWT_FLOTY      26      /* opto across the float loop, when wired */
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
