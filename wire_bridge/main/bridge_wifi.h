#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool ok;
    int  rssi;     // STA mode only, else 0
} wb_wifi_state_t;

void wb_wifi_start(void);
wb_wifi_state_t wb_wifi_get_state(void);
