#include "button.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_state.h"
#include "relay.h"
#include "nvs_store.h"
#include "gatt_server.h"
#include "firebase_rtdb.h"

static const char *TAG = "button";

// ── Timing thresholds ────────────────────────────────────────────

#define POLL_MS         20      // polling interval
#define DEBOUNCE_MS     50      // ignore presses shorter than this
#define SHORT_MAX_MS    5000    // max duration for a "short press"
#define RESET_HOLD_MS   10000   // hold this long for factory reset

static gpio_num_t s_pin;

// ── Init ─────────────────────────────────────────────────────────

void button_init(gpio_num_t pin) {
    s_pin = pin;

    gpio_config_t cfg = {
        .pin_bit_mask  = (1ULL << pin),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,   // polling, no ISR
    };
    gpio_config(&cfg);

    ESP_LOGI(TAG, "Button ready on GPIO %d (pull-up, active-low)", pin);
}

// ── Task ─────────────────────────────────────────────────────────

void button_task(void *param) {
    bool was_pressed    = false;
    TickType_t press_start = 0;
    bool reset_fired    = false;

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    for (;;) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        bool pressed = (gpio_get_level(s_pin) == 0);   // active-low

        // ── Rising edge: button just pressed ─────────────────────
        if (pressed && !was_pressed) {
            press_start  = xTaskGetTickCount();
            reset_fired  = false;
        }

        // ── Held: check for factory-reset threshold ──────────────
        if (pressed && was_pressed && !reset_fired) {
            uint32_t held_ms =
                (xTaskGetTickCount() - press_start) * portTICK_PERIOD_MS;

            if (held_ms >= RESET_HOLD_MS) {
                reset_fired = true;
                ESP_LOGW(TAG, "=== FACTORY RESET (held %lu ms) ===",
                         (unsigned long)held_ms);

                // Switch the geyser off before wiping, so a factory
                // reset leaves it in a known, predictable state.  Via
                // the atomic setter — a bare relay_set() would leave
                // device_state believing ON, and the temperature task's
                // reassert could re-drive the GPIO during the erase.
                device_state_set_relay(false);

                // Erase all NVS (WiFi creds, UID, nickname, config).
                nvs_flash_erase();

                // Hard reboot — device comes up as "GeyserSwitch-Setup".
                esp_restart();
                // Does not return.
            }
        }

        // ── Falling edge: button released ────────────────────────
        if (!pressed && was_pressed) {
            uint32_t held_ms =
                (xTaskGetTickCount() - press_start) * portTICK_PERIOD_MS;

            if (held_ms >= DEBOUNCE_MS && held_ms <= SHORT_MAX_MS) {
                // Short press → toggle relay.
                bool on = !device_state_get_relay();
                device_state_set_relay(on);
                nvs_store_save_relay(on);
                gatt_server_notify_state(on);
                firebase_rtdb_request_settings_push();
                firebase_rtdb_request_live_push();

                ESP_LOGI(TAG, "Short press (%lu ms) → relay %s",
                         (unsigned long)held_ms, on ? "ON" : "OFF");
            } else if (held_ms > SHORT_MAX_MS && !reset_fired) {
                // Medium press (2–10 s) — intentionally ignored.
                ESP_LOGI(TAG, "Medium press (%lu ms) — ignored",
                         (unsigned long)held_ms);
            }
        }

        was_pressed = pressed;
    }
}
