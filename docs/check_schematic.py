"""Fail if the hub KiCad schematic disagrees with its own specification.

`check_pinmap.py` already holds the GPIO map identical across three documents and
the production firmware. This script does the one thing that check does not: it
compares `PCB_HUB_MOTHERBOARD.md` section 4 against what has actually been drawn
in `hardware/hub_motherboard/hub.kicad_sch`.

Why this exists. The hub schematic is being drawn by hand, section by section,
against a specification written weeks earlier. The failure mode is not a missing
net -- you notice those, because the board does not work. It is a net drawn under
a slightly different name: `IN_HPP_CT` typed as `IN_HPP_CT1`, `RS485_RX` swapped
with `RS485_TX`. KiCad cannot know; both are valid schematics. Only the spec knows,
and the spec is a table in a markdown file that nothing reads.

So this reads it, and it treats the three outcomes differently on purpose:

  drawn + matching   fine, say nothing except a count
  not drawn yet      NORMAL while the schematic is in progress -- reported, not failed
  drawn + unexpected FAILS. A net name in the schematic that section 4 never
                     mentions is either a typo or an undocumented design change,
                     and both need a human to look.

That third bucket is the whole point. It is the one a person cannot catch by
eye once the sheet has forty nets on it.

Needs kicad-cli, which ships with KiCad. Netlist export is done into a temp file;
nothing is written into the repo.

    python docs/check_schematic.py         # from the repo root
"""
import io
import os
import re
import subprocess
import sys
import tempfile

SPEC = 'docs/PCB_HUB_MOTHERBOARD.md'
SCH = 'hardware/hub_motherboard/hub.kicad_sch'

KICAD_CLI_CANDIDATES = [
    r'C:\Program Files\KiCad\10.0\bin\kicad-cli.exe',
    r'C:\Program Files\KiCad\9.0\bin\kicad-cli.exe',
    '/usr/bin/kicad-cli',
    '/usr/local/bin/kicad-cli',
    'kicad-cli',
]

# Section 4 rows look like:
#   | 16 | `RS485_RX` | `J_RS485` pin 3 | XY-485 `RXD` |
#   | 36 (`SP`) | `IN_HPP_CT` | section 7 CT channel 1 | ADC1_CH0. ... |
SPEC_ROW = re.compile(
    r'^\|\s*(\d{1,2})\s*(?:\([^)]*\))?\s*\|\s*`([A-Z0-9_]+)`\s*\|([^|]*)\|')

# These two are on the ESP32 module itself and reach no board net, so they must
# never appear in the schematic. Section 4 says so in its own "Notes" column.
ON_MODULE = {'LED_STATUS', 'BTN_BOOT'}

# Nets the schematic legitimately carries that section 4's GPIO table does not
# list, because they are not GPIOs. Rails come from section 6, BIAS from section 7.
NON_GPIO_NETS = {'GND', '+3V3', '+5V', '+12V', 'BIAS'}


def find_kicad_cli():
    for c in KICAD_CLI_CANDIDATES:
        if os.path.sep in c or '/' in c:
            if os.path.exists(c):
                return c
        else:
            try:
                subprocess.run([c, 'version'], capture_output=True, check=True)
                return c
            except Exception:
                pass
    return None


def parse_spec(path):
    """GPIO number -> net name, from section 4 only."""
    text = io.open(path, encoding='utf-8').read()
    m = re.search(r'\n## 4\.(.*?)(?=\n## 5\.)', text, re.S)
    if not m:
        sys.exit('FAIL: could not locate section 4 in %s' % path)
    out = {}
    for line in m.group(1).split('\n'):
        r = SPEC_ROW.match(line.strip())
        if r:
            out[int(r.group(1))] = r.group(2)
    if not out:
        sys.exit('FAIL: section 4 parsed to zero nets -- has the table format changed?')
    return out


