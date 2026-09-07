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
    static char json[512];
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

bool web_served_once(void) { return s_served; }

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 3;
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
    ESP_LOGI(TAG, "http://" CONFIG_GF_STATIC_IP "/api/telemetry");
    return ESP_OK;
}
