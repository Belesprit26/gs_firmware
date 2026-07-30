#include "scheduler.h"

#include <time.h>
#include "esp_log.h"
#include "esp_task_wdt.h"
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
    const TickType_t interval = pdMS_TO_TICKS(30000);   // 30 s

    int last_fired_hh = -1;
    int last_fired_mm = -1;
    int last_fired_day = -1;   // tm_yday — without it a timer only ever fires once

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    for (;;) {
        esp_task_wdt_reset();
        vTaskDelay(interval);

        if (!time_sync_is_valid()) continue;

        gs_timer_t timers[MAX_TIMERS];
        device_state_get_timers(timers);

        time_t now;
        time(&now);
        struct tm t;
        localtime_r(&now, &t);
        int hh = t.tm_hour;
        int mm = t.tm_min;

        if (hh == last_fired_hh && mm == last_fired_mm && t.tm_yday == last_fired_day) continue;

        for (int i = 0; i < MAX_TIMERS; i++) {
            if (!timers[i].enabled) continue;

            if (hh == timers[i].hour && mm == timers[i].minute) {
                if (!device_state_get_relay()) {
                    ESP_LOGI(TAG, "Timer %d → ON (%02d:%02d)", i, hh, mm);
                    device_state_set_relay(true);
                    nvs_store_save_relay(true);
                    gatt_server_notify_state(true);
                    firebase_rtdb_request_settings_push();
                    firebase_rtdb_request_live_push();

                    last_fired_hh = hh;
                    last_fired_mm = mm;
                    last_fired_day = t.tm_yday;
                }
            }
        }
    }
}
