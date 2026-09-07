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
 * Fields the hub cannot know are reported as OFFLINE rather than as zeros: the
 * ground sump, the borewell and the sump motor live on nodes 0x05 and 0x06 over
 * Wi-Fi, and a node with no IP typed on /cal is not fitted at all. The dashboard
 * already renders an offline node correctly, so an honest OFFLINE draws hatching
 * and no liquid, where a zero would draw an empty tank that looks measured.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_app_desc.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

#include "app_cal.h"
#include "app_priv.h"
#include "app_rs485.h"
#include "app_sensors.h"
#include "app_web.h"

/* Implemented in app_main.c: the poll task owns the relays and the RS485 bus,
 * so a web handler asks rather than drives. */
bool relay_test_start(int n);

static const char *TAG = "web";

/* dashboard.html is gzipped and embedded at build time - see main/CMakeLists.txt.
 * Serving it from flash rather than SPIFFS means one less partition, one less
 * mount to fail, and no way for the page and the firmware to be different ages.
 * Embedded as BINARY, so there is no trailing NUL and the length is exactly
 * end - start. */
extern const uint8_t dashboard_gz_start[] asm("_binary_dashboard_html_gz_start");
extern const uint8_t dashboard_gz_end[]   asm("_binary_dashboard_html_gz_end");

/* ---------------------------------------------------------------- helpers */

/* Both of these were literals in the format string - 0 and "Power on" - written
 * as placeholders and never replaced, so the page confidently reported a signal
 * strength of 0 dBm and a clean power-on after every crash. The reset reason is
 * the one that costs you: PANIC and BROWNOUT are exactly what you want to see on
 * a board whose supply has already killed one chip, and "Power on" hid them. */
static int wifi_rssi(void)
{
    wifi_ap_record_t ap;
    /* 0 dBm is not a possible reading, so it doubles as "not associated". */
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

static const char *reset_word(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "Power on";
    case ESP_RST_EXT:       return "External";
    case ESP_RST_SW:        return "Software";
    case ESP_RST_PANIC:     return "Panic";
    case ESP_RST_INT_WDT:   return "Interrupt WDT";
    case ESP_RST_TASK_WDT:  return "Task WDT";
    case ESP_RST_WDT:       return "Watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep";
    case ESP_RST_BROWNOUT:  return "Brownout";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "Unknown";
    }
}

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

