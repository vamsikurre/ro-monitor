/*
 * main.c - boot sequence and the OTA rollback watch.
 *
 * Order matters: sensors before net/web, because a sensor read never blocks
 * on the network and there is no reason the first sample should wait on a
 * Wi-Fi association that might take seconds. web_start() before the sample
 * task, so a slow first sensors_sample() cannot delay the hub's first poll
 * finding the server up.
 */
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "gf.h"

static const char *TAG = "gf";

/* Rollback. With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE a freshly OTA'd image
 * boots as pending-verify. It becomes permanent only when the node is on the
 * network AND has answered the hub once - the two things an update can break.
 * If that has not happened within OTA_CONFIRM_MS the node reboots into the
 * previous slot on its own. A non-OTA boot has nothing pending and this is a
 * no-op. */
static void ota_watch(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) return;
    int64_t t0 = esp_timer_get_time();
    while (true) {
        if (net_up() && web_served_once()) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "OTA image confirmed valid");
            return;
        }
        if (esp_timer_get_time() - t0 > (int64_t)OTA_CONFIRM_MS * 1000) {
            ESP_LOGE(TAG, "new image never served the hub - rolling back");
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void sample_task(void *arg)
{
    (void)arg;
    while (true) {
        sensors_sample();
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "gf_node %s (id 0x%02X) fw %s", GF_ROLE_NAME, GF_NODE_ID, esp_app_get_description()->version);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    sensors_init();
    ESP_ERROR_CHECK(net_start());
    ESP_ERROR_CHECK(web_start());
    xTaskCreate(sample_task, "sample", 4096, NULL, 5, NULL);
    ota_watch();   /* returns immediately on a normal boot */
}
