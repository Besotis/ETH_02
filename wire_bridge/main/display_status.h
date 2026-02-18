#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool eth_link;
    bool wifi_up;
    int  rssi;          // jei neturi - dėk 0
    uint32_t udp_tx;
    uint32_t udp_rx;
    uint32_t udp_drop;
} status_t;

void display_init(void);
void display_set_status(const status_t *s);
