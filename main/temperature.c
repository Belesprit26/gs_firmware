#include "temperature.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "onewire_bus.h"
#include "ds18b20.h"

#include "device_state.h"
#include "gatt_server.h"
#include "relay.h"
#include "nvs_store.h"
#include "event_buffer.h"
#include "firebase_rtdb.h"

static const char *TAG = "temp";

static onewire_bus_handle_t    s_bus    = NULL;
static ds18b20_device_handle_t s_sensor = NULL;
static gpio_num_t              s_pin    = GPIO_NUM_NC;

// ── EMA smoothing ────────────────────────────────────────────────

#define EMA_ALPHA 0.3f

static float s_smoothed_temp  = 0.0f;
static bool  s_ema_initialised = false;

// ── Sensor recovery ──────────────────────────────────────────────

#define SENSOR_RETRY_AFTER 3

/// Readings outside this window are treated as sensor faults.
/// A geyser will never legitimately read below 0 or above 100.
#define TEMP_SANE_MIN  0.0f
#define TEMP_SANE_MAX  100.0f

static int s_consec_fails = 0;

// ── Event debounce ───────────────────────────────────────────────

/// Minimum interval between firing the same event type (5 minutes).
#define EVENT_DEBOUNCE_TICKS  pdMS_TO_TICKS(300000)

static TickType_t s_last_event_tick[EVT_TYPE_COUNT] = {0};

/// Try to fire an event.  Returns true if fired, false if debounced.
static bool try_fire_event(uint8_t type, uint8_t temp_int) {
    TickType_t now = xTaskGetTickCount();
    uint8_t idx = type - 1;   // EVT_* are 1-indexed

    if (s_last_event_tick[idx] != 0 &&
        (now - s_last_event_tick[idx]) < EVENT_DEBOUNCE_TICKS) {
        return false;
    }

    s_last_event_tick[idx] = now;

    // Push into RAM ring buffer (for BLE reconnect sync).
    event_buffer_push_event(type, temp_int);

    // Notify connected BLE client in real-time (if any).
    gatt_server_notify_event(type, temp_int);

    // Push to Firebase RTDB → triggers Cloud Function → FCM push.
    firebase_rtdb_request_event_push(type, temp_int);

    return true;
}

// ── Sensor failure flag ───────────────────────────────────────────

static bool s_sensor_fail_notified = false;

// ── Max continuous run timer ─────────────────────────────────────

static int  s_relay_on_seconds = 0;
static bool s_was_relay_on     = false;

// ── Telemetry buffering ──────────────────────────────────────────

/// Buffer a telemetry snapshot every 15 × 10 s = 2.5 minutes.
#define TELEM_BUF_INTERVAL 15

static int s_telem_counter = 0;

// ── Init ─────────────────────────────────────────────────────────

esp_err_t temperature_init(gpio_num_t data_pin) {
    s_pin = data_pin;

    // Create the OneWire bus once — the RMT channel stays allocated
    // for the lifetime of the firmware so rediscovery can reuse it.
    if (!s_bus) {
        onewire_bus_config_t bus_cfg = { .bus_gpio_num = data_pin };
        onewire_bus_rmt_config_t rmt_cfg = { .max_rx_bytes = 10 };

        esp_err_t err = onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &s_bus);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OneWire bus init failed: %s",
                     esp_err_to_name(err));
            return err;
        }
    }

    s_sensor = NULL;
    ds18b20_config_t ds_cfg = {};
    esp_err_t err = ds18b20_new_device_from_bus(s_bus, &ds_cfg, &s_sensor);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS18B20 not found on GPIO %d: %s",
                 data_pin, esp_err_to_name(err));
        return err;
    }

    err = ds18b20_set_resolution(s_sensor, DS18B20_RESOLUTION_12B);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS18B20 set_resolution failed — sensor not responding");
        s_sensor = NULL;
        return err;
    }

    // Validation: do one real conversion to prove the sensor is alive
    // and returning sane data before declaring it ready.
    float validation;
    err = ds18b20_trigger_temperature_conversion(s_sensor);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS18B20 validation conversion failed");
        s_sensor = NULL;
        return err;
    }
    err = ds18b20_get_temperature(s_sensor, &validation);
    if (err != ESP_OK ||
        validation < TEMP_SANE_MIN || validation > TEMP_SANE_MAX) {
        ESP_LOGW(TAG, "DS18B20 validation bad: %.1f°C — rejecting",
                 validation);
        s_sensor = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "DS18B20 ready on GPIO %d (%.1f°C)", data_pin, validation);
    return ESP_OK;
}

