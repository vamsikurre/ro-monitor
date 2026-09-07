/*
 * app_gf.h — the ground-floor Wi-Fi nodes
 *
 * Two ESP32s on the house LAN, polled over plain HTTP at addresses typed on
 * /cal. This module owns the poll task and a snapshot of what each node last
 * said; the 2 s poll task in app_main.c copies the snapshot in each cycle and
 * applies the hub's calibration. Nodes report raw readings only.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     valid;                /* at least one good reply since boot */
    bool     online;
    uint8_t  misses;               /* consecutive failed polls */
    int64_t  last_ok_us;
    int64_t  next_poll_us;
    char     fw[16];
    int8_t   rssi;
    uint32_t uptime_s;
} gf_link_t;

typedef struct {
    gf_link_t       link;
    bool            pressure;      /* "source":"pressure" */
    uint16_t        distance_mm;   /* ultrasonic; 0 = none */
    uint8_t         quality;
    sensor_status_t sensor;
    uint32_t        loop_ua;       /* 4-20 mA loop; 0 = none */
} gf_sump_t;

typedef struct {
    gf_link_t link;
    uint16_t  bore_mv[3];          /* raw RMS mV per clamp channel */
    uint16_t  sump_mv[3];
    bool      sump_on;             /* Astero PUMP ON contact */
    int8_t    rwt_floty;           /* -1 null (not wired), 0 open, 1 closed */
    int16_t   temp_deci_c;
    uint16_t  hum_deci_pct;
    bool      sht_ok;
} gf_util_t;

esp_err_t gf_init(void);
void      gf_snapshot(gf_sump_t *sump, gf_util_t *util);

/* Pure; host-tested by docs/check_gf.py. Parsers leave out->link untouched
 * except fw, rssi and uptime_s, and return false on anything malformed. */
bool gf_parse_sump(const char *json, gf_sump_t *out);
bool gf_parse_util(const char *json, gf_util_t *out);
void gf_link_result(gf_link_t *l, bool ok, int64_t now_us);

#ifdef __cplusplus
}
#endif
