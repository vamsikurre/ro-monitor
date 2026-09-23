"""Pin the two window picks in app_sensors.c.

One window type, two questions. The CT clamp filter asks which reading is
typical and wants the median; the dosing drum asks where the liquid is and wants
the farthest. Getting the dosing one wrong is not a cosmetic bug - it is what
happened on 2026-09-23, when the drum ran dry while the hub reported 93% for a
day and a half, because the median kept choosing a fixed object in the beam over
the liquid behind it (WIRING.md 13).

Three ways to break this, none of which crashes anything:

  - use the median for dosing and the obstruction wins again, the more surely
    the emptier the drum gets;
  - let a zero into either window and every figure is dragged toward the
    transducer, which reads as a FULLER tank than is really there;
  - change median_u16_push() while adding the second pick, and the motor current
    filter silently changes shape with it.

Same approach as check_bore_dry.py: the picks are lifted out of the firmware
between the WINDOW_PICK markers and built against a stub, so what is tested is
what ships.

    python docs/check_far_echo.py       # from the repo root
"""
import os
import re
import subprocess
import sys
import tempfile

SENSORS = 'firmware/hub_prod/main/app_sensors.c'
HEADER = 'firmware/hub_prod/main/app_sensors.h'
BEGIN, END = '/* WINDOW_PICK_BEGIN', '/* WINDOW_PICK_END */'

HARNESS = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define SENSOR_WINDOW %(window)d

typedef struct {
    uint16_t v[SENSOR_WINDOW];
    uint8_t  n;
    uint8_t  next;
} median_u16_t;

%(code)s

static int fails = 0;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL  line %%d: %%s\n", __LINE__, #expr); fails++; } } while (0)

/* The plant's own figures, 2026-09-23. OBSTRUCTION is the fixed thing in the
   cone; SURFACE_* are the true readings as the drum drained through the night
   and then the two-hour plant run. FULL is after it was refilled over the top
   of the obstruction, where the object cannot be seen at all. */
#define OBSTRUCTION 267
#define SURFACE_EVE 479
#define SURFACE_AM  511
#define FULL        240

static uint16_t feed_far(uint16_t *seq, int n) {
    median_u16_t w = {0};
    uint16_t out = 0;
    for (int i = 0; i < n; i++) out = far_u16_push(&w, seq[i]);
    return out;
}

