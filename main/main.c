#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_state.h"
#include "nvs_store.h"
#include "relay.h"
#include "temperature.h"
#include "ble_init.h"
#include "scheduler.h"
#include "wifi_prov.h"
#include "time_sync.h"
#include "event_buffer.h"
#include "button.h"
#include "firebase_auth.h"
#include "firebase_rtdb.h"

static const char *TAG = "main";

// ── Pin assignments (Seeed XIAO ESP32-C6) ────────────────────────

#define PIN_RELAY       GPIO_NUM_23     // D5 / SCL
#define PIN_DS18B20     GPIO_NUM_21     // D3
#define PIN_BUTTON      GPIO_NUM_2      // A2 / D2

// ── Task stack sizes ─────────────────────────────────────────────

#define SENSOR_STACK    4096
#define SCHED_STACK     2048
#define BUTTON_STACK    2048
#define FIREBASE_STACK  12288

void app_main(void) {
    ESP_LOGI(TAG, "GeyserSwitch firmware starting");

    // 1. NVS — must come first, BLE also depends on it.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Device state — initialise mutex + defaults, then load config.
    device_state_init();

    device_state_lock();
    device_state_t *s = device_state_get_ptr();
    nvs_store_load(s);
    device_state_unlock();

    // Force preset timer times to their hardcoded values (NVS may
    // have been edited by an old firmware or corrupted).
    device_state_enforce_presets();

    // 3. Event buffer — initialise ring buffers before tasks start.
    event_buffer_init();

    // 4. Time sync — initialise before BLE/WiFi.
    time_sync_init();

    // 5. Relay — configure GPIO and restore last known position.
    relay_init(PIN_RELAY);
    relay_set(device_state_get_relay());
    ESP_LOGI(TAG, "Relay restored → %s", device_state_get_relay() ? "ON" : "OFF");

    // 6. Temperature sensor — non-fatal if missing.
    if (temperature_init(PIN_DS18B20) != ESP_OK) {
        ESP_LOGW(TAG, "Running without temperature sensor");
    }

    // 7. Load device nickname (before BLE, so advertising name is correct).
    wifi_prov_load_nickname();

    // 8. BLE — init stack, register GATT services, start advertising.
    //    Name is "GeyserSwitch-{nick}" if provisioned with nickname,
    //    "GeyserSwitch-Setup" if unprovisioned, "GeyserSwitch" otherwise.
    ble_init();

    // 9. WiFi — if WiFi creds are stored, reconnect in the background.
    //    time_sync_start_sntp() is called inside prov_connect_task
    //    after WiFi connects successfully.
    if (wifi_prov_has_wifi()) {
        ESP_LOGI(TAG, "WiFi credentials stored — reconnecting");
        wifi_prov_start_wifi();
    } else if (wifi_prov_is_provisioned()) {
        ESP_LOGI(TAG, "BLE-only provisioned — no WiFi to connect");
    } else {
        ESP_LOGI(TAG, "Not provisioned — waiting for setup via BLE");
    }

    // 10. Physical button — configure GPIO (non-fatal if no button wired).
    button_init(PIN_BUTTON);

    // 11. Firebase — init the async request flags, then start the
    //     sync task if WiFi creds + auth credentials exist.
    firebase_rtdb_init();
    if (wifi_prov_has_wifi() && firebase_auth_init()) {
        ESP_LOGI(TAG, "Firebase auth ready — starting sync task");
        xTaskCreate(firebase_task, "firebase", FIREBASE_STACK, NULL, 2, NULL);
    } else {
        ESP_LOGI(TAG, "Firebase sync not available (wifi=%d, auth=%d)",
                 wifi_prov_has_wifi(), firebase_auth_is_ready());
    }

    // 12. Background tasks.
    xTaskCreate(temperature_task, "sensor",    SENSOR_STACK, NULL, 5, NULL);
    xTaskCreate(scheduler_task,   "scheduler", SCHED_STACK,  NULL, 3, NULL);
    xTaskCreate(button_task,      "button",    BUTTON_STACK, NULL, 4, NULL);

    ESP_LOGI(TAG, "All systems go");
}
