#include "display_status.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "driver/spi_master.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_types.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "sdkconfig.h"

#include "soc/soc_caps.h"
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif

static const char *TAG = "disp";

// ===== WT32-ETH02 + ST7789 pins =====
#define LCD_HOST   SPI3_HOST
#define PIN_SCK    14
#define PIN_MOSI   15
#define PIN_DC     33
#define PIN_RST    32
#define PIN_CS     4

#define LCD_W      240
#define LCD_H      240

#define WB_SPI_HZ   (40 * 1000 * 1000)
#define WB_SPI_MODE 3

#define WB_LCD_X_GAP 80
#define WB_LCD_Y_GAP 0

// ===== Geometry (explicit areas; no overlap) =====
#define OUTER_BORDER_W   1
#define OUTER_RADIUS     14

#define INNER_PAD        6

#define TOP_H            22
#define TITLE_H          18
#define BOT_H            18
#define SEP_H            1

#define AREA_X           (INNER_PAD)
#define AREA_W           (LCD_W - (INNER_PAD * 2))

#define AREA_Y_TOP       (INNER_PAD)
#define AREA_Y_TITLE     (AREA_Y_TOP + TOP_H)
#define AREA_Y_SEP1      (AREA_Y_TITLE + TITLE_H)
#define AREA_Y_CONTENT   (AREA_Y_SEP1 + SEP_H)

#define AREA_Y_BOTSEP    (LCD_H - INNER_PAD - BOT_H - SEP_H)
#define AREA_Y_FOOTER    (LCD_H - INNER_PAD - BOT_H)

#define AREA_H_CONTENT   (AREA_Y_BOTSEP - AREA_Y_CONTENT)

// Menu
#define MENU_VISIBLE 5
#define MENU_CENTER  2
#define MENU_ROW_H   26
#define MENU_ROW_GAP 4   // +1px as requested

// ===== LVGL handles =====
static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t    s_panel = NULL;
static lv_disp_t               * s_disp = NULL;

static status_t s_last = {0};

// Baseline counters
static uint32_t s_base_tx = 0, s_base_rx = 0, s_base_drop = 0;

// Rates
static int64_t  s_prev_us = 0;
static uint32_t s_prev_tx = 0, s_prev_rx = 0;
static float    s_rate_tx_pps = 0.0f;
static float    s_rate_rx_pps = 0.0f;

// CPU temp
static float s_cpu_temp_c = 0.0f;
static bool  s_cpu_temp_valid = false;
static int64_t s_last_temp_us = 0;

#if SOC_TEMP_SENSOR_SUPPORTED
static temperature_sensor_handle_t s_tsens = NULL;
#endif

// ===== Screens =====
typedef enum {
    SCR_MENU = 0,
    SCR_STATUS,
    SCR_TRAFFIC,
    SCR_NETWORK,
    SCR_SYSTEM,
    SCR_ABOUT,
} screen_t;

static screen_t s_screen = SCR_MENU;
static int s_traffic_view = 0;

typedef struct { const char *name; screen_t screen; } menu_item_t;
static const menu_item_t s_main_menu[] = {
    { "Status",  SCR_STATUS  },
    { "Traffic", SCR_TRAFFIC },
    { "Network", SCR_NETWORK },
    { "System",  SCR_SYSTEM  },
    { "About",   SCR_ABOUT   },
};
static const int s_main_menu_count = (int)(sizeof(s_main_menu) / sizeof(s_main_menu[0]));
static int s_menu_index = 0;

// ===== Fonts =====
static const lv_font_t* font_title(void) {
#if LV_FONT_MONTSERRAT_16
    return &lv_font_montserrat_16;
#else
    return LV_FONT_DEFAULT;
#endif
}
static const lv_font_t* font_ui(void) {
#if LV_FONT_MONTSERRAT_14
    return &lv_font_montserrat_14;
#else
    return LV_FONT_DEFAULT;
#endif
}
static const lv_font_t* font_small(void) {
#if LV_FONT_MONTSERRAT_12
    return &lv_font_montserrat_12;
#else
    return LV_FONT_DEFAULT;
#endif
}

// ===== Colors =====
static inline lv_color_t C_BLACK(void){ return lv_color_black(); }
static inline lv_color_t C_BAR_BG(void){ return lv_color_hex(0x0F0F0F); }  // table bars bg
static inline lv_color_t C_OUTER(void){ return lv_color_hex(0x3A3A3A); }   // outer border + separators (light grey)
static inline lv_color_t C_PILL_BG(void){ return lv_color_hex(0x101010); }
static inline lv_color_t C_PILL_BR(void){ return lv_color_hex(0x2A2A2A); }
static inline lv_color_t C_GREY(void){ return lv_color_hex(0x8A8A8A); }
static inline lv_color_t C_YELLOW(void){ return lv_color_hex(0xFFD24A); }
static inline lv_color_t C_CYAN(void){ return lv_color_hex(0x33DDFF); }
static inline lv_color_t C_GREEN(void){ return lv_color_hex(0x33FF66); }
static inline lv_color_t C_RED(void){ return lv_color_hex(0xFF3333); }

