#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef void (*wb_frame_rx_cb_t)(const uint8_t *frame, size_t len, void *user);

void wb_udp_start(wb_frame_rx_cb_t cb, void *user);
bool wb_udp_send_frame(const uint8_t *frame, size_t len);

uint32_t wb_udp_get_tx(void);
uint32_t wb_udp_get_rx(void);
uint32_t wb_udp_get_drop(void);