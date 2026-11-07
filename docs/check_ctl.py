"""Pin the utility-node control logic in firmware/gf_node/main/ctl.c. The file is
pure C (no ESP-IDF), so the harness just links it with gcc -- what is tested is
what ships.

    python docs/check_ctl.py     # from the repo root
"""
import os, subprocess, sys, tempfile

CTL_DIR = 'firmware/gf_node/main'

HARNESS = r'''
#include <stdio.h>
#include "ctl.h"
static int fails = 0;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL %s (line %d)\n", #expr, __LINE__); fails++; } } while (0)

static ctl_cfg_t cfg(bool node) {
    ctl_cfg_t c = { .bore_apv_x100 = 3000, .bore_turns = 1, .sump_apv_x100 = 3000, .sump_turns = 1,
                    .bore_run_da = 10, .ctrl_node = node };
    return c;
}
/* Harness rig: 1 s steps; bore current is 0.1 V (3.0 A, running) or 20 mV (0.6 A, idle).
 * The sump clamp always reads something so the boot settle sees a clamp. */
typedef struct { ctl_state_t s; ctl_cfg_t c; int64_t t; int starts; bool stop_seen; ctl_out_t o; } rig_t;
static void rig(rig_t *r, bool node) {
    ctl_persist_t p = {0};
    r->c = cfg(node); r->t = 0; r->starts = 0; r->stop_seen = false;
    ctl_init(&r->s, &p, 0);
}
static void step(rig_t *r, int8_t fl, bool running) {
    ctl_in_t in = { .now_ms = r->t, .float_low = fl };
    in.bore_mv[0] = running ? 100 : 20;  in.sump_mv[0] = 50;
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

int main(void) {
    t_conversion_matches_hub();
    t_nec_mode_never_drives_float_logic();
    t_float_low_starts_after_settle();
    t_settle_needs_two_float_reads();
    t_float_chatter_under_debounce_does_nothing();
    t_float_high_pulses_stop();
    t_no_current_retries_once_then_flags();
    t_current_cancels_retry();
    t_zero_thresholds_disable_checks();
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
