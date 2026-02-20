// display_status.c — ST7789 + LVGL (esp_lvgl_port) + minimal menu (3 buttons)
// FIXES:
//  - rotation +180° from previous (swap_xy stays, mirror_x/mirror_y flipped)
//  - menu background BLACK
//  - visible selection arrow ">" in menu
//  - colored text by status (ETH/WIFI green/red, UDP cyan/yellow)
//
// Pins: SCK=14 MOSI=15 DC=33 RST=32 CS(dummy)=4

#include "display_status.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "driver/spi_master.h"

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

#define WB_SPI_HZ   (40 * 1000 * 1000)
#define WB_SPI_MODE 3

#define WB_LCD_X_GAP 80
#define WB_LCD_Y_GAP 0

static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t    s_panel = NULL;
static lv_disp_t               * s_disp = NULL;

// ---- UI state
static bool s_menu = false;
static int  s_menu_idx = 0;

static status_t s_last = {0};
static uint32_t s_base_tx = 0, s_base_rx = 0, s_base_drop = 0;

// ---- LVGL objects
static lv_obj_t *s_status_root = NULL;
static lv_obj_t *s_menu_root = NULL;

static lv_obj_t *s_lbl_title = NULL;
static lv_obj_t *s_lbl_eth = NULL;
static lv_obj_t *s_lbl_wifi = NULL;
static lv_obj_t *s_lbl_udp = NULL;

static lv_obj_t *s_lbl_status_text = NULL;

static lv_obj_t *s_menu_items[3] = {0};   // labels (with arrow)
static const char *s_menu_names[3] = {
    "Reset counters",
    "Info",
    "Back"
};

// ---- colors
static inline lv_color_t C_BLACK(void){ return lv_color_black(); }
static inline lv_color_t C_WHITE(void){ return lv_color_white(); }
static inline lv_color_t C_RED(void){ return lv_color_hex(0xFF3333); }
static inline lv_color_t C_GREEN(void){ return lv_color_hex(0x33FF66); }
static inline lv_color_t C_YELLOW(void){ return lv_color_hex(0xFFDD33); }
static inline lv_color_t C_CYAN(void){ return lv_color_hex(0x33DDFF); }
static inline lv_color_t C_GRAY(void){ return lv_color_hex(0x999999); }

// ---------- helpers ----------
static void scr_black(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, C_BLACK(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, C_WHITE(), 0);
}

static void set_label_color(lv_obj_t *lbl, lv_color_t c)
{
    if (!lbl) return;
    lv_obj_set_style_text_color(lbl, c, 0);
}

static void ui_update_topbar_locked(void)
{
    // ETH
    if (s_lbl_eth) {
        lv_label_set_text(s_lbl_eth, s_last.eth_link ? "ETH:UP" : "ETH:DOWN");
        set_label_color(s_lbl_eth, s_last.eth_link ? C_GREEN() : C_RED());
    }

    // WIFI
    if (s_lbl_wifi) {
        char w[32];
        if (s_last.wifi_up) snprintf(w, sizeof(w), "WIFI:UP %ddBm", s_last.rssi);
        else snprintf(w, sizeof(w), "WIFI:DOWN");
        lv_label_set_text(s_lbl_wifi, w);
        set_label_color(s_lbl_wifi, s_last.wifi_up ? C_GREEN() : C_RED());
    }

    // UDP counters (baseline)
    if (s_lbl_udp) {
        uint32_t tx = s_last.udp_tx - s_base_tx;
        uint32_t rx = s_last.udp_rx - s_base_rx;
        uint32_t dr = s_last.udp_drop - s_base_drop;

        char u[64];
        snprintf(u, sizeof(u), "UDP %u/%u D%u", (unsigned)tx, (unsigned)rx, (unsigned)dr);
        lv_label_set_text(s_lbl_udp, u);

        // if drops >0 -> yellow else cyan
        set_label_color(s_lbl_udp, (dr > 0) ? C_YELLOW() : C_CYAN());
    }
}

static void menu_set_item_text_locked(int idx, bool selected)
{
    if (!s_menu_items[idx]) return;

    char line[64];
    snprintf(line, sizeof(line), "%s %s", selected ? ">" : " ", s_menu_names[idx]);
    lv_label_set_text(s_menu_items[idx], line);

    // selection highlight: selected -> white, others -> gray
    set_label_color(s_menu_items[idx], selected ? C_WHITE() : C_GRAY());
}

static void menu_refresh_locked(void)
{
    for (int i = 0; i < 3; i++) {
        menu_set_item_text_locked(i, i == s_menu_idx);
    }
}

// ---------- build screens ----------
static void ui_build_status(void)
{
    lv_obj_t *scr = lv_disp_get_scr_act(s_disp);
    lv_obj_clean(scr);
    scr_black(scr);

    s_status_root = scr;
    s_menu_root = NULL;

    // Title
    s_lbl_title = lv_label_create(scr);
    lv_label_set_text(s_lbl_title, "WT32 BRIDGE");
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_MID, 0, 4);
    set_label_color(s_lbl_title, C_WHITE());

    // Top bar info (3 lines)
    s_lbl_eth = lv_label_create(scr);
    lv_obj_align(s_lbl_eth, LV_ALIGN_TOP_LEFT, 6, 26);

    s_lbl_wifi = lv_label_create(scr);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_TOP_LEFT, 6, 44);

    s_lbl_udp = lv_label_create(scr);
    lv_obj_align(s_lbl_udp, LV_ALIGN_TOP_LEFT, 6, 62);

    // Main status text
    s_lbl_status_text = lv_label_create(scr);
    lv_obj_set_width(s_lbl_status_text, 228);
    lv_label_set_long_mode(s_lbl_status_text, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_lbl_status_text, LV_ALIGN_TOP_LEFT, 6, 92);
    set_label_color(s_lbl_status_text, C_WHITE());

    // init draw
    ui_update_topbar_locked();
    lv_label_set_text(s_lbl_status_text,
                      "ENTER: Menu\nUP/DOWN: Navigate\n\nWaiting...");

    // clear menu items pointers
    for (int i=0;i<3;i++) s_menu_items[i]=NULL;
}

