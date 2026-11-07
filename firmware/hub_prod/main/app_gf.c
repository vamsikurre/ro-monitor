/*
 * app_gf.c — poll the ground-floor Wi-Fi nodes
 *
 * See app_gf.h. The pure part sits between GF_PARSE_BEGIN/END so
 * docs/check_gf.py can build it on the host against cJSON.
 */
#include <string.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_cal.h"
#include "app_gf.h"

static const char *TAG = "gf";

static SemaphoreHandle_t s_mux;
static gf_sump_t s_sump;
static gf_util_t s_util;
static QueueHandle_t s_cmdq;            /* gf_cmd_t, drained by gf_task */
static volatile bool s_cfg_dirty;       /* push /api/ctl/cfg after the next good utility poll */
#define GF_CMDQ_LEN 4

/* GF_PARSE_BEGIN */
static bool jint(const cJSON *o, const char *k, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(v)) return false;
    *out = v->valueint;
    return true;
}

/* Number or null. Returns false only if the key is missing or another type. */
static bool jint_or_null(const cJSON *o, const char *k, int *out, int if_null)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNull(v)) { *out = if_null; return true; }
    if (!cJSON_IsNumber(v)) return false;
    *out = v->valueint;
    return true;
}

static bool jlink(const cJSON *o, int want_id, gf_link_t *l)
{
    int id, up, rssi;
    if (!jint(o, "id", &id) || id != want_id) return false;
    const cJSON *fw = cJSON_GetObjectItemCaseSensitive(o, "fw");
    if (!cJSON_IsString(fw)) return false;
    if (!jint(o, "uptime_s", &up) || !jint(o, "rssi", &rssi)) return false;
    strncpy(l->fw, fw->valuestring, sizeof(l->fw) - 1);
    l->fw[sizeof(l->fw) - 1] = '\0';
    l->uptime_s = (uint32_t)up;
    l->rssi = (int8_t)rssi;
    return true;
}

static bool jmv3(const cJSON *o, const char *k, uint16_t out[3])
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != 3) return false;
    for (int i = 0; i < 3; i++) {
        const cJSON *v = cJSON_GetArrayItem(a, i);
        if (!cJSON_IsNumber(v) || v->valueint < 0 || v->valueint > 65535) return false;
        out[i] = (uint16_t)v->valueint;
    }
    return true;
}

bool gf_parse_sump(const char *json, gf_sump_t *out)
{
    cJSON *o = cJSON_Parse(json);
    if (o == NULL) return false;
    bool ok = false;
    gf_sump_t s = *out;
    int dist, q, ua;
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(o, "source");
    const cJSON *st  = cJSON_GetObjectItemCaseSensitive(o, "status");
    if (!jlink(o, 5, &s.link) || !cJSON_IsString(src) || !cJSON_IsString(st)) goto done;
    if (!jint_or_null(o, "distance_mm", &dist, 0) || !jint_or_null(o, "quality", &q, 0) ||
        !jint_or_null(o, "loop_ua", &ua, 0)) goto done;
    s.pressure    = strcmp(src->valuestring, "pressure") == 0;
    s.distance_mm = (uint16_t)(dist < 0 ? 0 : dist);
    s.quality     = (uint8_t)(q < 0 ? 0 : (q > 100 ? 100 : q));
    s.loop_ua     = (uint32_t)(ua < 0 ? 0 : ua);
    if      (strcmp(st->valuestring, "OK") == 0)       s.sensor = SENSOR_OK;
    else if (strcmp(st->valuestring, "BLIND") == 0)    s.sensor = SENSOR_BLIND;
    else if (strcmp(st->valuestring, "NO_ECHO") == 0)  s.sensor = SENSOR_NO_ECHO;
    else if (strcmp(st->valuestring, "HW_FAULT") == 0) s.sensor = SENSOR_HW_FAULT;
    else goto done;
    *out = s;
    ok = true;
done:
    cJSON_Delete(o);
    return ok;
}

