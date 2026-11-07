"""Pin the ground-floor pure maths in app_cal.c: loop current to distance,
clamp millivolts to deci-amps, and phase imbalance. Same approach as
check_frame.py - the code is lifted out of the firmware and built with gcc,
so what is tested is what ships.

    python docs/check_gf.py     # from the repo root
"""
import io, os, re, subprocess, sys, tempfile

CAL = 'firmware/hub_prod/main/app_cal.c'
BEGIN, END = '/* GF_PURE_BEGIN */', '/* GF_PURE_END */'

HARNESS = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#define CT_NOISE_FLOOR_MV 15
#define PRESS_MIN_UA 3500
#define PRESS_MAX_UA 21000
%s
static int fails = 0;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL %%s\n", #expr); fails++; } } while (0)
int main(void) {
    /* loop: 4 mA = full range (empty tank), 20 mA = 0 mm head... distance-alike */
    CHECK(gfLoopDistanceMM(4000, 2000) == 2000);
    CHECK(gfLoopDistanceMM(20000, 2000) == 1);      /* clamped to range-1 head */
    CHECK(gfLoopDistanceMM(12000, 2000) == 1000);
    CHECK(gfLoopDistanceMM(3400, 2000) == 0);       /* open loop */
    CHECK(gfLoopDistanceMM(21500, 2000) == 0);      /* shorted */
    CHECK(gfLoopDistanceMM(12000, 0) == 0);         /* ultrasonic tank, no range */
    /* clamp: 30 A/V, 1 turn: 100 mV rms = 3.0 A */
    CHECK(gfPhaseDeciAmps(100, 3000, 1) == 30);
    CHECK(gfPhaseDeciAmps(100, 3000, 2) == 15);
    CHECK(gfPhaseDeciAmps(10, 3000, 1) == -1);      /* under the noise floor: no clamp */
    CHECK(gfPhaseDeciAmps(15, 3000, 1) == 4);       /* exactly the floor counts */
    int16_t a[3] = { 40, 40, 40 }; CHECK(gfImbalancePct(a) == 0);
    int16_t b[3] = { 40, 30, -1 }; CHECK(gfImbalancePct(b) == 25);
    int16_t c[3] = { 40, -1, -1 }; CHECK(gfImbalancePct(c) == 0);
    int16_t d[3] = { -1, -1, -1 }; CHECK(gfImbalancePct(d) == 0);
    printf(fails ? "check_gf: %%d FAILED\n" : "check_gf: OK\n", fails);
    return fails ? 1 : 0;
}
'''

def main():
    src = io.open(CAL, encoding='utf-8').read()
    if BEGIN not in src or END not in src:
        print('markers %s / %s not found in %s' % (BEGIN, END, CAL)); return 1
    block = src[src.index(BEGIN) + len(BEGIN):src.index(END)]
    d = tempfile.mkdtemp()
    c_path, exe = os.path.join(d, 'gf.c'), os.path.join(d, 'gf.exe')
    io.open(c_path, 'w', encoding='utf-8').write(HARNESS % block)
    if subprocess.call(['gcc', '-Wall', '-Wextra', '-Werror', '-o', exe, c_path]) != 0:
        print('gcc refused the extracted code -- that is the finding.'); return 1
    return subprocess.call([exe])

GF = 'firmware/hub_prod/main/app_gf.c'
GF_BEGIN, GF_END = '/* GF_PARSE_BEGIN */', '/* GF_PARSE_END */'

PARSE_HARNESS = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "cJSON.h"
typedef enum { SENSOR_OK=0, SENSOR_BLIND=1, SENSOR_NO_ECHO=2, SENSOR_HW_FAULT=3 } sensor_status_t;
#define GF_POLL_MS 5000
#define GF_REPROBE_MS 30000
#define GF_OFFLINE_MISSES 3
typedef struct { bool valid; bool online; uint8_t misses; int64_t last_ok_us; int64_t next_poll_us;
                 char fw[16]; int8_t rssi; uint32_t uptime_s; } gf_link_t;
typedef struct { gf_link_t link; bool pressure; uint16_t distance_mm; uint8_t quality;
                 sensor_status_t sensor; uint32_t loop_ua; } gf_sump_t;
typedef struct { gf_link_t link; uint16_t bore_mv[3]; uint16_t sump_mv[3]; bool sump_on;
                 int8_t rwt_floty; int16_t temp_deci_c; uint16_t hum_deci_pct; bool sht_ok;
                 int8_t float_low; bool ctrl_node, bore_act, sump_act; uint16_t flags; bool relays[3];
                 char last_cmd[24]; } gf_util_t;
enum { GF_MOTOR_BORE = 0, GF_MOTOR_SUMP = 1 };
enum { GF_CMD_START = 0, GF_CMD_STOP = 1 };
enum { GF_SRC_AUTO = 0, GF_SRC_APP = 1, GF_SRC_DASH = 2 };
typedef struct { uint8_t motor, cmd, src; } gf_cmd_t;
typedef struct { uint16_t bore_apv_x100, sump_apv_x100; uint8_t bore_turns, sump_turns;
                 uint16_t bore_run_da, bore_dry_da, sump_noload_da, max_run_min;
                 bool ctrl_node, bore_act, sump_act, ctrl_node_set; } gf_ctl_cfg_t;
%s
static int fails = 0;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL %%s\n", #expr); fails++; } } while (0)
int main(int argc, char **argv) {
    gf_sump_t s = {0};
    CHECK(gf_parse_sump("{\"id\":5,\"fw\":\"a3cb57d\",\"uptime_s\":3840,\"rssi\":-64,"
                        "\"source\":\"ultrasonic\",\"distance_mm\":1750,\"quality\":95,\"status\":\"OK\","
                        "\"loop_ua\":null}", &s));
    CHECK(!s.pressure && s.distance_mm == 1750 && s.quality == 95 && s.sensor == SENSOR_OK && s.loop_ua == 0);
    CHECK(strcmp(s.link.fw, "a3cb57d") == 0 && s.link.rssi == -64 && s.link.uptime_s == 3840);
    CHECK(gf_parse_sump("{\"id\":5,\"fw\":\"x\",\"uptime_s\":1,\"rssi\":-70,\"source\":\"pressure\","
                        "\"distance_mm\":null,\"quality\":null,\"status\":\"HW_FAULT\",\"loop_ua\":2100}", &s));
    CHECK(s.pressure && s.distance_mm == 0 && s.loop_ua == 2100 && s.sensor == SENSOR_HW_FAULT);
    CHECK(!gf_parse_sump("{\"id\":6,\"fw\":\"x\"}", &s));          /* wrong node */
    CHECK(!gf_parse_sump("{\"id\":5,\"fw\":\"x\"", &s));            /* truncated */
    gf_util_t u = {0};
    CHECK(gf_parse_util("{\"id\":6,\"fw\":\"b\",\"uptime_s\":9,\"rssi\":-68,\"bore_mv\":[412,405,398],"
                        "\"sump_mv\":[398,0,0],\"sump_on\":true,\"rwt_floty\":null,"
                        "\"t_deci_c\":312,\"rh_deci_pct\":548,\"sht_ok\":true}", &u));
    CHECK(u.bore_mv[0] == 412 && u.bore_mv[2] == 398 && u.sump_mv[0] == 398 && u.sump_mv[2] == 0);
    CHECK(u.sump_on && u.rwt_floty == -1 && u.temp_deci_c == 312 && u.hum_deci_pct == 548 && u.sht_ok);
    CHECK(gf_parse_util("{\"id\":6,\"fw\":\"b\",\"uptime_s\":9,\"rssi\":-68,\"bore_mv\":[0,0,0],"
                        "\"sump_mv\":[0,0,0],\"sump_on\":false,\"rwt_floty\":false,"
                        "\"t_deci_c\":0,\"rh_deci_pct\":0,\"sht_ok\":false}", &u));
    CHECK(u.rwt_floty == 0 && !u.sht_ok);
    /* Task 7: node control fields. Older firmware without them still parses. */
    CHECK(u.flags == 0 && u.float_low == -1 && !u.ctrl_node && !u.bore_act && !u.sump_act &&
          !u.relays[0] && !u.relays[1] && !u.relays[2] && u.last_cmd[0] == 0);
    CHECK(gf_parse_util("{\"id\":6,\"fw\":\"b\",\"uptime_s\":9,\"rssi\":-68,\"bore_mv\":[0,0,0],"
                        "\"sump_mv\":[0,0,0],\"sump_on\":false,\"rwt_floty\":null,"
                        "\"t_deci_c\":0,\"rh_deci_pct\":0,\"sht_ok\":true,"
                        "\"float_low\":true,\"ctrl_node\":true,\"bore_act\":true,\"sump_act\":false,"
                        "\"flags\":1029,\"relays\":[false,true,true],\"last_cmd\":\"web:bore:start\"}", &u));
    CHECK(u.float_low == 1 && u.ctrl_node && u.bore_act && !u.sump_act && u.flags == 1029);
    CHECK(!u.relays[0] && u.relays[1] && u.relays[2] && strcmp(u.last_cmd, "web:bore:start") == 0);
    CHECK(gf_parse_util("{\"id\":6,\"fw\":\"b\",\"uptime_s\":9,\"rssi\":-68,\"bore_mv\":[0,0,0],"
                        "\"sump_mv\":[0,0,0],\"sump_on\":false,\"rwt_floty\":null,"
                        "\"t_deci_c\":0,\"rh_deci_pct\":0,\"sht_ok\":true,\"float_low\":null,"
                        "\"last_cmd\":\"abcdefghijklmnopqrstuvwxyz0123456789\"}", &u));
    CHECK(u.float_low == -1 && !u.ctrl_node && u.flags == 0 && !u.relays[1]);   /* stale ctl fields cleared */
    CHECK(strlen(u.last_cmd) == 23 && strncmp(u.last_cmd, "abcdefghijklmnopqrstuvw", 23) == 0);
    CHECK(!gf_parse_util("{\"id\":6,\"fw\":\"b\",\"bore_mv\":[1,2]}", &u));   /* short array */
    /* Task 8: command and cfg bodies, exactly what the node's ctl_parse_cmd/cfg read. */
    {
        static const char *const mn[] = { "bore", "sump" }, *const cn[] = { "start", "stop" };
        char b[160], want[160];
        for (int m = 0; m < 2; m++) for (int k = 0; k < 2; k++) for (int s = GF_SRC_APP; s <= GF_SRC_DASH; s++) {
            gf_cmd_t c = { (uint8_t)m, (uint8_t)k, (uint8_t)s };
            snprintf(want, sizeof want, "{\"token\":\"tok\",\"motor\":\"%%s\",\"cmd\":\"%%s\",\"src\":\"%%s\"}",
                     mn[m], cn[k], s == GF_SRC_APP ? "app" : "dash");
            int n = gf_ctl_body(&c, "tok", b, sizeof b);
            CHECK(n == (int)strlen(want) && strcmp(b, want) == 0);
        }
        gf_cmd_t c = { GF_MOTOR_BORE, GF_CMD_STOP, GF_SRC_DASH };
        CHECK(gf_ctl_body(&c, "a\"b\\c", b, sizeof b) > 0 &&                 /* token a"b\c, escaped */
              strcmp(b, "{\"token\":\"a\\\"b\\\\c\",\"motor\":\"bore\",\"cmd\":\"stop\",\"src\":\"dash\"}") == 0);
        CHECK(gf_ctl_body(&c, "tok", b, 20) == -1);                 /* truncation */
        CHECK(gf_ctl_body(&c, NULL, b, sizeof b) == -1);
        gf_cmd_t bad1 = { GF_MOTOR_BORE, GF_CMD_START, GF_SRC_AUTO }, bad2 = { 2, 0, 1 }, bad3 = { 0, 2, 1 };
        CHECK(gf_ctl_body(&bad1, "t", b, sizeof b) == -1);          /* the node refuses src auto */
        CHECK(gf_ctl_body(&bad2, "t", b, sizeof b) == -1 && gf_ctl_body(&bad3, "t", b, sizeof b) == -1);
        char cb[400];
        gf_ctl_cfg_t g = { 3000, 2950, 1, 2, 10, 0, 30, 120, true, false, true, true };
        int n = gf_cfg_body(&g, "t", cb, sizeof cb);
        CHECK(n == (int)strlen(CFG_EXPECT) && strcmp(cb, CFG_EXPECT) == 0);
        if (strcmp(cb, CFG_EXPECT) != 0) printf("  got  %%s\n", cb);
        /* never saved on /cal (or a wiped hub NVS): no ctrl_node, the node keeps its own */
        g.ctrl_node_set = false;
        CHECK(gf_cfg_body(&g, "t", cb, sizeof cb) > 0 && strstr(cb, "ctrl_node") == NULL &&
              strstr(cb, "\"bore_act\":false,\"sump_act\":true}") != NULL);
        g.ctrl_node_set = true;
        CHECK(gf_cfg_body(&g, "t", cb, 40) == -1);
        CHECK(gf_cfg_body(&g, NULL, cb, sizeof cb) == -1);
        /* why -> event b */
        CHECK(gf_why_index(200, "{\"ok\":true}") == 0);
        CHECK(gf_why_index(409, "{\"ok\":false,\"why\":\"single_phase\"}") == 1);
        CHECK(gf_why_index(409, "{\"ok\":false,\"why\":\"sump_hold\"}") == 2);
        CHECK(gf_why_index(409, "{\"ok\":false,\"why\":\"not_settled\"}") == 3);
        CHECK(gf_why_index(409, "{\"ok\":false,\"why\":\"sump_full\"}") == 4);
        CHECK(gf_why_index(409, "{\"ok\":false,\"why\":\"no_float\"}") == 5);
        CHECK(gf_why_index(409, "{\"ok\":false,\"why\":\"bogus\"}") == 15);
        CHECK(gf_why_index(409, "not json") == 15 && gf_why_index(409, NULL) == 15);
        CHECK(gf_why_index(403, "{\"ok\":false,\"why\":\"token\"}") == 15 && gf_why_index(0, NULL) == 15);
        /* /api/motor request body */
        gf_cmd_t r = { 9, 9, 9 };
        CHECK(gf_parse_motor_req("{\"motor\":\"sump\",\"cmd\":\"stop\"}", &r) &&
              r.motor == GF_MOTOR_SUMP && r.cmd == GF_CMD_STOP && r.src == 9);
        CHECK(gf_parse_motor_req("{\"motor\":\"bore\",\"cmd\":\"start\"}", &r) &&
              r.motor == GF_MOTOR_BORE && r.cmd == GF_CMD_START);
        r.motor = 7;
        CHECK(!gf_parse_motor_req("{\"motor\":\"well\",\"cmd\":\"start\"}", &r) && r.motor == 7);
        CHECK(!gf_parse_motor_req("{\"motor\":\"bore\"}", &r) && !gf_parse_motor_req("{\"motor\":\"bore\",\"cmd\":1}", &r));
        CHECK(!gf_parse_motor_req("motor=bore", &r) && !gf_parse_motor_req(NULL, &r));
    }
    /* latch: 3 misses to offline, reprobe slower, first reply restores */
    gf_link_t l = {0}; int64_t t = 1000000;
    gf_link_result(&l, true, t);  CHECK(l.online && l.valid && l.misses == 0 && l.next_poll_us == t + 5000000LL);
    gf_link_result(&l, false, t); CHECK(l.online && l.misses == 1 && l.next_poll_us == t + 5000000LL);
    gf_link_result(&l, false, t); CHECK(l.online && l.misses == 2);
    gf_link_result(&l, false, t); CHECK(!l.online && l.misses == 3 && l.next_poll_us == t + 30000000LL);
    gf_link_result(&l, false, t); CHECK(!l.online && l.misses == 4 && l.next_poll_us == t + 30000000LL);
    gf_link_result(&l, true, t + 7); CHECK(l.online && l.misses == 0 && l.last_ok_us == t + 7 && l.next_poll_us == t + 7 + 5000000LL);
    if (argc >= 3) {
        FILE *f = fopen(argv[1], "rb"); static char b1[600]; size_t n1 = fread(b1, 1, 599, f); fclose(f); b1[n1] = 0;
        f = fopen(argv[2], "rb"); static char b2[600]; size_t n2 = fread(b2, 1, 599, f); fclose(f); b2[n2] = 0;
        gf_sump_t fs = {0}; gf_util_t fu = {0};
        CHECK(gf_parse_sump(b1, &fs)); CHECK(gf_parse_util(b2, &fu));
    }
    const char *label = (argc >= 3) ? "check_gf parse+fake" : "check_gf parse";
    printf(fails ? "%%s: %%d FAILED\n" : "%%s: OK\n", label, fails);
    return fails ? 1 : 0;
}
'''

