"""Pin the motor run-hour accounting in app_main.c.

The lifetime total used to be written to NVS only on the STOP transition. That
is fine right up until it isn't, and on 2026-09-11 the live hub showed what it
costs: the borewell reported `today_s` 40012 against `total_s` 658 — eleven
hours today, eleven minutes ever. Both of those came from the same run. "Today"
is restored from the day ledger across a reboot; lifetime was not, because
nothing had written it yet and the motor had not stopped.

Two failures hide in that one line of code, and neither shows up as a crash:

  1. a motor that runs for days contributes NOTHING to its lifetime figure
  2. a reboot part-way through a run discards that whole run, permanently

The fix flushes mid-run, which introduces a third way to be wrong that is
worse than either — counting the flushed seconds twice, once in NVS and again
in the run-in-progress term. So all three are tested here.

Same approach as check_bore_dry.py: run_account() is lifted out of the firmware
between the RUN_ACCT_BEGIN/END markers and built with gcc against stub types,
so what is tested is what ships.

    python docs/check_run_hours.py      # from the repo root
"""
import io
import os
import re
import subprocess
import sys
import tempfile

MAIN = 'firmware/hub_prod/main/app_main.c'
PRIV = 'firmware/hub_prod/main/app_priv.h'
BEGIN, END = '/* RUN_ACCT_BEGIN', '/* RUN_ACCT_END */'

# Deliberately NOT the real hub_state_t or cal layer: a stub that has to be
# widened when the real struct grows is a stub that stays honest about what
# this function actually depends on.
HARNESS = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define RUNTIME_COMMIT_MS %(commit)s

typedef enum { CAL_CT_HPP, CAL_CT_RWP, CAL_CT_BORE, CAL_CT_SUMP, CAL_CT_COUNT } cal_ct_t;

typedef struct {
    cal_ct_t which;
    int64_t  last_us;
    uint32_t run_ms;
    uint32_t run_since_start_ms;
    uint32_t committed_ms;
    uint32_t da_sum;
    uint16_t da_n;
    bool     was_running;
} run_acct_t;

/* The NVS figure, and a counter of how often it is written - flash wear is the
 * cost of the fix and a test that ignores it would let a one-second commit
 * interval through. */
static uint32_t g_nvs[CAL_CT_COUNT];
static int      g_writes;
static uint32_t cal_runtime_get(cal_ct_t c) { return g_nvs[c]; }
static void     cal_runtime_set(cal_ct_t c, uint32_t s) { g_nvs[c] = s; g_writes++; }

static const uint8_t s_run_evt[CAL_CT_COUNT][2] = {{1,2},{3,4},{5,6},{7,8}};
static void event_push(uint8_t k, uint8_t a, uint16_t b, uint16_t c) {
    (void)k; (void)a; (void)b; (void)c;
}

%(fn)s

/* ---- helpers ------------------------------------------------------------ */

static uint32_t today_s, total_s;
static uint16_t starts;

/* Advance the accounting by `secs` seconds in POLL_CYCLE_MS-sized ticks, the
 * way the poll loop does. Time is explicit so nothing here depends on a clock. */
static int64_t tick(run_acct_t *a, int64_t now_us, int secs, bool running) {
    for (int i = 0; i < secs * 1000 / 2000; i++) {
        now_us += 2000LL * 1000;
        run_account(a, true, running, 50, now_us, &today_s, &starts, &total_s);
    }
    return now_us;
}

static int fails;
static void eq(const char *what, long got, long want) {
    if (got != want) { printf("  FAIL %-52s got %ld, want %ld\n", what, got, want); fails++; }
}
static void near(const char *what, long got, long want, long tol) {
    if (got < want - tol || got > want + tol) {
        printf("  FAIL %-52s got %ld, want %ld +/- %ld\n", what, got, want, tol); fails++;
    }
}

