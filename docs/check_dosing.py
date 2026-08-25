"""Compile the hub's dosing level maths on the host and assert it behaves.

Extracts the DOS_* constants and dosingLevelPercent() verbatim out of
esp32_hub_test.ino -- so this tests the code that actually ships, not a copy --
wraps them in a main() of assertions, builds with gcc and runs it.

    python docs/check_dosing.py         # from the repo root
"""
import io
import os
import re
import subprocess
import sys
import tempfile

SKETCH = 'firmware/esp32_hub_test/esp32_hub_test.ino'

HARNESS = """
#include <stdint.h>
#include <stdio.h>

%s

static int failures = 0;

static void expect(uint16_t mm, int want) {
    int got = dosingLevelPercent(mm);
    if (got != want) {
        printf("dosingLevelPercent(%%u) = %%d, expected %%d\\n", mm, got, want);
        failures++;
    }
}

int main(void) {
    expect(0, 255);                        /* no echo */
    expect(DOS_FULL_MM - 1, 255);          /* blind zone / miscalibrated */
    expect(DOS_FULL_MM, 100);              /* full */
    expect(DOS_EMPTY_MM, 0);               /* empty */
    expect(DOS_EMPTY_MM + 500, 0);         /* past empty, still empty not negative */
    expect((DOS_FULL_MM + DOS_EMPTY_MM) / 2, 50);

    /* never inverts: more distance is never more chemical */
    int prev = 101;
    for (uint16_t mm = DOS_FULL_MM; mm <= DOS_EMPTY_MM; mm++) {
        int pct = dosingLevelPercent(mm);
        if (pct > prev) { printf("not monotonic at %%u mm\\n", mm); failures++; break; }
        if (pct > 100)  { printf("over 100%%%% at %%u mm\\n", mm); failures++; break; }
        prev = pct;
    }

    /* the low-chemical warning must be reachable before the tank is dry */
    if (dosingLevelPercent(DOS_EMPTY_MM - 1) >= DOS_LOW_PCT) {
        printf("level never drops below DOS_LOW_PCT before empty\\n");
        failures++;
    }

    if (failures) return 1;
    printf("OK: dosing level maths holds (%%d mm full .. %%d mm empty, low at %%d %%%%).\\n",
           DOS_FULL_MM, DOS_EMPTY_MM, DOS_LOW_PCT);
    return 0;
}
"""


def extract(text):
    """The DOS_* defines and dosingLevelPercent(), lifted out of the sketch."""
    defines = re.findall(r'^#define\s+DOS_[A-Z_]+\s+\d+.*$', text, re.M)
    assert len(defines) == 3, 'expected 3 DOS_* defines, found %d' % len(defines)

    start = text.index('uint8_t dosingLevelPercent(')
    end = text.index('\n}', start) + 2
    return '\n'.join(defines) + '\n\n' + text[start:end] + '\n'


def main():
    src = HARNESS % extract(io.open(SKETCH, encoding='utf-8').read())

    tmp = tempfile.mkdtemp()
    c_path = os.path.join(tmp, 'dosing.c')
    exe = os.path.join(tmp, 'dosing.exe')
    io.open(c_path, 'w', encoding='utf-8').write(src)

    build = subprocess.call(['gcc', '-Wall', '-Wextra', '-Werror', '-o', exe, c_path])
    if build != 0:
        print('gcc refused the extracted code -- that is the finding.')
        return build
    return subprocess.call([exe])


if __name__ == '__main__':
    sys.exit(main())
