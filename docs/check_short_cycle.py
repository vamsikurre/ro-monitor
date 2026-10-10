"""Pin hpp_short_cycling() in app_main.c.

Lifted from between the SHORT_CYCLE_BEGIN/END markers and built with gcc, so
what is tested is what ships. The 2026-10-09 pattern (on ~20 s, off ~30 s) must
trip within six starts; a normal run with a few flush drop-outs must not.

    python docs/check_short_cycle.py    # from the repo root
"""
import io
import os
import subprocess
import sys
import tempfile

MAIN = 'firmware/hub_prod/main/app_main.c'
PRIV = 'firmware/hub_prod/main/app_priv.h'
BEGIN, END = '/* SHORT_CYCLE_BEGIN', '/* SHORT_CYCLE_END */'

HARNESS = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
%s
%s
#define S 1000000LL
static int64_t t = S;   /* esp_timer is never 0 once the poll task runs */
/* Run `on` s, stop `off` s, `n` times, 2 s ticks. Returns true if it ever tripped. */
static bool cycle(int on, int off, int n) {
    bool hit = false;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < on;  k += 2, t += 2*S) hit |= hpp_short_cycling(true,  t);
        for (int k = 0; k < off; k += 2, t += 2*S) hit |= hpp_short_cycling(false, t);
    }
    return hit;
}
int main(void) {
    int bad = 0;
    /* Normal: a 2 h run with three 3-minute flush gaps. Four starts, never trips. */
    if (cycle(1800, 180, 4)) { puts("FAIL normal run tripped"); bad++; }
    /* Quiet for a while so the ring ages out. */
    if (cycle(0, 1200, 1))   { puts("FAIL idle tripped"); bad++; }
    /* Five fast starts is one short of the threshold. */
    if (cycle(20, 30, 5))    { puts("FAIL tripped on 5 starts"); bad++; }
    /* The sixth start trips it. */
    if (!cycle(20, 30, 1))   { puts("FAIL did not trip on 6th start"); bad++; }
    /* Still true mid-cycling, so the hourly reminder can fire. */
    if (!cycle(20, 30, 20))  { puts("FAIL dropped while still cycling"); bad++; }
    /* Fixed: one long run. Clears once the window passes the last fast start. */
    cycle(660, 0, 1);
    if (hpp_short_cycling(true, t)) { puts("FAIL did not clear after fix"); bad++; }
    /* Slow cycling, a start every 2.5 min: 6 starts span 12.5 min, no trip. */
    if (cycle(0, 1200, 1) || cycle(60, 90, 10)) { puts("FAIL slow cycling tripped"); bad++; }
    puts(bad ? "short-cycle: FAILED" : "short-cycle: ok");
    return bad != 0;
}
'''


def main():
    src = io.open(MAIN, encoding='utf-8').read()
    a, b = src.index(BEGIN), src.index(END)
    # The real thresholds, so a retune is tested too.
    defs = ''.join(l for l in io.open(PRIV, encoding='utf-8')
                     if l.startswith('#define SHORT_CYCLE_'))
    with tempfile.TemporaryDirectory() as d:
        c, exe = os.path.join(d, 't.c'), os.path.join(d, 't.exe')
        io.open(c, 'w', encoding='utf-8').write(HARNESS % (defs, src[a:b]))
        subprocess.check_call(['gcc', '-Wall', '-Werror', '-o', exe, c])
        return subprocess.call([exe])


if __name__ == '__main__':
    sys.exit(main())