CFG_EXPECT = ('{"token":"t","bore_apv_x100":3000,"sump_apv_x100":2950,"bore_turns":1,"sump_turns":2,'
              '"bore_run_da":10,"bore_dry_da":0,"sump_noload_da":30,"max_run_min":120,'
              '"ctrl_node":true,"bore_act":false,"sump_act":true}')

def check_cfg_keys():
    """gf_cfg_body must send exactly the keys the node's ctl_parse_cfg accepts:
    an unknown key is a 400, a missing one is a setting the hub never pushes."""
    import json
    glue = io.open('firmware/gf_node/main/ctl_glue.c', encoding='utf-8').read()
    m = re.search(r'keys\[\]\s*=\s*\{([^}]*)\}', glue)
    node = set(re.findall(r'"(\w+)"', m.group(1))) if m else set()
    hub = set(json.loads(CFG_EXPECT))
    if not node or node != hub:
        print('cfg keys differ hub vs node: %s' % sorted(hub ^ node or {'(node list not found)'})); return 1
    return 0

FAKE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fake_gf_node.py')

def idf_cjson():
    idf = os.environ.get('IDF_PATH') or 'C:/esp/v5.4.4/esp-idf'
    d = os.path.join(idf, 'components', 'json', 'cJSON')
    if not os.path.isfile(os.path.join(d, 'cJSON.c')):
        # the layout above is this machine's default; if IDF_PATH points
        # somewhere else entirely, look for cJSON anywhere under C:\esp
        for root, _, files in os.walk('C:/esp'):
            if 'cJSON.c' in files and root.endswith('cJSON'):
                return root
        return None
    return d

