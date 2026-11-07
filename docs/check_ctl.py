"""Pin the utility-node control logic in firmware/gf_node/main/ctl.c. The file is
pure C (no ESP-IDF), so the harness just links it with gcc -- what is tested is
what ships.

    python docs/check_ctl.py     # from the repo root
"""
import os, subprocess, sys, tempfile

CTL_DIR = 'firmware/gf_node/main'

HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include "ctl.h"
static int fails = 0;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL %s (line %d)\n", #expr, __LINE__); fails++; } } while (0)

static ctl_cfg_t cfg(bool node) {
    ctl_cfg_t c = { .bore_apv_x100 = 3000, .bore_turns = 1, .sump_apv_x100 = 3000, .sump_turns = 1,
                    .bore_run_da = 10, .ctrl_node = node };
    return c;
}
/* Harness rig: 1 s steps; bore clamp is 0.1 V (3.0 A) when running, else 0 mV like a real
 * idle power-up. Sump clamp is 0 unless a test sets it. */
typedef struct { ctl_state_t s; ctl_cfg_t c; int64_t t; int starts; bool stop_seen; ctl_out_t o; } rig_t;
static void rig(rig_t *r, bool node) {
    ctl_persist_t p = {0};
    r->c = cfg(node); r->t = 0; r->starts = 0; r->stop_seen = false;
    ctl_init(&r->s, &p, 0);
}
static void step(rig_t *r, int8_t fl, bool running) {
    ctl_in_t in = { .now_ms = r->t, .float_low = fl };
    in.bore_mv[0] = running ? 100 : 0;
    ctl_step(&r->s, &r->c, &in, &r->o);
    if (r->o.bore_start) r->starts++;
    if (r->o.bore_stop) r->stop_seen = true;
    r->t += 1000;
}
static void run_for(rig_t *r, int secs, int8_t fl, bool running) { for (int i = 0; i < secs; i++) step(r, fl, running); }

static void t_conversion_matches_hub(void) {
    CHECK(ctl_deci_amps(100, 3000, 1) == 30);
    CHECK(ctl_deci_amps(100, 3000, 2) == 15);
    CHECK(ctl_deci_amps(10, 3000, 1) == -1);
    CHECK(ctl_deci_amps(15, 3000, 1) == 4);
}
static void t_nec_mode_never_drives_float_logic(void) {
    rig_t r; rig(&r, false);
    run_for(&r, 60, 1, false);
    CHECK(r.starts == 0); CHECK(!r.stop_seen);
    CHECK(ctl_float_low(&r.s) == -1);
}
static void t_float_low_starts_after_settle(void) {
    rig_t r; rig(&r, true);
    run_for(&r, 30, 1, false);                 /* t = 0..29 s */
    CHECK(r.starts == 0); CHECK(ctl_flags(&r.s) & CTL_F_NOT_SETTLED);
    step(&r, 1, false);                        /* t = 30 s */
    CHECK(r.starts == 1); CHECK(r.o.bore_start);
    CHECK(!(ctl_flags(&r.s) & CTL_F_NOT_SETTLED));
    step(&r, 1, false);
    CHECK(r.starts == 1);                      /* an edge, not a level */
}
static void t_settle_with_all_clamps_idle_starts(void) {
    rig_t r; rig(&r, true);
    run_for(&r, 30, 1, false);
    CHECK(r.starts == 0);
    step(&r, 1, false);
    CHECK(r.starts == 1);
    run_for(&r, 5, 1, false);
    CHECK(r.starts == 1);
}
static void t_settle_needs_two_float_reads(void) {
    rig_t r; rig(&r, true);
    run_for(&r, 40, -1, false);                /* float unreadable: never settles */
    CHECK(ctl_flags(&r.s) & CTL_F_NOT_SETTLED); CHECK(r.starts == 0);
}
static void t_float_chatter_under_debounce_does_nothing(void) {
    rig_t r; rig(&r, true);
    run_for(&r, 40, 1, true);                  /* settled, float low, motor running */
    r.starts = 0;
    for (int i = 0; i < 30; i++) run_for(&r, 2, !(i & 1), true);   /* flips every 2 s */
    CHECK(r.starts == 0); CHECK(!r.stop_seen);
}
static void t_float_high_pulses_stop(void) {
    rig_t r; rig(&r, true);
    run_for(&r, 30, 1, true);                  /* settle with float low and running */
    int on = 0;
    for (int i = 0; i < 12; i++) { step(&r, 0, true); if (r.o.bore_stop) on++; }
    /* float high from t=30 s; debounced at 35 s, pulse 35,36,37 s */
    CHECK(on == 3);
    CHECK(!r.o.bore_stop);
}
static void t_no_current_retries_once_then_flags(void) {
    rig_t r; rig(&r, true);
    run_for(&r, 30, 1, false);
    step(&r, 1, false);                        /* 30 s: edge 1 */
    CHECK(r.starts == 1);
    run_for(&r, 9, 1, false);                  /* 31..39 s */
    CHECK(r.starts == 1);
    step(&r, 1, false);                        /* 40 s: edge 2 */
    CHECK(r.starts == 2);
    run_for(&r, 10, 1, false);                 /* 41..50 s: verify expires at 50 s */
    CHECK(ctl_flags(&r.s) & CTL_F_START_FAILED);
    run_for(&r, 600, 1, false);
    CHECK(r.starts == 2);
    run_for(&r, 6, 0, false);                  /* float high clears it */
    CHECK(!(ctl_flags(&r.s) & CTL_F_START_FAILED));
    run_for(&r, 6, 1, false);                  /* low again, debounced */
    CHECK(r.starts == 3);
}
static void t_current_cancels_retry(void) {
    rig_t r; rig(&r, true);
    run_for(&r, 31, 1, false);
    CHECK(r.starts == 1);
    run_for(&r, 30, 1, true);                  /* motor came up */
    CHECK(r.starts == 1); CHECK(!(ctl_flags(&r.s) & CTL_F_START_FAILED));
    CHECK(ctl_bore_running(&r.s));
}
static void t_zero_thresholds_disable_checks(void) {
    rig_t r; rig(&r, true);
    r.c.bore_dry_da = 0; r.c.sump_noload_da = 0; r.c.max_run_min = 0;
    run_for(&r, 3600, 1, true);
    CHECK(!(ctl_flags(&r.s) & (CTL_F_BORE_DRY | CTL_F_SUMP_DRY | CTL_F_MAX_RUN)));
}