def netlist_nets(cli, sch):
    """Net name -> sorted ['REF.PIN', ...] as KiCad itself computes it."""
    tmp = os.path.join(tempfile.gettempdir(), 'ro_hub_check.net')
    p = subprocess.run([cli, 'sch', 'export', 'netlist', '--format', 'kicadsexpr',
                        '-o', tmp, sch], capture_output=True, text=True)
    if p.returncode != 0 or not os.path.exists(tmp):
        sys.exit('FAIL: netlist export failed\n%s%s' % (p.stdout, p.stderr))
    if 'annotation errors' in (p.stdout + p.stderr):
        sys.exit('FAIL: schematic has annotation errors -- '
                 'open it in Eeschema and run Tools > Annotate Schematic')
    s = io.open(tmp, encoding='utf-8').read()
    sec = s[s.index('(nets'):] if '(nets' in s else ''
    nets, comps = {}, {}
    for m in re.finditer(r'\(net\s*\n\s*\(code "\d+"\)\s*\n\s*\(name "([^"]*)"\)', sec):
        nxt = sec.find('(net\n', m.end())
        blk = sec[m.end():nxt if nxt > 0 else len(sec)]
        nodes = re.findall(r'\(ref "([^"]+)"\)\s*\n\s*\(pin "([^"]+)"\)', blk)
        nets[m.group(1)] = sorted('%s.%s' % n for n in nodes)
    for m in re.finditer(r'\(comp\s*\n\s*\(ref "([^"]+)"\)(.*?)(?=\n\s*\(comp\s|\Z)', s, re.S):
        fp = re.search(r'\(footprint "([^"]*)"\)', m.group(2))
        comps[m.group(1)] = fp.group(1) if fp else ''
    os.remove(tmp)
    return nets, comps


def main():
    for f in (SPEC, SCH):
        if not os.path.exists(f):
            sys.exit('FAIL: %s not found -- run from the repo root' % f)

    cli = find_kicad_cli()
    if cli is None:
        sys.exit('FAIL: kicad-cli not found. Install KiCad, or add it to PATH.')

    spec = parse_spec(SPEC)
    nets, comps = netlist_nets(cli, SCH)

    expected = {n for n in spec.values() if n not in ON_MODULE}
    drawn = set(nets)

    matched = sorted(expected & drawn)
    pending = sorted(expected - drawn)
    unexpected = sorted(n for n in drawn - expected
                        if n not in NON_GPIO_NETS and not n.startswith('Net-('))
    on_module_drawn = sorted(n for n in drawn if n in ON_MODULE)

    print('hub schematic vs PCB_HUB_MOTHERBOARD.md section 4')
    print('  spec nets (excluding on-module) : %d' % len(expected))
    print('  drawn and matching              : %d' % len(matched))
    print('  not drawn yet                   : %d' % len(pending))

    if pending:
        print('\nstill to draw:')
        rev = {v: k for k, v in spec.items()}
        for n in pending:
            print('    GPIO %-3s %s' % (rev.get(n, '?'), n))

    fail = False

    if unexpected:
        fail = True
        print('\nFAIL: net names in the schematic that section 4 does not list.')
        print('      A typo, or a design change that never reached the spec:')
        for n in unexpected:
            print('    %-16s on %s' % (n, ', '.join(nets[n]) or '(no pins)'))

    if on_module_drawn:
        fail = True
        print('\nFAIL: these are on the ESP32 module and reach no board net,')
        print('      but the schematic gives them one:')
        for n in on_module_drawn:
            print('    %-16s on %s' % (n, ', '.join(nets[n])))

    # Footprints. The two CT jacks are deliberately unset until the socket is
    # metered (spec section 7), so they are reported but do not fail.
    missing_fp = sorted(r for r, fp in comps.items() if not fp and not r.startswith('#'))
    jacks = [r for r in missing_fp if r.startswith('J_CT')]
    others = [r for r in missing_fp if not r.startswith('J_CT')]
    if jacks:
        print('\nfootprint deliberately unset (spec section 7 -- meter the socket first):')
        for r in jacks:
            print('    %s' % r)
    if others:
        fail = True
        print('\nFAIL: no footprint assigned, and no reason on record:')
        for r in others:
            print('    %s' % r)

    if fail:
        print('\nFAILED')
        return 1
    if pending:
        print('\nOK so far -- nothing drawn contradicts the spec. %d nets still to go.'
              % len(pending))
    else:
        print('\nOK -- every net in section 4 is drawn, and nothing extra.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
