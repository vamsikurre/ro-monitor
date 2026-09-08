"""Pin the borewell dry-run detectors in app_main.c.

Both are cheap to get subtly wrong in ways nothing would notice: a debounce
that counts cycles instead of milliseconds, a window scan that treats "no
clamp fitted" as "no current", or a yield check that forgets the sump motor
was pumping out the whole time and reports a healthy bore as dry. None of
those show up as a crash - they show up as an alarm that never fires, or one
that fires every afternoon until somebody stops believing it.

Same approach as check_gf.py: the two functions are lifted out of the firmware
between the BORE_DRY_BEGIN/END markers and built with gcc against stub types,
so what is tested is what ships.

    python docs/check_bore_dry.py       # from the repo root
"""
import os
import re
import subprocess
import sys
import tempfile

MAIN = 'firmware/hub_prod/main/app_main.c'
PRIV = 'firmware/hub_prod/main/app_priv.h'
BEGIN, END = '/* BORE_DRY_BEGIN', '/* BORE_DRY_END */'

# Only the fields and constants the two functions actually touch. Deliberately
# NOT the real hub_state_t: a stub that has to be widened when the real struct
# grows is a stub that stays honest about what these functions depend on.
HARNESS = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define ALERT_TANK_FULL_PCT   95
#define BORE_DRY_WINDOW_MIN   %(window)d
#define BORE_DRY_RISE_PCT     %(rise)d
#define BORE_DRY_DEBOUNCE_S   %(debounce)d

typedef enum { CAL_CT_HPP, CAL_CT_RWP, CAL_CT_BORE, CAL_CT_SUMP, CAL_CT_COUNT } cal_ct_t;
typedef struct { uint16_t run_deci_amps, dry_deci_amps; } cal_ct_cfg_t;
static cal_ct_cfg_t g_ct[CAL_CT_COUNT];
static const cal_ct_cfg_t *cal_ct(cal_ct_t c) { return &g_ct[c]; }

typedef struct { int16_t pct; } tank_t;
typedef struct { bool running; int16_t deci_amps; } gf_motor_t;
typedef struct {
    gf_motor_t borewell;
    tank_t     sump;
    bool       sump_online;
} hub_state_t;

typedef struct { int8_t sump; int16_t bore_da, smot_da; } hist_rec_t;
static hist_rec_t g_hist[256];
static uint16_t   g_hist_n;
static uint16_t history_count(void) { return g_hist_n; }
static const hist_rec_t *history_at(uint16_t i) { return &g_hist[i]; }

%(code)s

static int fails = 0;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL  %%s\n", #expr); fails++; } } while (0)

/* Fill the window with a healthy-and-dry pattern: bore running, sump motor
   off, level flat. Then each test perturbs exactly one thing. */
static void fill(int n, int8_t sump, int16_t bore_da, int16_t smot_da) {
    g_hist_n = n;
    for (int i = 0; i < n; i++) {
        g_hist[i].sump = sump; g_hist[i].bore_da = bore_da; g_hist[i].smot_da = smot_da;
    }
}
static hub_state_t live(int8_t sump_pct) {
    hub_state_t s = {0};
    s.borewell.running = true; s.borewell.deci_amps = 70;
    s.sump_online = true; s.sump.pct = sump_pct;
    return s;
}

