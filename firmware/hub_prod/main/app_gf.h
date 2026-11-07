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
#include <stddef.h>
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
    /* Pump-control fields (node firmware with ctl). Absent keys parse as
     * float_low -1, bools false, flags 0, last_cmd "" - an un-reflashed node. */
    int8_t    float_low;           /* -1 null/unknown, 0 water ok, 1 low */
    bool      ctrl_node, bore_act, sump_act;
    uint16_t  flags;               /* GF_CTL_F_* */
    bool      relays[3];           /* bore_stop, bore_start, sump_stop */
    char      last_cmd[24];        /* "<src>:<motor>:<cmd>" or "" */
} gf_util_t;

/* Pump commands to the utility node. Values = the node's ctl.h enums
 * (ctl_motor_t, ctl_cmd_t, ctl_src_t); EVT_MOTOR_CMD packs them as they are. */
enum { GF_MOTOR_BORE = 0, GF_MOTOR_SUMP = 1 };
enum { GF_CMD_START = 0, GF_CMD_STOP = 1 };
enum { GF_SRC_AUTO = 0, GF_SRC_APP = 1, GF_SRC_DASH = 2 };
typedef struct { uint8_t motor, cmd, src; } gf_cmd_t;

/* What POST /api/ctl/cfg carries - the node's ctl_cfg_t, field for field. */
typedef struct {
    uint16_t bore_apv_x100, sump_apv_x100;
    uint8_t  bore_turns, sump_turns;
    uint16_t bore_run_da, bore_dry_da, sump_noload_da, max_run_min;
    bool     ctrl_node, bore_act, sump_act;
    bool     ctrl_node_set;   /* false: leave "ctrl_node" out, the node keeps its own */
} gf_ctl_cfg_t;

esp_err_t gf_init(void);
void      gf_snapshot(gf_sump_t *sump, gf_util_t *util);

/* Enqueue only (zero wait): gf_task does the HTTP. false if the queue is full
 * or gf_init() has not run. Safe from any task. */
bool gf_cmd_post(const gf_cmd_t *c);
/* A ground-floor control cal value changed: gf_task pushes /api/ctl/cfg after
 * its next good poll of the utility node, and again after each failed try. */
void gf_cfg_push(void);

/* Pure; host-tested by docs/check_gf.py. Parsers leave out->link untouched
 * except fw, rssi and uptime_s, and return false on anything malformed. */
bool gf_parse_sump(const char *json, gf_sump_t *out);
bool gf_parse_util(const char *json, gf_util_t *out);
void gf_link_result(gf_link_t *l, bool ok, int64_t now_us);
/* Request bodies for the node, compact JSON, token escaped. Return strlen, or
 * -1 on truncation, a NULL token, or a value the node would refuse (src auto). */
int  gf_ctl_body(const gf_cmd_t *c, const char *token, char *buf, size_t len);
int  gf_cfg_body(const gf_ctl_cfg_t *cfg, const char *token, char *buf, size_t len);
/* EVT_MOTOR_CMD's b: 0 for 200, 1-5 for a 409's why (single_phase, sump_hold,
 * not_settled, sump_full, no_float), 15 for anything else. */
int  gf_why_index(int status, const char *body);
/* POST /api/motor {"motor":"bore"|"sump","cmd":"start"|"stop"}. Sets motor and
 * cmd only, and only on success; src is the caller's. */
bool gf_parse_motor_req(const char *json, gf_cmd_t *out);

#ifdef __cplusplus
}
#endif
