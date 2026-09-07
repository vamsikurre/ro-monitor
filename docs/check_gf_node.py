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
hub's app_gf.c parser (jint_or_null / cJSON_IsNull) actually requires. It also
statically asserts (task 10 fix round 1, after a mutation-tested review found
both silently passing): every quoted ALL-CAPS literal anywhere in the file is
one of the hub's four recognised statuses, and every nullable sump reading is
actually emitted through a bare %s splice - the only shape that can produce a
real JSON null - rather than a hardcoded literal that never could be.

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

# The hub's app_gf.c only recognises these four (SENSOR_OK/BLIND/NO_ECHO/
# HW_FAULT in app_priv.h); anything else parses as SENSOR_OK by default in a
# strcmp ladder that falls through silently - a typo'd status would read as
# a healthy sensor instead of failing loudly.
STATUS_VALUES = ('OK', 'BLIND', 'NO_ECHO', 'HW_FAULT')

# distance_mm/quality/loop_ua (sump) and rwt_floty (util) are null on
# whichever sensor path is NOT fitted (docs/fake_gf_node.py STATE, the hub's
# jint_or_null / cJSON_IsNull parse). The format string can only encode that
# by splicing in a sub-buffer holding either the literal "null" or a plain
# value - a bare, unquoted %s - since extract_format() never sees which
# branch of the C code's ternary actually ran. So it cannot check the VALUE
# a nullable field takes at runtime; what it CAN check statically is that
# the field is even capable of being null - that it is emitted through that
# bare-%s splice at all, rather than a hardcoded literal (a number, or for
# rwt_floty a bare `false`) that could never be null no matter what the
# sensor does. rwt_floty is exactly this shape but for a bool: false means
# "the float is open", null means "the optocoupler is not fitted, we cannot
# see it" - distinct facts a hardcoded false would erase.
NULLABLE_BARE_KEYS = {
    'sump': ('distance_mm', 'quality', 'loop_ua'),
    'util': ('rwt_floty',),
}

def check_status_literals(src):
    """Every quoted ALL-CAPS token anywhere in the file is a status string
    literal, however it is assigned - a bare `=`, a ternary branch, the
    static initializer - so this does not need to know the variable name or
    the assignment shape to find them all. JSON keys ("distance_mm") and
    English prose (comments, ESP_LOGI text) are never pure upper-case, so
    this does not have to separate code from comments to stay accurate."""
    bad = sorted(set(re.findall(r'"([A-Z][A-Z_]*)"', src)) - set(STATUS_VALUES))
    return bad

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

def fill_format(fmt, role):
    """Replace printf conversions with JSON-shaped placeholders.

    A %s directly between two quotes ("...":"%s") is a JSON string field -
    filled with an unquoted token so the literal's own quotes wrap it, same as
    before. A BARE %s (no adjacent quote either side) is the pattern used for
    a field spliced from a sub-buffer the C code fills with one of two
    literals picked by a branch this extractor never sees - either
    "null"/a number (sensors_sump.c's distance_mm/quality/loop_ua) or
    "null"/"true"/"false" (sensors_util.c's rwt_floty). For exactly those
    fields (NULLABLE_BARE_KEYS[role]) it fills `null`, one of the values that
    spot can actually hold at runtime.

    A bare %s can also be a field that is NEVER null - sump_on and sht_ok are
    real JSON booleans, spliced unquoted the same way because C has no %b,
    but always "true" or "false", never "null". Filling those with `null`
    too would fail the schema check for every required bool below no matter
    what the C code does, which is not a defect in this file - it is this
    function guessing wrong. Fill anything bare that is not a known-nullable
    key with `true` instead: a valid literal for both a bool field and (since
    Python's bool is an int) an int-typed one, so it does not fail the type
    check that follows. Every other conversion here is a bare number."""
    nullable = set(NULLABLE_BARE_KEYS.get(role, ()))
    def repl(m):
        conv = m.group(0)
        if conv[-1] != 's':
            return '0'
        start, end = m.span()
        quoted = start > 0 and end < len(fmt) and fmt[start - 1] == '"' and fmt[end] == '"'
        if quoted:
            return 'x'
        key_m = re.search(r'"([A-Za-z_][A-Za-z0-9_]*)"\s*:\s*$', fmt[:start])
        key = key_m.group(1) if key_m else None
        return 'null' if key in nullable else 'true'
    return _CONV.sub(repl, fmt)

def load_fake_state():
    spec = importlib.util.spec_from_file_location('fake_gf_node', FAKE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)      # module-level code only defines STATE/H/main - safe to import
    return mod.STATE

def check_role(role, c_path, fake_state):
    fails = []
    try:
        fmt = extract_format(c_path)
        obj = json.loads(fill_format(fmt, role))
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

    if 'status' in schema:
        bad = check_status_literals(io_read(c_path))
        if bad:
            fails.append('status literal(s) %s not in %s' % (bad, STATUS_VALUES))

    for key in NULLABLE_BARE_KEYS.get(role, ()):
        if ('"%s":%%s' % key) not in fmt:
            fails.append('%s is not spliced via a bare %%s in the format string, so it can '
                         'never be null at runtime regardless of what the sensor reports' % key)

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
