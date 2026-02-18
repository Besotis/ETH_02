// display_status.c (ESP-IDF 6.x) — ST7789 240x240, TFT_eSPI style (rotation=3)
#include "display_status.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "disp";

// ===== atkartojam tavo TFT_eSPI User_Setup.h =====
#define LCD_HOST   SPI3_HOST     // jei juoda -> bandyk SPI3_HOST

#define PIN_SCK    14
#define PIN_MOSI   15
#define PIN_DC     33
#define PIN_RST    32
#define PIN_CS     4             // "dummy CS" kaip TFT_eSPI

#define LCD_HRES   240
#define LCD_VRES   240

static esp_lcd_panel_handle_t s_panel = NULL;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}

// Saugus rect fill (be 0 pločio/aukščio ir su clamp)
static void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) return;

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

static void test_fill_colors(void)
{
    fill_rect(0, 0,   240, 80,  rgb565(255, 0, 0));
    fill_rect(0, 80,  240, 80,  rgb565(0, 255, 0));
    fill_rect(0, 160, 240, 80,  rgb565(0, 0, 255));
}

void display_init(void)
{
    ESP_LOGI(TAG, "LCD init (mode0, 40MHz, rot=3) ...");

    // Dummy CS: laikom LOW (jei netyčia realiai pajungtas/pull-up)
    gpio_config_t cs = {
        .pin_bit_mask = 1ULL << PIN_CS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs);
    gpio_set_level(PIN_CS, 0);

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_HRES * 80 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,              // kaip TFT_eSPI
        .pclk_hz = 40 * 1000 * 1000,        // kaip SPI_FREQUENCY
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,                      // TFT_eSPI default
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                            &io_cfg, &io_handle));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, // jei spalvos “kreivos” -> LCD_RGB_ELEMENT_ORDER_RGB
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // Rotation 3 (TFT_eSPI): swap + mirror (praktinis atitikmuo)
    //ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    //ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, true));

    // Gap 0,0 (240x240)
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, 80));

    // Testas: R/G/B juostos
    test_fill_colors();

    ESP_LOGI(TAG, "LCD ready (color test drawn)");
}

void display_set_status(const status_t *s)
{
    fill_rect(0, 0, LCD_HRES, 70, rgb565(10, 10, 10));

    uint16_t c_wifi = s->wifi_up ? rgb565(0, 200, 0) : rgb565(200, 0, 0);
    uint16_t c_eth  = s->eth_link ? rgb565(0, 200, 0) : rgb565(200, 0, 0);
    uint16_t c_udp  = (s->udp_rx > 0) ? rgb565(0, 200, 0) : rgb565(200, 160, 0);

    fill_rect(10,  10, 60, 50, c_wifi);
    fill_rect(90,  10, 60, 50, c_eth);
    fill_rect(170, 10, 60, 50, c_udp);

    fill_rect(0, 90,  240, 12, rgb565(25, 25, 25));
    fill_rect(0, 110, 240, 12, rgb565(25, 25, 25));
    fill_rect(0, 130, 240, 12, rgb565(25, 25, 25));

    int txw = (int)((s->udp_tx % 1000) * 240 / 1000); if (txw < 1) txw = 1;
    int rxw = (int)((s->udp_rx % 1000) * 240 / 1000); if (rxw < 1) rxw = 1;
    int drw = (int)((s->udp_drop % 1000) * 240 / 1000); if (drw < 1) drw = 1;

    fill_rect(0, 90,  txw, 12, rgb565(0, 120, 255));
    fill_rect(0, 110, rxw, 12, rgb565(0, 255, 120));
    fill_rect(0, 130, drw, 12, rgb565(255, 60, 60));
}
