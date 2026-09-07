#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gf.h"

static const char *TAG = "ota";

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
        return httpd_resp_send_500(req);
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
        return httpd_resp_send_500(req);
    }

    static char buf[4096];
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf, sizeof buf);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            esp_ota_abort(h);
            ESP_LOGE(TAG, "receive failed at %d/%d", got, total);
            return httpd_resp_send_500(req);
        }
        err = esp_ota_write(h, buf, r);
        if (err != ESP_OK) {
            esp_ota_abort(h);
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            return httpd_resp_send_500(req);
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
        return httpd_resp_send_500(req);
    }

    char msg[64];
    snprintf(msg, sizeof msg, "OK %d bytes, rebooting into %s\n", got, next->label);
    httpd_resp_sendstr(req, msg);
    ESP_LOGI(TAG, "%s", msg);
    vTaskDelay(pdMS_TO_TICKS(500));   /* let the response leave */
    esp_restart();
    return ESP_OK;
}
