// wire_bridge.c — full app glue: LCD + WiFi (AP/STA via Kconfig) + UDP tunnel + ETH TAP
// Topology: ETH <-> ESP32 (WT32-ETH02) <-> WiFi+UDP <-> ESP32 <-> ETH
// Role selected in menuconfig: Wire Bridge -> Bridge role

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "display_status.h"
#include "bridge_wifi.h"
#include "udp_tunnel.h"
#include "eth_tap.h"

static const char *TAG = "wire_bridge";

static status_t g_st = {0};

// ETH -> UDP
static void on_eth_frame(const uint8_t *frame, size_t len, void *user)
{
    (void)user;

    // Forward raw L2 frame into UDP tunnel
    if (!wb_udp_send_frame(frame, len)) {
        // queue full etc.
        g_st.udp_drop++;
    }
}

// UDP -> ETH
static void on_udp_frame(const uint8_t *frame, size_t len, void *user)
{
    (void)user;

    // Inject frame back to Ethernet
    (void)wb_eth_send(frame, len);
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
    // Base system init (required for WiFi + ETH events)
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // LCD (LVGL) first so we see life early
    display_init();

    ESP_LOGI(TAG, "Starting WiFi (%s)...",
#if CONFIG_WB_ROLE_AP
             "AP"
#else
             "STA"
#endif
    );
    wb_wifi_start();

    // Start UDP tunnel (socket + rx/tx tasks)
    wb_udp_start(on_udp_frame, NULL);

    // Start Ethernet + TAP (raw RX frames -> callback)
    wb_eth_start(on_eth_frame, NULL);

    // Status update to LCD
    xTaskCreate(status_task, "status", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "Bridge running. Role=%s. UDP port=%d payload=%d",
#if CONFIG_WB_ROLE_AP
             "AP"
#else
             "STA"
#endif
             , CONFIG_WB_UDP_PORT, CONFIG_WB_MAX_PAYLOAD);
}