bool gf_parse_util(const char *json, gf_util_t *out)
{
    cJSON *o = cJSON_Parse(json);
    if (o == NULL) return false;
    bool ok = false;
    gf_util_t u = *out;
    int t, rh;
    const cJSON *on  = cJSON_GetObjectItemCaseSensitive(o, "sump_on");
    const cJSON *fl  = cJSON_GetObjectItemCaseSensitive(o, "rwt_floty");
    const cJSON *sht = cJSON_GetObjectItemCaseSensitive(o, "sht_ok");
    if (!jlink(o, 6, &u.link)) goto done;
    if (!jmv3(o, "bore_mv", u.bore_mv) || !jmv3(o, "sump_mv", u.sump_mv)) goto done;
    if (!cJSON_IsBool(on) || !cJSON_IsBool(sht)) goto done;
    if (cJSON_IsNull(fl)) u.rwt_floty = -1;
    else if (cJSON_IsBool(fl)) u.rwt_floty = cJSON_IsTrue(fl) ? 1 : 0;
    else goto done;
    if (!jint(o, "t_deci_c", &t) || !jint(o, "rh_deci_pct", &rh)) goto done;
    u.sump_on      = cJSON_IsTrue(on);
    u.sht_ok       = cJSON_IsTrue(sht);
    u.temp_deci_c  = (int16_t)t;
    u.hum_deci_pct = (uint16_t)(rh < 0 ? 0 : rh);
    /* control fields: optional, reset every parse so nothing stale survives */
    const cJSON *fv = cJSON_GetObjectItemCaseSensitive(o, "float_low");
    u.float_low = cJSON_IsBool(fv) ? (cJSON_IsTrue(fv) ? 1 : 0) : -1;
    u.ctrl_node = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "ctrl_node"));
    u.bore_act  = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "bore_act"));
    u.sump_act  = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "sump_act"));
    int fg = 0;
    u.flags = (jint(o, "flags", &fg) && fg >= 0 && fg <= 65535) ? (uint16_t)fg : 0;
    const cJSON *ra = cJSON_GetObjectItemCaseSensitive(o, "relays");
    for (int i = 0; i < 3; i++)
        u.relays[i] = cJSON_IsArray(ra) && cJSON_IsTrue(cJSON_GetArrayItem(ra, i));
    const cJSON *lc = cJSON_GetObjectItemCaseSensitive(o, "last_cmd");
    u.last_cmd[0] = '\0';
    if (cJSON_IsString(lc)) {
        strncpy(u.last_cmd, lc->valuestring, sizeof(u.last_cmd) - 1);
        u.last_cmd[sizeof(u.last_cmd) - 1] = '\0';
    }
    *out = u;
    ok = true;
done:
    cJSON_Delete(o);
    return ok;
}

/* The latch. Same shape as the RS485 side's give-up-and-reprobe: a node is
 * not offline on one miss, and an offline node is still asked - just slowly. */
void gf_link_result(gf_link_t *l, bool ok, int64_t now_us)
{
    if (ok) {
        l->misses = 0;
        l->online = true;
        l->valid = true;
        l->last_ok_us = now_us;
    } else {
        if (l->misses < 255) l->misses++;
        if (l->misses >= GF_OFFLINE_MISSES) l->online = false;
    }
    l->next_poll_us = now_us + (int64_t)(l->online ? GF_POLL_MS : GF_REPROBE_MS) * 1000;
}

/* cJSON builds the bodies so the token is escaped by construction. Prints into
 * buf without allocating; false there means it did not fit. */
static int jprint(cJSON *o, char *buf, size_t len)
{
    int n = -1;
    if (o && len > 0 && len <= 0x7fffffff && cJSON_PrintPreallocated(o, buf, (int)len, false)) n = (int)strlen(buf);
    cJSON_Delete(o);
    return n;
}

static const char *const s_motor_name[] = { "bore", "sump" };
static const char *const s_cmd_name[]   = { "start", "stop" };

