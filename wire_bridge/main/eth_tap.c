#include "eth_tap.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_eth.h"
#include "esp_event.h"

#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_eth_mac_esp.h"

// iš managed_components espressif__lan87xx
#include "esp_eth_phy_lan87xx.h"

static const char *TAG = "wb_eth";

static esp_eth_handle_t s_eth = NULL;
static bool s_link = false;

static wb_eth_rx_cb_t s_rx_cb = NULL;
static void *s_rx_user = NULL;

// ---- hook: called for every received Ethernet frame (we consume buffer)
static esp_err_t wb_input_path(esp_eth_handle_t h, uint8_t *buffer, uint32_t length, void *priv)
{
    (void)h;
    (void)priv;

    if (s_rx_cb && buffer && length) {
        s_rx_cb(buffer, (size_t)length, s_rx_user);
    }

    // IMPORTANT: we consume the frame, so free it
    free(buffer);
    return ESP_OK;
}

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == ETHERNET_EVENT_CONNECTED) {
        s_link = true;
        ESP_LOGI(TAG, "ETH LINK UP");
    } else if (event_id == ETHERNET_EVENT_DISCONNECTED) {
        s_link = false;
        ESP_LOGW(TAG, "ETH LINK DOWN");
    } else if (event_id == ETHERNET_EVENT_START) {
        ESP_LOGI(TAG, "ETH START");
    } else if (event_id == ETHERNET_EVENT_STOP) {
        ESP_LOGI(TAG, "ETH STOP");
    }
}

static void eth_init_start(void)
{
    // register events once
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));

    // MAC config
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();

    // ESP32 EMAC specific config (IDF 6.x)
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    // WT32-ETH02: external RMII clock in to GPIO0
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

    // install driver -> STORE into s_eth
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &s_eth));
    ESP_ERROR_CHECK(esp_eth_start(s_eth));
}

void wb_eth_start(wb_eth_rx_cb_t cb, void *user)
{
    s_rx_cb = cb;
    s_rx_user = user;

    // init ETH and start it
    eth_init_start();

    if (!s_eth) {
        ESP_LOGE(TAG, "ETH handle is NULL");
        return;
    }

    // Hook RX path: gives us raw Ethernet frames
    ESP_ERROR_CHECK(esp_eth_update_input_path(s_eth, wb_input_path, NULL));

    ESP_LOGI(TAG, "ETH TAP ready");
}

bool wb_eth_send(const uint8_t *frame, size_t len)
{
    if (!s_eth || !frame || len == 0) return false;
    return (esp_eth_transmit(s_eth, (void *)frame, len) == ESP_OK);
}

bool wb_eth_link_up(void)
{
    return s_link;
}
