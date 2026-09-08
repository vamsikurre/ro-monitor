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

The ground-floor nodes were the same gap until 2026-09-08: 0x05 and 0x06 had pin
maps in HARDWARE.md 3.4 / 3.5 and in WIRING.md 11, validated by nothing, and six
of 0x06's pins are freshly assigned CT channels. firmware/gf_node/main/gf.h now
exists, so it is a fourth firmware source and those two tables are cross-checked
against it the way the hub's three are.

gf.h holds both roles in one header and the roles reuse pins - GPIO 34 is the
sump's loop sense and the borewell's L3, GPIO 25 is J-PRESS and PUMP ON - so it
is split at its "Utility node." comment and each half compared against its own
role's table. A pin one side documents and the other does not read fails here,
which is the whole point: that is the shape of the bug that hid IN_ALARM.

    python docs/check_pinmap.py        # from the repo root
"""
import io
import re
import sys

# The production firmware's pin map, as C #defines. Parsed as a fourth source
# alongside the three prose tables.
FIRMWARE = 'firmware/hub_prod/main/app_priv.h'

# The ground-floor node firmware: one header, both roles, split at this comment.
NODE_FIRMWARE = 'firmware/gf_node/main/gf.h'
NODE_SPLIT = '/* Utility node. */'

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

# One table per node role. Their signal names are gf.h's #define names with the
# GPIO_ prefix stripped, so these need no alias table. None means end of file.
NODE_TABLES = [
    ('docs/HARDWARE.md', '### 3.4. Ground Floor ESP32 Node 1', '### 3.5.'),
    ('docs/HARDWARE.md', '### 3.5. Ground Floor ESP32 Node 2', None),
]

# | **GPIO 32** | `IN_TWT_FLOT`| ...     and     | 32 | `IN_TWT_FLOT` | ...
ROW = re.compile(r'^\|\s*\*{0,2}(?:GPIO\s*)?(\d{1,2})\*{0,2}\s*\|\s*`([A-Z0-9_]+)`')


def section(path, start, end):
    text = io.open(path, encoding='utf-8').read()
    i = text.index(start)
    j = len(text) if end is None else text.index(end, i)
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


def firmware_pinmap(path, text=None):
    """{32: 'IN_TWT_FLOT', ...} from a firmware header's GPIO #defines."""
    out = {}
    if text is None:
        text = io.open(path, encoding='utf-8').read()
    for line in text.splitlines():
        m = FW_DEFINE.match(line)
        if not m:
            continue
        name, gpio = m.group(1), int(m.group(2))
        signal = FW_ALIASES.get(name, name)
        assert gpio not in out, '%s: GPIO %d assigned twice' % (path, gpio)
        out[gpio] = signal
    assert out, 'no GPIO defines parsed from %s' % path
    return out


def node_pinmaps():
    """(sump, utility) halves of gf.h. Split, because the two roles reuse pins
    and a combined map would report the overlap as a duplicate assignment."""
    text = io.open(NODE_FIRMWARE, encoding='utf-8').read()
    i = text.index(NODE_SPLIT)
    return (firmware_pinmap(NODE_FIRMWARE, text[:i]),
            firmware_pinmap(NODE_FIRMWARE, text[i:]))


def compare(label, maps):
    """maps: [(source, {gpio: signal})], the first authoritative."""
    ref_path, ref = maps[0]
    bad = 0
    for path, m in maps[1:]:
        for gpio in sorted(set(ref) | set(m)):
            a, b = ref.get(gpio), m.get(gpio)
            if a != b:
                print('%s  GPIO %-2d  %s: %-14s  %s: %s'
                      % (label, gpio, ref_path, a or '-', path, b or '-'))
                bad += 1
    if not bad:
        print('OK: %-9s %2d GPIOs agree across %d sources (%d doc + firmware).'
              % (label, len(ref), len(maps), len(maps) - 1))
    return bad


def main():
    groups = [('hub 0x00',
               [(path, pinmap(path, start, end)) for path, start, end in TABLES]
               + [(FIRMWARE, firmware_pinmap(FIRMWARE))])]

    sump_fw, util_fw = node_pinmaps()
    for label, table, fw in (('node 0x05', NODE_TABLES[0], sump_fw),
                             ('node 0x06', NODE_TABLES[1], util_fw)):
        path, start, end = table
        groups.append((label, [(path, pinmap(path, start, end)),
                               (NODE_FIRMWARE, fw)]))

    bad = sum(compare(label, maps) for label, maps in groups)
    if bad:
        print('\n%d mismatch(es). For the hub, WIRING.md section 1 is '
              'authoritative (it matches the built hub) - correct the others to '
              'it. For the nodes, gf.h is what actually runs.' % bad)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
