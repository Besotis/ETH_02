#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "display_status.h"
#include "buttons.h"

#include "bridge_wifi.h"
#include "udp_tunnel.h"
#include "eth_tap.h"

static const char *TAG = "wire_bridge";
static status_t g_st = {0};

// ETH -> UDP
static void on_eth_frame(const uint8_t *frame, size_t len, void *user)
{
    (void)user;
    if (!wb_udp_send_frame(frame, len)) {
        // queue full etc.
        g_st.udp_drop++;
    }
}

// UDP -> ETH
static void on_udp_frame(const uint8_t *frame, size_t len, void *user)
{
    (void)user;
    (void)wb_eth_send(frame, len);
}

static void on_button(wb_btn_t btn, bool pressed, void *user)
{
    (void)user;
    if (!pressed) return; // only on press

    if (btn == BTN_ENTER) ui_menu_enter(); // enter toggles menu if not in menu
    else if (btn == BTN_UP) ui_menu_up();
    else if (btn == BTN_DOWN) ui_menu_down();
}

static void status_task(void *arg)
{
    (void)arg;

    while (1) {
        wb_wifi_state_t ws = wb_wifi_get_state();

        g_st.eth_link = wb_eth_link_up();
        g_st.wifi_up  = ws.ok;
        g_st.rssi     = ws.rssi;

        g_st.udp_tx   = wb_udp_get_tx();
        g_st.udp_rx   = wb_udp_get_rx();
        g_st.udp_drop = wb_udp_get_drop();

        display_set_status(&g_st);

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    display_init();
    buttons_init(on_button, NULL);

    ESP_LOGI(TAG, "Starting WiFi...");
    wb_wifi_start();

    wb_udp_start(on_udp_frame, NULL);
    wb_eth_start(on_eth_frame, NULL);

    xTaskCreate(status_task, "status", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "Bridge running");
}
