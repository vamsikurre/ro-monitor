"""Pin gf_node's telemetry JSON against what the hub actually parses.

sensors_sump.c and sensors_util.c each own one function - sensors_json() -
that builds the whole /api/telemetry body (see gf.h). Tasks 10 and 11 replace
today's fixed-value format strings with real sensor readings; nothing else
stops a renamed, dropped, or re-typed key from surviving that rewrite until
someone is standing at a sump with a laptop, reading "unparseable reply" in a
hub log. This extracts each format string straight out of the .c file - not a
frozen copy of it - fills in its printf conversions with placeholders, and
checks the resulting object's key set against docs/fake_gf_node.py's STATE
dicts (the already-agreed shape) and its null-vs-number keys against what the
hub's app_gf.c parser (jint_or_null / cJSON_IsNull) actually requires.

    python docs/check_gf_node.py
"""
import importlib.util, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUMP_C = os.path.join(ROOT, 'firmware', 'gf_node', 'main', 'sensors_sump.c')
UTIL_C = os.path.join(ROOT, 'firmware', 'gf_node', 'main', 'sensors_util.c')
FAKE   = os.path.join(ROOT, 'docs', 'fake_gf_node.py')

NODE_ID = {'sump': 5, 'util': 6}

# The hub's contract (app_gf.c's jlink/jint/jint_or_null/jmv3/cJSON_IsBool),
# not this script's opinion: a key's Python type once the format string is
# filled in and parsed as JSON, or a tuple including NoneType for the three
# sump readings and the one util reading the hub parses with jint_or_null /
# cJSON_IsNull - "no clamp fitted" everywhere else would be a type the hub's
# parser rejects outright, which is exactly the class of bug this exists to
# catch before Tasks 10/11 ship it.
SCHEMA = {
    'sump': {
        'id': int, 'fw': str, 'uptime_s': int, 'rssi': int,
        'source': str, 'status': str,
        'distance_mm': (int, type(None)),
        'quality':     (int, type(None)),
        'loop_ua':     (int, type(None)),
    },
    'util': {
        'id': int, 'fw': str, 'uptime_s': int, 'rssi': int,
        'bore_mv': list, 'sump_mv': list,
        'sump_on': bool, 'sht_ok': bool,
        'rwt_floty': (bool, type(None)),
        't_deci_c': int, 'rh_deci_pct': int,
    },
}

def _is_nullable(role, key):
    t = SCHEMA[role][key]
    return isinstance(t, tuple) and type(None) in t

def _parse_concat_string(s, pos):
    """s[pos:], after whitespace, is a C string literal - possibly the first
    of several adjacent ones (plain C string concatenation, and how both
    sensors_*.c wrap their format string across two lines). Returns the
    unescaped, concatenated content and the position just past the last
    literal."""
    parts = []
    while True:
        while s[pos] in ' \t\r\n':
            pos += 1
        if s[pos] != '"':
            break
        pos += 1
        buf = []
        while s[pos] != '"':
            if s[pos] == '\\':
                buf.append(s[pos + 1])
                pos += 2
                continue
            buf.append(s[pos])
            pos += 1
        pos += 1
        parts.append(''.join(buf))
    return ''.join(parts), pos

def extract_format(path):
    """Locate sensors_json()'s format string by anchoring on its return
    statement, not on the function body or a fixed line range - so this still
    finds it once Tasks 10/11 rewrite everything else around it."""
    src = io_read(path)
    m = re.search(r'return\s+snprintf\s*\(\s*buf\s*,\s*len\s*,', src)
    if not m:
        raise AssertionError('%s: no `return snprintf(buf, len, ...)` in sensors_json' % path)
    fmt, _ = _parse_concat_string(src, m.end())
    return fmt

def io_read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()

_CONV = re.compile(r'%[-+0 #]*\d*(?:\.\d+)?(?:hh|h|ll|l|j|z|t|L)?[diouxXeEfFgGaAcs]')

def fill_format(fmt):
    """Replace printf conversions with JSON-shaped placeholders. %s already
    sits inside quotes the literal supplies; every other conversion here is a
    bare number - so this only has to get the shape right, never the value."""
    return _CONV.sub(lambda m: 'x' if m.group(0)[-1] == 's' else '0', fmt)

def load_fake_state():
    spec = importlib.util.spec_from_file_location('fake_gf_node', FAKE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)      # module-level code only defines STATE/H/main - safe to import
    return mod.STATE

def check_role(role, c_path, fake_state):
    fails = []
    try:
        fmt = extract_format(c_path)
        obj = json.loads(fill_format(fmt))
    except Exception as e:
        print('FAIL %s: %s' % (c_path, e))
        return False

    schema = SCHEMA[role]
    want_keys = set(schema)
    fake_keys = set(fake_state[role]) | {'uptime_s', 'rssi'}
    got_keys = set(obj)
    if got_keys != want_keys:
        fails.append('keys %s, want %s (per gf.h/hub contract)' % (sorted(got_keys), sorted(want_keys)))
    if got_keys != fake_keys:
        fails.append('keys %s do not match docs/fake_gf_node.py STATE[%r] keys %s' %
                      (sorted(got_keys), role, sorted(fake_keys)))

    if obj.get('id') != NODE_ID[role]:
        fails.append('id is %r, want %d' % (obj.get('id'), NODE_ID[role]))

    for k in got_keys & want_keys:
        v, t = obj[k], schema[k]
        if not isinstance(v, t):
            fails.append('%s is %r (%s), want %s' % (k, v, type(v).__name__, t))
        elif v is None and not _is_nullable(role, k):
            fails.append('%s is null but the hub requires a value there' % k)

    for arr_key in ('bore_mv', 'sump_mv'):
        if arr_key in schema and isinstance(obj.get(arr_key), list) and len(obj[arr_key]) != 3:
            fails.append('%s has %d elements, want 3' % (arr_key, len(obj[arr_key])))

    for f in fails:
        print('FAIL %s: %s' % (c_path, f))
    return not fails

def main():
    fake_state = load_fake_state()
    sump_ok = check_role('sump', SUMP_C, fake_state)
    util_ok = check_role('util', UTIL_C, fake_state)
    ok = sump_ok and util_ok
    print('check_gf_node: %s' % ('OK' if ok else 'FAILED'))
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
