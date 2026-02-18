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
#include "esp_eth_phy_lan87xx.h"

static const char *TAG = "eth_test";

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "ETH LINK UP");
    } else if (event_id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "ETH LINK DOWN");
    } else if (event_id == ETHERNET_EVENT_START) {
        ESP_LOGI(TAG, "ETH START");
    } else if (event_id == ETHERNET_EVENT_STOP) {
        ESP_LOGI(TAG, "ETH STOP");
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));

    // 1) MAC config
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();

    // 2) ESP32 EMAC specific config (naujas IDF 6.x)
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    // WT32-ETHxx dažnai reikalauja external RMII clock per GPIO0
    // Jei pas tave kitaip – pakeisim vėliau.
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = 0;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    assert(mac);

    // 3) PHY config
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;           // dažnai 1 (jei nekils link, bandysim 0)
    phy_config.reset_gpio_num = 16;    // dažnai PHY power/reset (jei ne, bandysim -1)

    // 4) LAN87xx PHY (LAN8720 priklauso LAN87xx šeimai)
    eth_lan87xx_config_t lan87xx_config = ETH_LAN87XX_DEFAULT_CONFIG(&phy_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&lan87xx_config);
    assert(phy);

    // 5) Driver
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;

    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "ETH init done, plug/unplug RJ45 to see LINK events");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
