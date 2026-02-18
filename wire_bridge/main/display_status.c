// display_status.c — ST7789 + LVGL via esp_lvgl_port (ESP-IDF 6.x)
// Based on your working example: SPI3_HOST + spi_mode=3 + data_endian=LITTLE + invert_color(true)
//
// UI: WT32 BRIDGE + ETH/WIFI/UDP counters
// display_set_status(): updates LVGL labels (no manual drawing)

#include "display_status.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_types.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "disp";

// ===== Pins (tavo) =====
#define LCD_HOST   SPI3_HOST
#define PIN_SCK    14
#define PIN_MOSI   15
#define PIN_DC     33
#define PIN_RST    32
#define PIN_CS     4        // dummy CS

#define LCD_W      240
#define LCD_H      240

static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t    s_panel = NULL;
static lv_disp_t               * s_disp = NULL;

// LVGL objects
static lv_obj_t *lbl_title = NULL;
static lv_obj_t *lbl_eth   = NULL;
static lv_obj_t *lbl_wifi  = NULL;
static lv_obj_t *lbl_udp   = NULL;

// Cached values (to avoid unnecessary redraw)
static bool     last_eth_link = false;
static bool     last_wifi_up  = false;
static uint32_t last_udp_tx   = 0;
static uint32_t last_udp_rx   = 0;
static uint32_t last_udp_drop = 0;
static bool     cache_valid   = false;

static void ui_create(lv_disp_t *disp)
{
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    lv_obj_clean(scr);

    // Optional: set background to black (depends on theme; safe)
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Title
    lbl_title = lv_label_create(scr);
    lv_label_set_text(lbl_title, "WT32 BRIDGE");
    lv_obj_set_style_text_color(lbl_title, lv_color_white(), 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 10);

    // ETH
    lbl_eth = lv_label_create(scr);
    lv_label_set_text(lbl_eth, "ETH: ...");
    lv_obj_set_style_text_color(lbl_eth, lv_color_white(), 0);
    lv_obj_align(lbl_eth, LV_ALIGN_TOP_LEFT, 10, 50);

    // WIFI
    lbl_wifi = lv_label_create(scr);
    lv_label_set_text(lbl_wifi, "WIFI: ...");
    lv_obj_set_style_text_color(lbl_wifi, lv_color_white(), 0);
    lv_obj_align(lbl_wifi, LV_ALIGN_TOP_LEFT, 10, 80);

    // UDP counters
    lbl_udp = lv_label_create(scr);
    lv_label_set_text(lbl_udp, "UDP TX:0 RX:0 D:0");
    lv_obj_set_style_text_color(lbl_udp, lv_color_white(), 0);
    lv_obj_align(lbl_udp, LV_ALIGN_TOP_LEFT, 10, 110);

    lvgl_port_unlock();
}

static void ui_update(const status_t *s)
{
    // status_t laukų prielaidos:
    //  - bool eth_link
    //  - bool wifi_up
    //  - uint32_t udp_tx, udp_rx, udp_drop
    //
    // Jei pas tave kitaip — pakeisi 3 eilutes žemiau.

    bool     eth_link = s->eth_link;
    bool     wifi_up  = s->wifi_up;
    uint32_t udp_tx   = s->udp_tx;
    uint32_t udp_rx   = s->udp_rx;
    uint32_t udp_drop = s->udp_drop;

    // Jei niekas nepasikeitė — neliečiam LVGL
    if (cache_valid &&
        eth_link == last_eth_link &&
        wifi_up  == last_wifi_up &&
        udp_tx   == last_udp_tx &&
        udp_rx   == last_udp_rx &&
        udp_drop == last_udp_drop) {
        return;
    }

    last_eth_link = eth_link;
    last_wifi_up  = wifi_up;
    last_udp_tx   = udp_tx;
    last_udp_rx   = udp_rx;
    last_udp_drop = udp_drop;
    cache_valid   = true;

    char buf[64];

    lvgl_port_lock(0);

    if (lbl_eth) {
        snprintf(buf, sizeof(buf), "ETH: %s", eth_link ? "LINK UP" : "LINK DOWN");
        lv_label_set_text(lbl_eth, buf);
        lv_obj_set_style_text_color(lbl_eth, eth_link ? lv_color_hex(0x00FF00) : lv_color_hex(0xFF0000), 0);
    }

    if (lbl_wifi) {
        snprintf(buf, sizeof(buf), "WIFI: %s", wifi_up ? "UP" : "DOWN");
        lv_label_set_text(lbl_wifi, buf);
        lv_obj_set_style_text_color(lbl_wifi, wifi_up ? lv_color_hex(0x00FF00) : lv_color_hex(0xFF0000), 0);
    }

    if (lbl_udp) {
        // jei nori mažiau triukšmo: rodyk mod 100000 ar pan.
        snprintf(buf, sizeof(buf), "UDP TX:%lu RX:%lu D:%lu",
                 (unsigned long)udp_tx, (unsigned long)udp_rx, (unsigned long)udp_drop);
        lv_label_set_text(lbl_udp, buf);
        lv_obj_set_style_text_color(lbl_udp, lv_color_hex(0xFFFF00), 0);
    }

    lvgl_port_unlock();
}

void display_init(void)
{
    ESP_LOGI(TAG, "LCD init: ST7789 + esp_lvgl_port (bridge UI)");

    // ---- SPI bus
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 40 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // ---- LCD IO (SPI)
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = 40 * 1000 * 1000,     // stabilu. vėliau gali kelti (20/26/40)
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 3,                   // kaip tavo veikiantis variantas
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));

    // ---- ST7789 panel
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,

        // Jei raudona/mėlyna sukeista -> keisk į LCD_RGB_ELEMENT_ORDER_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,

        .bits_per_pixel = 16,

        // Tavo FIX: byte order
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 80, 0));   // X gap 80px (rotation 270 atveju)

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    // kaip tavo rankiniam init'e: INVON
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // ---- LVGL port init
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    // ---- Add display to LVGL
    const lvgl_port_display_cfg_t disp_cfg = {
    .io_handle = s_io,
    .panel_handle = s_panel,
    .buffer_size = LCD_W * 40,
    .double_buffer = true,
    .hres = LCD_W,
    .vres = LCD_H,
    .monochrome = false,
    .rotation = {
        .swap_xy = true,
        .mirror_x = false,
        .mirror_y = true,
    },
    .flags = {
        .buff_dma = true,
    },
};
  
    s_disp = lvgl_port_add_disp(&disp_cfg);

    // Create UI
    ui_create(s_disp);

    // Reset cache so first update redraws
    cache_valid = false;

    ESP_LOGI(TAG, "LCD UI ready");
}

void display_set_status(const status_t *s)
{
    if (!s_disp || !lbl_title) return;
    ui_update(s);
}