/* Protection rig: settled node, 1 s steps, explicit raw clamp mV. 100 mV = 30 da, 17 mV = 5 da. */
static void stepmv(rig_t *r, int8_t fl, uint16_t a, uint16_t b, uint16_t c3) {
    ctl_in_t in = { .now_ms = r->t, .float_low = fl, .bore_mv = { a, b, c3 } };
    ctl_step(&r->s, &r->c, &in, &r->o);
    if (r->o.bore_start) r->starts++;
    if (r->o.bore_stop) r->stop_seen = true;
    r->t += 1000;
}
static void runmv(rig_t *r, int secs, int8_t fl, uint16_t a, uint16_t b, uint16_t c3) { for (int i = 0; i < secs; i++) stepmv(r, fl, a, b, c3); }
static void settled(rig_t *r) { rig(r, true); runmv(r, 31, 1, 100, 100, 100); r->starts = 0; r->stop_seen = false; }

static void t_single_phase_alert_only_does_not_stop(void) {
    rig_t r; settled(&r);
    runmv(&r, 6, 1, 200, 200, 17);
    CHECK(ctl_flags(&r.s) & CTL_F_SINGLE_PHASE); CHECK(!r.stop_seen);
}
static void t_single_phase_act_holds_stop_and_blocks_auto(void) {
    rig_t r; settled(&r);  r.c.bore_act = true;
    runmv(&r, 6, 1, 200, 200, 17);
    CHECK(ctl_flags(&r.s) & CTL_F_SINGLE_PHASE); CHECK(r.o.bore_stop);
    runmv(&r, 600, 1, 0, 0, 0);
    CHECK(r.o.bore_stop); CHECK(r.starts == 0);
}
static void t_missing_clamp_is_not_single_phasing(void) {
    rig_t r; settled(&r);
    runmv(&r, 60, 1, 200, 200, 0);
    runmv(&r, 60, 1, 200, 0, 0);
    runmv(&r, 60, 1, 0, 200, 0);
    CHECK(!(ctl_flags(&r.s) & CTL_F_SINGLE_PHASE));
}
static void t_lost_phase_with_clamp_present_is_single_phasing(void) {
    rig_t r; settled(&r);
    runmv(&r, 6, 1, 200, 200, 3);
    CHECK(ctl_flags(&r.s) & CTL_F_SINGLE_PHASE);
}
static void t_dry_retry_three_times_then_latch(void) {
    rig_t r; settled(&r);  r.c.bore_act = true; r.c.bore_dry_da = 20;
    for (int k = 1; k <= 3; k++) {
        runmv(&r, 20, 1, 34, 34, 34);                 /* 10 da: running but dry */
        CHECK(!(ctl_flags(&r.s) & CTL_F_BORE_DRY));
        stepmv(&r, 1, 34, 34, 34);                    /* 20 s */
        CHECK(ctl_flags(&r.s) & CTL_F_BORE_DRY); CHECK(r.o.bore_stop);
        if (k == 3) break;
        int before = r.starts;
        runmv(&r, 1790, 1, 0, 0, 0);                  /* resting, held */
        CHECK(r.o.bore_stop); CHECK(r.starts == before);
        runmv(&r, 11, 1, 0, 0, 0);                    /* rest over: the one START edge */
        CHECK(r.starts == before + 1);
        CHECK(!(ctl_flags(&r.s) & CTL_F_BORE_DRY));
    }
    CHECK(ctl_flags(&r.s) & CTL_F_BORE_DRY_LATCH);
    int before = r.starts;
    runmv(&r, 3 * 3600, 1, 0, 0, 0);
    CHECK(r.starts == before); CHECK(r.o.bore_stop);
}
static void t_max_run_flags_never_stops(void) {
    rig_t r; settled(&r);  r.c.max_run_min = 60; r.c.bore_act = true;
    runmv(&r, 60 * 60 - 40, 1, 200, 200, 200);
    CHECK(!(ctl_flags(&r.s) & CTL_F_MAX_RUN));
    runmv(&r, 100, 1, 200, 200, 200);
    CHECK(ctl_flags(&r.s) & CTL_F_MAX_RUN); CHECK(!r.stop_seen);
}

