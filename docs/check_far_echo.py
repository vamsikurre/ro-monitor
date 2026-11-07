"""Pin the median window in app_sensors.c - the CT clamp filter rides on it.

The dosing drum used a farthest-echo pick from the same window until the
ultrasonic was replaced by a point sensor (WIRING.md 13.2). The median checks
stay: zeros must not count, one wild sample must not move it, and the ring must
wrap. The code is lifted between the WINDOW_PICK markers and built against a
stub, so what is tested is what ships.

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

int main(void) {
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

    /* Zeros are "no reading": they never count, and all-zero says so. */
    {
        median_u16_t w = {0};
        uint16_t seq[] = { 511, 0, 0, 0, 0 }, out = 0;
        for (int i = 0; i < 5; i++) out = median_u16_push(&w, seq[i]);
        CHECK(out == 511);
        median_u16_t z = {0};
        for (int i = 0; i < 5; i++) out = median_u16_push(&z, 0);
        CHECK(out == 0);
    }
    /* No warm-up: the first sample is the answer. */
    {
        median_u16_t w = {0};
        CHECK(median_u16_push(&w, 511) == 511);
    }

    /* ---- the ring wraps, and old samples leave ---- */
    {
        median_u16_t w = {0};
        uint16_t out = 0;
        for (int i = 0; i < SENSOR_WINDOW; i++) out = median_u16_push(&w, 511);
        for (int i = 0; i < SENSOR_WINDOW; i++) out = median_u16_push(&w, 240);
        CHECK(out == 240);                 /* the old window is gone */
    }

    if (fails) { printf("\n%%d check(s) failed\n", fails); return 1; }
    printf("OK: the CT median window behaves\n");
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