static const char *tank_state_word(const tank_state_t *t, bool online)
{
    /* The node not answering outranks anything its last reading said. Checking
     * pct first reported SENSOR_ERROR for a tank whose node is simply offline,
     * which points the finger at the transducer when the fault is on the bus -
     * and it printed alongside "sensor":"OK", which is a contradiction on its
     * face. The dosing tank is wired to the hub and has no node, so it passes
     * true: there is no link for it to lose. */
    if (!online) {
        return "OFFLINE";
    }
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
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"RO Hub\"");
    httpd_resp_send(req, "This page needs the hub password.", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

typedef struct {
    const char      *uri;
    httpd_method_t   method;
    esp_err_t      (*fn)(httpd_req_t *);
    bool             open;      /* true = reachable without the password */
} route_t;

/* Every request lands here first. Nothing reaches a handler without passing
 * this, so "protected" is the default and "open" is a decision someone had to
 * write down. */
static esp_err_t gate(httpd_req_t *req)
{
    const route_t *r = (const route_t *)req->user_ctx;
    if (!r->open && !authorised(req)) {
        return deny(req);
    }
    return r->fn(req);
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

/* ppm and water temperature as JSON, or null. Both or neither: the node sets
 * its two fault bits together because TDS without a temperature is
 * uncompensated (RS485_PROTOCOL.md 4.5), so half a reading is never emitted. */
static void wq_json(char *ppm, size_t ppm_len, char *temp, size_t temp_len,
                    const wq_state_t *wq)
{
    if (!wq->fitted || wq->ppm == TDS_INVALID) {
        snprintf(ppm, ppm_len, "null");
        snprintf(temp, temp_len, "null");
        return;
    }
    snprintf(ppm, ppm_len, "%u", wq->ppm);
    snprintf(temp, temp_len, "%d.%d", wq->temp_deci_c / 10, abs(wq->temp_deci_c % 10));
}

/* Deci-amps as JSON. Same rule as the pump amps: a channel with no clamp is -1
 * and must reach the page as null, not as 0.0. */
static void da_json(char *out, size_t n, int16_t da)
{
    if (da < 0) snprintf(out, n, "null");
    else        snprintf(out, n, "%d.%d", da / 10, da % 10);
}

/* [4.1,4.0,null] - the three clamp channels of a remote motor. */
static void phases_json(char *out, size_t n, const int16_t da[3])
{
    char a[12], b[12], c[12];
    da_json(a, sizeof a, da[0]); da_json(b, sizeof b, da[1]); da_json(c, sizeof c, da[2]);
    snprintf(out, n, "[%s,%s,%s]", a, b, c);
}

/* A node's firmware string is text from another device, and it lands both in
 * this JSON and in /cal's HTML. One quote in it breaks the document and the
 * dashboard silently drops to its demo simulator, so keep it to the characters a
 * version string actually uses. */
static void fw_word(char *out, size_t n, const char *fw)
{
    size_t o = 0;
    for (; fw[o] && o + 1 < n; o++) {
        char c = fw[o];
        out[o] = (c > 0x20 && c < 0x7f && !strchr("\"\\<>&'", c)) ? c : '?';
    }
    out[o] = '\0';
}

/* A node with no IP typed on /cal is not fitted; a node that has one and is not
 * answering is offline. The dashboard draws both as hatching and no value, which
 * is the honest rendering - a zero would look measured. */
static const char *gf_node_state(bool configured, bool online)
{
    return (configured && online) ? "ONLINE" : "OFFLINE";
}

/* Every browser asks for /favicon.ico on every page, unprompted, and an ESP-IDF
 * http server logs a warning for each miss:
 *
 *     W httpd_uri: URI '/favicon.ico' not found
 *     W httpd_txrx: httpd_resp_send_err: 404 Not Found
 *
 * Two warnings per page load, in a log whose whole value is that a warning in it
 * means something. Answering costs less than filtering: an inline SVG needs no
 * file, no SPIFFS, no binary blob in flash, and one route covers the dashboard,
 * /cal and anything added later - which a <link rel="icon"> in each page would
 * not.
 *
 * A drop, in the treated-water blue the dashboard already uses for permeate.
 */
static esp_err_t favicon_get(httpd_req_t *req)
{
    static const char ICON[] =
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'>"
        "<path d='M16 3C10 12 7 16 7 20a9 9 0 0 0 18 0c0-4-3-8-9-17z' fill='#2B8FD4'/>"
        "</svg>";
    httpd_resp_set_type(req, "image/svg+xml");
    /* A day: it never changes between reboots, and re-fetching it on every poll
     * of a page that refreshes once a second would be absurd. */
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, ICON, sizeof(ICON) - 1);
}

/*
 * 24 h of one-minute rows, oldest first, as a compact array-of-arrays:
 *   [t, rwt%, twt%, dos%, flags, hpp_dA, rwp_dA, ro_dC, bat_dC, sump%, bore_dA, smot_dA, util_dC]
 * -1 / null where there was no reading. ~60 KB at full depth, so it goes out in
 * chunks rather than through a static buffer. Read-only, no password, same as
 * /api/telemetry - this is what the dashboard draws its trend strip from.
 */
static esp_err_t history_get(httpd_req_t *req)
{
    char buf[1400];
    int n = snprintf(buf, sizeof(buf), "{\"period_s\":%d,\"rows\":[", HIST_PERIOD_S);
    httpd_resp_set_type(req, "application/json");

    hub_state_lock();
    uint16_t count = history_count();
    for (uint16_t i = 0; i < count; i++) {
        const hist_rec_t *r = history_at(i);
        char ro[8], bat[8], ut[8];
        if (r->ro_t == INT16_MIN) snprintf(ro, sizeof ro, "null"); else snprintf(ro, sizeof ro, "%d", r->ro_t);
        if (r->bat_t == INT16_MIN) snprintf(bat, sizeof bat, "null"); else snprintf(bat, sizeof bat, "%d", r->bat_t);
        if (r->util_t == INT16_MIN) snprintf(ut, sizeof ut, "null"); else snprintf(ut, sizeof ut, "%d", r->util_t);
        n += snprintf(buf + n, sizeof(buf) - n, "%s[%lu,%d,%d,%d,%u,%d,%d,%s,%s,%d,%d,%d,%s]",
                      i ? "," : "", (unsigned long)r->t, r->rwt, r->twt, r->dos, r->flags,
                      r->hpp_da, r->rwp_da, ro, bat, r->sump, r->bore_da, r->smot_da, ut);
        if (n > (int)sizeof(buf) - 100) {
            hub_state_unlock();
            if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) return ESP_FAIL;
            n = 0;
            hub_state_lock();
            /* The ring may have advanced by one during the unlock; count is
             * re-read so we never run past the end, and one duplicated or
             * skipped minute is invisible on a 24 h strip. */
            count = history_count();
        }
    }
    hub_state_unlock();
    /* The day ledger rides along: [local_midnight, hpp_min, rwp_min], oldest
     * first, up to CAL_DAYS entries. Week and month totals are sums over it. */
    n += snprintf(buf + n, sizeof(buf) - n, "],\"days\":[");
    const cal_day_t *d; uint16_t nd = cal_days(&d);
    for (uint16_t i = 0; i < nd; i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s[%lu,%u,%u]", i ? "," : "",
                      (unsigned long)d[i].midnight, d[i].hpp_min, d[i].rwp_min);
        if (n > (int)sizeof(buf) - 80) {
            if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) return ESP_FAIL;
            n = 0;
        }
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}");
    if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* [up_s, kind, arg, a, b], oldest first. The dashboard turns kind into words and
 * up_s into a wall-clock time from the telemetry's uptime. */