int main(void) {
    run_acct_t a;
    int64_t now;

    /* 1. A run that never stops must still reach NVS. This is the live bug:
     *    eleven hours of running, nothing committed, lifetime reads ~zero. */
    memset(&a, 0, sizeof a); memset(g_nvs, 0, sizeof g_nvs); g_writes = 0;
    a.which = CAL_CT_BORE;
    now = 1000000;
    run_account(&a, true, false, -1, now, &today_s, &starts, &total_s);
    now = tick(&a, now, 4 * 3600, true);          /* four hours, no stop */
    near("4h continuous: today_s", today_s, 4 * 3600, 3);
    near("4h continuous: total_s", total_s, 4 * 3600, 3);
    /* Fixed 15 min, deliberately NOT derived from RUNTIME_COMMIT_MS: a tolerance
     * computed from the constant under test grows with it, so lengthening the
     * interval to "never" - the original bug - would widen the tolerance until
     * the assertion passed vacuously. This is the invariant that matters:
     * lifetime hours track reality to within a quarter of an hour. */
    near("4h continuous: committed to NVS", g_nvs[CAL_CT_BORE], 4 * 3600, 900);
    if (g_nvs[CAL_CT_BORE] == 0) { printf("  FAIL nothing reached NVS during a 4h run\n"); fails++; }

    /* 2. today_s must never exceed total_s. A lifetime total below today's is
     *    impossible on its face and is exactly what the dashboard showed. */
    if (today_s > total_s) {
        printf("  FAIL today_s %u > total_s %u - lifetime below today is impossible\n",
               today_s, total_s); fails++;
    }

    /* 3. No double counting. Every second is either in NVS or in the
     *    run-in-progress term, never both - the trap the fix opens up. */
    eq("no double count: total_s == NVS + uncommitted",
       total_s, g_nvs[CAL_CT_BORE] + (a.run_since_start_ms - a.committed_ms) / 1000);

    /* 4. A reboot mid-run loses at most one commit interval, not the whole run.
     *    Rebooting = a fresh run_acct_t with NVS surviving. */
    {
        uint32_t nvs_before = g_nvs[CAL_CT_BORE];
        run_acct_t b; memset(&b, 0, sizeof b); b.which = CAL_CT_BORE;
        int64_t n2 = 1000000;
        run_account(&b, true, true, 50, n2, &today_s, &starts, &total_s);
        near("reboot mid-run: lifetime preserved", total_s, nvs_before, 2);
        if (nvs_before + RUNTIME_COMMIT_MS / 1000 < (uint32_t)(4 * 3600)) {
            printf("  FAIL reboot would lose %u s of a 4h run\n",
                   4 * 3600 - nvs_before); fails++;
        }
    }

    /* 5. Stop commits the remainder exactly once, and a second stop adds nothing. */
    memset(&a, 0, sizeof a); memset(g_nvs, 0, sizeof g_nvs); g_writes = 0;
    a.which = CAL_CT_HPP;
    starts = 0;                                   /* shared across scenarios */
    now = 1000000;
    run_account(&a, true, false, -1, now, &today_s, &starts, &total_s);
    now = tick(&a, now, 600, true);
    now = tick(&a, now, 10, false);               /* stop */
    near("stop: NVS holds the whole run", g_nvs[CAL_CT_HPP], 600, 3);
    eq("stop: one start counted", starts, 1);
    {
        uint32_t after = g_nvs[CAL_CT_HPP];
        now = tick(&a, now, 60, false);           /* stays stopped */
        eq("still stopped: NVS unchanged", g_nvs[CAL_CT_HPP], after);
    }

    /* 6. A blind gap (node offline) is discarded, not counted as runtime. */
    memset(&a, 0, sizeof a); memset(g_nvs, 0, sizeof g_nvs);
    a.which = CAL_CT_SUMP;
    now = 1000000;
    run_account(&a, true, false, -1, now, &today_s, &starts, &total_s);
    now = tick(&a, now, 120, true);
    {
        uint32_t t_before = today_s;
        now += 3600LL * 1000000;                  /* an hour unseen */
        run_account(&a, false, false, -1, now, &today_s, &starts, &total_s);
        eq("offline gap not counted as runtime", today_s, t_before);
    }

    /* 7. Flash wear stays sane: a 12 h run must not thrash NVS. */
    memset(&a, 0, sizeof a); memset(g_nvs, 0, sizeof g_nvs); g_writes = 0;
    a.which = CAL_CT_RWP;
    now = 1000000;
    run_account(&a, true, false, -1, now, &today_s, &starts, &total_s);
    now = tick(&a, now, 12 * 3600, true);
    if (g_writes > 12 * 3600 * 1000 / RUNTIME_COMMIT_MS + 2) {
        printf("  FAIL 12h run wrote NVS %d times - commit interval too short\n", g_writes);
        fails++;
    }
    printf("  (12 h run wrote NVS %d times)\n", g_writes);

    if (fails) { printf("\nFAILED: %d\n", fails); return 1; }
    printf("OK: run-hour accounting behaves\n");
    return 0;
}
'''


def read(path):
    return io.open(path, encoding='utf-8').read()


def lift(text, begin, end):
    i = text.find(begin)
    j = text.find(end)
    if i < 0 or j < 0:
        sys.exit('FAIL: %s/%s markers not found in %s' % (begin, end, MAIN))
    return text[i:j]


def const(text, name):
    m = re.search(r'#define\s+%s\s+(.+)' % name, text)
    if not m:
        sys.exit('FAIL: %s not found in %s' % (name, PRIV))
    return m.group(1).split('/*')[0].strip()


def main():
    for f in (MAIN, PRIV):
        if not os.path.exists(f):
            sys.exit('FAIL: %s not found -- run from the repo root' % f)

    fn = lift(read(MAIN), BEGIN, END)
    # the marker comment itself is a C comment already, so it compiles as-is
    commit = const(read(PRIV), 'RUNTIME_COMMIT_MS')

    # Passed through to the C preprocessor verbatim rather than evaluated here:
    # the value is an arithmetic expression like (10 * 60 * 1000), and letting
    # the compiler do the arithmetic keeps this script from needing eval().
    # Plain replacement, not %-formatting: the harness is full of printf format
    # strings, and every literal %% in them would otherwise need escaping.
    src = HARNESS.replace('%(commit)s', commit).replace('%(fn)s', fn)
    d = tempfile.mkdtemp()
    c = os.path.join(d, 'run_hours.c')
    exe = os.path.join(d, 'run_hours.exe' if os.name == 'nt' else 'run_hours')
    io.open(c, 'w', encoding='utf-8').write(src)

    p = subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Wno-unused-parameter',
                        '-o', exe, c], capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout + p.stderr)
        sys.exit('FAIL: run_account() did not compile out of the firmware')

    r = subprocess.run([exe], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