int gf_ctl_body(const gf_cmd_t *c, const char *token, char *buf, size_t len)
{
    if (!token || c->motor > GF_MOTOR_SUMP || c->cmd > GF_CMD_STOP ||
        (c->src != GF_SRC_APP && c->src != GF_SRC_DASH)) return -1;
    cJSON *o = cJSON_CreateObject();
    if (o && (!cJSON_AddStringToObject(o, "token", token) ||
              !cJSON_AddStringToObject(o, "motor", s_motor_name[c->motor]) ||
              !cJSON_AddStringToObject(o, "cmd", s_cmd_name[c->cmd]) ||
              !cJSON_AddStringToObject(o, "src", c->src == GF_SRC_APP ? "app" : "dash"))) {
        cJSON_Delete(o);
        return -1;
    }
    return jprint(o, buf, len);
}

int gf_cfg_body(const gf_ctl_cfg_t *g, const char *token, char *buf, size_t len)
{
    if (!token) return -1;
    cJSON *o = cJSON_CreateObject();
    bool ok = o && cJSON_AddStringToObject(o, "token", token)
        && cJSON_AddNumberToObject(o, "bore_apv_x100", g->bore_apv_x100)
        && cJSON_AddNumberToObject(o, "sump_apv_x100", g->sump_apv_x100)
        && cJSON_AddNumberToObject(o, "bore_turns", g->bore_turns)
        && cJSON_AddNumberToObject(o, "sump_turns", g->sump_turns)
        && cJSON_AddNumberToObject(o, "bore_run_da", g->bore_run_da)
        && cJSON_AddNumberToObject(o, "bore_dry_da", g->bore_dry_da)
        && cJSON_AddNumberToObject(o, "sump_noload_da", g->sump_noload_da)
        && cJSON_AddNumberToObject(o, "max_run_min", g->max_run_min)
        && cJSON_AddBoolToObject(o, "ctrl_node", g->ctrl_node)
        && cJSON_AddBoolToObject(o, "bore_act", g->bore_act)
        && cJSON_AddBoolToObject(o, "sump_act", g->sump_act);
    if (!ok) { cJSON_Delete(o); return -1; }
    return jprint(o, buf, len);
}

int gf_why_index(int status, const char *body)
{
    if (status == 200) return 0;
    static const char *const why[] = { "single_phase", "sump_hold", "not_settled", "sump_full", "no_float" };
    int idx = 15;
    cJSON *o = (status == 409 && body) ? cJSON_Parse(body) : NULL;
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(o, "why");
    if (cJSON_IsString(w))
        for (int i = 0; i < 5; i++)
            if (strcmp(w->valuestring, why[i]) == 0) idx = i + 1;
    cJSON_Delete(o);
    return idx;
}

bool gf_parse_motor_req(const char *json, gf_cmd_t *out)
{
    cJSON *o = json ? cJSON_Parse(json) : NULL;
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(o, "motor");
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(o, "cmd");
    int mi = -1, ci = -1;
    for (int i = 0; i < 2; i++) {
        if (cJSON_IsString(m) && strcmp(m->valuestring, s_motor_name[i]) == 0) mi = i;
        if (cJSON_IsString(c) && strcmp(c->valuestring, s_cmd_name[i]) == 0) ci = i;
    }
    cJSON_Delete(o);
    if (mi < 0 || ci < 0) return false;
    out->motor = (uint8_t)mi;
    out->cmd = (uint8_t)ci;
    return true;
}
/* GF_PARSE_END */

void gf_snapshot(gf_sump_t *sump, gf_util_t *util)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (sump) *sump = s_sump;
    if (util) *util = s_util;
    xSemaphoreGive(s_mux);
}

/* One GET, body into buf. Returns bytes read, or -1. Plain HTTP on the LAN;
 * a 2 s timeout because a node that takes longer is not answering. */