def build_parse_exe():
    """Compile PARSE_HARNESS + the extracted parser block. Returns the exe path,
    or None (having already printed why) on failure."""
    src = io.open(GF, encoding='utf-8').read()
    if GF_BEGIN not in src or GF_END not in src:
        print('markers %s / %s not found in %s' % (GF_BEGIN, GF_END, GF)); return None
    cj = idf_cjson()
    if cj is None:
        print('cJSON.c not found - set IDF_PATH'); return None
    block = src[src.index(GF_BEGIN) + len(GF_BEGIN):src.index(GF_END)]
    d = tempfile.mkdtemp()
    c_path, exe = os.path.join(d, 'gfp.c'), os.path.join(d, 'gfp.exe')
    cfg_c = '#define CFG_EXPECT "%s"\n' % CFG_EXPECT.replace('"', '\\"')   # CFG_EXPECT has no backslashes
    io.open(c_path, 'w', encoding='utf-8').write(cfg_c + PARSE_HARNESS % block)
    if subprocess.call(['gcc', '-Wall', '-Wextra', '-Werror', '-I', cj, '-o', exe, c_path,
                        os.path.join(cj, 'cJSON.c')]) != 0:
        print('gcc refused the extracted parser -- that is the finding.'); return None
    return exe

def check_ctl_flags():
    """Every CTL_F_* in the node's ctl.h must be mirrored, same value, as GF_CTL_F_* in app_priv.h."""
    rx = r'#define\s+(?:GF_)?CTL_F_(\w+)\s+(0x[0-9A-Fa-f]+|\d+)'
    node = dict(re.findall(rx, io.open('firmware/gf_node/main/ctl.h', encoding='utf-8').read()))
    hub = dict(re.findall(r'#define\s+GF_CTL_F_(\w+)\s+(0x[0-9A-Fa-f]+|\d+)',
                          io.open('firmware/hub_prod/main/app_priv.h', encoding='utf-8').read()))
    bad = [k for k in node if k not in hub or int(hub[k], 0) != int(node[k], 0)]
    if not node or bad or set(hub) - set(node):
        print('CTL_F_* mismatch ctl.h vs app_priv.h: %s' % (bad or sorted(set(hub) ^ set(node)) or 'none found')); return 1
    return 0