static esp_err_t events_get(httpd_req_t *req)
{
    static char buf[EVENTS_N * 32 + 64];
    int n = snprintf(buf, sizeof(buf), "{\"events\":[");
    hub_state_lock();
    uint16_t count = events_count();
    for (uint16_t i = 0; i < count && n < (int)sizeof(buf) - 40; i++) {
        const event_t *e = event_at(i);
        n += snprintf(buf + n, sizeof(buf) - n, "%s[%lu,%u,%u,%u,%u]", i ? "," : "",
                      (unsigned long)e->up_s, e->kind, e->arg, e->a, e->b);
    }
    hub_state_unlock();
    n += snprintf(buf + n, sizeof(buf) - n, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t telemetry_get(httpd_req_t *req)
{
    static char json[4200];   /* +200 quality, +300 run block, +900 ground floor */

    hub_state_lock();
    const hub_state_t *s = hub_state();

    /* A clamp that is not fitted, or a bias pedestal that is not there, gives
     * deci_amps < 0. That has to reach the page as null, not as 0.0: zero amps
     * is what an idle pump reads, so conflating "idle" with "not measured" would
     * make a missing clamp look like a stopped motor. Same rule as an
     * uncalibrated tank refusing to report a percentage. */
    char hpp_amps[12], rwp_amps[12];
    if (s->hpp.deci_amps < 0) {
        snprintf(hpp_amps, sizeof(hpp_amps), "null");
    } else {
        snprintf(hpp_amps, sizeof(hpp_amps), "%d.%d", s->hpp.deci_amps / 10, s->hpp.deci_amps % 10);
    }
    if (s->rwp.deci_amps < 0) {
        snprintf(rwp_amps, sizeof(rwp_amps), "null");
    } else {
        snprintf(rwp_amps, sizeof(rwp_amps), "%d.%d", s->rwp.deci_amps / 10, s->rwp.deci_amps % 10);
    }

    /* Same rule as the amps above, and it matters more here: 0 ppm is what
     * distilled water reads, so an unfitted or faulty probe reporting 0 would
     * look like the cleanest water the plant has ever made. null, not zero. */
    char rwt_ppm[12], twt_ppm[12], rwt_wt[12], twt_wt[12], rejection[12];
    wq_json(rwt_ppm, sizeof(rwt_ppm), rwt_wt, sizeof(rwt_wt), &s->rwt_wq);
    wq_json(twt_ppm, sizeof(twt_ppm), twt_wt, sizeof(twt_wt), &s->twt_wq);
    if (s->rejection_pct < 0) {
        snprintf(rejection, sizeof(rejection), "null");
    } else {
        snprintf(rejection, sizeof(rejection), "%d", s->rejection_pct);
    }

    /* Safe to interpolate into JSON unescaped: rs485_error_report() emits only
     * hex addresses, fixed command mnemonics, digits, '/' and spaces. If a new
     * command name ever contains a quote or a backslash, escape it here. */
    char rs485_failures[160];
    rs485_error_report(rs485_failures, sizeof(rs485_failures));

    char bore_amps[12], smot_amps[12], bore_ph[40], smot_ph[40];
    da_json(bore_amps, sizeof bore_amps, s->borewell.deci_amps);
    da_json(smot_amps, sizeof smot_amps, s->sump_motor.deci_amps);
    phases_json(bore_ph, sizeof bore_ph, s->borewell.phase_da);
    phases_json(smot_ph, sizeof smot_ph, s->sump_motor.phase_da);

    /* gf_apply() clears only pct and distance when the sump node stops
     * answering, so sump_pressure and sensor still hold what it last said. This
     * is the first field to read them, so the staleness is filtered here rather
     * than published: no node, no source. */
    bool sump_live = s->sump_configured && s->sump_online;
    const char *sump_sensor = !s->sump_configured ? "Not configured"
                            : (!s->sump_online ? "Offline" : sensor_word(s->sump.sensor));
    const char *sump_source = !sump_live ? "none" : (s->sump_pressure ? "pressure" : "ultrasonic");

    char sump_fw[16], util_fw[16];
    fw_word(sump_fw, sizeof sump_fw, s->sump_fw);
    fw_word(util_fw, sizeof util_fw, s->utility_fw);

    int n = snprintf(json, sizeof(json),
        "{"
        "\"sys\":{\"uptime_s\":%lld,\"rssi\":%d,\"fw\":\"%s\",\"reset_reason\":\"%s\",\"heap_free\":%u,\"heap_min\":%u},"
        "\"rs485\":{\"online\":%d,\"total\":3,\"errors\":%lu,\"last_poll_ms\":%lu,"
          "\"failures\":\"%s\"},"
        "\"tanks\":{"
          "\"sump\":{\"pct\":%d,\"distance_mm\":%u,\"state\":\"%s\",\"sensor\":\"%s\",\"source\":\"%s\"},"
          "\"rwt\":{\"pct\":%d,\"distance_mm\":%u,\"state\":\"%s\",\"sensor\":\"%s\"},"
          "\"dosing\":{\"pct\":%d,\"distance_mm\":%u,\"state\":\"%s\",\"sensor\":\"%s\"},"
          "\"twt\":{\"pct\":%d,\"distance_mm\":%u,\"state\":\"%s\",\"sensor\":\"%s\"}"
        "},"
        "\"pumps\":{"
          "\"borewell\":{\"on\":%s,\"state\":\"%s\"},"
          "\"sump_motor\":{\"on\":%s,\"state\":\"%s\"},"
          "\"rwp\":{\"on\":%s,\"state\":\"ONLINE\"},"
          "\"hpp\":{\"on\":%s,\"state\":\"ONLINE\"}"
        "},"
        "\"quality\":{"
          "\"rwt\":{\"ppm\":%s,\"t\":%s,\"fitted\":%s,\"live\":%s,\"age_s\":%d},"
          "\"twt\":{\"ppm\":%s,\"t\":%s,\"fitted\":%s,\"live\":%s,\"age_s\":%d},"
          "\"rejection\":%s"
        "},"
        "\"aster\":{\"twt_floty\":%s,\"rwt_floty\":%s,\"sump_floty\":false,"
                   "\"dos_lvl\":false,\"rl1\":%s,\"rl2\":%s,\"alarm\":%s,\"lps\":%s},"
        "\"env\":{"
          "\"ro_room\":{\"t\":%d.%d,\"rh\":%d.%d,\"state\":\"%s\",\"src\":\"SHT30 . I2C 0x44\",\"age_s\":%d},"
          "\"battery_room\":{\"t\":%d.%d,\"rh\":%d.%d,\"fan\":%s,\"state\":\"%s\",\"src\":\"SHT30 . Node 0x04\",\"age_s\":%d},"
          "\"utility_room\":{\"t\":%d.%d,\"rh\":%d.%d,\"state\":\"%s\",\"src\":\"SHT30 . Node 0x06\",\"age_s\":%d}"
        "},"
        "\"motors\":{"
          "\"hpp\":{\"amps\":%s,\"mv_lo\":%lu,\"mv_hi\":%lu},"
          "\"rwp\":{\"amps\":%s,\"mv_lo\":%lu,\"mv_hi\":%lu},"
          "\"borewell\":{\"amps\":%s,\"phases\":%s,\"imbalance_pct\":%u,\"running\":%s},"
          "\"sump_motor\":{\"amps\":%s,\"phases\":%s,\"imbalance_pct\":%u,\"running\":%s},"
          "\"overcurrent\":%s,\"no_production\":%s"
        "},"
        "\"run\":{"
          "\"hpp\":{\"today_s\":%lu,\"starts\":%u,\"total_s\":%lu},"
          "\"rwp\":{\"today_s\":%lu,\"starts\":%u,\"total_s\":%lu},"
          "\"plant_lph\":%u"
        "},"
        "\"nodes\":["
          "{\"id\":\"0x02\",\"role\":\"Raw Water\",\"link\":\"RS485\",\"state\":\"%s\",\"age_s\":%d},"
          "{\"id\":\"0x03\",\"role\":\"Treated Water\",\"link\":\"RS485\",\"state\":\"%s\",\"age_s\":%d},"
          "{\"id\":\"0x04\",\"role\":\"Battery Room\",\"link\":\"RS485\",\"state\":\"%s\",\"age_s\":%d},"
          "{\"id\":\"0x05\",\"role\":\"Sump\",\"link\":\"Wi-Fi\",\"state\":\"%s\",\"age_s\":%d,\"ip\":\"%s\",\"fw\":\"%s\"},"
          "{\"id\":\"0x06\",\"role\":\"Utility\",\"link\":\"Wi-Fi\",\"state\":\"%s\",\"age_s\":%d,\"ip\":\"%s\",\"fw\":\"%s\"}"
        "]"
        "}",
        esp_timer_get_time() / 1000000, wifi_rssi(),
        esp_app_get_description()->version, reset_word(),
        /* Both, because they answer different questions. free_heap bounces with
         * every HTTP response and says almost nothing on its own; the minimum
         * EVER seen since boot only moves one way, so a slow leak shows there
         * long before it shows as a reboot - which is the failure this hub would
         * otherwise announce by simply restarting one night. */
        (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
        (s->rwt_online ? 1 : 0) + (s->twt_online ? 1 : 0) + (s->battery_online ? 1 : 0),
        (unsigned long)rs485_error_count(), (unsigned long)s->last_cycle_ms,
        /* Empty string on a healthy bus. A plain string rather than nested JSON
         * on purpose: it is a diagnostic to read, not a series to chart, and the
         * dashboard shows it verbatim. */
        rs485_failures,

        s->sump.pct < 0 ? 0 : s->sump.pct, s->sump.distance_mm,
        gf_node_state(s->sump_configured, s->sump_online), sump_sensor, sump_source,
        s->rwt.pct < 0 ? 0 : s->rwt.pct, s->rwt.distance_mm,
        tank_state_word(&s->rwt, s->rwt_online), sensor_word(s->rwt.sensor),
        s->dosing.pct < 0 ? 0 : s->dosing.pct, s->dosing.distance_mm,
        tank_state_word(&s->dosing, true), sensor_word(s->dosing.sensor),
        s->twt.pct < 0 ? 0 : s->twt.pct, s->twt.distance_mm,
        tank_state_word(&s->twt, s->twt_online), sensor_word(s->twt.sensor),

        s->borewell.running ? "true" : "false",
        gf_node_state(s->utility_configured, s->utility_online),
        s->sump_motor.running ? "true" : "false",
        gf_node_state(s->utility_configured, s->utility_online),
        s->rwp.running ? "true" : "false",
        s->hpp.running ? "true" : "false",

        rwt_ppm, rwt_wt, s->rwt_wq.fitted ? "true" : "false",
        s->rwt_wq.live ? "true" : "false", age_s(s->rwt_wq.last_ok_us),
        twt_ppm, twt_wt, s->twt_wq.fitted ? "true" : "false",
        s->twt_wq.live ? "true" : "false", age_s(s->twt_wq.last_ok_us),
        rejection,

        s->twt_float_closed ? "true" : "false",
        s->rwt_floty == 1 ? "true" : "false",
        s->rl1_active ? "true" : "false",
        s->rl2_active ? "true" : "false",
        s->alarm_active ? "true" : "false",
        s->lps_active ? "true" : "false",

        s->ro_room.temp_deci_c / 10, abs(s->ro_room.temp_deci_c % 10),
        s->ro_room.hum_deci_pct / 10, s->ro_room.hum_deci_pct % 10,
        s->ro_room.fault ? "SENSOR_ERROR" : link_word(s->ro_room.last_ok_us, true),
        age_s(s->ro_room.last_ok_us),

        s->battery_room.temp_deci_c / 10, abs(s->battery_room.temp_deci_c % 10),
        s->battery_room.hum_deci_pct / 10, s->battery_room.hum_deci_pct % 10,
        s->fan_on ? "true" : "false",
        !s->battery_online ? "OFFLINE"
                           : (s->battery_room.fault ? "SENSOR_ERROR"
                                                    : link_word(s->battery_room.last_ok_us, true)),
        age_s(s->battery_room.last_ok_us),

        s->utility_room.temp_deci_c / 10, abs(s->utility_room.temp_deci_c % 10),
        s->utility_room.hum_deci_pct / 10, s->utility_room.hum_deci_pct % 10,
        !s->utility_online ? "OFFLINE"
                           : (s->utility_room.fault ? "SENSOR_ERROR" : "ONLINE"),
        age_s(s->utility_room.last_ok_us),

        hpp_amps, (unsigned long)s->hpp.mv_lo, (unsigned long)s->hpp.mv_hi,
        rwp_amps, (unsigned long)s->rwp.mv_lo, (unsigned long)s->rwp.mv_hi,
        bore_amps, bore_ph, (unsigned)s->borewell.imbalance_pct,
        s->borewell.running ? "true" : "false",
        smot_amps, smot_ph, (unsigned)s->sump_motor.imbalance_pct,
        s->sump_motor.running ? "true" : "false",
        s->overcurrent ? "true" : "false",
        s->no_production ? "true" : "false",

        (unsigned long)s->hpp_run_today_s, (unsigned)s->hpp_starts_today, (unsigned long)s->hpp_run_total_s,
        (unsigned long)s->rwp_run_today_s, (unsigned)s->rwp_starts_today, (unsigned long)s->rwp_run_total_s,
        (unsigned)cal_plant_lph(),

        link_word(s->rwt.last_ok_us, s->rwt_online), age_s(s->rwt.last_ok_us),
        link_word(s->twt.last_ok_us, s->twt_online), age_s(s->twt.last_ok_us),
        link_word(s->battery_room.last_ok_us, s->battery_online), age_s(s->battery_room.last_ok_us),

        gf_node_state(s->sump_configured, s->sump_online),
        age_s(s->sump_last_us), cal_gf_ip(CAL_GF_SUMP), sump_fw,
        gf_node_state(s->utility_configured, s->utility_online),
        age_s(s->utility_last_us), cal_gf_ip(CAL_GF_UTIL), util_fw);

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
    /* 8 K, not 4 K. The page was 4027 bytes against a 4096 buffer before the relay
     * test was added - 69 bytes of headroom, and one more sentence anywhere would
     * have tipped it. The guard at the end turns an overflow into a 500 rather
     * than a truncated page, which is the right failure, but it is still /cal
     * simply not opening. Static, so this is BSS rather than stack. */
    static char page[14336];   /* +1.5 k for the ground-floor rows and their notes */
    int n = 0;

    n += snprintf(page + n, sizeof(page) - n,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>RO Hub calibration</title>"
        "<style>body{font:15px/1.5 system-ui,sans-serif;margin:0 auto;padding:16px;max-width:44rem}"
        "h2{margin:0 0 4px}h3{margin:20px 0 6px}input{padding:4px;font:inherit}"
        "form{margin:6px 0}small{color:#666}fieldset{border:1px solid #ccc;border-radius:6px;margin:10px 0}fieldset{scroll-margin-top:12px}"
        "</style>"
        "<h2>RO Hub calibration</h2>"
        "<p><small>Distances are transducer face to liquid surface, in millimetres. "
        "Fill or empty the tank, read the live figure, then save it. Every value is "
        "range-checked before it is stored.</small></p>");

    hub_state_lock();
    const hub_state_t *s = hub_state();
    uint16_t live[CAL_TANK_COUNT]     = { s->rwt.distance_mm, s->twt.distance_mm, s->dosing.distance_mm, s->sump.distance_mm };
    int16_t  live_pct[CAL_TANK_COUNT] = { s->rwt.pct, s->twt.pct, s->dosing.pct, s->sump.pct };
    uint32_t ct_lo[CAL_CT_COUNT] = { s->hpp.mv_lo, s->rwp.mv_lo, 0, 0 };
    uint32_t ct_hi[CAL_CT_COUNT] = { s->hpp.mv_hi, s->rwp.mv_hi, 0, 0 };
    int16_t  ct_a[CAL_CT_COUNT]  = { s->hpp.deci_amps, s->rwp.deci_amps, s->borewell.deci_amps, s->sump_motor.deci_amps };
    int16_t  ct_ph[CAL_CT_COUNT][3] = {{0}};
    memcpy(ct_ph[CAL_CT_BORE], s->borewell.phase_da, sizeof(ct_ph[0]));
    memcpy(ct_ph[CAL_CT_SUMP], s->sump_motor.phase_da, sizeof(ct_ph[0]));
    bool sump_pressure = s->sump_pressure, sump_online = s->sump_online, util_online = s->utility_online;
    /* Copied, not pointed at: the lock is released two lines down and the poll
     * task rewrites these strings on every cycle. */
    char s_sump_fw[16], s_util_fw[16];
    fw_word(s_sump_fw, sizeof s_sump_fw, s->sump_fw);
    fw_word(s_util_fw, sizeof s_util_fw, s->utility_fw);
    hub_state_unlock();

    n += snprintf(page + n, sizeof(page) - n, "<fieldset id=tanks><legend>Tank levels</legend>");
    for (int i = 0; i < CAL_TANK_COUNT; i++) {
        const cal_tank_cfg_t *c = cal_tank(i);

        /* Show the percentage the CURRENT calibration produces, not just whether
         * one exists. Calibrating means typing two numbers while looking at a
         * tank, and the only useful feedback is "does that percentage match what
         * I can see". "measured" told you nothing. */
        char pctbuf[24];
        if (live[i] == 0) {
            snprintf(pctbuf, sizeof(pctbuf), "no echo");
        } else if (live_pct[i] < 0) {
            snprintf(pctbuf, sizeof(pctbuf), "no level - out of range");
        } else {
            snprintf(pctbuf, sizeof(pctbuf), "%d %%", live_pct[i]);
        }

        /* The sump has two possible sources and the shunt that picks one is on
         * the node, not here - so say which one the reading came from rather
         * than leaving somebody on a roof to guess. */
        const char *src = "";
        if (i == CAL_TANK_SUMP) {
            src = !sump_online ? " (node offline)" : (sump_pressure ? " (4-20 mA loop)" : " (ultrasonic)");
        }

        n += snprintf(page + n, sizeof(page) - n,
            "<h3>%s</h3><p>live <b>%u mm</b> &rarr; <b>%s</b>%s"
            "<br><small>full and empty are both distances from the transducer FACE "
            "to the water. Measure straight down. <b>full</b> must be at least "
            "%d mm or the top of the scale is inside the blind zone.</small></p>"
            "<form method=post action='/api/cal/tank'>"
            "<input type=hidden name=tank value='%s'>"
            "full <input name=full size=6 value='%u'> "
            "empty <input name=empty size=6 value='%u'> ",
            cal_tank_label(i), live[i], pctbuf, src, BLIND_ZONE_MM,
            cal_tank_key(i), c->full_mm, c->empty_mm);
        if (i == CAL_TANK_SUMP) {
            n += snprintf(page + n, sizeof(page) - n,
                "transducer range mm <input name=range size=6 value='%u'> ", c->press_range_mm);
        }
        n += snprintf(page + n, sizeof(page) - n, "<button>Save</button></form>");
    }
    n += snprintf(page + n, sizeof(page) - n,
        "<p><small>Empty must be a longer distance than full, and full must be "
        "outside the %d mm blind zone. Sump: transducer range is the 4-20 mA "
        "sensor's full scale in mm, 0 when the ultrasonic is fitted; the node's "
        "J-PRESS shunt decides which one it reads.</small></p></fieldset>", BLIND_ZONE_MM);

    n += snprintf(page + n, sizeof(page) - n, "<fieldset id=clamps><legend>Current clamps</legend>");
    for (int i = 0; i < CAL_CT_COUNT; i++) {
        const cal_ct_cfg_t *c = cal_ct(i);

        /* The hub's own clamps read one conductor off an ADC it owns, so the
         * useful feedback there is the bias pedestal. The remote pair has three
         * channels on another board: the per-phase amps are what tells you a
         * clamp is on backwards or not on at all. */
        char reading[64];
        if (i >= CAL_CT_BORE) {
            char p[40], a[3][8];
            for (int k = 0; k < 3; k++) {
                if (ct_ph[i][k] < 0) snprintf(a[k], sizeof a[k], "--");
                else snprintf(a[k], sizeof a[k], "%d.%d", ct_ph[i][k] / 10, ct_ph[i][k] % 10);
            }
            snprintf(p, sizeof p, "%s / %s / %s A", a[0], a[1], a[2]);
            snprintf(reading, sizeof reading, "%s%s", util_online ? "phases " : "node offline; last ", p);
        } else {
            snprintf(reading, sizeof reading, "pedestal %lu-%lu mV, reading %s",
                     (unsigned long)ct_lo[i], (unsigned long)ct_hi[i],
                     ct_a[i] < 0 ? "none (no clamp or no pedestal)" : "live");
        }

        n += snprintf(page + n, sizeof(page) - n,
            "<h3>%s</h3><p>%s</p>"
            "<form method=post action='/api/cal/ct'>"
            "<input type=hidden name=ct value='%s'>"
            "A per V <input name=apv size=6 value='%u.%02u'> "
            "turns <input name=turns size=3 value='%u'> "
            "run A <input name=run size=5 value='%u.%u'> "
            "trip A <input name=oc size=5 value='%u.%u'> "
            "<button>Save</button></form>",
            cal_ct_label(i), reading, cal_ct_key(i),
            c->amps_per_volt_x100 / 100, c->amps_per_volt_x100 % 100, c->turns,
            c->run_deci_amps / 10, c->run_deci_amps % 10,
            c->oc_deci_amps / 10, c->oc_deci_amps % 10);
    }
    n += snprintf(page + n, sizeof(page) - n,
        "<p><small>An SCT-013-030 is nominally 30 A per volt, but two-point "
        "calibrate against a clamp meter: this is a trend instrument and "
        "consistency matters more than absolute accuracy. <b>Turns</b> is how many "
        "times the conductor passes through the jaws &mdash; the reading divides by "
        "it. A pedestal that is not ~1650 mV means the breakout is not right, and "
        "no current will be reported at all. <b>Run A</b>: the borewell has no "
        "contact of its own, so it is running when its highest phase is above "
        "this. Borewell and sump motor share one calibration across their three "
        "channels.</small></p></fieldset>");

    n += snprintf(page + n, sizeof(page) - n,
        "<fieldset id=fan><legend>Battery room fan</legend>"
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

    n += snprintf(page + n, sizeof(page) - n, "<fieldset id=wq><legend>Water quality probes</legend>");
    for (int i = 0; i < 2; i++) {          /* RWT, TWT - dosing and sump have no TDS probe */
        const cal_tank_cfg_t *c = cal_tank(i);
        n += snprintf(page + n, sizeof(page) - n,
            "<form method=post action='/api/cal/wq'><input type=hidden name=tank value=%d>"
            "<b>%s</b> &nbsp; k <input name=k size=5 value='%u.%02u'> &nbsp; "
            "probe under water at or above <input name=min size=3 value='%u'> %% "
            "<button>Save</button></form>",
            i, i == CAL_TANK_RWT ? "RWT" : "TWT",
            c->tds_k_x100 / 100, c->tds_k_x100 % 100, c->tds_min_pct);
    }
    n += snprintf(page + n, sizeof(page) - n,
        "<p><small><b>k</b> scales the TDS reading: a 707 ppm sachet reading 640 wants "
        "707/640 = 1.10. Re-do it after extending a probe lead. <b>Under water at</b> is "
        "the level where the probe tip goes dry: below it the hub keeps the last good "
        "reading and the dashboard shows its age, instead of believing a probe in air. "
        "0.50&ndash;2.00 and 0&ndash;100.</small></p></fieldset>");

    n += snprintf(page + n, sizeof(page) - n,
        "<fieldset id=plant><legend>Plant output</legend>"
        "<form method=post action='/api/cal/plant'>"
        "rated permeate <input name=lph size=5 value='%u'> L/h <button>Save</button></form>"
        "<p><small>Multiplies HPP run hours into \"litres today\" on the dashboard. "
        "Nameplate is 1200; read the skid's flow meter while producing and put that "
        "here instead &mdash; it falls as the membranes age. %d&ndash;%d.</small></p></fieldset>",
        cal_plant_lph(), PLANT_LPH_MIN, PLANT_LPH_MAX);

    n += snprintf(page + n, sizeof(page) - n,
        "<fieldset id=gf><legend>Ground floor nodes</legend>"
        "<p><small>Wi-Fi nodes the hub polls every %d s. Give each a fixed address on the "
        "router (or in its firmware) and type it here. Empty = not fitted: nothing is polled "
        "and nothing alerts. <code>a.b.c.d</code>, or <code>a.b.c.d:port</code> for a bench "
        "node.</small></p>", GF_POLL_MS / 1000);
    static const char *gf_label[CAL_GF_COUNT] = { "Sump level node 0x05", "Utility room node 0x06" };
    bool gf_on[CAL_GF_COUNT] = { sump_online, util_online };
    const char *gf_fw[CAL_GF_COUNT] = { s_sump_fw, s_util_fw };
    for (int i = 0; i < CAL_GF_COUNT; i++) {
        const char *ip = cal_gf_ip(i);
        n += snprintf(page + n, sizeof(page) - n,
            "<h3>%s</h3><p>%s</p>"
            "<form method=post action='/api/cal/gf'>"
            "<input type=hidden name=node value='%s'>"
            "IP <input name=ip size=21 value='%s' placeholder='192.168.1.50'> "
            "<button>Save</button></form>",
            gf_label[i],
            ip[0] == '\0' ? "not fitted" : (gf_on[i] ? "online" : "configured, not answering"),
            cal_gf_key(i), ip);
        if (ip[0] && gf_on[i] && gf_fw[i][0]) {
            n += snprintf(page + n, sizeof(page) - n, "<p><small>fw %s</small></p>", gf_fw[i]);
        }
    }
    n += snprintf(page + n, sizeof(page) - n, "</fieldset>");

    n += snprintf(page + n, sizeof(page) - n,
        "<fieldset id=relays><legend>Relay test</legend>");
    for (int i = 0; i < RELAY_HUB_COUNT; i++) {
        n += snprintf(page + n, sizeof(page) - n,
            "<form method=post action='/api/cal/relay' style='display:inline'>"
            "<input type=hidden name=n value='%d'>"
            "<button>%d &middot; %s</button></form> ", i + 1, i + 1, relay_name(i));
    }
    n += snprintf(page + n, sizeof(page) - n,
        "<form method=post action='/api/cal/relay' style='display:inline'>"
        "<input type=hidden name=n value='%d'>"
        "<button>%d &middot; Battery fan</button></form>"
        "<p><small>Each button energises that relay for <b>about %d s</b> and the hub "
        "releases it &mdash; the release runs from the poll loop, so it lands on the "
        "next cycle and the pulse is %d&ndash;%d s rather than exact. That is the "
        "right trade: a timer of its own could be missed, whereas the poll loop runs "
        "whatever else fails, and a watchdog reset de-energises every relay anyway. "
        "There is no latch and no off button on purpose: relays 1&ndash;4 "
        "are float emulation, and energised is what tells the Aster the tank is full, "
        "the raw water is empty, or the dosing is low (&sect;7.2) &mdash; one left on "
        "stops the plant. Listen for the click and meter <code>COM</code>&ndash;"
        "<code>NC</code>; de-energised is closed. The fan is on node 0x04, so it "
        "answers on the next poll rather than instantly, and a test will not override "
        "a fan you have deliberately forced OFF.</small></p></fieldset>",
        RELAY_HUB_COUNT + 1, RELAY_HUB_COUNT + 1,
        RELAY_TEST_MS / 1000, RELAY_TEST_MS / 1000,
        (RELAY_TEST_MS + POLL_CYCLE_MS) / 1000);

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

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Read the body and pull one field out of an application/x-www-form-urlencoded
 * payload.
 *
 * %XX is decoded, because the node IP field carries a colon for "a.b.c.d:port"
 * and a browser sends that as %3A - undecoded it reaches gf_ip_valid() as three
 * characters and every bench address is rejected. '+' is deliberately left
 * alone: nothing on this page wants a leading or embedded space, and turning it
 * into one would change what an existing password means. %00 is left literal
 * too: decoded, it would end the value early and hand the caller a silently
 * truncated string.
 *
 * A value too long for `out` returns false, which every caller reads as "field
 * absent". Size the buffer for the longest ENCODED value a field can carry, or
 * a typo becomes a missing field rather than an error.
 * ponytail: %XX only, no '+'. Decode it too if a field ever needs a space. */
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
            if (len >= out_len) {   /* decoding only shrinks, so this is safe */
                return false;
            }
            size_t o = 0;
            for (size_t i = 0; i < len; i++) {
                int hi, lo;
                if (v[i] == '%' && i + 2 < len &&
                    (hi = hex_digit(v[i + 1])) >= 0 && (lo = hex_digit(v[i + 2])) >= 0 &&
                    (hi | lo) != 0) {   /* %00 would end the string early, silently */
                    out[o++] = (char)(hi * 16 + lo);
                    i += 2;
                } else {
                    out[o++] = v[i];
                }
            }
            out[o] = '\0';
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

/*
 * Post/Redirect/Get, back to the SECTION the form was in rather than the top of
 * the page. Every form here reloads /cal, and without a fragment you land at the
 * top each time - worst for the relay test, where the whole point is pressing
 * several buttons in a row while listening for clicks.
 *
 * `location` must outlive the response: httpd_resp_set_hdr stores the pointer
 * rather than copying it. Every caller passes a string literal, so it does.
 */
static esp_err_t redirect_to(httpd_req_t *req, const char *location)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location);
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

    /* Only the sump form carries this one, so its absence is not an error - and
     * for the same reason rs[] is far wider than the five digits a legal range
     * needs. A value too long for it would read as absent, and the save would
     * quietly do nothing instead of saying no. */
    char rs[16];
    if (form_field(body, "range", rs, sizeof(rs))) {
        unsigned r = (unsigned)strtoul(rs, NULL, 10);
        if (cal_set_press_range((cal_tank_t)idx, (uint16_t)r) != ESP_OK) {
            return bad(req, "rejected: transducer range must be 0 (ultrasonic) or 500-10000 mm");
        }
    }
    return redirect_to(req, "/cal#tanks");
}

static esp_err_t cal_ct_post(httpd_req_t *req)
{
    char body[192], f[16];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return bad(req, "body too long");

    if (!form_field(body, "ct", f, sizeof(f))) return bad(req, "need ct");
    int idx = key_index(f, ct_key_i, CAL_CT_COUNT);
    if (idx < 0) return bad(req, "unknown clamp");

    char apv[16], turns[8], oc[16], run[16];
    if (!form_field(body, "apv", apv, sizeof(apv)) ||
        !form_field(body, "turns", turns, sizeof(turns)) ||
        !form_field(body, "oc", oc, sizeof(oc)) ||
        !form_field(body, "run", run, sizeof(run))) {
        return bad(req, "need apv, turns, oc and run");
    }

    uint16_t apv_x100 = 0, oc_deci = 0, run_deci = 0;
    if (!parse_x100(apv, &apv_x100)) return bad(req, "amps per volt not a number");
    if (!parse_deci(oc, &oc_deci))   return bad(req, "trip current not a number");
    if (!parse_deci(run, &run_deci)) return bad(req, "run current not a number");
    unsigned t = (unsigned)strtoul(turns, NULL, 10);

    if (cal_set_ct((cal_ct_t)idx, apv_x100, (uint8_t)t, oc_deci, run_deci) != ESP_OK) {
        return bad(req, "rejected: check A/V (1-200), turns (1-10), run (>= 0.1 A) and trip (1.0-30.0 A, above run)");
    }
    return redirect_to(req, "/cal#clamps");
}

static esp_err_t cal_fan_post(httpd_req_t *req)
{
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
    return redirect_to(req, "/cal#fan");
}

static esp_err_t cal_wq_post(httpd_req_t *req)
{
    char body[128], f[8], k[12], mn[8];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return bad(req, "body too long");
    if (!form_field(body, "tank", f, sizeof(f)) ||
        !form_field(body, "k", k, sizeof(k)) ||
        !form_field(body, "min", mn, sizeof(mn))) {
        return bad(req, "need tank, k and min");
    }
    int idx = atoi(f);
    if (idx < 0 || idx > 1) return bad(req, "tank must be 0 or 1");
    /* k arrives as "1.10"; store x100. Two decimals is the resolution that matters. */
    double kd = atof(k);
    int    mp = atoi(mn);
    if (kd <= 0 || mp < 0 || mp > 100) return bad(req, "k or min not a number in range");
    if (cal_set_tds((cal_tank_t)idx, (uint16_t)(kd * 100.0 + 0.5), (uint8_t)mp) != ESP_OK) {
        return bad(req, "rejected: k 0.50-2.00, level 0-100");
    }
    return redirect_to(req, "/cal#wq");
}

static const char *gf_key_i(int i) { return cal_gf_key((cal_gf_t)i); }

static esp_err_t cal_gf_post(httpd_req_t *req)
{
    /* ip[] is bigger than the 24 cal_set_gf_ip() will accept, on purpose. The
     * input has no maxlength, and form_field() reports a value that does not fit
     * as absent - which here means "clear the node". One digit too many would
     * then unconfigure it behind a 303 that reads as a successful save. With
     * room to spare, an over-long address reaches cal_set_gf_ip() and is
     * rejected out loud. */
    char body[128], f[8], ip[32];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return bad(req, "body too long");
    if (!form_field(body, "node", f, sizeof(f))) return bad(req, "need node");
    int idx = key_index(f, gf_key_i, CAL_GF_COUNT);
    if (idx < 0) return bad(req, "unknown node");
    if (!form_field(body, "ip", ip, sizeof(ip))) ip[0] = '\0';   /* no field at all = clear */

    /* Trim the spaces a phone keyboard adds either side of a typed address. */
    char *q = ip; while (*q == ' ') q++;
    size_t l = strlen(q); while (l && q[l - 1] == ' ') q[--l] = '\0';

    if (cal_set_gf_ip((cal_gf_t)idx, q) != ESP_OK) {
        return bad(req, "rejected: a.b.c.d or a.b.c.d:port, or empty to remove");
    }
    return redirect_to(req, "/cal#gf");
}

static esp_err_t cal_plant_post(httpd_req_t *req)
{
    char body[64], lph[8];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return bad(req, "body too long");
    if (!form_field(body, "lph", lph, sizeof(lph))) return bad(req, "need lph");
    int v = atoi(lph);
    if (v <= 0 || cal_set_plant_lph((uint16_t)v) != ESP_OK) {
        return bad(req, "rejected: 100-5000 L/h");
    }
    return redirect_to(req, "/cal#plant");
}

/*
 * Momentary relay test. One button, one pulse, and the firmware releases it -
 * there is deliberately no "off" button and no latch.
 *
 * Every hub relay reads as a FAULT at the Aster when energised (WIRING.md 7.2),
 * so a latching control here would let somebody stop the plant by clicking a
 * thing and walking away. RELAY_TEST_MS is long enough to hear the click and get
 * a meter on the contact.
 *
 * Behind the password like everything else that changes something. That is not a
 * per-handler decision any more - gate() refuses anything not marked .open.
 */
static esp_err_t cal_relay_post(httpd_req_t *req)
{
    char body[64], nbuf[8];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return bad(req, "body too long");
    if (!form_field(body, "n", nbuf, sizeof(nbuf))) return bad(req, "need n");

    int n = atoi(nbuf);
    if (!relay_test_start(n)) {
        return bad(req, "relay must be 1-5");
    }
    return redirect_to(req, "/cal#relays");
}

static esp_err_t cal_pass_post(httpd_req_t *req)
{
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
    /* PUBLIC is opt-in. Every route goes through gate(), so a handler added
     * later is password-protected unless whoever adds it deliberately writes
     * .open = true next to it. The old arrangement put the check inside each
     * handler, which protects exactly the handlers somebody remembered - and
     * the next thing to land here is a relay toggle. */
    static const route_t routes[] = {
        { "/",              HTTP_GET,  dashboard_get,  true  },  /* read-only, no password */
        { "/api/telemetry", HTTP_GET,  telemetry_get,  true  },  /* what the dashboard polls */
        { "/api/history",   HTTP_GET,  history_get,    true  },  /* 24 h trend strip */
        { "/api/events",    HTTP_GET,  events_get,     true  },  /* what happened, when */
        { "/favicon.ico",   HTTP_GET,  favicon_get,    true  },  /* asked for unprompted, by everyone */
        { "/cal",           HTTP_GET,  cal_get,        false },
        { "/api/cal/tank",  HTTP_POST, cal_tank_post,  false },
        { "/api/cal/ct",    HTTP_POST, cal_ct_post,    false },
        { "/api/cal/fan",   HTTP_POST, cal_fan_post,   false },
        { "/api/cal/plant", HTTP_POST, cal_plant_post, false },
        { "/api/cal/wq",    HTTP_POST, cal_wq_post,    false },
        { "/api/cal/gf",    HTTP_POST, cal_gf_post,    false },
        { "/api/cal/relay", HTTP_POST, cal_relay_post, false },
        { "/api/cal/pass",  HTTP_POST, cal_pass_post,  false },
    };

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WEB_PORT;
    /* One slot per route, derived. This was a literal 10; 130c3dc added two
     * routes and the hub boot-looped on ESP_ERR_HTTPD_HANDLERS_FULL. */
    cfg.max_uri_handlers = sizeof(routes) / sizeof(routes[0]);
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

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_uri_t u = {
            .uri      = routes[i].uri,
            .method   = routes[i].method,
            .handler  = gate,
            .user_ctx = (void *)&routes[i],
        };
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u));
    }

    ESP_LOGI(TAG, "dashboard on :%d, calibration at /cal", WEB_PORT);
    return ESP_OK;
}
