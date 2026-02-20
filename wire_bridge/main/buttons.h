#pragma once
#include <stdbool.h>

typedef enum {
    BTN_UP = 0,
    BTN_DOWN,
    BTN_ENTER,
} wb_btn_t;

typedef void (*wb_btn_cb_t)(wb_btn_t btn, bool pressed, void *user);

void buttons_init(wb_btn_cb_t cb, void *user);
