"""Every alert message must fit what RainMaker will actually deliver.

esp_rmaker_raise_alert() caps the string at ESP_RMAKER_MAX_ALERT_LEN, and going
over is a SILENT failure: the notification is the one thing nobody is watching a
console for, so a message that is one word too long is a fault the plant never
tells you about. This lifts the cap from the SDK header and every alert literal
out of evaluate_alerts(), and fails if any of them will not fit.

Format specifiers are expanded to a worst case before measuring -- a tank at
100%% is one character longer than one at 9%%, and that is exactly the margin a
message written against the short case eats.

    python docs/check_alerts.py          # from the repo root
"""
import io
import re
import sys

MAIN = 'firmware/hub_prod/main/app_main.c'
SDK = ('firmware/hub_prod/managed_components/espressif__esp_rainmaker/'
       'include/esp_rmaker_core.h')

# Worst case each specifier can print, for the values this firmware passes:
# percentages and deci-amps are small, but a negative or a 3-digit one is not
# impossible and the margin is what we are measuring.
WIDTHS = {'d': 4, 'u': 4, 'lu': 10, 's': 8}


def literals(text):
    """Every run of adjacent C string literals, concatenated as the compiler would."""
    out, buf, end = [], [], None
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', text):
        if end is not None and text[end:m.start()].strip() != '':
            out.append(''.join(buf))
            buf = []
        buf.append(m.group(1))
        end = m.end()
    if buf:
        out.append(''.join(buf))
    return out


def expanded_len(lit):
    """Bytes on the wire, with %d and friends at their widest."""
    def sub(m):
        return 'X' * WIDTHS.get(m.group(1), 8)
    s = re.sub(r'%[-+ #0-9.]*(lu|[dusf])', sub, lit)
    return len(s.encode('utf-8'))


def main():
    sdk = io.open(SDK, encoding='utf-8').read()
    m = re.search(r'#define\s+ESP_RMAKER_MAX_ALERT_LEN\s+(\d+)', sdk)
    if not m:
        print('could not find ESP_RMAKER_MAX_ALERT_LEN in the SDK header')
        return 1
    cap = int(m.group(1))

    main_c = io.open(MAIN, encoding='utf-8').read()
    start = main_c.index('static void evaluate_alerts')
    block = main_c[start:main_c.index('\n}\n', start)]

    failures = 0
    for lit in literals(block):
        if len(lit) < 25:          # not an alert message, a format fragment or a label
            continue
        n = expanded_len(lit)
        if n > cap:
            print('OVER  %3d > %d  %s' % (n, cap, lit))
            failures += 1
        elif n > cap - 10:
            print('tight %3d / %d  %s' % (n, cap, lit))

    if failures:
        print('\n%d alert message(s) will be rejected or truncated by RainMaker.' % failures)
        return 1
    print('OK: every alert message fits ESP_RMAKER_MAX_ALERT_LEN (%d bytes), '
          'format specifiers at their widest.' % cap)
    return 0


if __name__ == '__main__':
    sys.exit(main())
