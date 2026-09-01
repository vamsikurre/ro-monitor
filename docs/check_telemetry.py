"""Fail if /api/telemetry cannot produce valid JSON, or drops a key the dashboard reads.

This one exists because of how the dashboard fails. `poll()` in
firmware/hub/data/dashboard.html wraps its fetch in a try/catch, and on ANY error
it quietly restarts the built-in demo simulator:

    } catch {
      if (!demoTimer) { demoTimer = setInterval(stepDemo, 2000); stepDemo(); }
    }

That is the right behaviour for a page served off a hub that has not booted yet.
It is a terrible way to find out about a typo in a format string, because the
symptom is a dashboard that looks perfect and animates convincingly while showing
invented numbers. Nobody reads "Demo" in the corner once the tanks are moving.

So: lift the real format string out of app_web.c, fill every conversion with a
plausible value, and require that the result parses as JSON and carries every key
the dashboard actually consumes.

    python docs/check_telemetry.py     # from the repo root
"""
import io
import json
import re
import sys

WEB = 'firmware/hub_prod/main/app_web.c'
DASH = 'firmware/hub/data/dashboard.html'

# Every path the dashboard reads out of the telemetry object. Sourced from its
# render/update functions, not guessed - if one of these disappears the page
# either throws (and silently reverts to the demo) or renders "undefined".
REQUIRED = [
    'sys.uptime_s', 'sys.rssi', 'sys.fw', 'sys.reset_reason',
    'rs485.online', 'rs485.total', 'rs485.errors', 'rs485.last_poll_ms',
    'tanks.sump.pct', 'tanks.sump.distance_mm', 'tanks.sump.state', 'tanks.sump.sensor',
    'tanks.rwt.pct', 'tanks.rwt.distance_mm', 'tanks.rwt.state', 'tanks.rwt.sensor',
    'tanks.twt.pct', 'tanks.twt.distance_mm', 'tanks.twt.state', 'tanks.twt.sensor',
    'tanks.dosing.pct', 'tanks.dosing.distance_mm', 'tanks.dosing.state', 'tanks.dosing.sensor',
    'pumps.borewell.on', 'pumps.borewell.state',
    'pumps.sump_motor.on', 'pumps.sump_motor.state',
    'pumps.rwp.on', 'pumps.rwp.state',
    'pumps.hpp.on', 'pumps.hpp.state',
    'quality.rwt.ppm', 'quality.rwt.t', 'quality.rwt.fitted',
    'quality.twt.ppm', 'quality.twt.t', 'quality.twt.fitted',
    'quality.rejection',
    'aster.twt_floty', 'aster.rwt_floty', 'aster.sump_floty',
    'aster.dos_lvl', 'aster.rl1', 'aster.rl2', 'aster.alarm', 'aster.lps',
    'env.ro_room.t', 'env.ro_room.rh', 'env.ro_room.state', 'env.ro_room.src', 'env.ro_room.age_s',
    'env.battery_room.t', 'env.battery_room.rh', 'env.battery_room.fan',
    'env.battery_room.state', 'env.battery_room.src', 'env.battery_room.age_s',
    'motors.hpp.amps', 'motors.rwp.amps', 'motors.overcurrent',
    'nodes',
]

# The tank state vocabulary the dashboard branches on. A fifth value would fall
# through every branch and render a tank with no liquid and no explanation.
TANK_STATES = {'ONLINE', 'STALE', 'OFFLINE', 'SENSOR_ERROR'}

# Plausible filler per conversion. The values only have to be the right JSON
# shape - this checks structure, not arithmetic.
FILL = [
    (r'%lld', '123456'),
    (r'%llu', '123456'),
    (r'%lu', '4321'),
    (r'%ld', '4321'),
    (r'%u', '1234'),
    (r'%d\.%d', '31.4'),      # the temperature/current idiom, before bare %d
    (r'%d', '42'),
]

# %s is the awkward one: the firmware uses it for quoted strings ("ONLINE"), for
# bare booleans (true/false) and for amps, which is a number or a literal null.
# Quoted ones are substituted first, then whatever bare %s remains becomes null -
# the one token that is valid JSON in every position, which is what keeps this a
# STRUCTURE check rather than a type check it was never meant to be.
def fill_strings(fmt):
    fmt = fmt.replace('"%s"', '"X"')
    return fmt.replace('%s', 'null')


def read(path):
    return io.open(path, encoding='utf-8').read()


def format_string(text):
    """Concatenate the adjacent literals making up telemetry_get's format."""
    start = text.index('int n = snprintf(json, sizeof(json),')
    out = []
    for line in text[start:].splitlines()[1:]:
        lits = re.findall(r'"((?:[^"\\]|\\.)*)"', line)
        if not lits:
            # the format is one unbroken run of string literals; the first line
            # without one is the start of the argument list
            break
        out.extend(lits)
        if lits[-1] == '}':
            break
    assert out, 'no format literals found in %s' % WEB
    joined = ''.join(out)
    # undo the C escaping so the result is the JSON the device actually writes
    return joined.replace('\\"', '"').replace('\\\\', '\\').replace('\\n', '\n')


def fill(fmt):
    for pat, val in FILL:
        fmt = re.sub(pat, val, fmt)
    return fill_strings(fmt)


def dig(obj, path):
    cur = obj
    for part in path.split('.'):
        if not isinstance(cur, dict) or part not in cur:
            return None, False
        cur = cur[part]
    return cur, True


def main():
    fmt = format_string(read(WEB))
    filled = fill(fmt)

    try:
        doc = json.loads(filled)
    except ValueError as e:
        print('The telemetry format string does not produce valid JSON.')
        print('  %s' % e)
        # Show the neighbourhood of the failure; a stray brace is hard to see in
        # 2 kB of one-line JSON.
        pos = getattr(e, 'pos', None)
        if pos:
            print('  ...%s...' % filled[max(0, pos - 70):pos + 70])
        return 1

    bad = []
    for path in REQUIRED:
        _, ok = dig(doc, path)
        if not ok:
            bad.append(path)

    if not isinstance(doc.get('nodes'), list) or not doc['nodes']:
        bad.append('nodes must be a non-empty array')
    else:
        for i, n in enumerate(doc['nodes']):
            for k in ('id', 'role', 'link', 'state', 'age_s'):
                if k not in n:
                    bad.append('nodes[%d].%s' % (i, k))

    # Every tank state the firmware can emit must be one the dashboard handles.
    dash = read(DASH)
    for word in sorted(TANK_STATES):
        if word not in dash:
            bad.append('dashboard does not handle tank state %r' % word)

    if bad:
        print('%d telemetry contract problem(s):' % len(bad))
        for b in bad:
            print('  missing or wrong: %s' % b)
        print('\nThe dashboard reverts to its demo simulator on any of these,')
        print('which looks like a working page showing invented numbers.')
        return 1

    print('OK: /api/telemetry emits valid JSON with all %d keys the dashboard reads.'
          % len(REQUIRED))
    return 0


if __name__ == '__main__':
    sys.exit(main())