static void t_nec_mode_single_phase_act_holds_stop(void) {
    rig_t r; rig(&r, false);  r.c.bore_act = true;
    runmv(&r, 31, -1, 200, 200, 200);
    runmv(&r, 6, -1, 200, 200, 17);
    CHECK(ctl_flags(&r.s) & CTL_F_SINGLE_PHASE); CHECK(r.o.bore_stop);
    CHECK(!(ctl_flags(&r.s) & CTL_F_NOT_SETTLED));
}
static void t_nec_mode_dry_rest_ends_with_start(void) {
    rig_t r; rig(&r, false);  r.c.bore_act = true; r.c.bore_dry_da = 20;
    runmv(&r, 31, -1, 34, 34, 34);
    runmv(&r, 25, -1, 34, 34, 34);
    CHECK(ctl_flags(&r.s) & CTL_F_BORE_DRY); CHECK(r.o.bore_stop);
    runmv(&r, 1790, -1, 0, 0, 0);
    CHECK(r.o.bore_stop); CHECK(r.starts == 0);
    runmv(&r, 11, -1, 0, 0, 0);
    CHECK(r.starts == 1); CHECK(!r.o.bore_stop);
}
static void t_no_start_edge_while_stop_held(void) {
    rig_t r; settled(&r);  r.c.bore_act = true; r.c.bore_dry_da = 20;
    runmv(&r, 21, 1, 34, 34, 34);                       /* dry stop, rest begins */
    CHECK(ctl_flags(&r.s) & CTL_F_BORE_DRY);
    r.s.flags |= CTL_F_SINGLE_PHASE;                    /* latched during the rest */
    runmv(&r, 1900, 1, 0, 0, 0);
    CHECK(r.starts == 0); CHECK(r.o.bore_stop);
}
static void t_brief_recovery_does_not_reset_dry_attempts(void) {
    rig_t r; settled(&r);  r.c.bore_act = true; r.c.bore_dry_da = 20;
    for (int k = 1; k <= 3; k++) {
        runmv(&r, 21, 1, 34, 34, 34);
        CHECK(ctl_flags(&r.s) & CTL_F_BORE_DRY);
        if (k == 3) break;
        runmv(&r, 1801, 1, 0, 0, 0);                    /* rest, START edge */
        runmv(&r, 120, 1, 200, 200, 200);               /* 2 min healthy */
    }
    CHECK(ctl_flags(&r.s) & CTL_F_BORE_DRY_LATCH);
}
static void t_alert_only_dry_flag_follows_condition(void) {
    rig_t r; settled(&r);  r.c.bore_dry_da = 20;
    runmv(&r, 25, 1, 34, 34, 34);
    CHECK(ctl_flags(&r.s) & CTL_F_BORE_DRY); CHECK(!r.stop_seen);
    runmv(&r, 3, 1, 200, 200, 200);
    CHECK(!(ctl_flags(&r.s) & CTL_F_BORE_DRY));
    runmv(&r, 3000, 1, 34, 34, 34);
    CHECK(!(ctl_flags(&r.s) & CTL_F_BORE_DRY_LATCH)); CHECK(!r.stop_seen);
}

