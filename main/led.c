#include "led.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "device_state.h"
#include "button.h"
#include "wifi_prov.h"
#include "ble_init.h"
#include "firebase_auth.h"

static const char *TAG = "led";

// ── Tunables ─────────────────────────────────────────────────────

#define LED_TICK_MS    30                    // animation tick
#define LED_MAX        48                    // brightness cap (0-255) — dim indicator
#define CONFIRM_TICKS  (1000 / LED_TICK_MS)  // toggle-confirm flash length
#define BREATHE_TICKS  (2500 / LED_TICK_MS)  // breathe period

typedef struct { uint8_t r, g, b; } rgb_t;

static const rgb_t C_OFF   = {  0,   0,   0};
static const rgb_t C_WHITE = {255, 255, 255};
static const rgb_t C_BLUE  = {  0,   0, 255};
static const rgb_t C_GREEN = {  0, 255,   0};
static const rgb_t C_AMBER = {255, 100,   0};
static const rgb_t C_RED   = {255,   0,   0};

static led_strip_handle_t s_strip;
static bool s_ok;

// ── Driver ───────────────────────────────────────────────────────

// NOTE: uses the led_strip 2.x config fields (led_pixel_format /
// led_model). If the build errors on this struct your led_strip is 3.x —
// rename to `.color_component_format` per that version's headers.
void led_init(gpio_num_t pin)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num   = pin,
        .max_leds         = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,   // WS2812B byte order
        .led_model        = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,         // 10 MHz
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    s_ok = (err == ESP_OK);
    if (s_ok) {
        led_strip_clear(s_strip);
        ESP_LOGI(TAG, "Status LED ready on GPIO %d", pin);
    } else {
        ESP_LOGW(TAG, "Status LED init failed: %s — running without it",
                 esp_err_to_name(err));
    }
}

// Write the pixel: colour `c` at animation `level` (0-255), capped at
// LED_MAX overall so the indicator stays dim.
static void put(rgb_t c, uint8_t level)
{
    if (!s_ok) return;
    uint32_t r = (uint32_t)c.r * level / 255 * LED_MAX / 255;
    uint32_t g = (uint32_t)c.g * level / 255 * LED_MAX / 255;
    uint32_t b = (uint32_t)c.b * level / 255 * LED_MAX / 255;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

// 30..255 triangle wave — floored so a breathing "ON" never goes fully dark.
static uint8_t breathe(uint32_t tick, uint32_t period)
{
    uint32_t half = period / 2;
    if (half == 0) return 255;
    uint32_t x  = tick % period;
    uint32_t up = (x < half) ? x : (period - x);   // 0..half
    return (uint8_t)(30 + up * (255 - 30) / half);
}

// Resting colour = where the device is connected.
static rgb_t connectivity_colour(void)
{
    if (!wifi_prov_is_provisioned()) return C_WHITE;   // setup mode
    if (ble_is_connected())          return C_BLUE;    // phone session
    if (wifi_prov_is_link_up() && firebase_auth_is_ready())
                                     return C_GREEN;    // cloud online
    return C_AMBER;                                     // provisioned, offline
}

// ── Task ─────────────────────────────────────────────────────────

void led_task(void *param)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    uint32_t tick       = 0;
    bool     last_relay = device_state_get_relay();
    int      confirm    = 0;         // ticks left in a toggle-confirm flash
    bool     confirm_on = false;     // the relay state it announces
    rgb_t    base       = C_WHITE;   // cached connectivity colour
    bool     setup      = true;

    for (;;) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
        tick++;

        uint32_t held  = button_held_ms();
        bool     relay = device_state_get_relay();

        // Relay edge (button / BLE / schedule) → start a ~1 s confirm flash.
        if (relay != last_relay) {
            confirm    = CONFIRM_TICKS;
            confirm_on = relay;
            last_relay = relay;
        }

        // Priority 1 — button held into factory-wipe territory.
        if (held >= BUTTON_SHORT_MAX_MS) {
            uint32_t to_wipe = (held < BUTTON_RESET_HOLD_MS)
                             ? (BUTTON_RESET_HOLD_MS - held) : 0;
            if (to_wipe <= 2000) put(C_RED, 255);                     // solid: release now
            else                 put(C_RED, (tick % 6 < 3) ? 255 : 0); // strobe: heading to wipe
            continue;
        }

        // Priority 2 — button held (pre-toggle): blackout = "input received".
        if (held > 0) {
            put(C_OFF, 0);
            confirm = 0;     // a fresh press cancels a stale confirm
            continue;
        }

        // Priority 3 — toggle-confirm: white → green(on)/red(off) → white.
        if (confirm > 0) {
            confirm--;
            int third = CONFIRM_TICKS / 3;
            rgb_t c = (confirm > 2 * third) ? C_WHITE
                    : (confirm > third)     ? (confirm_on ? C_GREEN : C_RED)
                                            : C_WHITE;
            put(c, 255);
            continue;
        }

        // Priority 4 — resting: colour = connectivity, motion = relay.
        if (tick % 10 == 0) {                       // re-evaluate ~3x/sec
            setup = !wifi_prov_is_provisioned();
            base  = connectivity_colour();
        }
        if (setup) {
            // Provisioning feedback while unprovisioned.
            switch (wifi_prov_status()) {
                case PROV_CONNECTING:
                    put(C_GREEN, (tick % 16 < 8) ? 255 : 0);   // ~2 Hz: connecting
                    break;
                case PROV_WIFI_FAIL:
                case PROV_ERROR:
                    put(C_RED, (tick % 16 < 8) ? 255 : 0);     // ~2 Hz: setup failed
                    break;
                default:
                    put(C_WHITE, breathe(tick, BREATHE_TICKS)); // waiting for setup
                    break;
            }
        } else if (relay) {
            put(base, breathe(tick, BREATHE_TICKS));   // breathe = relay ON
        } else {
            put(base, 90);                             // steady dim = relay OFF
        }
    }
}