// Selected item look (keep like Arduino)
static inline lv_color_t C_SEL_BG(void){ return lv_color_hex(0xF2F2F2); }  // "white" but softer
static inline lv_color_t C_SEL_BR(void){ return lv_color_hex(0x4A4A4A); }

// ===== Root objects =====
static lv_obj_t *g_outer = NULL;
static lv_obj_t *g_top_bar = NULL;
static lv_obj_t *g_title_row = NULL;
static lv_obj_t *g_footer_row = NULL;
static lv_obj_t *g_sep1 = NULL;
static lv_obj_t *g_sep2 = NULL;
static lv_obj_t *g_body = NULL;

static lv_obj_t *g_hdr_left = NULL;
static lv_obj_t *g_hdr_E = NULL;
static lv_obj_t *g_hdr_W = NULL;
static lv_obj_t *g_hdr_U = NULL;

static lv_obj_t *g_title_lbl = NULL;
static lv_obj_t *g_footer_lbl = NULL;

// Menu rows
static lv_obj_t *g_menu_row[MENU_VISIBLE] = {0};
static lv_obj_t *g_menu_lbl[MENU_VISIBLE] = {0};

// Screen widgets
typedef struct {
    lv_obj_t *st_role, *st_ip, *st_rssi, *st_udp, *st_rate;
    lv_obj_t *tr_mode, *tr_rx, *tr_tx, *tr_drop;
    lv_obj_t *nw_role, *nw_ssid, *nw_ip, *nw_rssi, *nw_wmac, *nw_emac;
    lv_obj_t *sy_uptime, *sy_heap, *sy_temp;
    lv_obj_t *ab_dev, *ab_bridge, *ab_build;
} ui_widgets_t;

static ui_widgets_t W = {0};

// ===== Helpers: strip ONLY unwanted theme borders for base containers =====
static inline void strip_theme_frame(lv_obj_t *o)
{
    // remove default/theme styles so we don't get extra "white frames"
    lv_obj_remove_style_all(o);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
}

static inline int wrap_index(int idx, int n)
{
    if (n <= 0) return 0;
    while (idx < 0) idx += n;
    while (idx >= n) idx -= n;
    return idx;
}

static bool label_set_text_if_changed(lv_obj_t *lbl, const char *txt)
{
    if (!lbl || !txt) return false;
    const char *old = lv_label_get_text(lbl);
    if (old && strcmp(old, txt) == 0) return false;
    lv_label_set_text(lbl, txt);
    return true;
}

static bool is_role_ap(void)
{
#if defined(CONFIG_WB_ROLE_AP) && CONFIG_WB_ROLE_AP
    return true;
#else
    return false;
#endif
}
static const char *role_str(void) { return is_role_ap() ? "AP" : "STA"; }
static wifi_interface_t wifi_if(void) { return is_role_ap() ? WIFI_IF_AP : WIFI_IF_STA; }
static const char *wifi_ifkey(void) { return is_role_ap() ? "WIFI_AP_DEF" : "WIFI_STA_DEF"; }

static void mac_to_str(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool get_wifi_ip(char out[16])
{
    out[0] = 0;
    esp_netif_t *n = esp_netif_get_handle_from_ifkey(wifi_ifkey());
    if (!n) return false;

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(n, &ip) != ESP_OK) return false;

    uint32_t a = ip.ip.addr;
    uint8_t b0 = (uint8_t)(a & 0xFF);
    uint8_t b1 = (uint8_t)((a >> 8) & 0xFF);
    uint8_t b2 = (uint8_t)((a >> 16) & 0xFF);
    uint8_t b3 = (uint8_t)((a >> 24) & 0xFF);
    snprintf(out, 16, "%u.%u.%u.%u", b0, b1, b2, b3);
    return true;
}

static bool get_rssi_dbm(int *out_rssi)
{
    if (!out_rssi) return false;
    *out_rssi = 0;

    if (!is_role_ap()) {
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            *out_rssi = ap.rssi;
            return true;
        }
        return false;
    }
    if (s_last.rssi != 0) {
        *out_rssi = s_last.rssi;
        return true;
    }
    return false;
}

