#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gf.h"

static const char *TAG = "ota";

/* A stalled peer (AP drops association silently, which is the expected way
 * Wi-Fi fails inside a steel panel or a manhole, not a clean FIN/RST) makes
 * httpd_req_recv() return HTTPD_SOCK_ERR_TIMEOUT forever. The server runs
 * this handler on its one task, so a handler that never returns takes the
 * node's telemetry and its next update attempt down with it - exactly what
 * the rollback design exists to avoid. This is a wall-clock deadline on the
 * whole transfer, not a cap on consecutive timeouts, so it also catches a
 * live-but-glacial drip that never actually times out on any single recv.
 * ~730 KB at a sustained 12 KB/s (a genuinely poor link, not merely a slow
 * one) still finishes inside 60 s; a healthy LAN transfer finishes long
 * before this ever runs the numbers. */
#define OTA_RECV_DEADLINE_MS  60000

/* Every 500 here is a different fact for whoever is standing at the panel:
 * "no free slot", "the connection died", "the write failed", "the image
 * is fine, only the boot pointer didn't move" all need different next steps
 * (mostly: try again) and none of them should look identical. */
static esp_err_t err500(httpd_req_t *req, const char *reason)
{
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_sendstr(req, reason);
}

/* The whole image arrives as the POST body and is streamed into the slot that
 * is not running. esp_ota_end() validates the image header and checksum, so a
 * text file or a hub build sent here by mistake is refused before it can be
 * booted. Rollback is main.c's job: the new image must prove itself. */
esp_err_t ota_handle(httpd_req_t *req)
{
    int total = req->content_len;
    if (total < 100000) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "body too small to be a firmware image\n");
    }
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next == NULL) {
        return err500(req, "no free OTA partition\n");
    }
    if (total > (int)next->size) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "image larger than the OTA slot\n");
    }
    ESP_LOGI(TAG, "receiving %d bytes into %s", total, next->label);

    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return err500(req, "could not start the OTA write - try again\n");
    }

    int64_t deadline = esp_timer_get_time() + (int64_t)OTA_RECV_DEADLINE_MS * 1000;
    static char buf[4096];
    int got = 0;
    while (got < total) {
        if (esp_timer_get_time() > deadline) {
            esp_ota_abort(h);
            ESP_LOGE(TAG, "transfer stalled at %d/%d - timed out", got, total);
            httpd_resp_set_status(req, "408 Request Timeout");
            return httpd_resp_sendstr(req, "transfer stalled - push the image again\n");
        }
        int r = httpd_req_recv(req, buf, sizeof buf);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            esp_ota_abort(h);
            ESP_LOGE(TAG, "receive failed at %d/%d", got, total);
            return err500(req, "connection dropped - try again\n");
        }
        err = esp_ota_write(h, buf, r);
        if (err != ESP_OK) {
            esp_ota_abort(h);
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            return err500(req, "write to flash failed - try again\n");
        }
        got += r;
    }

    err = esp_ota_end(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s (not a valid image?)", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "image rejected - not a valid ESP32 app image\n");
    }
    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        return err500(req, "image written and valid, but could not set boot partition - try again\n");
    }

    char msg[64];
    snprintf(msg, sizeof msg, "OK %d bytes, rebooting into %s\n", got, next->label);
    httpd_resp_sendstr(req, msg);
    ESP_LOGI(TAG, "%s", msg);
    vTaskDelay(pdMS_TO_TICKS(500));   /* let the response leave */
    esp_restart();
    return ESP_OK;
}
