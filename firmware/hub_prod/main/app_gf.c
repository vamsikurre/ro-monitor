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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_cal.h"
#include "app_gf.h"

static const char *TAG = "gf";

static SemaphoreHandle_t s_mux;
static gf_sump_t s_sump;
static gf_util_t s_util;

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

static void gf_poll_one(cal_gf_t which, int64_t now_us)
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
        return;
    }
    bool due = now_us >= l->next_poll_us;
    xSemaphoreGive(s_mux);
    if (!due) return;

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
    if (!ok && was_online && !l->online) ESP_LOGW(TAG, "%s node %s offline after %d misses - probing every %d s",
                                                   cal_gf_key(which), ip, l->misses, GF_REPROBE_MS / 1000);
    xSemaphoreGive(s_mux);
}

static void gf_task(void *arg)
{
    (void)arg;
    while (true) {
        int64_t now = esp_timer_get_time();
        gf_poll_one(CAL_GF_SUMP, now);
        gf_poll_one(CAL_GF_UTIL, now);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t gf_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    if (s_mux == NULL) return ESP_FAIL;
    memset(&s_sump, 0, sizeof(s_sump));
    memset(&s_util, 0, sizeof(s_util));
    s_util.rwt_floty = -1;
    ESP_LOGI(TAG, "ground-floor nodes: sump %s, utility %s",
             cal_gf_ip(CAL_GF_SUMP)[0] ? cal_gf_ip(CAL_GF_SUMP) : "not fitted",
             cal_gf_ip(CAL_GF_UTIL)[0] ? cal_gf_ip(CAL_GF_UTIL) : "not fitted");
    if (xTaskCreate(gf_task, "gf", 6144, NULL, 4, NULL) != pdPASS) return ESP_FAIL;
    return ESP_OK;
}
