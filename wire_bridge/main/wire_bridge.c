#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy_lan87xx.h"   // tu jau turi per managed_components espressif__lan87xx

#include "display_status.h"

static const char *TAG = "wire_bridge";

static volatile bool s_eth_link = false;

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_id == ETHERNET_EVENT_CONNECTED) {
        s_eth_link = true;
        ESP_LOGI(TAG, "ETH LINK UP");
    } else if (event_id == ETHERNET_EVENT_DISCONNECTED) {
        s_eth_link = false;
        ESP_LOGW(TAG, "ETH LINK DOWN");
    } else if (event_id == ETHERNET_EVENT_START) {
        ESP_LOGI(TAG, "ETH START");
    } else if (event_id == ETHERNET_EVENT_STOP) {
        ESP_LOGI(TAG, "ETH STOP");
    }
}

static void eth_init_start(void)
{
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));

    // MAC config
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();

    // ESP32 EMAC specific config (IDF 6.x)
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    // WT32-ETHxx dažnai naudoja external RMII clock į GPIO0
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = 0;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    assert(mac);

    // PHY config
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = 16;

    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
    assert(phy);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;

    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // LCD
    display_init();

    // Ethernet
    eth_init_start();
    ESP_LOGI(TAG, "Init done (LCD+ETH).");

    status_t st = {0};
    uint32_t t = 0;

    while (1) {
        // Šitam etape WiFi/UDP dar nejungiam – tik testuojam monitoringą.
        st.eth_link = s_eth_link;
        st.wifi_up = false;

        // Demo counters kad matytum jog ekranas gyvas
        st.udp_tx++;
        if ((t++ % 3) == 0) st.udp_rx++;
        if ((t % 11) == 0) st.udp_drop++;

        display_set_status(&st);

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
