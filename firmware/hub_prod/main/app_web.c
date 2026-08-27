/*
 * app_web.c — esp_http_server: dashboard, telemetry, calibration
 *
 * The telemetry JSON shape is not invented here. firmware/hub/data/dashboard.html
 * already existed, already polls /api/telemetry once a second, and its built-in
 * demo object documents the exact contract — including the tank state vocabulary
 * (ONLINE / STALE / OFFLINE / SENSOR_ERROR) and the fact that it computes its own
 * alert list client-side. This file's job is to produce that shape faithfully;
 * changing a key here breaks a working 1400-line dashboard.
 *
 * Fields the Phase-1 hub cannot know are reported as OFFLINE rather than as
 * zeros: the ground sump, the borewell and the sump motor live on nodes 0x05 and
 * 0x06, which are Phase 2. The dashboard already renders an offline node
 * correctly, so an honest OFFLINE draws hatching and no liquid, where a zero
 * would draw an empty tank that looks measured.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

#include "app_cal.h"
#include "app_priv.h"
#include "app_rs485.h"
#include "app_sensors.h"
#include "app_web.h"

static const char *TAG = "web";

/* dashboard.html is gzipped and embedded at build time - see main/CMakeLists.txt.
 * Serving it from flash rather than SPIFFS means one less partition, one less
 * mount to fail, and no way for the page and the firmware to be different ages.
 * Embedded as BINARY, so there is no trailing NUL and the length is exactly
 * end - start. */
extern const uint8_t dashboard_gz_start[] asm("_binary_dashboard_html_gz_start");
extern const uint8_t dashboard_gz_end[]   asm("_binary_dashboard_html_gz_end");

/* ---------------------------------------------------------------- helpers */

static const char *link_word(int64_t last_ok_us, bool ever)
{
    if (!ever || last_ok_us == 0) {
        return "OFFLINE";
    }
    int64_t age_ms = (esp_timer_get_time() - last_ok_us) / 1000;
    if (age_ms > NODE_OFFLINE_MS) return "OFFLINE";
    if (age_ms > NODE_STALE_MS)   return "STALE";
    return "ONLINE";
}

static int age_s(int64_t last_ok_us)
{
    if (last_ok_us == 0) {
        return 9999;
    }
    return (int)((esp_timer_get_time() - last_ok_us) / 1000000);
}

/* The dashboard's own vocabulary for the sensor sub-label. "OK" makes it show
 * "Reading"; anything else is surfaced verbatim as a warning, so these strings
 * are user-facing. */
static const char *sensor_word(sensor_status_t s)
{
    switch (s) {
        case SENSOR_OK:       return "OK";
        case SENSOR_BLIND:    return "Blind zone";
        case SENSOR_NO_ECHO:  return "No echo";
        case SENSOR_HW_FAULT: return "Sensor fault";
        default:              return "Unknown";
    }
}

static const char *tank_state_word(const tank_state_t *t)
{
    if (t->sensor == SENSOR_HW_FAULT) {
        return "SENSOR_ERROR";
    }
    /* A tank that answers but cannot produce a level - uncalibrated, or reading
     * outside its calibration - is not ONLINE for display purposes. Showing a
     * gauge for it would be showing a number we refused to compute. */
    if (t->pct < 0) {
        return "SENSOR_ERROR";
    }
    return link_word(t->last_ok_us, true);
}

/* ------------------------------------------------------------ Basic auth */

static bool authorised(httpd_req_t *req)
{
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    if (strncmp(hdr, "Basic ", 6) != 0) {
        return false;
    }

    unsigned char decoded[96];
    size_t out_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &out_len,
                              (const unsigned char *)hdr + 6, strlen(hdr + 6)) != 0) {
        return false;
    }
    decoded[out_len] = '\0';

    char *colon = strchr((char *)decoded, ':');
    if (colon == NULL) {
        return false;
    }
    *colon = '\0';
    return cal_password_matches((char *)decoded, colon + 1);
}

