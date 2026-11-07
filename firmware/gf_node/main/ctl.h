/* Utility-node control decisions. Pure: no ESP-IDF, no clocks, no GPIO -- the
 * caller feeds readings and the time in, and drives the relays from `out`.
 * That is what lets docs/check_ctl.py build this file with plain gcc. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CT_NOISE_FLOOR_MV 15   /* under this a clamp channel is "no clamp" (same as the hub) */

typedef enum { CTL_BORE = 0, CTL_SUMP = 1 } ctl_motor_t;
typedef enum { CTL_CMD_START, CTL_CMD_STOP } ctl_cmd_t;
typedef enum { CTL_SRC_AUTO, CTL_SRC_APP, CTL_SRC_DASH } ctl_src_t;

/* flag bits - the hub mirrors these values (Task 7 pins them) */
#define CTL_F_START_FAILED   0x0001  /* commanded on, no current after retry */
#define CTL_F_SINGLE_PHASE   0x0002  /* latched */
#define CTL_F_BORE_DRY       0x0004  /* dry stop active (resting or latched) */
#define CTL_F_BORE_DRY_LATCH 0x0008  /* 3 attempts used, waits for a person */
#define CTL_F_MAX_RUN        0x0010  /* alert only, never stops */
#define CTL_F_SUMP_HOLD      0x0020  /* borewell failed + float low */
#define CTL_F_SUMP_DRY       0x0040  /* no-load current */
#define CTL_F_USER_STOP_BORE 0x0080
#define CTL_F_USER_STOP_SUMP 0x0100
#define CTL_F_NOT_SETTLED    0x0200
#define CTL_F_CAP_SUSPECT    0x0400  /* optional, Task 3 */

#define CTL_FLOAT_DEBOUNCE_MS 5000
#define CTL_START_VERIFY_MS   10000
#define CTL_STOP_PULSE_MS     3000
#define CTL_SETTLE_MS         30000
#define CTL_SINGLE_PHASE_MS   5000
#define CTL_DRY_MS            20000
#define CTL_DRY_REST_MS       (30LL * 60 * 1000)
#define CTL_DRY_OK_MS         (10LL * 60 * 1000)
#define CTL_DRY_ATTEMPTS      3
#define CTL_SUMP_RELEASE_MS   (5LL * 60 * 1000)

typedef struct {
    uint16_t bore_apv_x100, sump_apv_x100;  uint8_t bore_turns, sump_turns;
    uint16_t bore_run_da;      /* "drawing current" threshold, as hub cal run_deci_amps */
    uint16_t bore_dry_da;      /* 0 = dry check off */
    uint16_t sump_noload_da;   /* 0 = sump-dry check off */
    uint16_t max_run_min;      /* 0 = off */
    bool     ctrl_node;        /* false = NEC-49 in charge (default) */
    bool     bore_act, sump_act;   /* false = Alert only (default) */
} ctl_cfg_t;

typedef struct {
    int64_t  now_ms;
    int8_t   float_low;        /* raw: 1 low, 0 high, -1 not readable (ctrl_node false) */
    uint16_t bore_mv[3], sump_mv[3];
    bool     pump_on;          /* Astero PUMP ON closed */
} ctl_in_t;

typedef struct { bool bore_stop, bore_start, sump_stop; } ctl_out_t;  /* true = energise */
typedef struct { bool user_stop_bore, user_stop_sump, single_phase; } ctl_persist_t;

typedef enum { CTL_OK = 0, CTL_REFUSED_SINGLE_PHASE, CTL_REFUSED_SUMP_HOLD, CTL_REFUSED_NOT_SETTLED,
               CTL_REFUSED_SUMP_FULL } ctl_result_t;

struct ctl_state {
    uint16_t flags;
    int64_t  boot_ms;
    /* float debounce: raw value and when it last changed */
    int8_t   raw_float;  int64_t raw_since_ms;  int8_t float_db;
    /* boot settle */
    uint8_t  float_reads;  bool settled;
    /* borewell start verify: 0 idle, 1 first try in flight, 2 retry in flight */
    uint8_t  start_try;  int64_t start_deadline_ms;
    bool     stop_sent;  int64_t stop_until_ms;   /* one STOP pulse per float-high episode */
    bool     bore_running;
    /* borewell protections: timers use *_on as "condition currently holding" */
    bool     sp_on, dry_on, ok_on, run_on;
    int64_t  sp_since_ms, dry_since_ms, ok_since_ms, run_since_ms, rest_until_ms;
    uint8_t  dry_attempts;
    /* sump motor */
    bool     sc_on, sd_on, cap_on, cap_seen;
    int64_t  sc_since_ms, sd_since_ms, sd_until_ms, cap_since_ms;
    /* commands */
    bool     pend_start;   /* a person's borewell Start, emitted by the next step */
    bool     sp_now;       /* single-phasing measured on the last step */
    ctl_persist_t saved;   /* what ctl_persist() last handed out */
    char     last_cmd[24];
};
typedef struct ctl_state ctl_state_t;  /* opaque to callers by convention */

void     ctl_init(ctl_state_t *s, const ctl_persist_t *p, int64_t boot_ms);
void     ctl_step(ctl_state_t *s, const ctl_cfg_t *c, const ctl_in_t *in, ctl_out_t *out);
uint16_t ctl_flags(const ctl_state_t *s);
bool     ctl_bore_running(const ctl_state_t *s);   /* any phase >= bore_run_da */
int8_t   ctl_float_low(const ctl_state_t *s);      /* debounced: 1 low, 0 high, -1 not read */
int16_t  ctl_deci_amps(uint16_t mv, uint16_t apv_x100, uint8_t turns);  /* == hub gfPhaseDeciAmps */

/* Stop is always accepted. Start is refused (and changes nothing) before settle, while single-phasing
 * is measured now, while the sump is held (sump), or while the tank reads full (borewell, node mode). */
ctl_result_t ctl_command(ctl_state_t *s, const ctl_cfg_t *c, ctl_motor_t m, ctl_cmd_t cmd, ctl_src_t src, int64_t now_ms);
void        ctl_persist(const ctl_state_t *s, ctl_persist_t *p);   /* what to write to NVS */
bool        ctl_persist_changed(const ctl_state_t *s);             /* since last ctl_persist() */
const char *ctl_last_cmd(const ctl_state_t *s);                    /* "app:bore:start" etc.; "" before any */
