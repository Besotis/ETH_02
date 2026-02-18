#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "lvgl_ui";

// tavo pinai
#define LCD_HOST   SPI3_HOST
#define PIN_SCK    14
#define PIN_MOSI   15
#define PIN_DC     33
#define PIN_RST    32
#define PIN_CS     4    // dummy kaip TFT_eSPI (paliekam)
#define LCD_HRES   240
#define LCD_VRES   240

void lvgl_ui_start(void)
{
    // 1) SPI bus
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_HRES * 40 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // 2) Panel IO (čia svarbiausia: SPI3 + mode3)
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = 10 * 1000 * 1000,   // pradžiai 10MHz, vėliau kelsi
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 3,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    // 3) ST7789 panel
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, // jei spalvos keistos -> RGB
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // Jei bus “artefaktai” ar invert problema — dažnai padeda:
    // ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));

    // 4) LVGL port init
    const esp_lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(esp_lvgl_port_init(&lvgl_cfg));

    // 5) Pririšam display prie LVGL (esp_lvgl_port)
    const esp_lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_HRES * 40,   // mažas buferis stabilumui
        .double_buffer = false,
        .hres = LCD_HRES,
        .vres = LCD_VRES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565, // LVGL 9
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
        },
    };

    lv_disp_t *disp = esp_lvgl_port_add_disp(&disp_cfg);
    (void)disp;

    // 6) UI: paprastas label
    esp_lvgl_port_lock(0);
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "WT32 LVGL OK");
    lv_obj_center(label);
    esp_lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL started");
}
