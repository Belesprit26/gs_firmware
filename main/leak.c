#include "leak.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_state.h"
#include "event_buffer.h"
#include "gatt_server.h"
#include "firebase_rtdb.h"
#include "nvs_store.h"

static const char *TAG = "leak";

// ── Timing / polarity ────────────────────────────────────────────

#define LEAK_POLL_MS          100
#define LEAK_WET_DEBOUNCE_MS  2000    // sustained wet before we act
#define LEAK_DRY_DEBOUNCE_MS  10000   // sustained dry before we release

// Probes bridged by water pull the internally-pulled-up pin LOW. VERIFY on
// the bench against the real probe/module wiring and flip if it's
// active-high.
#define LEAK_WET_LEVEL        0

static gpio_num_t s_pin;

void leak_init(gpio_num_t pin)
{
    s_pin = pin;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,   // polling, no ISR
    };
    gpio_config(&cfg);
    ESP_LOGI(TAG, "Leak sensor ready on GPIO %d (pull-up, wet = level %d)",
             pin, LEAK_WET_LEVEL);
}

void leak_task(void *param)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    bool       active    = false;   // debounced leak state
    TickType_t wet_since = 0;
    TickType_t dry_since = 0;

    for (;;) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(LEAK_POLL_MS));

        bool       wet = (gpio_get_level(s_pin) == LEAK_WET_LEVEL);
        TickType_t now = xTaskGetTickCount();

        if (wet) {
            dry_since = 0;
            if (wet_since == 0) wet_since = now;

            if (!active &&
                (now - wet_since) * portTICK_PERIOD_MS >= LEAK_WET_DEBOUNCE_MS) {
                active = true;
                ESP_LOGW(TAG, "=== WATER LEAK detected — cutting power ===");

                // Lock power OFF first, then cut: while the lockout is set
                // device_state_set_relay(true) is refused, so the scheduler,
                // auto-reheat, button and remote all fail to re-energise a
                // wet geyser.
                device_state_set_leak_lockout(true);
                device_state_set_relay(false);
                nvs_store_save_relay(false);
                gatt_server_notify_state(false);

                event_buffer_push_event(EVT_LEAK, 0);
                gatt_server_notify_event(EVT_LEAK, 0);
                firebase_rtdb_request_event_push(EVT_LEAK, 0);
                firebase_rtdb_request_live_push();
            }
        } else {
            wet_since = 0;
            if (dry_since == 0) dry_since = now;

            if (active &&
                (now - dry_since) * portTICK_PERIOD_MS >= LEAK_DRY_DEBOUNCE_MS) {
                active = false;
                ESP_LOGI(TAG, "Leak cleared — power control restored");

                // Release the lockout; normal control (schedule / user /
                // auto-reheat) resumes. The relay stays OFF until one of
                // those turns it back on.
                device_state_set_leak_lockout(false);

                event_buffer_push_event(EVT_LEAK_CLEAR, 0);
                gatt_server_notify_event(EVT_LEAK_CLEAR, 0);
                firebase_rtdb_request_event_push(EVT_LEAK_CLEAR, 0);
            }
        }
    }
}
