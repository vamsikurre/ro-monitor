"""Fail if the hub GPIO map disagrees between the documents and the firmware.

The hub pin map is written down in three places for three audiences: the wiring
guide, the hardware spec, and the phase-B design doc. They drifted once (GPIO
25/26/27 and 18/19/23 were double-assigned, see phase-B spec section 9). This
script is the thing that fails if that happens again.

It used to compare documents to documents ONLY, and that gap cost a day:
`IN_ALARM` on GPIO 33 sat in all three tables, passed this check, and was read
by no firmware at all. So the production firmware's own #defines are now a
fourth source. A pin that is documented but unread, or read but undocumented,
fails here.

NOT covered yet: node 0x06's pin map. As of 2026-08-27 it is written down twice
(WIRING.md 11.2 + 11.3 and HARDWARE.md 3.1's node-2 table) and validated nowhere,
which is the same shape as the gap that hid IN_ALARM. It is left out on purpose
rather than half-parsed out of prose bullets: node 0x06 has no firmware yet, and
the right time to add it here is when that firmware exists and can be the third
source. Six of its pins are freshly assigned CT channels, so this is a live risk,
not a theoretical one.

    python docs/check_pinmap.py        # from the repo root
"""
import io
import re
import sys

# The production firmware's pin map, as C #defines. Parsed as a fourth source
# alongside the three prose tables.
FIRMWARE = 'firmware/hub_prod/main/app_priv.h'

# #define GPIO_IN_TWT_FLOT        32   /* ... */    ->    (32, 'IN_TWT_FLOT')
# Only GPIO_* defines are pins; the rest of the header is protocol and thresholds.
FW_DEFINE = re.compile(r'^#define\s+GPIO_([A-Z0-9_]+)\s+(\d{1,2})\b')

# Firmware names are the signal names with the GPIO_ prefix stripped, except the
# relay outputs and the status LED, which the documents spell out in full.
FW_ALIASES = {
    'RLY_TWT': 'OUT_RLY_TWT',
    'RLY_RWT': 'OUT_RLY_RWT',
    'RLY_DOS': 'OUT_RLY_DOS',
    'RLY_AUX': 'OUT_RLY_AUX',
    'RS485_RX': 'RS485_RX',
    'RS485_TX': 'RS485_TX',
    'I2C_SDA': 'I2C_SDA',
    'I2C_SCL': 'I2C_SCL',
    'LED_STATUS': 'LED_STATUS',
    'BOOT_BUTTON': 'BTN_BOOT',
}

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


def firmware_pinmap(path):
    """{32: 'IN_TWT_FLOT', ...} from the production firmware's #defines."""
    out = {}
    for line in io.open(path, encoding='utf-8').read().splitlines():
        m = FW_DEFINE.match(line)
        if not m:
            continue
        name, gpio = m.group(1), int(m.group(2))
        signal = FW_ALIASES.get(name, name)
        assert gpio not in out, '%s: GPIO %d assigned twice' % (path, gpio)
        out[gpio] = signal
    assert out, 'no GPIO defines parsed from %s' % path
    return out


def main():
    maps = [(path, pinmap(path, start, end)) for path, start, end in TABLES]
    maps.append((FIRMWARE, firmware_pinmap(FIRMWARE)))
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
    print('OK: %d GPIOs agree across %d sources (%d documents + the firmware).'
          % (len(ref), len(maps), len(maps) - 1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