/* Sump rig: bore clamp a on [0], sump Common/Start on sump_mv[0]/[1]. 67 mV = 20 da, 100 mV = 30 da. */
static void stepsump(rig_t *r, int8_t fl, uint16_t bore, uint16_t common, uint16_t start, bool pump) {
    ctl_in_t in = { .now_ms = r->t, .float_low = fl, .bore_mv = { bore, bore, bore }, .sump_mv = { common, start, 0 }, .pump_on = pump };
    ctl_step(&r->s, &r->c, &in, &r->o);
    r->t += 1000;
}
static void runsump(rig_t *r, int secs, int8_t fl, uint16_t bore, uint16_t common, uint16_t start, bool pump) {
    for (int i = 0; i < secs; i++) stepsump(r, fl, bore, common, start, pump);
}
static void bore_failed(rig_t *r) { settled(r); runsump(r, 40, 1, 0, 0, 0, false); }   /* low float, no current: START_FAILED */

static void t_sump_hold_only_when_bore_failed_and_float_low(void) {
    rig_t r; bore_failed(&r);  r.c.sump_act = true;
    runsump(&r, 2, 1, 0, 0, 0, false);
    CHECK(ctl_flags(&r.s) & CTL_F_START_FAILED); CHECK(ctl_flags(&r.s) & CTL_F_SUMP_HOLD); CHECK(r.o.sump_stop);
    rig_t m; settled(&m);  m.c.sump_act = true; m.c.max_run_min = 1;
    runsump(&m, 70, 1, 100, 0, 0, false);
    CHECK(ctl_flags(&m.s) & CTL_F_MAX_RUN); CHECK(!(ctl_flags(&m.s) & CTL_F_SUMP_HOLD)); CHECK(!m.o.sump_stop);
}
static void t_sump_hold_releases_after_five_clear_minutes(void) {
    rig_t r; bore_failed(&r);  r.c.sump_act = true;
    runsump(&r, 2, 1, 0, 0, 0, false);
    CHECK(r.o.sump_stop);
    runsump(&r, 300, 1, 200, 0, 0, false);              /* borewell running: +0 .. +299 s */
    CHECK(r.o.sump_stop);
    stepsump(&r, 1, 200, 0, 0, false);                  /* +300 s */
    CHECK(!r.o.sump_stop); CHECK(!(ctl_flags(&r.s) & CTL_F_SUMP_HOLD));
}
static void t_sump_dry_needs_threshold_and_pump_on(void) {
    rig_t r; settled(&r);  r.c.sump_noload_da = 0;
    runsump(&r, 60, 1, 100, 67, 0, true);
    CHECK(!(ctl_flags(&r.s) & CTL_F_SUMP_DRY));
    rig_t a; settled(&a);  a.c.sump_noload_da = 30;
    runsump(&a, 20, 1, 100, 67, 0, true);
    CHECK(!(ctl_flags(&a.s) & CTL_F_SUMP_DRY));
    runsump(&a, 1, 1, 100, 67, 0, true);
    CHECK(ctl_flags(&a.s) & CTL_F_SUMP_DRY);
    rig_t b; settled(&b);  b.c.sump_noload_da = 30;
    runsump(&b, 60, 1, 100, 67, 0, false);
    CHECK(!(ctl_flags(&b.s) & CTL_F_SUMP_DRY));
}
static void t_sump_alert_only_never_energises(void) {
    rig_t r; bore_failed(&r);  r.c.sump_noload_da = 30;
    runsump(&r, 30, 1, 0, 67, 0, true);
    CHECK(ctl_flags(&r.s) & CTL_F_SUMP_HOLD); CHECK(ctl_flags(&r.s) & CTL_F_SUMP_DRY);
    CHECK(!r.o.sump_stop);
}
static void t_unplugged_common_clamp_is_not_sump_dry(void) {
    rig_t r; settled(&r);  r.c.sump_noload_da = 30; r.c.sump_act = true;
    runsump(&r, 60, 1, 100, 0, 0, true);
    CHECK(!(ctl_flags(&r.s) & CTL_F_SUMP_DRY)); CHECK(!r.o.sump_stop);
}
static void t_present_common_at_zero_amps_is_sump_dry(void) {
    rig_t r; settled(&r);  r.c.sump_noload_da = 30;
    runsump(&r, 21, 1, 100, 3, 0, true);
    CHECK(ctl_flags(&r.s) & CTL_F_SUMP_DRY);
}
static void t_sump_dry_stop_releases_after_five_minutes(void) {
    rig_t r; settled(&r);  r.c.sump_noload_da = 30; r.c.sump_act = true;
    runsump(&r, 21, 1, 100, 67, 0, true);               /* stop begins at the 21st step */
    CHECK(r.o.sump_stop);
    runsump(&r, 299, 1, 100, 0, 0, false);              /* stopped: pump off, held */
    CHECK(r.o.sump_stop);
    stepsump(&r, 1, 100, 0, 0, false);
    CHECK(!r.o.sump_stop); CHECK(!(ctl_flags(&r.s) & CTL_F_SUMP_DRY));
}
static void t_nec_mode_sump_dry_still_runs(void) {
    rig_t r; rig(&r, false);  r.c.sump_noload_da = 30; r.c.sump_act = true;
    runsump(&r, 31, -1, 100, 100, 0, false);
    runsump(&r, 21, -1, 100, 67, 0, true);
    CHECK(ctl_flags(&r.s) & CTL_F_SUMP_DRY); CHECK(r.o.sump_stop);
}
static void t_cap_suspect_alert_only(void) {
    rig_t r; settled(&r);  r.c.sump_noload_da = 30; r.c.sump_act = true;
    runsump(&r, 25, 1, 100, 100, 0, true);              /* Start never read: not suspect */
    CHECK(!(ctl_flags(&r.s) & CTL_F_CAP_SUSPECT));
    runsump(&r, 3, 1, 100, 100, 100, true);             /* Start reads once */
    runsump(&r, 25, 1, 100, 100, 0, true);              /* unplugged Start: not suspect */
    CHECK(!(ctl_flags(&r.s) & CTL_F_CAP_SUSPECT));
    runsump(&r, 25, 1, 100, 100, 3, true);              /* present at 0 A while Common is fine */
    CHECK(ctl_flags(&r.s) & CTL_F_CAP_SUSPECT); CHECK(!r.o.sump_stop);
    runsump(&r, 3, 1, 100, 100, 100, true);
    CHECK(!(ctl_flags(&r.s) & CTL_F_CAP_SUSPECT));
}

