#include "eth_tap.h"

#include <assert.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_eth.h"

#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy_lan87xx.h"   // managed_components espressif__lan87xx

static const char *TAG = "wb_eth";

static esp_eth_handle_t s_eth = NULL;
static bool s_link = false;

static wb_eth_rx_cb_t s_rx_cb = NULL;
static void *s_rx_user = NULL;

// ---- RX hook: called for every received Ethernet frame
static esp_err_t wb_input_path(esp_eth_handle_t h, uint8_t *buffer, uint32_t length, void *priv)
{
    (void)h; (void)priv;

    if (s_rx_cb && buffer && length) {
        s_rx_cb(buffer, (size_t)length, s_rx_user);
    }

    free(buffer); // we consume it
    return ESP_OK;
}

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;

    if (id == ETHERNET_EVENT_CONNECTED) {
        s_link = true;
        ESP_LOGI(TAG, "ETH LINK UP");
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        s_link = false;
        ESP_LOGW(TAG, "ETH LINK DOWN");
    } else if (id == ETHERNET_EVENT_START) {
        ESP_LOGI(TAG, "ETH START");
    } else if (id == ETHERNET_EVENT_STOP) {
        ESP_LOGI(TAG, "ETH STOP");
    }
}

static esp_err_t eth_init_start(void)
{
    esp_err_t err;

    // register handler once
    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_handler_register(ETH_EVENT) failed: %s", esp_err_to_name(err));
        return err;
    }

    // MAC config
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();

    // ESP32 EMAC specific config
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    // WT32-ETH02: external RMII clock input on GPIO0
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = 0;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (!mac) {
        ESP_LOGE(TAG, "esp_eth_mac_new_esp32 failed");
        return ESP_FAIL;
    }

    // PHY config (WT32-ETH02 typical)
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = 16;

    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
    if (!phy) {
        ESP_LOGE(TAG, "esp_eth_phy_new_lan87xx failed");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);

    // install driver
    err = esp_eth_driver_install(&eth_config, &s_eth);
    if (err != ESP_OK || s_eth == NULL) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s (hdl=%p)", esp_err_to_name(err), s_eth);
        return (err == ESP_OK) ? ESP_FAIL : err;
    }

    // start
    err = esp_eth_start(s_eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(err));
        return err;
    }

    // PROMISCUOUS = allow multicast (sACN) etc.
    bool promisc = true;
    err = esp_eth_ioctl(s_eth, ETH_CMD_S_PROMISCUOUS, &promisc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ETH_CMD_S_PROMISCUOUS failed: %s (continuing)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "ETH promiscuous enabled");
    }

    return ESP_OK;
}

void wb_eth_start(wb_eth_rx_cb_t cb, void *user)
{
    s_rx_cb = cb;
    s_rx_user = user;

    esp_err_t err = eth_init_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ETH init failed: %s", esp_err_to_name(err));
        return; // don't abort/reboot
    }

    // Hook raw RX frames
    err = esp_eth_update_input_path(s_eth, wb_input_path, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_update_input_path failed: %s", esp_err_to_name(err));
        return;
    }

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
