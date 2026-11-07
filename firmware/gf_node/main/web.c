/*
 * web.c - the three routes the hub and a technician need.
 *
 * "/" is a plain-text banner for a browser or curl at the bench; the hub
 * never fetches it. "/api/telemetry" is the one the hub polls every 5 s and
 * whose shape is the contract pinned by docs/fake_gf_node.py and
 * docs/check_gf.py - sensors_json() owns that body, this file only ships it.
 * "/ota" hands off to ota.c (Task 9); routing it here rather than folding it
 * into main.c keeps the httpd registration and the update logic in one place
 * each.
 */
#include "esp_app_desc.h"
#include "esp_log.h"
#include "gf.h"

static const char *TAG = "web";
static bool s_served = false;

static esp_err_t root_get(httpd_req_t *req)
{
    char b[96];
    int n = snprintf(b, sizeof b, "gf_node %s id %d fw %s\nGET /api/telemetry  POST /ota\n",
                     GF_ROLE_NAME, GF_NODE_ID, esp_app_get_description()->version);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, b, n);
}

static esp_err_t telemetry_get(httpd_req_t *req)
{
    static char json[768];
    int n = sensors_json(json, sizeof json);
    if (n <= 0 || n >= (int)sizeof json) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, json, n);
    if (e == ESP_OK) s_served = true;
    return e;
}

static esp_err_t ota_post(httpd_req_t *req) { return ota_handle(req); }

#if CONFIG_GF_ROLE_UTILITY
#define CTL_BODY_MAX 512

/* Reads the whole body into b (NUL-terminated). false = already answered with an error. */
static bool read_body(httpd_req_t *req, char *b)
{
    if (req->content_len == 0 || req->content_len > CTL_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body 1..512 bytes");
        return false;
    }
    size_t got = 0;
    int stalls = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, b + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {      /* a stalled client must not hold the one httpd task */
            if (++stalls > 3) { httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "stalled"); return false; }
            continue;
        }
        stalls = 0;
        if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body"); return false; }
        got += n;
    }
    b[got] = 0;
    return true;
}

static esp_err_t send_parse_err(httpd_req_t *req, int rc)
{
    if (rc == 403) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"token\"}");
    }
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
}

static esp_err_t ctl_post(httpd_req_t *req)
{
    char b[CTL_BODY_MAX + 1];
    if (!read_body(req, b)) return ESP_OK;
    ctl_motor_t m;  ctl_cmd_t c;  ctl_src_t src;
    int rc = ctl_parse_cmd(b, CONFIG_GF_CTL_TOKEN, &m, &c, &src);
    if (rc) return send_parse_err(req, rc);
    ctl_result_t r = ctl_glue_command(m, c, src);
    if (r == CTL_REFUSED_INVALID) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid");
    httpd_resp_set_type(req, "application/json");
    if (r == CTL_OK) return httpd_resp_sendstr(req, "{\"ok\":true}");
    static const char *const why[] = { [CTL_REFUSED_SINGLE_PHASE] = "single_phase", [CTL_REFUSED_SUMP_HOLD] = "sump_hold",
        [CTL_REFUSED_NOT_SETTLED] = "not_settled", [CTL_REFUSED_SUMP_FULL] = "sump_full", [CTL_REFUSED_NO_FLOAT] = "no_float" };
    if ((unsigned)r >= sizeof why / sizeof why[0] || !why[r]) return httpd_resp_send_500(req);
    char o[48];
    snprintf(o, sizeof o, "{\"ok\":false,\"why\":\"%s\"}", why[r]);
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, o);
}

static esp_err_t ctl_cfg_post(httpd_req_t *req)
{
    char b[CTL_BODY_MAX + 1];
    if (!read_body(req, b)) return ESP_OK;
    ctl_cfg_t cfg;
    ctl_glue_cfg_get(&cfg);
    int rc = ctl_parse_cfg(b, CONFIG_GF_CTL_TOKEN, &cfg);
    if (rc) return send_parse_err(req, rc);
    ctl_glue_cfg_set(&cfg);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}
#endif

bool web_served_once(void) { return s_served; }

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 5;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    httpd_handle_t srv = NULL;
    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err)); return err; }
    httpd_uri_t r = { .uri = "/",              .method = HTTP_GET,  .handler = root_get };
    httpd_uri_t t = { .uri = "/api/telemetry", .method = HTTP_GET,  .handler = telemetry_get };
    httpd_uri_t o = { .uri = "/ota",           .method = HTTP_POST, .handler = ota_post };
    httpd_register_uri_handler(srv, &r);
    httpd_register_uri_handler(srv, &t);
    httpd_register_uri_handler(srv, &o);
#if CONFIG_GF_ROLE_UTILITY
    httpd_uri_t k = { .uri = "/api/ctl",     .method = HTTP_POST, .handler = ctl_post };
    httpd_uri_t g = { .uri = "/api/ctl/cfg", .method = HTTP_POST, .handler = ctl_cfg_post };
    httpd_register_uri_handler(srv, &k);
    httpd_register_uri_handler(srv, &g);
#endif
    ESP_LOGI(TAG, "http://" CONFIG_GF_STATIC_IP "/api/telemetry");
    return ESP_OK;
}