static void update_rates(void)
{
    int64_t now = esp_timer_get_time();
    if (s_prev_us == 0) {
        s_prev_us = now;
        s_prev_tx = s_last.udp_tx;
        s_prev_rx = s_last.udp_rx;
        s_rate_tx_pps = 0;
        s_rate_rx_pps = 0;
        return;
    }
    int64_t dt_us = now - s_prev_us;
    if (dt_us < 300000) return;

    float dt_s = (float)dt_us / 1000000.0f;
    uint32_t dtx = s_last.udp_tx - s_prev_tx;
    uint32_t drx = s_last.udp_rx - s_prev_rx;

    s_rate_tx_pps = (dt_s > 0) ? ((float)dtx / dt_s) : 0.0f;
    s_rate_rx_pps = (dt_s > 0) ? ((float)drx / dt_s) : 0.0f;

    s_prev_us = now;
    s_prev_tx = s_last.udp_tx;
    s_prev_rx = s_last.udp_rx;
}

// CPU temp
#if CONFIG_IDF_TARGET_ESP32
extern uint8_t temprature_sens_read(void);
static bool read_cpu_temp_c(float *out_c)
{
    if (!out_c) return false;
    uint8_t raw = temprature_sens_read();
    *out_c = ((float)raw - 32.0f) / 1.8f;
    return true;
}
#else
static bool read_cpu_temp_c(float *out_c)
{
#if SOC_TEMP_SENSOR_SUPPORTED
    if (!out_c || !s_tsens) return false;
    float t = 0.0f;
    if (temperature_sensor_get_celsius(s_tsens, &t) == ESP_OK) {
        *out_c = t;
        return true;
    }
#endif
    (void)out_c;
    return false;
}
#endif

static void update_cpu_temp_throttled(void)
{
    int64_t now = esp_timer_get_time();
    if (s_last_temp_us != 0 && (now - s_last_temp_us) < 1000000) return;
    s_last_temp_us = now;

    float t = 0.0f;
    if (read_cpu_temp_c(&t)) {
        s_cpu_temp_c = t;
        s_cpu_temp_valid = true;
    } else {
        s_cpu_temp_valid = false;
    }
}

// ===== Styles =====
static lv_style_t st_outer;
static lv_style_t st_bar;
static lv_style_t st_sep;
static lv_style_t st_body;

// Pills (keep as before)
static lv_style_t st_pill;
static lv_style_t st_pill_sel;
static lv_style_t st_menu_text;
static lv_style_t st_menu_text_sel;

// KV pills (same pill style)
static lv_style_t st_k_key;
static lv_style_t st_k_val;

static void styles_init(void)
{
    // Outer table border
    lv_style_init(&st_outer);
    lv_style_set_bg_color(&st_outer, C_BLACK());
    lv_style_set_bg_opa(&st_outer, LV_OPA_COVER);
    lv_style_set_border_color(&st_outer, C_OUTER());
    lv_style_set_border_width(&st_outer, OUTER_BORDER_W);
    lv_style_set_radius(&st_outer, OUTER_RADIUS);
    lv_style_set_shadow_width(&st_outer, 0);
    lv_style_set_outline_width(&st_outer, 0);

    // Table bars: top/title/footer
    lv_style_init(&st_bar);
    lv_style_set_bg_color(&st_bar, C_BAR_BG());
    lv_style_set_bg_opa(&st_bar, LV_OPA_COVER);
    lv_style_set_border_width(&st_bar, 0);
    lv_style_set_shadow_width(&st_bar, 0);
    lv_style_set_outline_width(&st_bar, 0);

    // Separators (horizontal lines)
    lv_style_init(&st_sep);
    lv_style_set_bg_color(&st_sep, C_OUTER());
    lv_style_set_bg_opa(&st_sep, LV_OPA_COVER);
    lv_style_set_border_width(&st_sep, 0);
    lv_style_set_shadow_width(&st_sep, 0);
    lv_style_set_outline_width(&st_sep, 0);

    lv_style_init(&st_body);
    lv_style_set_bg_opa(&st_body, LV_OPA_TRANSP);
    lv_style_set_border_width(&st_body, 0);
    lv_style_set_shadow_width(&st_body, 0);
    lv_style_set_outline_width(&st_body, 0);

    // Normal pill
    lv_style_init(&st_pill);
    lv_style_set_bg_color(&st_pill, C_PILL_BG());
    lv_style_set_bg_opa(&st_pill, LV_OPA_COVER);
    lv_style_set_border_color(&st_pill, C_PILL_BR());
    lv_style_set_border_width(&st_pill, 1);
    lv_style_set_radius(&st_pill, 12);
    lv_style_set_shadow_width(&st_pill, 0);
    lv_style_set_outline_width(&st_pill, 0);
    lv_style_set_pad_left(&st_pill, 10);
    lv_style_set_pad_right(&st_pill, 10);
    lv_style_set_pad_top(&st_pill, 4);
    lv_style_set_pad_bottom(&st_pill, 4);

    // Selected pill (like Arduino: light fill, dark border)
    lv_style_init(&st_pill_sel);
    lv_style_set_bg_color(&st_pill_sel, C_SEL_BG());
    lv_style_set_bg_opa(&st_pill_sel, LV_OPA_COVER);
    lv_style_set_border_color(&st_pill_sel, C_SEL_BR());
    lv_style_set_border_width(&st_pill_sel, 1);
    lv_style_set_radius(&st_pill_sel, 12);
    lv_style_set_shadow_width(&st_pill_sel, 0);
    lv_style_set_outline_width(&st_pill_sel, 0);

    // Menu text
    lv_style_init(&st_menu_text);
    lv_style_set_text_color(&st_menu_text, C_YELLOW());
    lv_style_set_text_font(&st_menu_text, font_ui());

    lv_style_init(&st_menu_text_sel);
    lv_style_set_text_color(&st_menu_text_sel, C_BLACK());
    lv_style_set_text_font(&st_menu_text_sel, font_title()); // bigger like before

    // KV text
    lv_style_init(&st_k_key);
    lv_style_set_text_color(&st_k_key, C_CYAN());
    lv_style_set_text_font(&st_k_key, font_small());

    lv_style_init(&st_k_val);
    lv_style_set_text_color(&st_k_val, lv_color_white());
    lv_style_set_text_font(&st_k_val, font_ui());
}