/* Commands, latches, persistence */
static void t_user_stop_holds_even_in_alert_only_and_nec_mode(void) {
    rig_t r; settled(&r);                                   /* node, Alert only */
    CHECK(ctl_command(&r.s, &r.c, CTL_BORE, CTL_CMD_STOP, CTL_SRC_APP, r.t) == CTL_OK);
    CHECK(ctl_command(&r.s, &r.c, CTL_SUMP, CTL_CMD_STOP, CTL_SRC_APP, r.t) == CTL_OK);
    runmv(&r, 60, 1, 0, 0, 0);
    CHECK(r.o.bore_stop); CHECK(r.o.sump_stop); CHECK(r.starts == 0);
    CHECK(ctl_flags(&r.s) & CTL_F_USER_STOP_BORE); CHECK(ctl_flags(&r.s) & CTL_F_USER_STOP_SUMP);
    rig_t n; rig(&n, false); runmv(&n, 31, -1, 100, 100, 100);   /* NEC, Alert only */
    CHECK(ctl_command(&n.s, &n.c, CTL_BORE, CTL_CMD_STOP, CTL_SRC_DASH, n.t) == CTL_OK);
    runmv(&n, 5, -1, 100, 100, 100);
    CHECK(n.o.bore_stop);
}
static void t_start_clears_user_stop_and_starts(void) {
    rig_t r; settled(&r);
    ctl_command(&r.s, &r.c, CTL_BORE, CTL_CMD_STOP, CTL_SRC_APP, r.t);
    runmv(&r, 5, 1, 0, 0, 0);
    CHECK(r.starts == 0);
    CHECK(ctl_command(&r.s, &r.c, CTL_BORE, CTL_CMD_START, CTL_SRC_APP, r.t) == CTL_OK);
    stepmv(&r, 1, 0, 0, 0);
    CHECK(r.starts == 1); CHECK(!r.o.bore_stop);
    CHECK(!(ctl_flags(&r.s) & CTL_F_USER_STOP_BORE));
    runmv(&r, 30, 1, 0, 0, 0);                              /* start verify: one retry, then failed */
    CHECK(r.starts == 2); CHECK(ctl_flags(&r.s) & CTL_F_START_FAILED);
    rig_t n; rig(&n, false); runmv(&n, 31, -1, 0, 0, 0);    /* NEC: one edge, same verify */
    CHECK(ctl_command(&n.s, &n.c, CTL_BORE, CTL_CMD_START, CTL_SRC_DASH, n.t) == CTL_OK);
    runmv(&n, 30, -1, 0, 0, 0);
    CHECK(n.starts == 2); CHECK(ctl_flags(&n.s) & CTL_F_START_FAILED);
}
static void t_start_refused_while_phase_still_missing(void) {
    rig_t r; settled(&r);                                   /* Alert only: motor keeps running on two phases */
    runmv(&r, 6, 1, 200, 200, 17);
    CHECK(ctl_flags(&r.s) & CTL_F_SINGLE_PHASE);
    CHECK(ctl_command(&r.s, &r.c, CTL_BORE, CTL_CMD_START, CTL_SRC_APP, r.t) == CTL_REFUSED_SINGLE_PHASE);
    CHECK(ctl_flags(&r.s) & CTL_F_SINGLE_PHASE); CHECK(ctl_last_cmd(&r.s)[0] == 0);
}
static void t_start_clears_single_phase_when_gone(void) {
    rig_t r; settled(&r);  r.c.bore_act = true;
    runmv(&r, 6, 1, 200, 200, 17);
    runmv(&r, 5, 1, 0, 0, 0);                               /* held off: cannot be measured */
    CHECK(r.o.bore_stop);
    CHECK(ctl_command(&r.s, &r.c, CTL_BORE, CTL_CMD_START, CTL_SRC_APP, r.t) == CTL_OK);
    CHECK(!(ctl_flags(&r.s) & CTL_F_SINGLE_PHASE));
    stepmv(&r, 1, 0, 0, 0);
    CHECK(r.starts == 1); CHECK(!r.o.bore_stop);
}
static void t_repeated_stop_is_idempotent(void) {
    rig_t r; settled(&r);  ctl_persist_t p;
    ctl_persist(&r.s, &p);  CHECK(!ctl_persist_changed(&r.s));
    ctl_command(&r.s, &r.c, CTL_SUMP, CTL_CMD_STOP, CTL_SRC_APP, r.t);
    CHECK(ctl_persist_changed(&r.s));
    ctl_persist(&r.s, &p);  CHECK(p.user_stop_sump); CHECK(!p.user_stop_bore);
    ctl_command(&r.s, &r.c, CTL_SUMP, CTL_CMD_STOP, CTL_SRC_APP, r.t);
    CHECK(!ctl_persist_changed(&r.s));
}
static void t_boot_restores_held_stops_never_starts(void) {
    ctl_persist_t p = { .user_stop_bore = true, .single_phase = true };
    rig_t r; r.c = cfg(true); r.c.bore_act = true; r.t = 0; r.starts = 0; r.stop_seen = false;
    ctl_init(&r.s, &p, 0);
    stepmv(&r, 1, 0, 0, 0);
    CHECK(r.o.bore_stop);                                   /* first step, before settle */
    runmv(&r, 600, 1, 0, 0, 0);
    CHECK(r.o.bore_stop); CHECK(r.starts == 0);
    CHECK(ctl_command(&r.s, &r.c, CTL_BORE, CTL_CMD_STOP, CTL_SRC_APP, r.t) == CTL_OK);
    CHECK(!ctl_persist_changed(&r.s));                      /* seeded: nothing new to write */
    ctl_persist_t q = { .single_phase = true };             /* latch alone holds only in Act */
    r.c.bore_act = false;  ctl_init(&r.s, &q, 0);  stepmv(&r, 1, 0, 0, 0);
    CHECK(!r.o.bore_stop);
}
static void t_last_cmd_names_source(void) {
    rig_t r; settled(&r);
    CHECK(ctl_last_cmd(&r.s)[0] == 0);
    ctl_command(&r.s, &r.c, CTL_BORE, CTL_CMD_START, CTL_SRC_APP, r.t);
    CHECK(!strcmp(ctl_last_cmd(&r.s), "app:bore:start"));
    ctl_command(&r.s, &r.c, CTL_SUMP, CTL_CMD_STOP, CTL_SRC_DASH, r.t);
    CHECK(!strcmp(ctl_last_cmd(&r.s), "dash:sump:stop"));
}
static void t_bore_start_refused_when_sump_full(void) {
    rig_t r; settled(&r);
    runmv(&r, 10, 0, 100, 100, 100);                        /* float high, debounced */
    CHECK(ctl_command(&r.s, &r.c, CTL_BORE, CTL_CMD_START, CTL_SRC_APP, r.t) == CTL_REFUSED_SUMP_FULL);
}
static void t_start_refused_before_settle_stop_accepted(void) {
    rig_t r; rig(&r, true);  runmv(&r, 5, 1, 0, 0, 0);
    CHECK(ctl_command(&r.s, &r.c, CTL_BORE, CTL_CMD_START, CTL_SRC_APP, r.t) == CTL_REFUSED_NOT_SETTLED);
    CHECK(ctl_command(&r.s, &r.c, CTL_SUMP, CTL_CMD_START, CTL_SRC_APP, r.t) == CTL_REFUSED_NOT_SETTLED);
    CHECK(ctl_command(&r.s, &r.c, CTL_BORE, CTL_CMD_STOP, CTL_SRC_APP, r.t) == CTL_OK);
    stepmv(&r, 1, 0, 0, 0);
    CHECK(r.o.bore_stop);
}
static void t_sump_start_releases_stop_and_dry_but_not_hold(void) {
    rig_t r; settled(&r);  r.c.sump_noload_da = 30; r.c.sump_act = true;
    runsump(&r, 25, 1, 100, 5, 0, true);                    /* present common at ~0 A: dry stop */
    CHECK(ctl_flags(&r.s) & CTL_F_SUMP_DRY); CHECK(r.o.sump_stop);
    CHECK(ctl_command(&r.s, &r.c, CTL_SUMP, CTL_CMD_START, CTL_SRC_APP, r.t) == CTL_OK);
    CHECK(!(ctl_flags(&r.s) & CTL_F_SUMP_DRY));
    stepsump(&r, 1, 100, 5, 0, true);  CHECK(!r.o.sump_stop);
    rig_t h; bore_failed(&h);                               /* borewell failed + float low */
    runsump(&h, 1, 1, 0, 0, 0, false);
    CHECK(ctl_flags(&h.s) & CTL_F_SUMP_HOLD);
    CHECK(ctl_command(&h.s, &h.c, CTL_SUMP, CTL_CMD_START, CTL_SRC_APP, h.t) == CTL_REFUSED_SUMP_HOLD);
}
static void t_bore_start_clears_dry_latch_and_failed(void) {
    rig_t r; bore_failed(&r);
    CHECK(ctl_flags(&r.s) & CTL_F_START_FAILED);
    r.s.flags |= CTL_F_BORE_DRY | CTL_F_BORE_DRY_LATCH;  r.s.dry_attempts = 3;
    CHECK(ctl_command(&r.s, &r.c, CTL_BORE, CTL_CMD_START, CTL_SRC_DASH, r.t) == CTL_OK);
    CHECK(!(ctl_flags(&r.s) & (CTL_F_START_FAILED | CTL_F_BORE_DRY | CTL_F_BORE_DRY_LATCH)));
    CHECK(r.s.dry_attempts == 0);
}