static int gf_fetch(const char *ip_port, char *buf, size_t len)
{
    char url[64];
    snprintf(url, sizeof(url), "http://%s/api/telemetry", ip_port);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = GF_HTTP_TIMEOUT_MS,
        .method = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return -1;
    int got = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        int64_t clen = esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status == 200 && clen != 0) {
            int n = 0;
            while (n < (int)len - 1) {
                int r = esp_http_client_read(c, buf + n, (int)len - 1 - n);
                if (r <= 0) break;
                n += r;
            }
            buf[n] = '\0';
            got = n;
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return got;
}

/* One POST of a JSON body. Returns the HTTP status, 0 if it never got one;
 * the reply body (truncated to fit) lands in resp. Same timeout as the poll. */
static int gf_post(const char *ip_port, const char *path, const char *body, char *resp, size_t rlen)
{
    char url[64];
    snprintf(url, sizeof(url), "http://%s%s", ip_port, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = GF_HTTP_TIMEOUT_MS,
        .method = HTTP_METHOD_POST,
    };
    resp[0] = '\0';
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return 0;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    int status = 0, blen = (int)strlen(body);
    if (esp_http_client_open(c, blen) == ESP_OK) {
        if (esp_http_client_write(c, body, blen) == blen && esp_http_client_fetch_headers(c) >= 0) {
            status = esp_http_client_get_status_code(c);
            int n = 0;
            while (n < (int)rlen - 1) {
                int r = esp_http_client_read(c, resp + n, (int)rlen - 1 - n);
                if (r <= 0) break;
                n += r;
            }
            resp[n] = '\0';
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return status;
}

/* Does the hub itself have a LAN? A node lives on the local network, so with no
 * station IP the poll cannot possibly succeed - and worse, before
 * app_network_init() has run there is no lwIP mailbox at all and getaddrinfo
 * asserts "Invalid mbox", which panicked the hub on every boot once a node
 * address was stored. app_main() now starts this task after the network, but
 * start order in another file is not a guarantee anybody will preserve, so the
 * check lives here, where the network is actually used.
 *
 * The interface-count test comes FIRST, and its job is to keep the handle
 * lookup off a stack that may not exist yet - not to prove the stack is there.
 * esp_netif_get_nr_of_ifs() is a bare read of a counter
 * (esp_netif_objects.c:69), in a translation unit whose own header comment
 * says it has no dependency on lwIP, so asking it is always safe. What a
 * non-zero answer means here is weaker than it looks: it is non-zero only
 * because app_wifi_internal_init() (managed_components/espressif__rmaker_app_
 * network/app_wifi_internal.c) calls esp_netif_init() before
 * esp_netif_create_default_wifi_sta() - an ordering inside that component, not
 * anything ESP-IDF guarantees. Nor does reaching
 * esp_netif_get_handle_from_ifkey() with no stack necessarily assert: with
 * CONFIG_LWIP_TCPIP_CORE_LOCKING unset (sdkconfig:1817) the core-lock query
 * returns true while lwip_task is still NULL, so esp_netif_lwip_ipc_call_msg()
 * takes its local branch and never reaches the asserting call at all. The
 * guard is cheap and it is the right shape; it is not a guarantee, and this
 * comment used to claim one. */
static bool gf_lan_up(void)
{
    if (esp_netif_get_nr_of_ifs() == 0) return false;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL) return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(sta, &ip) != ESP_OK) return false;
    return ip.ip.addr != 0;
}