// ===== UI setters =====
static void set_title(const char *t) { if (g_title_lbl) lv_label_set_text(g_title_lbl, t); }
static void set_footer(const char *t) { if (g_footer_lbl) lv_label_set_text(g_footer_lbl, t); }

static void header_update(void)
{
    if (!g_hdr_E || !g_hdr_W || !g_hdr_U) return;

    lv_label_set_text(g_hdr_E, "E");
    lv_obj_set_style_text_color(g_hdr_E, s_last.eth_link ? C_GREEN() : C_RED(), 0);

    lv_label_set_text(g_hdr_W, "W");
    lv_obj_set_style_text_color(g_hdr_W, s_last.wifi_up ? C_GREEN() : C_RED(), 0);

    uint32_t dr = s_last.udp_drop - s_base_drop;
    lv_label_set_text(g_hdr_U, "U");
    lv_obj_set_style_text_color(g_hdr_U, (dr > 0) ? C_YELLOW() : C_GREY(), 0);
}

// ===== Create pill row (key/value) =====
static lv_obj_t* kv_pill_create(lv_obj_t *parent, const char *key, lv_obj_t **out_val_label)
{
    lv_obj_t *pill = lv_obj_create(parent);
    // IMPORTANT: do NOT strip theme here fully; pill style is applied and defines border.
    // But we remove shadows/outlines just in case theme adds them.
    lv_obj_remove_style_all(pill);
    lv_obj_add_style(pill, &st_pill, 0);

    lv_obj_set_width(pill, lv_pct(100));
    lv_obj_set_height(pill, LV_SIZE_CONTENT);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *k = lv_label_create(pill);
    strip_theme_frame(k);
    lv_obj_add_style(k, &st_k_key, 0);
    lv_label_set_text(k, key);

    lv_obj_t *v = lv_label_create(pill);
    strip_theme_frame(v);
    lv_obj_add_style(v, &st_k_val, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(v, 1);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(v, "");

    if (out_val_label) *out_val_label = v;
    return pill;
}

static void body_clean(void)
{
    if (g_body) lv_obj_clean(g_body);
    memset(&W, 0, sizeof(W));
    memset(g_menu_row, 0, sizeof(g_menu_row));
    memset(g_menu_lbl, 0, sizeof(g_menu_lbl));
}

// ===== Menu =====
static void build_menu(void)
{
    set_title("Main Menu");
    set_footer("UP/DOWN move    ENTER open");

    lv_obj_set_flex_flow(g_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_body, MENU_ROW_GAP, 0);

    for (int i = 0; i < MENU_VISIBLE; i++) {
        g_menu_row[i] = lv_obj_create(g_body);
        lv_obj_remove_style_all(g_menu_row[i]);               // remove theme frames
        lv_obj_add_style(g_menu_row[i], &st_pill, 0);
        lv_obj_add_style(g_menu_row[i], &st_pill_sel, LV_STATE_CHECKED);

        lv_obj_set_width(g_menu_row[i], lv_pct(100));
        lv_obj_set_height(g_menu_row[i], MENU_ROW_H);
        lv_obj_clear_flag(g_menu_row[i], LV_OBJ_FLAG_SCROLLABLE);

        g_menu_lbl[i] = lv_label_create(g_menu_row[i]);
        strip_theme_frame(g_menu_lbl[i]);
        lv_obj_set_width(g_menu_lbl[i], lv_pct(100));
        lv_label_set_long_mode(g_menu_lbl[i], LV_LABEL_LONG_DOT);
        lv_obj_align(g_menu_lbl[i], LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_add_style(g_menu_lbl[i], &st_menu_text, 0);
        lv_obj_add_style(g_menu_lbl[i], &st_menu_text_sel, LV_STATE_CHECKED);
    }
}

static void refresh_menu(void)
{
    for (int row = 0; row < MENU_VISIBLE; row++) {
        int idx = wrap_index(s_menu_index + (row - MENU_CENTER), s_main_menu_count);
        bool selected = (row == MENU_CENTER);

        if (!g_menu_row[row] || !g_menu_lbl[row]) continue;

        if (selected) {
            lv_obj_add_state(g_menu_row[row], LV_STATE_CHECKED);
            lv_obj_add_state(g_menu_lbl[row], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(g_menu_row[row], LV_STATE_CHECKED);
            lv_obj_clear_state(g_menu_lbl[row], LV_STATE_CHECKED);
        }

        char line[48];
        if (selected) snprintf(line, sizeof(line), ">  %s", s_main_menu[idx].name);
        else          snprintf(line, sizeof(line), "   %s", s_main_menu[idx].name);

        label_set_text_if_changed(g_menu_lbl[row], line);
    }
}

// ===== Screens =====
static void build_status(void)
{
    set_title("System Status");
    set_footer("ENTER back");

    lv_obj_set_flex_flow(g_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_body, 4, 0);

    kv_pill_create(g_body, "Role",   &W.st_role);
    kv_pill_create(g_body, "WiFi IP",&W.st_ip);
    kv_pill_create(g_body, "RSSI",   &W.st_rssi);
    kv_pill_create(g_body, "UDP",    &W.st_udp);
    kv_pill_create(g_body, "Rate",   &W.st_rate);
}

static void build_traffic(void)
{
    set_title("Traffic");
    set_footer("UP/DOWN view    ENTER back");

    lv_obj_set_flex_flow(g_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_body, 4, 0);

    kv_pill_create(g_body, "Mode", &W.tr_mode);
    kv_pill_create(g_body, "RX",   &W.tr_rx);
    kv_pill_create(g_body, "TX",   &W.tr_tx);
    kv_pill_create(g_body, "Drop", &W.tr_drop);
}

static void build_network(void)
{
    set_title("Network");
    set_footer("ENTER back");

    lv_obj_set_flex_flow(g_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_body, 4, 0);

    kv_pill_create(g_body, "Role",     &W.nw_role);
    kv_pill_create(g_body, "SSID",     &W.nw_ssid);
    kv_pill_create(g_body, "WiFi IP",  &W.nw_ip);
    kv_pill_create(g_body, "RSSI",     &W.nw_rssi);
    kv_pill_create(g_body, "WiFi MAC", &W.nw_wmac);
    kv_pill_create(g_body, "ETH MAC",  &W.nw_emac);
}

static void build_system(void)
{
    set_title("System");
    set_footer("ENTER back");

    lv_obj_set_flex_flow(g_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_body, 4, 0);

    kv_pill_create(g_body, "Uptime",    &W.sy_uptime);
    kv_pill_create(g_body, "Free heap", &W.sy_heap);
    kv_pill_create(g_body, "CPU temp",  &W.sy_temp);
}

static void build_about(void)
{
    set_title("About");
    set_footer("ENTER back");

    lv_obj_set_flex_flow(g_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_body, 4, 0);

    kv_pill_create(g_body, "Device", &W.ab_dev);
    kv_pill_create(g_body, "Bridge", &W.ab_bridge);
    kv_pill_create(g_body, "Build",  &W.ab_build);
}

// ===== Updates =====
static void update_status_values(void)
{
    if (!W.st_role) return;

    label_set_text_if_changed(W.st_role, role_str());

    char ip[16] = "N/A";
    if (!get_wifi_ip(ip)) strcpy(ip, "N/A");
    label_set_text_if_changed(W.st_ip, ip);

    int rssi = 0;
    char rssi_s[16];
    if (get_rssi_dbm(&rssi)) snprintf(rssi_s, sizeof(rssi_s), "%d dBm", rssi);
    else snprintf(rssi_s, sizeof(rssi_s), "N/A");
    label_set_text_if_changed(W.st_rssi, rssi_s);

    uint32_t tx = s_last.udp_tx - s_base_tx;
    uint32_t rx = s_last.udp_rx - s_base_rx;
    uint32_t dr = s_last.udp_drop - s_base_drop;

    char udp[48];
    snprintf(udp, sizeof(udp), "TX %u  RX %u  D %u", (unsigned)tx, (unsigned)rx, (unsigned)dr);
    label_set_text_if_changed(W.st_udp, udp);

    char rate[48];
    snprintf(rate, sizeof(rate), "TX %.1f/s  RX %.1f/s", (double)s_rate_tx_pps, (double)s_rate_rx_pps);
    label_set_text_if_changed(W.st_rate, rate);
}

static void update_traffic_values(void)
{
    if (!W.tr_mode) return;

    uint32_t tx = s_last.udp_tx - s_base_tx;
    uint32_t rx = s_last.udp_rx - s_base_rx;
    uint32_t dr = s_last.udp_drop - s_base_drop;

    if (s_traffic_view == 0) {
        label_set_text_if_changed(W.tr_mode, "Rates (pkt/s)");

        char a[24], b[24], c[24];
        snprintf(a, sizeof(a), "%.1f", (double)s_rate_rx_pps);
        snprintf(b, sizeof(b), "%.1f", (double)s_rate_tx_pps);
        snprintf(c, sizeof(c), "%u", (unsigned)dr);

        label_set_text_if_changed(W.tr_rx, a);
        label_set_text_if_changed(W.tr_tx, b);
        label_set_text_if_changed(W.tr_drop, c);
    } else {
        label_set_text_if_changed(W.tr_mode, "Totals (since reset)");

        char a[24], b[24], c[24];
        snprintf(a, sizeof(a), "%u", (unsigned)rx);
        snprintf(b, sizeof(b), "%u", (unsigned)tx);
        snprintf(c, sizeof(c), "%u", (unsigned)dr);

        label_set_text_if_changed(W.tr_rx, a);
        label_set_text_if_changed(W.tr_tx, b);
        label_set_text_if_changed(W.tr_drop, c);
    }
}

static void update_network_values(void)
{
    if (!W.nw_role) return;

    label_set_text_if_changed(W.nw_role, role_str());

#if defined(CONFIG_WB_WIFI_SSID)
    label_set_text_if_changed(W.nw_ssid, CONFIG_WB_WIFI_SSID);
#else
    label_set_text_if_changed(W.nw_ssid, "N/A");
#endif

    char ip[16] = "N/A";
    if (!get_wifi_ip(ip)) strcpy(ip, "N/A");
    label_set_text_if_changed(W.nw_ip, ip);

    int rssi = 0;
    char rssi_s[16];
    if (get_rssi_dbm(&rssi)) snprintf(rssi_s, sizeof(rssi_s), "%d dBm", rssi);
    else snprintf(rssi_s, sizeof(rssi_s), "N/A");
    label_set_text_if_changed(W.nw_rssi, rssi_s);

    uint8_t wmac[6] = {0};
    uint8_t emac[6] = {0};
    char wmac_s[18] = "N/A";
    char emac_s[18] = "N/A";

    if (esp_wifi_get_mac(wifi_if(), wmac) == ESP_OK) mac_to_str(wmac, wmac_s);
    if (esp_read_mac(emac, ESP_MAC_ETH) == ESP_OK) mac_to_str(emac, emac_s);

    label_set_text_if_changed(W.nw_wmac, wmac_s);
    label_set_text_if_changed(W.nw_emac, emac_s);
}

static void update_system_values(void)
{
    if (!W.sy_uptime) return;

    int64_t us = esp_timer_get_time();
    uint32_t up_s = (uint32_t)(us / 1000000ULL);
    uint32_t hh = up_s / 3600U;
    uint32_t mm = (up_s % 3600U) / 60U;
    uint32_t ss = (up_s % 60U);

    char up[24];
    snprintf(up, sizeof(up), "%02u:%02u:%02u", (unsigned)hh, (unsigned)mm, (unsigned)ss);
    label_set_text_if_changed(W.sy_uptime, up);

    uint32_t heap = esp_get_free_heap_size();
    char heap_s[24];
    snprintf(heap_s, sizeof(heap_s), "%u B", (unsigned)heap);
    label_set_text_if_changed(W.sy_heap, heap_s);

    update_cpu_temp_throttled();
    char t[24];
    if (s_cpu_temp_valid) snprintf(t, sizeof(t), "%.1f C", (double)s_cpu_temp_c);
    else snprintf(t, sizeof(t), "N/A");
    label_set_text_if_changed(W.sy_temp, t);
}

static void update_about_values(void)
{
    if (!W.ab_dev) return;

    label_set_text_if_changed(W.ab_dev, "WT32-ETH02");
    label_set_text_if_changed(W.ab_bridge, "ETH <-> UDP tunnel");

    char build[48];
    snprintf(build, sizeof(build), "%s %s", __DATE__, __TIME__);
    label_set_text_if_changed(W.ab_build, build);
}

static void update_active_screen_values(void)
{
    switch (s_screen) {
        case SCR_STATUS:  update_status_values(); break;
        case SCR_TRAFFIC: update_traffic_values(); break;
        case SCR_NETWORK: update_network_values(); break;
        case SCR_SYSTEM:  update_system_values(); break;
        case SCR_ABOUT:   update_about_values(); break;
        case SCR_MENU:    refresh_menu(); break;
        default: break;
    }
}

static void build_for_screen(screen_t scr)
{
    // clear body
    if (g_body) lv_obj_clean(g_body);
    memset(&W, 0, sizeof(W));
    memset(g_menu_row, 0, sizeof(g_menu_row));
    memset(g_menu_lbl, 0, sizeof(g_menu_lbl));

    switch (scr) {
        case SCR_MENU:    build_menu(); break;
        case SCR_STATUS:  build_status(); break;
        case SCR_TRAFFIC: build_traffic(); break;
        case SCR_NETWORK: build_network(); break;
        case SCR_SYSTEM:  build_system(); break;
        case SCR_ABOUT:   build_about(); break;
        default:          build_menu(); break;
    }
}

static void ui_switch(screen_t scr)
{
    s_screen = scr;
    build_for_screen(scr);
    header_update();
    if (scr == SCR_MENU) refresh_menu();
    else update_active_screen_values();
}

// ===== Root create (table-like) =====
static void ui_root_create(void)
{
    lv_obj_t *scr = lv_disp_get_scr_act(s_disp);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, C_BLACK(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Outer
    g_outer = lv_obj_create(scr);
    strip_theme_frame(g_outer);
    lv_obj_add_style(g_outer, &st_outer, 0);
    lv_obj_set_size(g_outer, LCD_W, LCD_H);
    lv_obj_set_pos(g_outer, 0, 0);
    lv_obj_clear_flag(g_outer, LV_OBJ_FLAG_SCROLLABLE);

    // Top bar (attached to outer, no own border)
    g_top_bar = lv_obj_create(g_outer);
    strip_theme_frame(g_top_bar);
    lv_obj_add_style(g_top_bar, &st_bar, 0);
    lv_obj_set_pos(g_top_bar, AREA_X, AREA_Y_TOP);
    lv_obj_set_size(g_top_bar, AREA_W, TOP_H);
    lv_obj_clear_flag(g_top_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g_top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_top_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_hdr_left = lv_label_create(g_top_bar);
    strip_theme_frame(g_hdr_left);
    lv_obj_set_style_text_font(g_hdr_left, font_ui(), 0);
    lv_obj_set_style_text_color(g_hdr_left, C_CYAN(), 0);
    lv_label_set_long_mode(g_hdr_left, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_hdr_left, 150);
    lv_label_set_text(g_hdr_left, "ProPlex Bridge");

    lv_obj_t *sp = lv_obj_create(g_top_bar);
    strip_theme_frame(sp);
    lv_obj_set_size(sp, 1, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_grow(sp, 1);

    g_hdr_E = lv_label_create(g_top_bar); strip_theme_frame(g_hdr_E); lv_obj_set_style_text_font(g_hdr_E, font_ui(), 0);
    g_hdr_W = lv_label_create(g_top_bar); strip_theme_frame(g_hdr_W); lv_obj_set_style_text_font(g_hdr_W, font_ui(), 0);
    g_hdr_U = lv_label_create(g_top_bar); strip_theme_frame(g_hdr_U); lv_obj_set_style_text_font(g_hdr_U, font_ui(), 0);

    // Title row
    g_title_row = lv_obj_create(g_outer);
    strip_theme_frame(g_title_row);
    lv_obj_add_style(g_title_row, &st_bar, 0);
    lv_obj_set_pos(g_title_row, AREA_X, AREA_Y_TITLE);
    lv_obj_set_size(g_title_row, AREA_W, TITLE_H);

    g_title_lbl = lv_label_create(g_title_row);
    strip_theme_frame(g_title_lbl);
    lv_obj_set_style_text_font(g_title_lbl, font_title(), 0);
    lv_obj_set_style_text_color(g_title_lbl, lv_color_white(), 0);
    lv_label_set_long_mode(g_title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_title_lbl, AREA_W);
    lv_obj_set_pos(g_title_lbl, 0, 0);
    lv_label_set_text(g_title_lbl, "Main Menu");

    // Separators
    g_sep1 = lv_obj_create(g_outer);
    strip_theme_frame(g_sep1);
    lv_obj_add_style(g_sep1, &st_sep, 0);
    lv_obj_set_pos(g_sep1, AREA_X, AREA_Y_SEP1);
    lv_obj_set_size(g_sep1, AREA_W, SEP_H);

    g_sep2 = lv_obj_create(g_outer);
    strip_theme_frame(g_sep2);
    lv_obj_add_style(g_sep2, &st_sep, 0);
    lv_obj_set_pos(g_sep2, AREA_X, AREA_Y_BOTSEP);
    lv_obj_set_size(g_sep2, AREA_W, SEP_H);

    // Footer row
    g_footer_row = lv_obj_create(g_outer);
    strip_theme_frame(g_footer_row);
    lv_obj_add_style(g_footer_row, &st_bar, 0);
    lv_obj_set_pos(g_footer_row, AREA_X, AREA_Y_FOOTER);
    lv_obj_set_size(g_footer_row, AREA_W, BOT_H);

    g_footer_lbl = lv_label_create(g_footer_row);
    strip_theme_frame(g_footer_lbl);
    lv_obj_set_style_text_font(g_footer_lbl, font_small(), 0);
    lv_obj_set_style_text_color(g_footer_lbl, C_GREY(), 0);
    lv_label_set_long_mode(g_footer_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_footer_lbl, AREA_W);
    lv_obj_set_pos(g_footer_lbl, 0, 0);
    lv_label_set_text(g_footer_lbl, "UP/DOWN move    ENTER open");

    // Body
    g_body = lv_obj_create(g_outer);
    strip_theme_frame(g_body);
    lv_obj_add_style(g_body, &st_body, 0);
    lv_obj_set_pos(g_body, AREA_X, AREA_Y_CONTENT);
    lv_obj_set_size(g_body, AREA_W, AREA_H_CONTENT);
    lv_obj_clear_flag(g_body, LV_OBJ_FLAG_SCROLLABLE);
}

// ===== Public controls =====
void ui_menu_toggle(void)
{
    if (!s_disp) return;
    lvgl_port_lock(0);
    if (s_screen != SCR_MENU) ui_switch(SCR_MENU);
    else ui_switch(SCR_STATUS);
    lvgl_port_unlock();
}

void ui_menu_up(void)
{
    if (!s_disp) return;
    lvgl_port_lock(0);

    if (s_screen == SCR_MENU) {
        s_menu_index = wrap_index(s_menu_index - 1, s_main_menu_count);
        refresh_menu();
    } else if (s_screen == SCR_TRAFFIC) {
        s_traffic_view = (s_traffic_view + 1) % 2;
        update_traffic_values();
    }

    lvgl_port_unlock();
}

void ui_menu_down(void)
{
    if (!s_disp) return;
    lvgl_port_lock(0);

    if (s_screen == SCR_MENU) {
        s_menu_index = wrap_index(s_menu_index + 1, s_main_menu_count);
        refresh_menu();
    } else if (s_screen == SCR_TRAFFIC) {
        s_traffic_view = (s_traffic_view + 1) % 2;
        update_traffic_values();
    }

    lvgl_port_unlock();
}

void ui_menu_enter(void)
{
    if (!s_disp) return;
    lvgl_port_lock(0);

    if (s_screen == SCR_MENU) {
        int idx = wrap_index(s_menu_index, s_main_menu_count);
        ui_switch(s_main_menu[idx].screen);
    } else {
        ui_switch(SCR_MENU);
    }

    lvgl_port_unlock();
}

// ===== Init + status update =====
void display_init(void)
{
    ESP_LOGI(TAG, "LCD init: ST7789 + LVGL 9.x (table + selected pill kept)");

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 40 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

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
            .swap_xy  = true,
            .mirror_x = false,
            .mirror_y = true,
        },
        .flags = { .buff_dma = true },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);

#if SOC_TEMP_SENSOR_SUPPORTED
    temperature_sensor_config_t tcfg = { .range_min = -10, .range_max = 80 };
    if (temperature_sensor_install(&tcfg, &s_tsens) == ESP_OK) {
        (void)temperature_sensor_enable(s_tsens);
    } else {
        s_tsens = NULL;
    }
#endif

    memset(&s_last, 0, sizeof(s_last));
    s_screen = SCR_MENU;
    s_menu_index = 0;
    s_traffic_view = 0;
    s_base_tx = s_base_rx = s_base_drop = 0;
    s_prev_us = 0;
    s_cpu_temp_valid = false;
    s_last_temp_us = 0;

    lvgl_port_lock(0);
    styles_init();
    ui_root_create();
    ui_switch(SCR_MENU);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "UI ready");
}

void display_set_status(const status_t *s)
{
    if (!s || !s_disp) return;

    s_last = *s;
    update_rates();

    lvgl_port_lock(0);
    header_update();
    update_active_screen_values();
    lvgl_port_unlock();
}