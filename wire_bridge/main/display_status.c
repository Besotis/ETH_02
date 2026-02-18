// display_status.c
#include "display_status.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "disp";

// ====== ST7789 240x240, 7-pin (be CS) ======
#define LCD_HOST   SPI2_HOST   // jei nerodys – bandyk SPI3_HOST
#define PIN_SCK    14
#define PIN_MOSI   15
#define PIN_DC     33
#define PIN_RST    32
#define PIN_CS     -1

#define LCD_HRES   240
#define LCD_VRES   240

static esp_lcd_panel_handle_t s_panel = NULL;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}

// Saugus rect fill: niekada nekviečia draw su 0 pločiu/aukščiu ir viską suclampina į ekraną
static void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) return;

    // clamp į ekraną
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= LCD_HRES || y >= LCD_VRES) return;

    if (x + w > LCD_HRES) w = LCD_HRES - x;
    if (y + h > LCD_VRES) h = LCD_VRES - y;

    if (w <= 0 || h <= 0) return;

    static uint16_t line[LCD_HRES];
    for (int i = 0; i < w; i++) line[i] = c;

    for (int yy = y; yy < y + h; yy++) {
        esp_lcd_panel_draw_bitmap(s_panel, x, yy, x + w, yy + 1, line);
    }
}

void display_init(void)
{
    ESP_LOGI(TAG, "Init LCD...");

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_HRES * 60 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, // jei spalvos keistos -> LCD_RGB_ELEMENT_ORDER_RGB
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // Clear screen
    fill_rect(0, 0, LCD_HRES, LCD_VRES, rgb565(0, 0, 0));
    ESP_LOGI(TAG, "LCD ready");
}

void display_set_status(const status_t *s)
{
    // Top bar background
    fill_rect(0, 0, LCD_HRES, 70, rgb565(10, 10, 10));

    // 3 indikatoriai: WIFI / ETH / UDP
    uint16_t c_wifi = s->wifi_up ? rgb565(0, 200, 0) : rgb565(200, 0, 0);
    uint16_t c_eth  = s->eth_link ? rgb565(0, 200, 0) : rgb565(200, 0, 0);
    uint16_t c_udp  = (s->udp_rx > 0) ? rgb565(0, 200, 0) : rgb565(200, 160, 0);

    fill_rect(10,  10, 60, 50, c_wifi);
    fill_rect(90,  10, 60, 50, c_eth);
    fill_rect(170, 10, 60, 50, c_udp);

    // Bar'ai: TX / RX / DROP
    // Pagrindas (tamsus)
    fill_rect(0, 90, 240, 12, rgb565(25, 25, 25));
    fill_rect(0, 110, 240, 12, rgb565(25, 25, 25));
    fill_rect(0, 130, 240, 12, rgb565(25, 25, 25));

    // TX (mėlyna)
    int txw = (int)((s->udp_tx % 1000) * 240 / 1000);
    if (txw < 1) txw = 1;
    fill_rect(0, 90, txw, 12, rgb565(0, 120, 255));

    // RX (žalia)
    int rxw = (int)((s->udp_rx % 1000) * 240 / 1000);
    if (rxw < 1) rxw = 1;
    fill_rect(0, 110, rxw, 12, rgb565(0, 255, 120));

    // DROP (raudona)
    int drw = (int)((s->udp_drop % 1000) * 240 / 1000);
    if (drw < 1) drw = 1;
    fill_rect(0, 130, drw, 12, rgb565(255, 60, 60));
}