/// Attempt to rediscover the sensor after consecutive failures.
/// Tears down the OneWire RMT bus first, because a physical
/// disconnect can leave the RMT peripheral in a broken state
/// that no amount of device-level retries will fix.
static bool try_rediscover(void) {
    if (s_pin == GPIO_NUM_NC) return false;

    ESP_LOGW(TAG, "Attempting sensor rediscovery on GPIO %d", s_pin);

    if (s_bus) {
        onewire_bus_del(s_bus);
        s_bus = NULL;
    }
    s_sensor = NULL;

    if (temperature_init(s_pin) == ESP_OK) {
        s_ema_initialised = false;
        s_consec_fails = 0;
        return true;
    }
    return false;
}

// ── Single read ──────────────────────────────────────────────────

esp_err_t temperature_read(float *out_temp_c) {
    if (s_sensor == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ds18b20_trigger_temperature_conversion(s_sensor);
    if (err != ESP_OK) return err;

    return ds18b20_get_temperature(s_sensor, out_temp_c);
}

// ── Task (EMA smoothing + thermostat + events + telemetry buf) ───

void temperature_task(void *param) {
    const TickType_t interval = pdMS_TO_TICKS(10000);   // 10 s

    // Watchdog-subscribed so a hang here (e.g. a wedged OneWire read)
    // reboots the unit rather than leaving it unresponsive to the app,
    // the schedule and the cloud.  The reboot restores the previous
    // relay state — the geyser regulates itself either way.
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    for (;;) {
        esp_task_wdt_reset();

        float raw_temp;
        bool valid_read = false;

        if (temperature_read(&raw_temp) == ESP_OK) {
            if (raw_temp >= TEMP_SANE_MIN && raw_temp <= TEMP_SANE_MAX) {
                valid_read = true;
            } else {
                ESP_LOGW(TAG, "Out-of-range reading %.1f°C — treating "
                         "as sensor fault", raw_temp);
            }
        }

        if (valid_read) {
            if (s_consec_fails > 0) {
                ESP_LOGI(TAG, "Sensor recovered after %d failed reads",
                         s_consec_fails);
            }
            s_consec_fails = 0;

            if (s_sensor_fail_notified) {
                ESP_LOGI(TAG, "Sensor back online — clearing sentinel");
                device_state_set_sensor_ok(true);
                s_sensor_fail_notified = false;
                try_fire_event(EVT_SENSOR_RECOVER, (uint8_t)raw_temp);
            }

            // ── EMA smoothing ────────────────────────────────────
            if (!s_ema_initialised) {
                s_smoothed_temp  = raw_temp;
                s_ema_initialised = true;
            } else {
                s_smoothed_temp = EMA_ALPHA * raw_temp
                                + (1.0f - EMA_ALPHA) * s_smoothed_temp;
            }

            device_state_set_temperature(s_smoothed_temp);
            gatt_server_notify_temperature(s_smoothed_temp);

            // ── Thermostat enforcement ───────────────────────────
            uint8_t min_t, max_t;
            device_state_get_temp_limits(&min_t, &max_t);
            bool relay = device_state_get_relay();
            uint8_t temp_int = (uint8_t)s_smoothed_temp;

            // Auto-OFF: temp reached or exceeded max → turn off.
            if (relay && s_smoothed_temp >= (float)max_t) {
                ESP_LOGI(TAG, "Temp %.1f°C >= max %d°C → auto-OFF",
                         s_smoothed_temp, max_t);
                device_state_set_relay(false);
                nvs_store_save_relay(false);
                gatt_server_notify_state(false);
                firebase_rtdb_request_settings_push();
                firebase_rtdb_request_live_push();

                try_fire_event(EVT_MAX_TEMP_OFF, temp_int);
            }

            // Min-temp logic — depends on auto-reheat setting.
            if (!relay && s_smoothed_temp <= (float)min_t) {
                if (device_state_get_auto_reheat()) {
                    ESP_LOGI(TAG, "Temp %.1f°C <= min %d°C "
                             "(auto-reheat) → auto-ON",
                             s_smoothed_temp, min_t);
                    device_state_set_relay(true);
                    nvs_store_save_relay(true);
                    gatt_server_notify_state(true);
                    firebase_rtdb_request_settings_push();
                    firebase_rtdb_request_live_push();

                    try_fire_event(EVT_MIN_TEMP_ON, temp_int);
                } else {
                    // Alert only — user disabled auto-reheat.
                    try_fire_event(EVT_MIN_TEMP_ALERT, temp_int);
                }
            }

            // ── Telemetry buffering (every 2.5 min) ──────────────
            s_telem_counter++;
            if (s_telem_counter >= TELEM_BUF_INTERVAL) {
                s_telem_counter = 0;
                event_buffer_push_telemetry(
                    (int16_t)(s_smoothed_temp * 100.0f),
                    device_state_get_relay() ? 1 : 0,
                    min_t, max_t,
                    device_state_get_auto_reheat() ? 1 : 0
                );
            }
        } else {
            s_consec_fails++;
            if (s_consec_fails % SENSOR_RETRY_AFTER == 0) {
                if (!try_rediscover()) {
                    ESP_LOGW(TAG, "Sensor still absent (%d fails) "
                             "— next retry in %ds",
                             s_consec_fails,
                             SENSOR_RETRY_AFTER * 10);
                }
            }

            if (!s_sensor_fail_notified &&
                s_consec_fails >= SENSOR_RETRY_AFTER) {
                ESP_LOGW(TAG, "Sensor failure confirmed — pushing sentinel");
                s_sensor_fail_notified = true;
                device_state_set_sensor_ok(false);
                device_state_set_temperature(-1.0f);
                gatt_server_notify_temperature(-1.0f);
                firebase_rtdb_request_live_push();
                try_fire_event(EVT_SENSOR_FAIL, 0);

                // The relay is deliberately LEFT AS-IS.  It switches the
                // geyser at the mains, upstream of the geyser's own
                // mechanical thermostat, which still regulates water
                // temperature on its own.  A dead sensor costs us
                // monitoring and smart scheduling, not safety — cutting
                // power here would turn a sensor fault into a
                // no-hot-water callout.  The user is notified instead.
            }
        }

        // ── Max continuous run timer ───────────────────────────────
        // An ENERGY / "left on too long" convenience: bounds how long
        // the geyser stays powered in one stretch, entirely as the user
        // configured it (0 = Off is a valid explicit choice).  This is
        // NOT a safety cutoff — the geyser's own mechanical thermostat
        // governs temperature and is untouched by this controller.  The
        // accumulator resets only when the relay is actually OFF, so a
        // flapping sensor cannot extend the window.
        {
            bool relay_now = device_state_get_relay();

            if (relay_now) {
                if (!s_was_relay_on) {
                    s_relay_on_seconds = 0;
                }
                s_relay_on_seconds += 10;

                uint16_t max_on = device_state_get_max_on_minutes();
                if (max_on > 0 &&
                    s_relay_on_seconds >= (int)max_on * 60) {
                    ESP_LOGI(TAG, "Max continuous run (%u min) reached — "
                             "switching off", max_on);
                    device_state_set_relay(false);
                    nvs_store_save_relay(false);
                    gatt_server_notify_state(false);
                    firebase_rtdb_request_settings_push();
                    firebase_rtdb_request_live_push();

                    try_fire_event(EVT_MAX_ON_TIMEOUT, 0);
                    s_relay_on_seconds = 0;
                    relay_now = false;
                }
            } else {
                s_relay_on_seconds = 0;
            }
            s_was_relay_on = relay_now;
        }

        // Reassert the GPIO from state every cycle — self-heals any
        // residual state/pin desync (belt-and-braces alongside the
        // atomic drive inside device_state_set_relay()).
        device_state_reassert_relay();

        vTaskDelay(interval);
    }
}