/* Returns true when this call polled the node and it answered. */
static bool gf_poll_one(cal_gf_t which, int64_t now_us)
{
    /* cal_set_gf_ip() (app_cal.c) writes this string with no lock of its own,
     * and a /cal save can race this task. Copying it here, under our mutex,
     * rather than holding the shared pointer across the blocking HTTP call
     * below, keeps the exposed window one strncpy long instead of up to
     * GF_HTTP_TIMEOUT_MS. */
    char ip[24];
    xSemaphoreTake(s_mux, portMAX_DELAY);
    strncpy(ip, cal_gf_ip(which), sizeof(ip) - 1);
    ip[sizeof(ip) - 1] = '\0';
    gf_link_t *l = (which == CAL_GF_SUMP) ? &s_sump.link : &s_util.link;
    if (ip[0] == '\0') {
        /* Not fitted: forget everything, so a node that is later removed from
         * /cal does not keep showing its last reading. */
        bool was = l->online;
        memset(l, 0, sizeof(*l));
        if (was) ESP_LOGI(TAG, "%s node removed from /cal", cal_gf_key(which));
        xSemaphoreGive(s_mux);
        return false;
    }
    bool due = now_us >= l->next_poll_us;
    xSemaphoreGive(s_mux);
    if (!due) return false;

    /* Deliberately NOT counted as a miss, and next_poll_us is left alone: the
     * node is not the thing that failed. Latching it offline because the hub's
     * own Wi-Fi dropped would report a fault at the far end of the link and
     * send somebody to the wrong floor. The node keeps whatever it last
     * honestly was, and the page shows the age. */
    if (!gf_lan_up()) return false;

    static char body[GF_REPLY_MAX];
    int n = gf_fetch(ip, body, sizeof(body));

    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool was_online = l->online, ok = false;
    if (n > 0) {
        ok = (which == CAL_GF_SUMP) ? gf_parse_sump(body, &s_sump) : gf_parse_util(body, &s_util);
        if (!ok) ESP_LOGW(TAG, "%s node %s: unparseable reply (%d bytes): %.80s", cal_gf_key(which), ip, n, body);
    }
    gf_link_result(l, ok, now_us);
    if (ok && !was_online)  ESP_LOGI(TAG, "%s node %s answering (fw %s, rssi %d)", cal_gf_key(which), ip, l->fw, l->rssi);
    /* A node that has been away may have been reflashed or reset to its
     * defaults: give it the hub's control config again. */
    if (ok && !was_online && which == CAL_GF_UTIL) s_cfg_dirty = true;
    if (!ok && was_online && !l->online) ESP_LOGW(TAG, "%s node %s offline after %d misses - probing every %d s",
                                                   cal_gf_key(which), ip, l->misses, GF_REPROBE_MS / 1000);
    xSemaphoreGive(s_mux);
    return ok;
}

static void util_ip(char ip[24])
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    strncpy(ip, cal_gf_ip(CAL_GF_UTIL), 23);
    ip[23] = '\0';
    xSemaphoreGive(s_mux);
}

/* Forward every queued command, oldest first, and log each with the node's
 * answer (EVT_MOTOR_CMD). Runs on every gf_task pass, not only on poll
 * passes: a Stop should not wait up to GF_POLL_MS. */
static void gf_drain_cmds(void)
{
    gf_cmd_t c;
    while (xQueueReceive(s_cmdq, &c, 0) == pdTRUE) {
        char ip[24], body[160], resp[64] = "";
        const char *mn = s_motor_name[c.motor], *cn = s_cmd_name[c.cmd];
        util_ip(ip);
        int status = 0;
        if (ip[0] == '\0') {
            ESP_LOGW(TAG, "%s %s dropped: no utility node on /cal", mn, cn);
        } else if (!gf_lan_up()) {
            ESP_LOGW(TAG, "%s %s dropped: hub has no LAN", mn, cn);
        } else if (gf_ctl_body(&c, cal_ctl()->token, body, sizeof(body)) < 0) {
            ESP_LOGW(TAG, "%s %s dropped: could not build the body", mn, cn);
        } else {
            status = gf_post(ip, "/api/ctl", body, resp, sizeof(resp));
            ESP_LOGI(TAG, "%s %s (src %u) -> HTTP %d %s", mn, cn, c.src, status, resp);
        }
        event_push(EVT_MOTOR_CMD, (uint8_t)((c.motor << 4) | (c.cmd << 2) | c.src),
                   (uint16_t)status, (uint16_t)gf_why_index(status, resp));
    }
}

