"""Fail if the node address tables ever drift apart again.

Four things must agree, and once did not (phase-B spec section 3.7):
  1. ADDR_MAP in WIRING.md section 9.1
  2. the same ADDR_MAP copied into the phase-B spec section 3.7
  3. the ADDR_MAP every node sketch actually flashes
  4. the as-built jumper audit in WIRING.md section 9.2

Plus two invariants: the three built boards must resolve to three DISTINCT ids,
and none of them may resolve to 0x00 (the "do not transmit" code).

    python docs/check_addrmap.py        # from the repo root
"""
import glob
import io
import re
import sys

WIRING = 'docs/WIRING.md'
SPEC = 'docs/superpowers/specs/2026-08-25-ro-monitor-phase-b-design.md'
SKETCHES = sorted(glob.glob('firmware/*/*.ino'))  # any sketch carrying an ADDR_MAP

# 0x03,   // 0b01  A1 to GND  -> TWT
ENTRY = re.compile(r'(0x[0-9A-Fa-f]{2})\s*,\s*//\s*(0b[01]{2})')

# | Nano #1 | **GND** | open | `0b10` | `0x02` RWT | ...
AUDIT = re.compile(
    r'^\|\s*([^|]+?)\s*\|\s*\*{0,2}(GND|open)\*{0,2}\s*\|'
    r'\s*\*{0,2}(GND|open)\*{0,2}\s*\|\s*`(0b[01]{2})`\s*\|\s*`(0x[0-9A-Fa-f]{2})`')


def read(path):
    return io.open(path, encoding='utf-8').read()


def addr_map(path):
    """{'0b01': 0x03, ...} from the first ADDR_MAP block in the file."""
    text = read(path)
    i = text.index('ADDR_MAP[4]')
    block = text[i:text.index('};', i)]
    out = {code: int(val, 16) for val, code in ENTRY.findall(block)}
    assert len(out) == 4, '%s: expected 4 ADDR_MAP entries, parsed %d' % (path, len(out))
    return out


def audit(path):
    """[(board, a0, a1, raw, expected_id)] from the as-built table."""
    rows = []
    for line in read(path).splitlines():
        m = AUDIT.match(line)
        if m:
            board, a0, a1, raw, node = m.groups()
            rows.append((board, a0, a1, raw, int(node, 16)))
    assert rows, '%s: parsed no as-built audit rows' % path
    return rows


def main():
    bad = []
    wiring = addr_map(WIRING)
    others = [(SPEC, addr_map(SPEC))]
    others += [(f, addr_map(f)) for f in SKETCHES if 'ADDR_MAP[4]' in read(f)]

    for path, other in others:
        for code in sorted(set(wiring) | set(other)):
            if wiring.get(code) != other.get(code):
                bad.append('%s: WIRING.md=%s %s=%s'
                           % (code, wiring.get(code), path, other.get(code)))

    seen = {}
    for board, a0, a1, raw, node in audit(WIRING):
        # the row's own jumper columns must produce the raw code it claims
        want = '0b%d%d' % (a1 == 'open', a0 == 'open')
        if want != raw:
            bad.append('%s: A0=%s A1=%s is %s, row says %s' % (board, a0, a1, want, raw))
        # and ADDR_MAP must resolve that code to the id the row claims
        got = wiring.get(raw)
        if got != node:
            bad.append('%s: ADDR_MAP[%s]=%s but audit expects 0x%02X' % (board, raw, got, node))
        if node == 0x00:
            bad.append('%s: resolves to 0x00, the do-not-transmit code' % board)
        if node in seen:
            bad.append('%s collides with %s: both are 0x%02X' % (board, seen[node], node))
        seen[node] = board

    if bad:
        print('\n'.join(bad))
        print('\n%d problem(s). WIRING.md section 9.1 is authoritative.' % len(bad))
        return 1
    print('OK: %d jumper codes agree across %d sources; %d built boards resolve to %s.'
          % (len(wiring), 1 + len(others), len(seen),
             ', '.join('0x%02X' % n for n in sorted(seen))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