int main(void) {
    /* ---- the failure this exists to stop ---- */
    /* One ping in four sees the liquid; the rest see the object. This is the
       real mix off the 24 h strip, and a median reports 93%% for a dry drum. */
    uint16_t mixed[] = { OBSTRUCTION, OBSTRUCTION, SURFACE_AM, OBSTRUCTION, OBSTRUCTION };
    CHECK(feed_far(mixed, 5) == SURFACE_AM);

    /* Even ONE sighting of the liquid in the whole window must win. Anything
       else and the guard fails exactly when the drum is worst placed to lose. */
    uint16_t once[] = { OBSTRUCTION, OBSTRUCTION, OBSTRUCTION, OBSTRUCTION, SURFACE_EVE };
    CHECK(feed_far(once, 5) == SURFACE_EVE);

    /* And the median really would have got it wrong - the thing being fixed. */
    {
        median_u16_t w = {0};
        uint16_t out = 0;
        for (int i = 0; i < 5; i++) out = median_u16_push(&w, mixed[i]);
        CHECK(out == OBSTRUCTION);
    }

    /* ---- a full drum hides the object, and must read full ---- */
    uint16_t full[] = { FULL, FULL, FULL, FULL, FULL };
    CHECK(feed_far(full, 5) == FULL);

    /* ---- draining: the window follows the liquid down, never back up ---- */
    /* The object stays in the beam the whole way and must never be reported. */
    {
        median_u16_t w = {0};
        uint16_t prev = 0;
        for (int i = 0; i < 60; i++) {
            uint16_t truth = (uint16_t)(300 + i * 3);
            uint16_t ping = (i %% 4 == 0) ? truth : OBSTRUCTION;
            uint16_t out = far_u16_push(&w, ping);
            CHECK(out != OBSTRUCTION || truth <= OBSTRUCTION);
            CHECK(out >= prev);            /* a draining drum never reads fuller */
            prev = out;
        }
        CHECK(prev >= 460);
    }

    /* ---- zeros are "no reading", not "zero distance" ---- */
    /* A zero must not win either pick, and must not evict the real samples: a
       window that reads 0 mm reports a tank brim-full, which is the direction
       that hides an empty drum. */
    uint16_t zeros[] = { SURFACE_AM, 0, 0, 0, 0 };
    CHECK(feed_far(zeros, 5) == SURFACE_AM);
    {
        median_u16_t w = {0};
        uint16_t out = 0;
        for (int i = 0; i < 5; i++) out = median_u16_push(&w, zeros[i]);
        CHECK(out == SURFACE_AM);
    }
    /* Nothing at all came back: both picks say so rather than inventing a zero. */
    uint16_t none[] = { 0, 0, 0, 0, 0 };
    CHECK(feed_far(none, 5) == 0);
    {
        median_u16_t w = {0};
        uint16_t out = 0;
        for (int i = 0; i < 5; i++) out = median_u16_push(&w, none[i]);
        CHECK(out == 0);
    }

    /* ---- a partly filled window works from the first sample ---- */
    /* There is no warm-up: the hub reports a level on its first cycle and it
       must be that sample, not a fifth of it. */
    {
        median_u16_t a = {0}, b = {0};
        CHECK(far_u16_push(&a, SURFACE_AM) == SURFACE_AM);
        CHECK(median_u16_push(&b, SURFACE_AM) == SURFACE_AM);
    }

    /* ---- the median is UNCHANGED: the CT clamp filter rides on it ---- */
    /* Five ascending samples, middle one. Plus the outlier rejection that is the
       whole reason the motor channels use a median at all. */
    {
        median_u16_t w = {0};
        uint16_t seq[] = { 10, 20, 30, 40, 50 }, out = 0;
        for (int i = 0; i < 5; i++) out = median_u16_push(&w, seq[i]);
        CHECK(out == 30);
    }
    {
        median_u16_t w = {0};
        uint16_t seq[] = { 30, 30, 9000, 30, 30 }, out = 0;
        for (int i = 0; i < 5; i++) out = median_u16_push(&w, seq[i]);
        CHECK(out == 30);                  /* one wild sample must not move it */
    }
    /* Even window contents (four valid, one zero) take the upper middle, which
       is what the old single-function version did. Pinned so the refactor that
       split the two picks apart cannot have shifted it. */
    {
        median_u16_t w = {0};
        uint16_t seq[] = { 10, 20, 30, 40, 0 }, out = 0;
        for (int i = 0; i < 5; i++) out = median_u16_push(&w, seq[i]);
        CHECK(out == 30);
    }

    /* ---- the ring wraps, and old samples leave ---- */
    /* SENSOR_WINDOW+1 pings of the truth must flush the object out entirely,
       so a drum refilled over the object stops reporting it within a window. */
    {
        median_u16_t w = {0};
        uint16_t out = 0;
        for (int i = 0; i < SENSOR_WINDOW; i++) out = far_u16_push(&w, SURFACE_AM);
        CHECK(out == SURFACE_AM);
        for (int i = 0; i < SENSOR_WINDOW; i++) out = far_u16_push(&w, FULL);
        CHECK(out == FULL);                /* the old far reading is gone */
    }

    if (fails) { printf("\n%%d check(s) failed\n", fails); return 1; }
    printf("OK: dosing takes the farthest echo, the CT median is unchanged\n");
    return 0;
}
'''


def slice_between(path, begin, end):
    text = open(path, encoding='utf-8').read()
    i = text.index(begin)
    j = text.index(end, i)
    return text[i:j]


def const(text, name, path):
    m = re.search(r'^#define\s+%s\s+(\d+)' % name, text, re.M)
    if not m:
        sys.exit('could not find #define %s in %s' % (name, path))
    return int(m.group(1))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)

    code = slice_between(SENSORS, BEGIN, END)
    # Drop the marker's own comment block; keep the functions.
    code = code[code.index('*/') + 2:]
    header = open(HEADER, encoding='utf-8').read()

    # The dosing path must actually USE the far pick. A checker that only proves
    # far_u16_push() is correct proves nothing if nothing calls it.
    main_c = open('firmware/hub_prod/main/app_main.c', encoding='utf-8').read()
    if 'far_u16_push(&s_dosing_win' not in main_c:
        sys.exit('app_main.c no longer feeds the dosing window to far_u16_push()')

    src = HARNESS % {
        'code': code,
        'window': const(header, 'SENSOR_WINDOW', HEADER),
    }

    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, 'far_echo.c')
        exe = os.path.join(d, 'far_echo.exe')
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