def main_parse():
    if check_ctl_flags() or check_cfg_keys(): return 1
    exe = build_parse_exe()
    return 1 if exe is None else subprocess.call([exe])

def load_fake_module():
    import importlib.util
    spec = importlib.util.spec_from_file_location('fake_gf_node', FAKE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

def main_fake():
    """Run the actual fake-node HTTP server (once per role, in a background
    thread) and feed its real /api/telemetry responses to the compiled
    parsers - proof the fake is a trustworthy stand-in for real firmware."""
    import threading, urllib.request
    exe = build_parse_exe()
    if exe is None:
        return 1
    fn = load_fake_module()
    d = tempfile.mkdtemp()
    paths = {}
    for role in ('sump', 'util'):
        fn.H.role = role
        srv = fn.ThreadingHTTPServer(('127.0.0.1', 0), fn.H)
        port = srv.server_address[1]
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        try:
            with urllib.request.urlopen('http://127.0.0.1:%d/api/telemetry' % port, timeout=2) as r:
                body = r.read()
        finally:
            srv.shutdown(); srv.server_close(); t.join(timeout=2)
        path = os.path.join(d, role + '.json')
        io.open(path, 'wb').write(body)
        paths[role] = path
    return subprocess.call([exe, paths['sump'], paths['util']])

WEB = 'firmware/hub_prod/main/app_web.c'
WEB_BEGIN, WEB_END = '/* WEB_PURE_BEGIN */', '/* WEB_PURE_END */'

WEB_HARNESS = r'''
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
%s
static int fails = 0;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL %%s\n", #expr); fails++; } } while (0)
int main(void) {
    /* CSRF guard on the hub's POSTs: Origin/Referer host must be the Host header. */
    CHECK(same_origin("192.168.2.158", "http://192.168.2.158"));
    CHECK(same_origin("192.168.2.158", "http://192.168.2.158/cal#ctl"));          /* Referer */
    CHECK(same_origin("192.168.2.158:8080", "http://192.168.2.158:8080"));
    CHECK(same_origin("Hub.local", "http://hub.LOCAL/"));
    CHECK(!same_origin("192.168.2.158", "http://192.168.2.158:8080"));            /* other port */
    CHECK(!same_origin("192.168.2.158", "http://evil.example"));
    CHECK(!same_origin("192.168.2.158", "http://192.168.2.158.evil.example/"));
    CHECK(!same_origin("192.168.2.158", "http://evil.example/http://192.168.2.158"));
    CHECK(!same_origin("192.168.2.158", "null"));                                  /* sandboxed / file: */
    CHECK(!same_origin("192.168.2.158", ""));
    CHECK(!same_origin("", "http://192.168.2.158") && !same_origin(NULL, "http://x"));
    CHECK(!same_origin("x", NULL));
    printf(fails ? "check_gf web: %%d FAILED\n" : "check_gf web: OK\n", fails);
    return fails ? 1 : 0;
}
'''

def main_web():
    src = io.open(WEB, encoding='utf-8').read()
    if WEB_BEGIN not in src or WEB_END not in src:
        print('markers %s / %s not found in %s' % (WEB_BEGIN, WEB_END, WEB)); return 1
    block = src[src.index(WEB_BEGIN) + len(WEB_BEGIN):src.index(WEB_END)]
    d = tempfile.mkdtemp()
    c_path, exe = os.path.join(d, 'web.c'), os.path.join(d, 'web.exe')
    io.open(c_path, 'w', encoding='utf-8').write(WEB_HARNESS % block)
    if subprocess.call(['gcc', '-Wall', '-Wextra', '-Werror', '-o', exe, c_path]) != 0:
        print('gcc refused the extracted web block -- that is the finding.'); return 1
    return subprocess.call([exe])

if __name__ == '__main__':
    sys.exit(main() or main_parse() or main_fake() or main_web())
