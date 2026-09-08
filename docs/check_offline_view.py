"""Fail if offlineView() in the dashboard leaves any live reading on screen.

When the hub stops answering, the dashboard re-renders the last payload with
every live figure cleared, so a stale sump level cannot sit there in live
typeface. offlineView() is what does the clearing, and the failure mode is
silent and specific: forget one field and that one number keeps showing a value
from minutes ago, on a page whose header says the hub is offline. Nobody
cross-checks a number against the header.

So this walks the cleared payload and asserts nothing readable survived. It is
written as a deny-list of shapes rather than a field list, so a NEW telemetry
field that carries a reading fails here until offlineView() handles it - which
is the point. `run`, and the event and trend history, are exempt on purpose:
those are historical by nature and blanking a lifetime total tells you less
than leaving it.

    python docs/check_offline_view.py       # from the repo root
"""
import io
import json
import os
import re
import subprocess
import sys
import tempfile

DASH = 'firmware/hub/data/dashboard.html'

# Fields that legitimately still hold a value after clearing, with why.
ALLOWED = {
    'sys',                    # header handles these itself via HUB_STALE
    'run',                    # ledger, historical
    'rs485.total',            # how many nodes exist, not a reading
    'rs485.errors',           # counter since boot, historical
    'rs485.last_poll_ms',     # ditto
    'quality.rwt.fitted',     # whether a probe exists is wiring, not a reading
    'quality.twt.fitted',
    'tanks.sump.source',      # which sensor, not what it says
    'tanks.sump.sensor', 'tanks.rwt.sensor', 'tanks.twt.sensor', 'tanks.dosing.sensor',
    'motors.borewell.dry_at_deci_a',   # the configured threshold, not a reading
    'motors.sump_motor.dry_at_deci_a',
}


def brace_object(text, start_marker):
    i = text.index(start_marker)
    j = text.index('{', i)
    depth = 0
    for k in range(j, len(text)):
        if text[k] == '{':
            depth += 1
        elif text[k] == '}':
            depth -= 1
            if depth == 0:
                return text[j:k + 1]
    sys.exit('unbalanced braces after ' + start_marker)


def function_src(text, name):
    i = text.index('function %s(' % name)
    j = text.index('{', i)
    depth = 0
    for k in range(j, len(text)):
        if text[k] == '{':
            depth += 1
        elif text[k] == '}':
            depth -= 1
            if depth == 0:
                return text[i:k + 1]
    sys.exit('unbalanced braces in ' + name)


def leaves(o, path=''):
    if isinstance(o, dict):
        for k, v in o.items():
            for r in leaves(v, path + ('.' if path else '') + k):
                yield r
    elif isinstance(o, list):
        for n, v in enumerate(o):
            for r in leaves(v, '%s[%d]' % (path, n)):
                yield r
    else:
        yield path, o


def allowed(path):
    if path in ALLOWED:
        return True
    # prefix match, so nodes[0].id is covered by 'nodes' style entries and any
    # indexed path under an allowed root passes
    base = re.sub(r'\[\d+\]', '', path)
    return base in ALLOWED or base.split('.')[0] in ALLOWED


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    text = io.open(DASH, encoding='utf-8').read()

    js = (function_src(text, 'offlineView')
          + '\nconst demo = ' + brace_object(text, 'const demo = {') + ';\n'
          + 'const before = JSON.stringify(demo);\n'
          + 'const after = offlineView(demo);\n'
          + 'console.log(JSON.stringify({after, mutated: JSON.stringify(demo) !== before}));\n')

    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 'ov.js')
        io.open(p, 'w', encoding='utf-8').write(js)
        run = subprocess.run(['node', p], capture_output=True, text=True)
        if run.returncode != 0:
            print(run.stdout + run.stderr)
            sys.exit('offlineView() did not run')
        res = json.loads(run.stdout)

    fails = []

    # 1. It must not mutate its argument: the caller keeps the good payload in
    #    LAST, and the header's "last good" time refers to it.
    if res['mutated']:
        fails.append('offlineView() mutated its argument - LAST would be destroyed')

    after = res['after']

    # 2. Structural claims: these are what the render keys off to show "no data".
    for name, t in (after.get('tanks') or {}).items():
        if t.get('state') != 'OFFLINE':
            fails.append('tanks.%s.state is %r, expected OFFLINE' % (name, t.get('state')))
    for name, p in (after.get('pumps') or {}).items():
        if p.get('state') != 'OFFLINE' or p.get('on'):
            fails.append('pumps.%s still reads %r' % (name, p))
    for name, e in (after.get('env') or {}).items():
        if e.get('state') != 'OFFLINE':
            fails.append('env.%s.state is %r, expected OFFLINE' % (name, e.get('state')))
    for name in ('hpp', 'rwp', 'borewell', 'sump_motor'):
        m = (after.get('motors') or {}).get(name)
        if not m:
            continue
        if m.get('amps') is not None or m.get('running'):
            fails.append('motors.%s still reads amps=%r running=%r'
                         % (name, m.get('amps'), m.get('running')))
    for k, v in (after.get('aster') or {}).items():
        if v is not None:
            fails.append('aster.%s is %r - a contact nobody is reading must be null' % (k, v))
    for n in (after.get('nodes') or []):
        if n.get('state') != 'OFFLINE':
            fails.append('nodes %s state is %r' % (n.get('id'), n.get('state')))

    # 3. The catch-all: no numeric or boolean-true leaf may survive outside the
    #    allow-list. This is the part that fails when a new reading is added to
    #    telemetry and nobody teaches offlineView() to clear it.
    for path, val in leaves(after):
        if allowed(path):
            continue
        if isinstance(val, bool):
            if val:
                fails.append('%s survived as true' % path)
        elif isinstance(val, (int, float)):
            if val not in (0, -1):
                fails.append('%s survived as %r' % (path, val))
        elif isinstance(val, str):
            if val not in ('OFFLINE', '', 'OK') and not path.endswith('.state'):
                # ids, roles and link names are labels, not readings
                if not re.search(r'\.(id|role|link|src|fw|ip|source|sensor|reset_reason)$', path):
                    fails.append('%s survived as %r' % (path, val))

    if fails:
        for f in fails:
            print('FAIL  ' + f)
        print('\n%d field(s) would still show a stale reading with the hub offline' % len(fails))
        sys.exit(1)
    print('OK: offlineView() clears every live reading (%d leaves checked)'
          % sum(1 for _ in leaves(after)))


if __name__ == '__main__':
    main()
