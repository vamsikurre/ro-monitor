"""Pin the ground-floor pure maths in app_cal.c: loop current to distance,
clamp millivolts to deci-amps, and phase imbalance. Same approach as
check_frame.py - the code is lifted out of the firmware and built with gcc,
so what is tested is what ships.

    python docs/check_gf.py     # from the repo root
"""
import io, os, subprocess, sys, tempfile

CAL = 'firmware/hub_prod/main/app_cal.c'
BEGIN, END = '/* GF_PURE_BEGIN */', '/* GF_PURE_END */'

HARNESS = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#define CT_NOISE_FLOOR_MV 15
#define PRESS_MIN_UA 3500
#define PRESS_MAX_UA 21000
%s
static int fails = 0;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL %%s\n", #expr); fails++; } } while (0)
int main(void) {
    /* loop: 4 mA = full range (empty tank), 20 mA = 0 mm head... distance-alike */
    CHECK(gfLoopDistanceMM(4000, 2000) == 2000);
    CHECK(gfLoopDistanceMM(20000, 2000) == 1);      /* clamped to range-1 head */
    CHECK(gfLoopDistanceMM(12000, 2000) == 1000);
    CHECK(gfLoopDistanceMM(3400, 2000) == 0);       /* open loop */
    CHECK(gfLoopDistanceMM(21500, 2000) == 0);      /* shorted */
    CHECK(gfLoopDistanceMM(12000, 0) == 0);         /* ultrasonic tank, no range */
    /* clamp: 30 A/V, 1 turn: 100 mV rms = 3.0 A */
    CHECK(gfPhaseDeciAmps(100, 3000, 1) == 30);
    CHECK(gfPhaseDeciAmps(100, 3000, 2) == 15);
    CHECK(gfPhaseDeciAmps(10, 3000, 1) == -1);      /* under the noise floor: no clamp */
    CHECK(gfPhaseDeciAmps(15, 3000, 1) == 4);       /* exactly the floor counts */
    int16_t a[3] = { 40, 40, 40 }; CHECK(gfImbalancePct(a) == 0);
    int16_t b[3] = { 40, 30, -1 }; CHECK(gfImbalancePct(b) == 25);
    int16_t c[3] = { 40, -1, -1 }; CHECK(gfImbalancePct(c) == 0);
    int16_t d[3] = { -1, -1, -1 }; CHECK(gfImbalancePct(d) == 0);
    printf(fails ? "check_gf: %%d FAILED\n" : "check_gf: OK\n", fails);
    return fails ? 1 : 0;
}
'''

def main():
    src = io.open(CAL, encoding='utf-8').read()
    if BEGIN not in src or END not in src:
        print('markers %s / %s not found in %s' % (BEGIN, END, CAL)); return 1
    block = src[src.index(BEGIN) + len(BEGIN):src.index(END)]
    d = tempfile.mkdtemp()
    c_path, exe = os.path.join(d, 'gf.c'), os.path.join(d, 'gf.exe')
    io.open(c_path, 'w', encoding='utf-8').write(HARNESS % block)
    if subprocess.call(['gcc', '-Wall', '-Wextra', '-Werror', '-o', exe, c_path]) != 0:
        print('gcc refused the extracted code -- that is the finding.'); return 1
    return subprocess.call([exe])

if __name__ == '__main__':
    sys.exit(main())