static esp_err_t deny(httpd_req_t *req)
{
    /* The realm string is what the browser shows in its password box. */
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"RO Hub calibration\"");
    httpd_resp_send(req, "Calibration requires the hub password.", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Basic auth sends the password base64-encoded, which is encoding and not
 * encryption. Over plain HTTP on a house LAN that is a deliberate, bounded
 * choice: the thing being protected is a calibration constant, the alternative
 * is a TLS certificate to provision and renew on an embedded box, and nothing
 * here can start a motor. It is documented rather than hidden.
 * ponytail: Basic over HTTP on the LAN. If this ever gains a control that moves
 * water, put it behind HTTPS or the RainMaker cloud path instead. */

/* -------------------------------------------------------------- handlers */

static esp_err_t dashboard_get(httpd_req_t *req)
{
    /* Only the compressed copy is in flash - that is the whole point - so a
     * client that cannot accept gzip gets an honest refusal rather than a
     * screenful of binary. Every browser sends this header; curl does not
     * unless asked, and someone debugging with curl deserves to be told why
     * rather than left staring at garbage. */
    char enc[64];
    bool gzip_ok = (httpd_req_get_hdr_value_str(req, "Accept-Encoding", enc, sizeof(enc)) == ESP_OK)
                   && (strstr(enc, "gzip") != NULL);
    if (!gzip_ok) {
        httpd_resp_set_status(req, "406 Not Acceptable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req,
            "The dashboard is stored gzip-compressed to save flash, and this "
            "client did not send Accept-Encoding: gzip.\n"
            "Use a browser, or curl --compressed.\n"
            "Telemetry is uncompressed either way: GET /api/telemetry\n",
            HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)dashboard_gz_start,
                           dashboard_gz_end - dashboard_gz_start);
}

static esp_err_t telemetry_get(httpd_req_t *req)
{
    static char json[2600];

    hub_state_lock();
    const hub_state_t *s = hub_state();

    int n = snprintf(json, sizeof(json),
        "{"
        "\"sys\":{\"uptime_s\":%lld,\"rssi\":%d,\"fw\":\"%s\",\"reset_reason\":\"%s\"},"
        "\"rs485\":{\"online\":%d,\"total\":3,\"errors\":%lu,\"last_poll_ms\":%lu},"
        "\"tanks\":{"
          "\"sump\":{\"pct\":0,\"distance_mm\":0,\"state\":\"OFFLINE\",\"sensor\":\"Phase 2\"},"
          "\"rwt\":{\"pct\":%d,\"distance_mm\":%u,\"state\":\"%s\",\"sensor\":\"%s\"},"
          "\"dosing\":{\"pct\":%d,\"distance_mm\":%u,\"state\":\"%s\",\"sensor\":\"%s\"},"
          "\"twt\":{\"pct\":%d,\"distance_mm\":%u,\"state\":\"%s\",\"sensor\":\"%s\"}"
        "},"
        "\"pumps\":{"
          "\"borewell\":{\"on\":false,\"state\":\"OFFLINE\"},"
          "\"sump_motor\":{\"on\":false,\"state\":\"OFFLINE\"},"
          "\"rwp\":{\"on\":%s,\"state\":\"ONLINE\"},"
          "\"hpp\":{\"on\":%s,\"state\":\"ONLINE\"}"
        "},"
        "\"aster\":{\"twt_floty\":%s,\"rwt_floty\":false,\"sump_floty\":false,"
                   "\"dos_lvl\":false,\"rl1\":%s,\"rl2\":%s,\"alarm\":%s},"
        "\"env\":{"
          "\"ro_room\":{\"t\":%d.%d,\"rh\":%d.%d,\"state\":\"%s\",\"src\":\"SHT30 . I2C 0x44\",\"age_s\":%d},"
          "\"battery_room\":{\"t\":%d.%d,\"rh\":%d.%d,\"fan\":%s,\"state\":\"%s\",\"src\":\"SHT30 . Node 0x04\",\"age_s\":%d}"
        "},"
        "\"motors\":{"
          "\"hpp\":{\"amps\":%d.%d,\"mv_lo\":%lu,\"mv_hi\":%lu},"
          "\"rwp\":{\"amps\":%d.%d,\"mv_lo\":%lu,\"mv_hi\":%lu},"
          "\"overcurrent\":%s"
        "},"
        "\"nodes\":["
          "{\"id\":\"0x02\",\"role\":\"Raw Water\",\"link\":\"RS485\",\"state\":\"%s\",\"age_s\":%d},"
          "{\"id\":\"0x03\",\"role\":\"Treated Water\",\"link\":\"RS485\",\"state\":\"%s\",\"age_s\":%d},"
          "{\"id\":\"0x04\",\"role\":\"Battery Room\",\"link\":\"RS485\",\"state\":\"%s\",\"age_s\":%d},"
          "{\"id\":\"0x05\",\"role\":\"Sump\",\"link\":\"Wi-Fi\",\"state\":\"OFFLINE\",\"age_s\":9999},"
          "{\"id\":\"0x06\",\"role\":\"Motors\",\"link\":\"Wi-Fi\",\"state\":\"OFFLINE\",\"age_s\":9999}"
        "]"
        "}",
        esp_timer_get_time() / 1000000, 0, FW_VERSION, "Power on",
        (s->rwt_online ? 1 : 0) + (s->twt_online ? 1 : 0) + (s->battery_online ? 1 : 0),
        (unsigned long)rs485_error_count(), (unsigned long)s->last_cycle_ms,

        s->rwt.pct < 0 ? 0 : s->rwt.pct, s->rwt.distance_mm,
        tank_state_word(&s->rwt), sensor_word(s->rwt.sensor),
        s->dosing.pct < 0 ? 0 : s->dosing.pct, s->dosing.distance_mm,
        tank_state_word(&s->dosing), sensor_word(s->dosing.sensor),
        s->twt.pct < 0 ? 0 : s->twt.pct, s->twt.distance_mm,
        tank_state_word(&s->twt), sensor_word(s->twt.sensor),

        s->rwp.running ? "true" : "false",
        s->hpp.running ? "true" : "false",

        s->twt_float_closed ? "true" : "false",
        s->rl1_active ? "true" : "false",
        s->rl2_active ? "true" : "false",
        s->alarm_active ? "true" : "false",

        s->ro_room.temp_deci_c / 10, abs(s->ro_room.temp_deci_c % 10),
        s->ro_room.hum_deci_pct / 10, s->ro_room.hum_deci_pct % 10,
        s->ro_room.fault ? "SENSOR_ERROR" : link_word(s->ro_room.last_ok_us, true),
        age_s(s->ro_room.last_ok_us),

        s->battery_room.temp_deci_c / 10, abs(s->battery_room.temp_deci_c % 10),
        s->battery_room.hum_deci_pct / 10, s->battery_room.hum_deci_pct % 10,
        s->fan_on ? "true" : "false",
        s->battery_room.fault ? "SENSOR_ERROR" : link_word(s->battery_room.last_ok_us, s->battery_online),
        age_s(s->battery_room.last_ok_us),

        s->hpp.deci_amps < 0 ? 0 : s->hpp.deci_amps / 10,
        s->hpp.deci_amps < 0 ? 0 : s->hpp.deci_amps % 10,
        (unsigned long)s->hpp.mv_lo, (unsigned long)s->hpp.mv_hi,
        s->rwp.deci_amps < 0 ? 0 : s->rwp.deci_amps / 10,
        s->rwp.deci_amps < 0 ? 0 : s->rwp.deci_amps % 10,
        (unsigned long)s->rwp.mv_lo, (unsigned long)s->rwp.mv_hi,
        s->overcurrent ? "true" : "false",

        link_word(s->rwt.last_ok_us, s->rwt_online), age_s(s->rwt.last_ok_us),
        link_word(s->twt.last_ok_us, s->twt_online), age_s(s->twt.last_ok_us),
        link_word(s->battery_room.last_ok_us, s->battery_online), age_s(s->battery_room.last_ok_us));

    hub_state_unlock();

    if (n < 0 || n >= (int)sizeof(json)) {
        ESP_LOGE(TAG, "telemetry buffer too small (%d)", n);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, n);
}

/* ------------------------------------------------------- calibration page */

static esp_err_t cal_get(httpd_req_t *req)
{
    if (!authorised(req)) {
        return deny(req);
    }

    static char page[4096];
    int n = 0;

    n += snprintf(page + n, sizeof(page) - n,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>RO Hub calibration</title>"
        "<style>body{font:15px/1.5 system-ui,sans-serif;margin:0 auto;padding:16px;max-width:44rem}"
        "h2{margin:0 0 4px}h3{margin:20px 0 6px}input{padding:4px;font:inherit}"
        "form{margin:6px 0}small{color:#666}fieldset{border:1px solid #ccc;border-radius:6px;margin:10px 0}"
        "</style>"
        "<h2>RO Hub calibration</h2>"
        "<p><small>Distances are transducer face to liquid surface, in millimetres. "
        "Fill or empty the tank, read the live figure, then save it. Every value is "
        "range-checked before it is stored.</small></p>");

    hub_state_lock();
    const hub_state_t *s = hub_state();
    uint16_t live[CAL_TANK_COUNT] = { s->rwt.distance_mm, s->twt.distance_mm, s->dosing.distance_mm };
    int16_t  live_pct[CAL_TANK_COUNT] = { s->rwt.pct, s->twt.pct, s->dosing.pct };
    uint32_t ct_lo[CAL_CT_COUNT] = { s->hpp.mv_lo, s->rwp.mv_lo };
    uint32_t ct_hi[CAL_CT_COUNT] = { s->hpp.mv_hi, s->rwp.mv_hi };
    int16_t  ct_a[CAL_CT_COUNT]  = { s->hpp.deci_amps, s->rwp.deci_amps };
    hub_state_unlock();

    n += snprintf(page + n, sizeof(page) - n, "<fieldset><legend>Tank levels</legend>");
    for (int i = 0; i < CAL_TANK_COUNT; i++) {
        const cal_tank_cfg_t *c = cal_tank(i);
        n += snprintf(page + n, sizeof(page) - n,
            "<h3>%s</h3><p>live <b>%u mm</b> &rarr; %s</p>"
            "<form method=post action='/api/cal/tank'>"
            "<input type=hidden name=tank value='%s'>"
            "full <input name=full size=6 value='%u'> "
            "empty <input name=empty size=6 value='%u'> "
            "<button>Save</button></form>",
            cal_tank_label(i), live[i],
            live_pct[i] < 0 ? "no level" : "measured",
            cal_tank_key(i), c->full_mm, c->empty_mm);
    }
    n += snprintf(page + n, sizeof(page) - n,
        "<p><small>Empty must be a longer distance than full, and full must be "
        "outside the %d mm blind zone.</small></p></fieldset>", BLIND_ZONE_MM);

    n += snprintf(page + n, sizeof(page) - n, "<fieldset><legend>Current clamps</legend>");
    for (int i = 0; i < CAL_CT_COUNT; i++) {
        const cal_ct_cfg_t *c = cal_ct(i);
        n += snprintf(page + n, sizeof(page) - n,
            "<h3>%s</h3><p>pedestal <b>%lu-%lu mV</b>, reading %s</p>"
            "<form method=post action='/api/cal/ct'>"
            "<input type=hidden name=ct value='%s'>"
            "A per V <input name=apv size=6 value='%u.%02u'> "
            "turns <input name=turns size=3 value='%u'> "
            "trip A <input name=oc size=5 value='%u.%u'> "
            "<button>Save</button></form>",
            cal_ct_label(i), (unsigned long)ct_lo[i], (unsigned long)ct_hi[i],
            ct_a[i] < 0 ? "none (no clamp or no pedestal)" : "live",
            cal_ct_key(i),
            c->amps_per_volt_x100 / 100, c->amps_per_volt_x100 % 100,
            c->turns, c->oc_deci_amps / 10, c->oc_deci_amps % 10);
    }
    n += snprintf(page + n, sizeof(page) - n,
        "<p><small>An SCT-013-030 is nominally 30 A per volt, but two-point "
        "calibrate against a clamp meter: this is a trend instrument and "
        "consistency matters more than absolute accuracy. <b>Turns</b> is how many "
        "times the conductor passes through the jaws &mdash; the reading divides by "
        "it. A pedestal that is not ~1650 mV means the breakout is not right, and "
        "no current will be reported at all.</small></p></fieldset>");

    n += snprintf(page + n, sizeof(page) - n,
        "<fieldset><legend>Battery room fan</legend>"
        "<form method=post action='/api/cal/fan'>"
        "on above <input name=on size=5 value='%u.%u'> &deg;C, "
        "off below <input name=off size=5 value='%u.%u'> &deg;C "
        "<button>Save</button></form>"
        "<p><small>%d.%d&ndash;%d.%d &deg;C, and ON must be at least %d.%d &deg;C "
        "above OFF. If this hub goes quiet for five minutes the node falls back to "
        "its own hotter backstop.</small></p></fieldset>",
        cal_fan_on_deci_c() / 10, cal_fan_on_deci_c() % 10,
        cal_fan_off_deci_c() / 10, cal_fan_off_deci_c() % 10,
        FAN_LIMIT_LOW_DECI / 10, FAN_LIMIT_LOW_DECI % 10,
        FAN_LIMIT_HIGH_DECI / 10, FAN_LIMIT_HIGH_DECI % 10,
        FAN_MIN_HYST_DECI / 10, FAN_MIN_HYST_DECI % 10);

    n += snprintf(page + n, sizeof(page) - n,
        "<fieldset><legend>This page's password</legend>"
        "<form method=post action='/api/cal/pass'>"
        "new password <input name=pass type=password size=20> <button>Change</button></form>"
        "<p><small>8&ndash;32 characters. User is <code>%s</code>. Served over plain "
        "HTTP on the local network &mdash; adequate for a calibration constant, not "
        "for anything that moves water.</small></p></fieldset>", CAL_USER);

    if (n >= (int)sizeof(page)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, n);
}

/* ------------------------------------------------------------ POST plumbing */

/* Read the body and pull one field out of an application/x-www-form-urlencoded
 * payload. No URL-decoding: every field on this page is a number or a password,
 * and a password containing '%' or '+' would be silently mangled by a partial
 * decoder - which is worse than rejecting it. Passwords are validated for length
 * only, so the honest limit is documented rather than half-implemented.
 * ponytail: no percent-decoding. Add it if a field ever needs to carry '&' or '='. */
static bool form_field(const char *body, const char *name, char *out, size_t out_len)
{
    char needle[24];
    snprintf(needle, sizeof(needle), "%s=", name);

    const char *p = body;
    size_t nlen = strlen(needle);
    while (p) {
        if (strncmp(p, needle, nlen) == 0) {
            const char *v = p + nlen;
            const char *end = strchr(v, '&');
            size_t len = end ? (size_t)(end - v) : strlen(v);
            if (len >= out_len) {
                return false;
            }
            memcpy(out, v, len);
            out[len] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t len)
{
    if (req->content_len >= len) {
        return ESP_FAIL;
    }
    int got = httpd_req_recv(req, buf, req->content_len);
    if (got <= 0) {
        return ESP_FAIL;
    }
    buf[got] = '\0';
    return ESP_OK;
}

static esp_err_t redirect_back(httpd_req_t *req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/cal");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t bad(httpd_req_t *req, const char *why)
{
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, why, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Tenths of a unit from a decimal string, without dragging in strtod's locale
 * behaviour: "38.5" -> 385, "38" -> 380. */
static bool parse_deci(const char *s, uint16_t *out)
{
    unsigned whole = 0, frac = 0;
    char *end = NULL;
    whole = (unsigned)strtoul(s, &end, 10);
    if (end == s) {
        return false;
    }
    if (*end == '.' && end[1] >= '0' && end[1] <= '9') {
        frac = (unsigned)(end[1] - '0');
    }
    if (whole > 6000) {
        return false;
    }
    *out = (uint16_t)(whole * 10 + frac);
    return true;
}

static bool parse_x100(const char *s, uint16_t *out)
{
    unsigned whole = 0, frac = 0;
    char *end = NULL;
    whole = (unsigned)strtoul(s, &end, 10);
    if (end == s) {
        return false;
    }
    if (*end == '.') {
        if (end[1] >= '0' && end[1] <= '9') {
            frac = (unsigned)(end[1] - '0') * 10;
            if (end[2] >= '0' && end[2] <= '9') {
                frac += (unsigned)(end[2] - '0');
            }
        }
    }
    if (whole > 600) {
        return false;
    }
    *out = (uint16_t)(whole * 100 + frac);
    return true;
}

static int key_index(const char *v, const char *(*keyfn)(int), int count)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(v, keyfn(i)) == 0) {
            return i;
        }
    }
    return -1;
}

static const char *tank_key_i(int i) { return cal_tank_key((cal_tank_t)i); }
static const char *ct_key_i(int i)   { return cal_ct_key((cal_ct_t)i); }

static esp_err_t cal_tank_post(httpd_req_t *req)
{
    if (!authorised(req)) return deny(req);

    char body[192], f[16];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return bad(req, "body too long");

    if (!form_field(body, "tank", f, sizeof(f))) return bad(req, "need tank");
    int idx = key_index(f, tank_key_i, CAL_TANK_COUNT);
    if (idx < 0) return bad(req, "unknown tank");

    char fs[16], es[16];
    if (!form_field(body, "full", fs, sizeof(fs)) ||
        !form_field(body, "empty", es, sizeof(es))) {
        return bad(req, "need full and empty");
    }

    unsigned full = (unsigned)strtoul(fs, NULL, 10);
    unsigned empty = (unsigned)strtoul(es, NULL, 10);
    if (full > 6000 || empty > 6000) return bad(req, "distances are millimetres, max 6000");

    if (cal_set_tank((cal_tank_t)idx, (uint16_t)full, (uint16_t)empty) != ESP_OK) {
        return bad(req, "rejected: empty must exceed full, and full must clear the blind zone");
    }
    return redirect_back(req);
}

static esp_err_t cal_ct_post(httpd_req_t *req)
{
    if (!authorised(req)) return deny(req);

    char body[192], f[16];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return bad(req, "body too long");

    if (!form_field(body, "ct", f, sizeof(f))) return bad(req, "need ct");
    int idx = key_index(f, ct_key_i, CAL_CT_COUNT);
    if (idx < 0) return bad(req, "unknown clamp");

    char apv[16], turns[8], oc[16];
    if (!form_field(body, "apv", apv, sizeof(apv)) ||
        !form_field(body, "turns", turns, sizeof(turns)) ||
        !form_field(body, "oc", oc, sizeof(oc))) {
        return bad(req, "need apv, turns and oc");
    }

    uint16_t apv_x100 = 0, oc_deci = 0;
    if (!parse_x100(apv, &apv_x100)) return bad(req, "amps per volt not a number");
    if (!parse_deci(oc, &oc_deci))   return bad(req, "trip current not a number");
    unsigned t = (unsigned)strtoul(turns, NULL, 10);

    if (cal_set_ct((cal_ct_t)idx, apv_x100, (uint8_t)t, oc_deci) != ESP_OK) {
        return bad(req, "rejected: check A/V (1-200), turns (1-10) and trip (1.0-30.0 A)");
    }
    return redirect_back(req);
}

static esp_err_t cal_fan_post(httpd_req_t *req)
{
    if (!authorised(req)) return deny(req);

    char body[192], on[16], off[16];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return bad(req, "body too long");
    if (!form_field(body, "on", on, sizeof(on)) ||
        !form_field(body, "off", off, sizeof(off))) {
        return bad(req, "need on and off");
    }

    uint16_t on_deci = 0, off_deci = 0;
    if (!parse_deci(on, &on_deci) || !parse_deci(off, &off_deci)) {
        return bad(req, "thresholds not numbers");
    }
    if (cal_set_fan(on_deci, off_deci) != ESP_OK) {
        return bad(req, "rejected: 25.0-55.0 C, and ON at least 1.0 C above OFF");
    }
    return redirect_back(req);
}

static esp_err_t cal_pass_post(httpd_req_t *req)
{
    if (!authorised(req)) return deny(req);

    char body[128], pass[48];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return bad(req, "body too long");
    if (!form_field(body, "pass", pass, sizeof(pass))) return bad(req, "need pass");

    if (cal_set_password(pass) != ESP_OK) {
        return bad(req, "rejected: 8-32 characters");
    }
    /* Deliberately not a redirect: the browser still holds the old credentials
     * and would immediately fail auth, which reads as a broken page rather than
     * as a successful change. */
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req,
        "<p>Password changed. Close the browser or clear its saved credentials, "
        "then reopen <a href='/cal'>/cal</a>.</p>", HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------------------- start */

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WEB_PORT;
    cfg.max_uri_handlers = 10;
    cfg.lru_purge_enable = true;
    /* The dashboard polls once a second and a phone may be open at the same time
     * as a wall display. The default of 4 sockets runs out sooner than you would
     * think once keep-alives are in play. */
    cfg.max_open_sockets = 7;
    cfg.stack_size = 6144;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        { .uri = "/",               .method = HTTP_GET,  .handler = dashboard_get },
        { .uri = "/api/telemetry",  .method = HTTP_GET,  .handler = telemetry_get },
        { .uri = "/cal",            .method = HTTP_GET,  .handler = cal_get },
        { .uri = "/api/cal/tank",   .method = HTTP_POST, .handler = cal_tank_post },
        { .uri = "/api/cal/ct",     .method = HTTP_POST, .handler = cal_ct_post },
        { .uri = "/api/cal/fan",    .method = HTTP_POST, .handler = cal_fan_post },
        { .uri = "/api/cal/pass",   .method = HTTP_POST, .handler = cal_pass_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }

    ESP_LOGI(TAG, "dashboard on :%d, calibration at /cal", WEB_PORT);
    return ESP_OK;
}
