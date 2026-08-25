"""Fail if the three ESP32 hub GPIO tables ever disagree again.

The hub pin map is written down in three places for three audiences: the wiring
guide, the hardware spec, and the phase-B design doc. They drifted once (GPIO
25/26/27 and 18/19/23 were double-assigned, see phase-B spec section 9). This
script is the thing that fails if that happens again.

    python docs/check_pinmap.py        # from the repo root
"""
import io
import re
import sys

# (path, start heading, end heading)
TABLES = [
    ('docs/WIRING.md', '## 1. ESP32-S Central Master Hub', '## 2.'),
    ('docs/HARDWARE.md', '### 3.1. ESP32-S Central Hub', '### 3.2.'),
    ('docs/superpowers/specs/2026-08-25-ro-monitor-phase-b-design.md',
     '### 3.5 ESP32 hub GPIO map', '### 3.6'),
]

# | **GPIO 32** | `IN_TWT_FLOT`| ...     and     | 32 | `IN_TWT_FLOT` | ...
ROW = re.compile(r'^\|\s*\*{0,2}(?:GPIO\s*)?(\d{1,2})\*{0,2}\s*\|\s*`([A-Z0-9_]+)`')


def section(path, start, end):
    text = io.open(path, encoding='utf-8').read()
    i = text.index(start)
    j = text.index(end, i)
    return text[i:j]


def pinmap(path, start, end):
    out = {}
    for line in section(path, start, end).splitlines():
        m = ROW.match(line)
        if not m:
            continue
        gpio, signal = int(m.group(1)), m.group(2)
        assert gpio not in out, '%s: GPIO %d assigned twice' % (path, gpio)
        out[gpio] = signal
    assert out, 'no rows parsed from %s' % path
    return out


def main():
    maps = [(path, pinmap(path, start, end)) for path, start, end in TABLES]
    ref_path, ref = maps[0]
    bad = 0
    for path, m in maps[1:]:
        for gpio in sorted(set(ref) | set(m)):
            a, b = ref.get(gpio), m.get(gpio)
            if a != b:
                print('GPIO %-2d  %s: %-14s  %s: %s'
                      % (gpio, ref_path, a or '-', path, b or '-'))
                bad += 1
    if bad:
        print('\n%d mismatch(es). WIRING.md section 1 is authoritative '
              '(it matches the built hub) - correct the others to it.' % bad)
        return 1
    print('OK: %d GPIOs agree across %d documents.' % (len(ref), len(maps)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