static void t_invalid_command_changes_nothing(void) {
    rig_t r; settled(&r);
    ctl_command(&r.s, &r.c, CTL_SUMP, CTL_CMD_STOP, CTL_SRC_APP, r.t);
    ctl_state_t before = r.s;
    CHECK(ctl_command(&r.s, &r.c, CTL_BORE, (ctl_cmd_t)7, CTL_SRC_APP, r.t) == CTL_REFUSED_INVALID);
    CHECK(ctl_command(&r.s, &r.c, (ctl_motor_t)5, CTL_CMD_START, CTL_SRC_APP, r.t) == CTL_REFUSED_INVALID);
    CHECK(ctl_command(&r.s, &r.c, CTL_BORE, CTL_CMD_START, (ctl_src_t)9, r.t) == CTL_REFUSED_INVALID);
    CHECK(!memcmp(&before, &r.s, sizeof before));
    runmv(&r, 60, 1, 100, 100, 100);
    CHECK(r.starts == 0);
}
static void t_pending_start_dropped_if_float_goes_high(void) {
    rig_t r; settled(&r);
    runmv(&r, 5, 0, 100, 100, 100);                               /* motor running so no auto start; raw high 4 s: still debounced low */
    CHECK(ctl_float_low(&r.s) == 1);
    CHECK(ctl_command(&r.s, &r.c, CTL_BORE, CTL_CMD_START, CTL_SRC_APP, r.t) == CTL_OK);
    stepmv(&r, 0, 100, 100, 100);                                 /* debounce flips to high on this step */
    CHECK(ctl_float_low(&r.s) == 0); CHECK(r.starts == 0);
    runmv(&r, 20, 0, 100, 100, 100);  CHECK(r.starts == 0);
}
static void t_bore_start_refused_without_float_in_node_mode(void) {
    rig_t r; settled(&r);
    runmv(&r, 10, -1, 0, 0, 0);
    CHECK(ctl_float_low(&r.s) == -1);
    CHECK(ctl_command(&r.s, &r.c, CTL_BORE, CTL_CMD_START, CTL_SRC_APP, r.t) == CTL_REFUSED_NO_FLOAT);
    rig_t n; rig(&n, false); runmv(&n, 31, -1, 0, 0, 0);
    CHECK(ctl_command(&n.s, &n.c, CTL_BORE, CTL_CMD_START, CTL_SRC_APP, n.t) == CTL_OK);
}