/* Called right after a good utility poll, so a node that keeps refusing is
 * retried at the poll rate and never in a loop. Dirty is cleared BEFORE the
 * POST: a /cal save that lands while it is in flight re-arms it. */
static void gf_push_cfg(void)
{
    const cal_ct_cfg_t *b = cal_ct(CAL_CT_BORE), *s = cal_ct(CAL_CT_SUMP);
    const cal_ctl_t *k = cal_ctl();
    gf_ctl_cfg_t g = {
        .bore_apv_x100 = b->amps_per_volt_x100, .sump_apv_x100 = s->amps_per_volt_x100,
        .bore_turns = b->turns, .sump_turns = s->turns,
        .bore_run_da = b->run_deci_amps, .bore_dry_da = b->dry_deci_amps,
        .sump_noload_da = k->sump_noload_da, .max_run_min = k->max_run_min,
        .ctrl_node = k->ctrl_node, .bore_act = k->bore_act, .sump_act = k->sump_act,
    };
    /* No token yet: the node refuses everything, so do not knock every 5 s.
     * Saving one on /cal re-arms the push. */
    if (k->token[0] == '\0') return;
    static char body[512];
    char ip[24], resp[64] = "";
    s_cfg_dirty = false;
    util_ip(ip);
    int status = 0;
    if (ip[0] && gf_cfg_body(&g, k->token, body, sizeof(body)) > 0) {
        status = gf_post(ip, "/api/ctl/cfg", body, resp, sizeof(resp));
    }
    if (status != 200) {
        s_cfg_dirty = true;
        ESP_LOGW(TAG, "control config push to %s: HTTP %d %s - retry next poll", ip, status, resp);
        return;
    }
    ESP_LOGI(TAG, "control config pushed: ctrl_node=%d bore_act=%d sump_act=%d",
             g.ctrl_node, g.bore_act, g.sump_act);
    /* The node holds these now. Put them in the snapshot rather than wait a
     * poll, so the app is told what the node has without a stale flicker. */
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_util.ctrl_node = g.ctrl_node;
    s_util.bore_act = g.bore_act;
    s_util.sump_act = g.sump_act;
    xSemaphoreGive(s_mux);
}

bool gf_cmd_post(const gf_cmd_t *c)
{
    if (c->motor > GF_MOTOR_SUMP || c->cmd > GF_CMD_STOP || c->src > GF_SRC_DASH) return false;
    return s_cmdq != NULL && xQueueSend(s_cmdq, c, 0) == pdTRUE;
}

void gf_cfg_push(void)
{
    s_cfg_dirty = true;
}

static void gf_task(void *arg)
{
    (void)arg;
    while (true) {
        int64_t now = esp_timer_get_time();
        gf_drain_cmds();
        gf_poll_one(CAL_GF_SUMP, now);
        if (gf_poll_one(CAL_GF_UTIL, now) && s_cfg_dirty) gf_push_cfg();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t gf_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    if (s_mux == NULL) return ESP_FAIL;
    s_cmdq = xQueueCreate(GF_CMDQ_LEN, sizeof(gf_cmd_t));
    if (s_cmdq == NULL) return ESP_FAIL;
    memset(&s_sump, 0, sizeof(s_sump));
    memset(&s_util, 0, sizeof(s_util));
    s_util.rwt_floty = -1;
    ESP_LOGI(TAG, "ground-floor nodes: sump %s, utility %s",
             cal_gf_ip(CAL_GF_SUMP)[0] ? cal_gf_ip(CAL_GF_SUMP) : "not fitted",
             cal_gf_ip(CAL_GF_UTIL)[0] ? cal_gf_ip(CAL_GF_UTIL) : "not fitted");
    if (xTaskCreate(gf_task, "gf", 6144, NULL, 4, NULL) != pdPASS) return ESP_FAIL;
    return ESP_OK;
}