int main(void) {
    g_ct[CAL_CT_BORE].run_deci_amps = 10;
    g_ct[CAL_CT_SUMP].run_deci_amps = 10;
    g_ct[CAL_CT_BORE].dry_deci_amps = 0;          /* off by default */

    /* ---- no-yield detector ---- */
    const int W = BORE_DRY_WINDOW_MIN;
    hub_state_t s = live(40);

    fill(W, 40, 120, 0);
    CHECK(bore_no_yield_now(&s) == true);          /* flat level, bore on, outlet shut */

    fill(W, 39, 120, 0);
    CHECK(bore_no_yield_now(&s) == false);         /* rose 1%%: yielding */

    /* The false positive this detector had to be built around: sump motor
       pumping out as fast as the bore fills leaves the level flat. */
    fill(W, 40, 120, 50);
    CHECK(bore_no_yield_now(&s) == false);

    fill(W, 40, 5, 0);
    CHECK(bore_no_yield_now(&s) == false);         /* bore was below run threshold */

    /* -1 is "no clamp fitted", which must void the window rather than read as
       zero current (bore) or as an idle outlet (sump motor). */
    fill(W, 40, -1, 0);
    CHECK(bore_no_yield_now(&s) == false);
    fill(W, 40, 120, -1);
    CHECK(bore_no_yield_now(&s) == false);

    fill(W - 1, 40, 120, 0);
    CHECK(bore_no_yield_now(&s) == false);         /* not enough history yet */

    fill(W, 40, 120, 0);
    s.sump.pct = ALERT_TANK_FULL_PCT - BORE_DRY_RISE_PCT;
    CHECK(bore_no_yield_now(&s) == false);         /* full: nowhere to rise */
    s.sump.pct = 40;

    s.sump_online = false;
    CHECK(bore_no_yield_now(&s) == false);
    s.sump_online = true;

    s.borewell.running = false;
    CHECK(bore_no_yield_now(&s) == false);
    s.borewell.running = true;

    fill(W, -1, 120, 0);
    CHECK(bore_no_yield_now(&s) == false);         /* no level at window start */

    /* ---- current detector ---- */
    uint32_t held = 0;
    hub_state_t d = live(40);
    d.borewell.deci_amps = 70;

    /* Threshold 0 means off, however low the current goes. */
    g_ct[CAL_CT_BORE].dry_deci_amps = 0;
    for (int i = 0; i < 100; i++) CHECK(bore_dry_by_amps(&d, 2000, &held) == false);
    CHECK(held == 0);

    /* Armed at 9.0 A, drawing 7.0 A: holds false until the debounce elapses. */
    g_ct[CAL_CT_BORE].dry_deci_amps = 90;
    held = 0;
    int ticks = BORE_DRY_DEBOUNCE_S * 1000 / 2000;
    for (int i = 0; i < ticks - 1; i++)
        CHECK(bore_dry_by_amps(&d, 2000, &held) == false);
    CHECK(bore_dry_by_amps(&d, 2000, &held) == true);      /* exactly at the debounce */
    CHECK(bore_dry_by_amps(&d, 2000, &held) == true);      /* and stays latched */

    /* One healthy reading resets the counter completely - no creeping up to a
       trip across unrelated dips, which is what a decaying counter would do. */
    d.borewell.deci_amps = 120;
    CHECK(bore_dry_by_amps(&d, 2000, &held) == false);
    CHECK(held == 0);
    d.borewell.deci_amps = 70;
    CHECK(bore_dry_by_amps(&d, 2000, &held) == false);     /* starts over */

    /* Motor off is not dry, and clears the counter: a stopped pump must not
       accumulate toward an alarm while it sits there. */
    held = 999999;
    d.borewell.running = false;
    CHECK(bore_dry_by_amps(&d, 2000, &held) == false);
    CHECK(held == 0);
    d.borewell.running = true;

    /* No clamp (-1) reads as "cannot judge", not as 0 A dry. */
    held = 0;
    d.borewell.deci_amps = -1;
    for (int i = 0; i < ticks + 5; i++) CHECK(bore_dry_by_amps(&d, 2000, &held) == false);
    d.borewell.deci_amps = 70;

    /* Debounce is measured in milliseconds, not cycles: one long cycle counts
       for what it really was. Trips in a single tick that spans the window. */
    held = 0;
    CHECK(bore_dry_by_amps(&d, BORE_DRY_DEBOUNCE_S * 1000, &held) == true);

    /* Saturation: a pump left dry must not wrap the counter and drop the flag. */
    held = UINT32_MAX - 10;
    CHECK(bore_dry_by_amps(&d, 2000, &held) == true);
    CHECK(held >= UINT32_MAX - 10);

    if (fails) { printf("\n%%d check(s) failed\n", fails); return 1; }
    printf("OK: borewell dry-run detectors behave\n");
    return 0;
}
'''


def slice_between(path, begin, end):
    text = open(path, encoding='utf-8').read()
    i = text.index(begin)
    j = text.index(end, i)
    return text[i:j]


def const(text, name):
    m = re.search(r'^#define\s+%s\s+(\d+)' % name, text, re.M)
    if not m:
        sys.exit('could not find #define %s in %s' % (name, PRIV))
    return int(m.group(1))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)

    code = slice_between(MAIN, BEGIN, END)
    # Drop the marker's own comment block; keep the functions.
    code = code[code.index('*/') + 2:]
    priv = open(PRIV, encoding='utf-8').read()

    src = HARNESS % {
        'code': code,
        'window': const(priv, 'BORE_DRY_WINDOW_MIN'),
        'rise': const(priv, 'BORE_DRY_RISE_PCT'),
        'debounce': const(priv, 'BORE_DRY_DEBOUNCE_S'),
    }

    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, 'bore_dry.c')
        exe = os.path.join(d, 'bore_dry.exe')
        with open(c, 'w', encoding='utf-8') as f:
            f.write(src)
        cc = subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra',
                             '-Wno-unused-function', '-o', exe, c],
                            capture_output=True, text=True)
        if cc.returncode != 0:
            print(cc.stdout + cc.stderr)
            sys.exit('the lifted code did not compile')
        if cc.stderr.strip():
            print(cc.stderr)
        run = subprocess.run([exe], capture_output=True, text=True)
        print(run.stdout + run.stderr, end='')
        sys.exit(run.returncode)


if __name__ == '__main__':
    main()
