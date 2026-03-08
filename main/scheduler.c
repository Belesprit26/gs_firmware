#include "scheduler.h"

#include <time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_state.h"
#include "relay.h"
#include "nvs_store.h"
#include "gatt_server.h"
#include "time_sync.h"
#include "firebase_rtdb.h"

static const char *TAG = "sched";

void scheduler_task(void *param) {
    const TickType_t interval = pdMS_TO_TICKS(60000);   // 60 s

    for (;;) {
        vTaskDelay(interval);

        // Only act when we have a trustworthy clock.
        if (!time_sync_is_valid()) continue;

        gs_timer_t timers[MAX_TIMERS];
        device_state_get_timers(timers);

        time_t now;
        time(&now);
        struct tm t;
        localtime_r(&now, &t);
        int hh = t.tm_hour;
        int mm = t.tm_min;

        for (int i = 0; i < MAX_TIMERS; i++) {
            if (!timers[i].enabled) continue;

            // ON-only: turn the geyser on at the scheduled time.
            // temp_max (thermostat) is responsible for turning it off.
            if (hh == timers[i].hour && mm == timers[i].minute) {
                if (!device_state_get_relay()) {
                    ESP_LOGI(TAG, "Timer %d → ON (%02d:%02d)", i, hh, mm);
                    device_state_set_relay(true);
                    relay_set(true);
                    nvs_store_save_relay(true);
                    gatt_server_notify_state(true);
                    firebase_rtdb_request_settings_push();
                    firebase_rtdb_request_live_push();
                }
            }
        }
    }
}
