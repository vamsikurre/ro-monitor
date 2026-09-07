/*
 * net.c - the station link.
 *
 * A static IP, not DHCP: the hub polls this node by an address typed once
 * into its /cal page, and a lease that moves is a node that silently
 * vanishes from the poll loop until someone notices and re-types it.
 *
 * Reconnect is unconditional and immediate. A ground-floor node with nobody
 * watching it has to get itself back on the LAN after every AP hiccup -
 * there is no console to nudge it, so on_wifi() retries the connection from
 * the same handler that noticed it dropped.
 */
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "gf.h"

static const char *TAG = "net";
static bool s_up = false;
static esp_netif_t *s_netif;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_up = false;
        ESP_LOGW(TAG, "disconnected - retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_up = true;
        ESP_LOGI(TAG, "up at " CONFIG_GF_STATIC_IP);
    }
}

esp_err_t net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    /* Static, because the hub polls this address and a DHCP lease that moves is
     * a node that vanishes. */
    esp_netif_dhcpc_stop(s_netif);
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr      = esp_ip4addr_aton(CONFIG_GF_STATIC_IP);
    ip.gw.addr      = esp_ip4addr_aton(CONFIG_GF_GATEWAY);
    ip.netmask.addr = esp_ip4addr_aton(CONFIG_GF_NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &ip));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, CONFIG_GF_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, CONFIG_GF_WIFI_PASS, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));   /* the hub polls; do not doze */
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

bool net_up(void) { return s_up; }

int net_rssi(void)
{
    wifi_ap_record_t ap;
    return (s_up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}