static void ui_build_menu(void)
{
    lv_obj_t *scr = lv_disp_get_scr_act(s_disp);
    lv_obj_clean(scr);
    scr_black(scr);

    s_menu_root = scr;
    s_status_root = NULL;

    // Header
    s_lbl_title = lv_label_create(scr);
    lv_label_set_text(s_lbl_title, "MENU");
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_MID, 0, 4);
    set_label_color(s_lbl_title, C_WHITE());

    // Status bar in menu as well
    s_lbl_eth = lv_label_create(scr);
    lv_obj_align(s_lbl_eth, LV_ALIGN_TOP_LEFT, 6, 26);

    s_lbl_wifi = lv_label_create(scr);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_TOP_LEFT, 6, 44);

    s_lbl_udp = lv_label_create(scr);
    lv_obj_align(s_lbl_udp, LV_ALIGN_TOP_LEFT, 6, 62);

    // Menu items (simple labels)
    for (int i = 0; i < 3; i++) {
        s_menu_items[i] = lv_label_create(scr);
        lv_obj_align(s_menu_items[i], LV_ALIGN_TOP_LEFT, 10, 100 + i * 28);
        lv_label_set_text(s_menu_items[i], "");
        lv_obj_set_style_text_font(s_menu_items[i], LV_FONT_DEFAULT, 0);
    }

    ui_update_topbar_locked();
    menu_refresh_locked();

    // In menu, we don't use status block
    s_lbl_status_text = NULL;
}

// ---------- public UI control ----------
void ui_menu_toggle(void)
{
    if (!s_disp) return;
    lvgl_port_lock(0);

    s_menu = !s_menu;
    if (s_menu) {
        ui_build_menu();
    } else {
        ui_build_status();
    }

    lvgl_port_unlock();
}

void ui_menu_up(void)
{
    if (!s_menu) return;
    if (s_menu_idx > 0) s_menu_idx--;

    lvgl_port_lock(0);
    menu_refresh_locked();
    lvgl_port_unlock();
}

void ui_menu_down(void)
{
    if (!s_menu) return;
    if (s_menu_idx < 2) s_menu_idx++;

    lvgl_port_lock(0);
    menu_refresh_locked();
    lvgl_port_unlock();
}

void ui_menu_enter(void)
{
    // If not in menu -> open it
    if (!s_menu) {
        ui_menu_toggle();
        return;
    }

    // Actions
    if (s_menu_idx == 0) {
        // reset displayed counters baseline
        s_base_tx = s_last.udp_tx;
        s_base_rx = s_last.udp_rx;
        s_base_drop = s_last.udp_drop;
    } else if (s_menu_idx == 1) {
        // Info: for now just blink/change title (minimal)
        // (čia galėsim vėliau padaryti atskirą ekraną)
    } else if (s_menu_idx == 2) {
        // Back
        ui_menu_toggle();
        return;
    }

    // refresh topbar + items after actions
    lvgl_port_lock(0);
    ui_update_topbar_locked();
    menu_refresh_locked();
    lvgl_port_unlock();
}

// ---------- init + update ----------
void display_init(void)
{
    ESP_LOGI(TAG, "LCD init: ST7789 + esp_lvgl_port (bridge UI)");

    // SPI bus
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 40 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // LCD IO
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = WB_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = WB_SPI_MODE,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));

    // Panel
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, WB_LCD_X_GAP, WB_LCD_Y_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // LVGL
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        .buffer_size = LCD_W * 40,
        .double_buffer = true,
        .hres = LCD_W,
        .vres = LCD_H,
        .monochrome = false,
        .rotation = {
            // +180° from previous:
            // previous: swap_xy=true, mirror_x=true, mirror_y=false
            // now:      swap_xy=true, mirror_x=false, mirror_y=true
            .swap_xy = true,
            .mirror_x = false,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);

    // Start in status screen
    s_menu = false;
    s_menu_idx = 0;
    s_base_tx = s_base_rx = s_base_drop = 0;

    lvgl_port_lock(0);
    ui_build_status();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LCD UI ready");
}

void display_set_status(const status_t *s)
{
    if (!s) return;
    s_last = *s;

    lvgl_port_lock(0);

    // update topbar for both screens
    ui_update_topbar_locked();

    // status screen main block
    if (!s_menu && s_lbl_status_text) {
        uint32_t tx = s_last.udp_tx - s_base_tx;
        uint32_t rx = s_last.udp_rx - s_base_rx;
        uint32_t dr = s_last.udp_drop - s_base_drop;

        char buf[256];
        snprintf(buf, sizeof(buf),
                 "ENTER: Menu\n"
                 "UP/DOWN: Navigate\n\n"
                 "ETH %s\nWIFI %s\n\n"
                 "UDP TX %u\nUDP RX %u\nDROP %u",
                 s_last.eth_link ? "UP" : "DOWN",
                 s_last.wifi_up ? "UP" : "DOWN",
                 (unsigned)tx, (unsigned)rx, (unsigned)dr);

        lv_label_set_text(s_lbl_status_text, buf);
    }

    // menu items keep arrow
    if (s_menu) {
        menu_refresh_locked();
    }

    lvgl_port_unlock();
}
