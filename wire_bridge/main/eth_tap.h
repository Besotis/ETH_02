#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef void (*wb_eth_rx_cb_t)(const uint8_t *frame, size_t len, void *user);

void wb_eth_start(wb_eth_rx_cb_t cb, void *user);
bool wb_eth_send(const uint8_t *frame, size_t len);
bool wb_eth_link_up(void);
