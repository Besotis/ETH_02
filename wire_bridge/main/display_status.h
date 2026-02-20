#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool eth_link;
    bool wifi_up;
    int  rssi;          // dBm; jei nėra - 0
    uint32_t udp_tx;
    uint32_t udp_rx;
    uint32_t udp_drop;
} status_t;

void display_init(void);
void display_set_status(const status_t *s);

// Buttons (be long-press)
void ui_menu_toggle(void);  // atidaro meniu iš bet kur (arba grįžta į status)
void ui_menu_up(void);
void ui_menu_down(void);
void ui_menu_enter(void);