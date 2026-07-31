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
#include "temperature.h"
#include "event_buffer.h"

static const char *TAG = "sched";

// ── Tuning ───────────────────────────────────────────────────────

/// A timer may not re-fire within this window of a run-limit cutoff.
/// Without it, a duration that exactly equals the gap between two
/// slots (e.g. 2 h with timers at 04:00 and 06:00) lets the limit
/// switch off at 06:00:00 and the 06:00 timer switch straight back on
/// seconds later, silently doubling the block.
#define CUTOFF_GUARD_MS  60000

/// How long after boot to wait for a usable clock before falling back
/// to interval mode.  SNTP normally lands in seconds and a phone may
/// connect shortly after; much longer than this and an early-morning
/// block would be missed after an overnight outage.
#define FALLBACK_GRACE_MS  (15 * 60 * 1000)

/// Block length assumed when deriving the interval duty and the user
/// has no run limit set (max_on = 0), so a duty can still be computed.
#define FALLBACK_ASSUMED_BLOCK_MIN  120

/// Never leave the geyser off for less than this in interval mode.
#define FALLBACK_MIN_OFF_MIN  30

// ── Interval (clock-less) fallback ───────────────────────────────
//
// The schedule is wall-clock based, so a unit with no usable clock —
// BLE-only and never told the time, or rebooted after an outage with
// the router still down — would do nothing at all.  Interval mode
// keeps heating on a free-running tick counter at the SAME DAILY DUTY
// the user's own schedule asks for, so their energy budget is
// preserved even though the phase is unknowable without a clock.
//
// It self-cancels the moment the clock becomes usable, which happens
// automatically as soon as the app connects over BLE (it pushes phone
// time on every connection) or SNTP succeeds.

/// Derive the interval on/off block lengths from the user's schedule:
/// n enabled timers of `block` minutes each, spread evenly over a day.
static void derive_interval(const gs_timer_t *timers,
                            int *out_on_min, int *out_off_min) {
    int enabled = 0;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].enabled) enabled++;
    }
    if (enabled < 1) enabled = 1;

    int block = (int)device_state_get_max_on_minutes();
    if (block <= 0) block = FALLBACK_ASSUMED_BLOCK_MIN;

    int period = 1440 / enabled;
    int off    = period - block;
    if (off < FALLBACK_MIN_OFF_MIN) off = FALLBACK_MIN_OFF_MIN;

    *out_on_min  = block;
    *out_off_min = off;
}

static void fire_event(uint8_t type) {
    uint8_t temp_int = (uint8_t)device_state_get_temperature();
    event_buffer_push_event(type, temp_int);
    gatt_server_notify_event(type, temp_int);
    firebase_rtdb_request_event_push(type, temp_int);
}

void scheduler_task(void *param) {
    const TickType_t interval = pdMS_TO_TICKS(30000);   // 30 s

    int last_fired_hh = -1;
    int last_fired_mm = -1;
    int last_fired_day = -1;   // tm_yday — without it a timer only ever fires once

    const TickType_t boot_tick = xTaskGetTickCount();

    // Interval-mode bookkeeping.
    bool       fb_prev_relay = device_state_get_relay();
    TickType_t fb_off_since  = xTaskGetTickCount();

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    for (;;) {
        esp_task_wdt_reset();
        vTaskDelay(interval);

        TickType_t now_tick = xTaskGetTickCount();

        // Track when the geyser last switched off, for interval mode.
        bool relay_now = device_state_get_relay();
        if (fb_prev_relay && !relay_now) {
            fb_off_since = now_tick;
        }
        fb_prev_relay = relay_now;

        // ── No usable clock → interval mode ──────────────────────
        if (!time_sync_is_valid()) {
            gs_timer_t timers[MAX_TIMERS];
            device_state_get_timers(timers);

            bool any_timer = false;
            for (int i = 0; i < MAX_TIMERS; i++) {
                if (timers[i].enabled) { any_timer = true; break; }
            }

            // Only stand in for a schedule the user actually relies on;
            // a manual-only user would find surprise cycling worse than
            // nothing.  Honour the opt-out, and give the clock time to
            // arrive before taking over.
            if (!any_timer || !device_state_get_fallback_enabled() ||
                (now_tick - boot_tick) < pdMS_TO_TICKS(FALLBACK_GRACE_MS)) {
                continue;
            }

            if (!device_state_get_fallback_active()) {
                ESP_LOGW(TAG, "Clock unusable — schedule paused, "
                              "interval mode on");
                device_state_set_fallback_active(true);
                fire_event(EVT_CLOCK_LOST);
            }

            int on_min, off_min;
            derive_interval(timers, &on_min, &off_min);

            if (relay_now) {
                // The run limit does the switching off whenever one is
                // set; only cover the case where it is not.
                if (device_state_get_max_on_minutes() == 0 &&
                    temperature_current_on_seconds() >= on_min * 60) {
                    ESP_LOGI(TAG, "Interval mode → OFF after %d min", on_min);
                    device_state_set_relay(false);
                    nvs_store_save_relay(false);
                    gatt_server_notify_state(false);
                }
            } else if ((now_tick - fb_off_since) >=
                       pdMS_TO_TICKS((uint32_t)off_min * 60000)) {
                ESP_LOGI(TAG, "Interval mode → ON (after %d min off)", off_min);
                device_state_set_relay(true);
                nvs_store_save_relay(true);
                gatt_server_notify_state(true);
            }
            continue;
        }

        // ── Clock usable → normal schedule ───────────────────────
        if (device_state_get_fallback_active()) {
            ESP_LOGI(TAG, "Clock recovered — normal schedule resumed");
            device_state_set_fallback_active(false);
            fire_event(EVT_SCHEDULE_OK);
        }

        gs_timer_t timers[MAX_TIMERS];
        device_state_get_timers(timers);

        time_t now;
        time(&now);
        struct tm t;
        localtime_r(&now, &t);
        int hh = t.tm_hour;
        int mm = t.tm_min;

        if (hh == last_fired_hh && mm == last_fired_mm && t.tm_yday == last_fired_day) continue;

        // Suppress a re-fire that coincides with a run-limit cutoff.
        TickType_t last_cutoff = temperature_last_limit_cutoff();
        if (last_cutoff != 0 &&
            (now_tick - last_cutoff) < pdMS_TO_TICKS(CUTOFF_GUARD_MS)) {
            continue;
        }

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
