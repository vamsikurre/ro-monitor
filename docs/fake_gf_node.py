"""A ground-floor node on a PC, so the hub's polling, offline latch, re-probe
and /cal maths are proven before either board exists.

    python docs/fake_gf_node.py --role sump --port 8085
    python docs/fake_gf_node.py --role util --port 8086

Then on the hub's /cal page: Sump node IP = <this PC>:8085, Utility = <PC>:8086.

Knobs, from a browser or curl on the same port:
    /set?distance_mm=1200&quality=95&status=OK       (sump, ultrasonic)
    /set?source=pressure&loop_ua=11200               (sump, loop)
    /set?bore_mv=400,395,390&sump_mv=380,0,0&sump_on=1&rwt_floty=1&t_deci_c=318
    /silent?on=1      answers nothing until /silent?on=0 - the hub must go OFFLINE
                      after 3 misses and come back on the first reply
"""
import argparse, json, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

STATE = {
    'sump': {'id': 5, 'fw': 'fake', 'source': 'ultrasonic', 'distance_mm': 1750, 'quality': 95,
             'status': 'OK', 'loop_ua': None},
    'util': {'id': 6, 'fw': 'fake', 'bore_mv': [412, 405, 398], 'sump_mv': [398, 0, 0], 'sump_on': False,
             'rwt_floty': None, 't_deci_c': 312, 'rh_deci_pct': 548, 'sht_ok': True},
}
SILENT = {'on': False}
T0 = time.time()

SOURCE_VALUES = ('ultrasonic', 'pressure')
STATUS_VALUES = ('OK', 'BLIND', 'NO_ECHO', 'HW_FAULT')

def coerce(role, k, v):
    # Raises ValueError on anything the real firmware would reject, so a typo
    # at the bench fails loudly here instead of shipping a body the hub's
    # parser silently drops with just an unhelpful log line.
    if k in ('bore_mv', 'sump_mv'):
        vals = [int(x) for x in v.split(',')]
        if len(vals) != 3:
            raise ValueError('%s needs exactly 3 values, got %d' % (k, len(vals)))
        if any(x < 0 or x > 65535 for x in vals):
            raise ValueError('%s values must be 0-65535' % k)
        return vals
    if k in ('sump_on', 'sht_ok'):
        return v not in ('0', 'false', '')
    if k == 'rwt_floty':
        return None if v in ('null', '') else v not in ('0', 'false')
    if k == 'source':
        if v not in SOURCE_VALUES:
            raise ValueError('source must be one of %s' % (SOURCE_VALUES,))
        return v
    if k == 'status':
        if v not in STATUS_VALUES:
            raise ValueError('status must be one of %s' % (STATUS_VALUES,))
        return v
    if k == 'loop_ua' and v in ('null', ''):
        return None
    return int(v)

class H(BaseHTTPRequestHandler):
    role = 'sump'
    def log_message(self, *a): pass
    def _send(self, code, body):
        data = body.encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == '/set':
            for k, v in q.items():
                if k in STATE[self.role]:
                    try:
                        STATE[self.role][k] = coerce(self.role, k, v)
                    except ValueError as e:
                        return self._send(400, json.dumps({'error': str(e), 'key': k, 'value': v}))
            return self._send(200, json.dumps(STATE[self.role]))
        if u.path == '/silent':
            SILENT['on'] = q.get('on', '1') not in ('0', 'false')
            return self._send(200, json.dumps(SILENT))
        if u.path == '/api/telemetry':
            if SILENT['on']:
                time.sleep(5)          # longer than the hub's 2 s timeout
                return
            body = dict(STATE[self.role])
            body['uptime_s'] = int(time.time() - T0)
            body['rssi'] = -60
            if self.role == 'sump' and body['source'] == 'pressure':
                body['distance_mm'] = None; body['quality'] = None
            return self._send(200, json.dumps(body))
        self._send(404, '{}')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--role', choices=['sump', 'util'], required=True)
    ap.add_argument('--port', type=int, required=True)
    a = ap.parse_args()
    H.role = a.role
    print('fake %s node on port %d - /api/telemetry, /set?..., /silent?on=1' % (a.role, a.port))
    ThreadingHTTPServer(('0.0.0.0', a.port), H).serve_forever()

if __name__ == '__main__':
    main()
