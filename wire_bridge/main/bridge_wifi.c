// bridge_wifi.c — AP/STA Wi-Fi link for UDP tunnel (no router/internet)
// Role selected via Kconfig: CONFIG_WB_ROLE_AP / CONFIG_WB_ROLE_STA
// Static IPs:
//   AP  : 192.168.50.1/24 (runs DHCP server for STA)
//   STA : 192.168.50.2/24 (static, no DHCP client)

#include "bridge_wifi.h"
#include "bridge_cfg.h"

#include <string.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "lwip/ip4_addr.h"   // IP4_ADDR

static const char *TAG = "wb_wifi";

static esp_netif_t *s_netif = NULL;
static wb_wifi_state_t s_state = {.ok = false, .rssi = 0};

static void set_static_ip_ap(void)
{
    esp_netif_ip_info_t ip = {0};
    IP4_ADDR(&ip.ip,      WB_NET_BASE_IP0, WB_NET_BASE_IP1, WB_NET_BASE_IP2, WB_IP_AP_LAST);
    IP4_ADDR(&ip.gw,      WB_NET_BASE_IP0, WB_NET_BASE_IP1, WB_NET_BASE_IP2, WB_IP_AP_LAST);
    IP4_ADDR(&ip.netmask, WB_NETMASK0, WB_NETMASK1, WB_NETMASK2, WB_NETMASK3);

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_netif));
}

#if CONFIG_WB_ROLE_STA
static void set_static_ip_sta(void)
{
    esp_netif_ip_info_t ip = {0};
    IP4_ADDR(&ip.ip,      WB_NET_BASE_IP0, WB_NET_BASE_IP1, WB_NET_BASE_IP2, WB_IP_STA_LAST);
    IP4_ADDR(&ip.gw,      WB_NET_BASE_IP0, WB_NET_BASE_IP1, WB_NET_BASE_IP2, WB_IP_AP_LAST);
    IP4_ADDR(&ip.netmask, WB_NETMASK0, WB_NETMASK1, WB_NETMASK2, WB_NETMASK3);

    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(s_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &ip));
}
#endif

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

#if CONFIG_WB_ROLE_STA
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_state.ok = false;
        s_state.rssi = 0;
        esp_wifi_connect();
    }
#endif
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

#if CONFIG_WB_ROLE_STA
    if (id == IP_EVENT_STA_GOT_IP) {
        s_state.ok = true;

        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_state.rssi = ap.rssi;
        } else {
            s_state.rssi = 0;
        }
        ESP_LOGI(TAG, "STA got IP, rssi=%d", s_state.rssi);
    }
#else
    // IDF 6.x: replacement for deprecated IP_EVENT_AP_STAIPASSIGNED
    if (id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        ESP_LOGI(TAG, "AP assigned IP to client");
    }
#endif
}

void wb_wifi_start(void)
{
    // safe if already initialized in app_main, but ok to keep as-is
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL));

#if CONFIG_WB_ROLE_AP
    s_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t w = {0};
    strncpy((char *)w.ap.ssid, CONFIG_WB_WIFI_SSID, sizeof(w.ap.ssid));
    strncpy((char *)w.ap.password, CONFIG_WB_WIFI_PASS, sizeof(w.ap.password));
    w.ap.ssid_len = (uint8_t)strlen(CONFIG_WB_WIFI_SSID);
    w.ap.channel = CONFIG_WB_WIFI_CHANNEL;
    w.ap.max_connection = 1;
    w.ap.authmode = (strlen(CONFIG_WB_WIFI_PASS) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &w));
    ESP_ERROR_CHECK(esp_wifi_start());

    set_static_ip_ap();
    s_state.ok = true;
    s_state.rssi = 0;

    ESP_LOGI(TAG, "AP started: ssid=%s ch=%d ip=192.168.50.1",
             CONFIG_WB_WIFI_SSID, CONFIG_WB_WIFI_CHANNEL);

#else
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_config_t w = {0};
    strncpy((char *)w.sta.ssid, CONFIG_WB_WIFI_SSID, sizeof(w.sta.ssid));
    strncpy((char *)w.sta.password, CONFIG_WB_WIFI_PASS, sizeof(w.sta.password));
    w.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    w.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &w));
    ESP_ERROR_CHECK(esp_wifi_start());

    set_static_ip_sta();

    ESP_LOGI(TAG, "STA started: ssid=%s ip=192.168.50.2 gw=192.168.50.1",
             CONFIG_WB_WIFI_SSID);
#endif
}

wb_wifi_state_t wb_wifi_get_state(void)
{
    return s_state;
}
