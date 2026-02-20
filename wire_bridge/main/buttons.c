#include "buttons.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "btn";

#define PIN_UP     39
#define PIN_DOWN   36
#define PIN_ENTER  35

// active-low buttons: connect pin -> GND when pressed
// IMPORTANT: GPIO35/36/39 have NO internal pull-ups on ESP32. Use external pull-ups.
#define POLL_MS        10
#define DEBOUNCE_TICKS 4   // 4*10ms=40ms

typedef struct {
    int pin;
    wb_btn_t id;
    uint8_t stable;       // pressed=1 / released=0
    uint8_t last_raw;
    uint8_t cnt;
} btn_state_t;

static wb_btn_cb_t s_cb = NULL;
static void *s_user = NULL;

static btn_state_t s_btns[] = {
    { .pin = PIN_UP,    .id = BTN_UP,    .stable = 0, .last_raw = 0, .cnt = 0 },
    { .pin = PIN_DOWN,  .id = BTN_DOWN,  .stable = 0, .last_raw = 0, .cnt = 0 },
    { .pin = PIN_ENTER, .id = BTN_ENTER, .stable = 0, .last_raw = 0, .cnt = 0 },
};

static inline uint8_t read_pressed(int pin)
{
    // gpio_get_level: 1 normally (pull-up), 0 when pressed -> return pressed=1
    return (gpio_get_level(pin) == 0) ? 1 : 0;
}

static void buttons_task(void *arg)
{
    (void)arg;

    while (1) {
        for (int i = 0; i < (int)(sizeof(s_btns)/sizeof(s_btns[0])); i++) {
            btn_state_t *b = &s_btns[i];
            uint8_t raw = read_pressed(b->pin);

            if (raw != b->last_raw) {
                b->last_raw = raw;
                b->cnt = 0;
            } else {
                if (b->cnt < DEBOUNCE_TICKS) b->cnt++;
                if (b->cnt == DEBOUNCE_TICKS && raw != b->stable) {
                    b->stable = raw;
                    if (s_cb) s_cb(b->id, b->stable ? true : false, s_user);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void buttons_init(wb_btn_cb_t cb, void *user)
{
    s_cb = cb;
    s_user = user;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL<<PIN_UP) | (1ULL<<PIN_DOWN) | (1ULL<<PIN_ENTER),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    ESP_LOGI(TAG, "Buttons: UP=%d DOWN=%d ENTER=%d (active-low, needs external pull-up)",
             PIN_UP, PIN_DOWN, PIN_ENTER);

    xTaskCreate(buttons_task, "buttons", 2048, NULL, 12, NULL);
}
