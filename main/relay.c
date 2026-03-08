#include "relay.h"

#include "esp_log.h"

static const char *TAG = "relay";
static gpio_num_t s_pin;

void relay_init(gpio_num_t pin) {
    s_pin = pin;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    ESP_LOGI(TAG, "Relay on GPIO %d", pin);
}

void relay_set(bool on) {
    gpio_set_level(s_pin, on ? 1 : 0);
}