int main(void) {
    t_conversion_matches_hub();
    t_nec_mode_never_drives_float_logic();
    t_float_low_starts_after_settle();
    t_settle_with_all_clamps_idle_starts();
    t_settle_needs_two_float_reads();
    t_float_chatter_under_debounce_does_nothing();
    t_float_high_pulses_stop();
    t_no_current_retries_once_then_flags();
    t_current_cancels_retry();
    t_zero_thresholds_disable_checks();
    t_single_phase_alert_only_does_not_stop();
    t_single_phase_act_holds_stop_and_blocks_auto();
    t_missing_clamp_is_not_single_phasing();
    t_lost_phase_with_clamp_present_is_single_phasing();
    t_dry_retry_three_times_then_latch();
    t_max_run_flags_never_stops();
    t_nec_mode_single_phase_act_holds_stop();
    t_nec_mode_dry_rest_ends_with_start();
    t_no_start_edge_while_stop_held();
    t_brief_recovery_does_not_reset_dry_attempts();
    t_alert_only_dry_flag_follows_condition();
    t_sump_hold_only_when_bore_failed_and_float_low();
    t_sump_hold_releases_after_five_clear_minutes();
    t_sump_dry_needs_threshold_and_pump_on();
    t_sump_alert_only_never_energises();
    t_sump_dry_stop_releases_after_five_minutes();
    t_unplugged_common_clamp_is_not_sump_dry();
    t_present_common_at_zero_amps_is_sump_dry();
    t_nec_mode_sump_dry_still_runs();
    t_cap_suspect_alert_only();
    t_user_stop_holds_even_in_alert_only_and_nec_mode();
    t_start_clears_user_stop_and_starts();
    t_start_refused_while_phase_still_missing();
    t_start_clears_single_phase_when_gone();
    t_repeated_stop_is_idempotent();
    t_boot_restores_held_stops_never_starts();
    t_last_cmd_names_source();
    t_bore_start_refused_when_sump_full();
    t_start_refused_before_settle_stop_accepted();
    t_sump_start_releases_stop_and_dry_but_not_hold();
    t_bore_start_clears_dry_latch_and_failed();
    t_invalid_command_changes_nothing();
    t_pending_start_dropped_if_float_goes_high();
    t_bore_start_refused_without_float_in_node_mode();
    printf(fails ? "check_ctl: %d FAILED\n" : "check_ctl: OK\n", fails);
    return fails ? 1 : 0;
}
'''

def main():
    d = tempfile.mkdtemp()
    c_path, exe = os.path.join(d, 'harness.c'), os.path.join(d, 'harness.exe')
    with open(c_path, 'w', encoding='utf-8') as f:
        f.write(HARNESS)
    if subprocess.call(['gcc', '-Wall', '-Wextra', '-Werror', '-I', CTL_DIR, '-o', exe,
                        c_path, os.path.join(CTL_DIR, 'ctl.c')]) != 0:
        print('gcc refused ctl.c or the harness.'); return 1
    return subprocess.call([exe])

sys.exit(main())
