#include "time_sync.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "time_sync";

/// 24 hours in seconds — threshold for accepting BLE time over stale NTP.
#define NTP_STALE_THRESHOLD  86400

// ── State ────────────────────────────────────────────────────────

static bool   s_time_valid    = false;  // do we have *any* trustworthy clock?
static time_t s_last_ntp_sync = 0;     // epoch seconds of last NTP success
static bool   s_sntp_running  = false;  // is esp_sntp currently initialised?

// ── SNTP callback ────────────────────────────────────────────────

static void on_ntp_sync(struct timeval *tv) {
    s_time_valid = true;
    time(&s_last_ntp_sync);

    struct tm t;
    localtime_r(&s_last_ntp_sync, &t);
    ESP_LOGI(TAG, "NTP sync OK: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
}

// ── Public API ───────────────────────────────────────────────────

void time_sync_init(void) {
    s_time_valid    = false;
    s_last_ntp_sync = 0;
    s_sntp_running  = false;

    // Set timezone to South Africa Standard Time (UTC+2).
    // POSIX TZ format: "SAST-2" means offset is +2 hours east of UTC.
    // This ensures localtime_r() returns SAST and the scheduler's
    // timer comparisons use local wall-clock time.
    setenv("TZ", "SAST-2", 1);
    tzset();

    ESP_LOGI(TAG, "Time sync initialised (TZ=SAST-2, clock invalid until sync)");
}

void time_sync_start_sntp(void) {
    if (s_sntp_running) return;

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(on_ntp_sync);
    esp_sntp_init();
    s_sntp_running = true;

    ESP_LOGI(TAG, "SNTP started (pool.ntp.org)");
}

void time_sync_stop_sntp(void) {
    if (!s_sntp_running) return;

    esp_sntp_stop();
    s_sntp_running = false;
    ESP_LOGI(TAG, "SNTP stopped — RTC continues");
}

void time_sync_set_from_ble(uint32_t unix_time) {
    // If NTP synced recently, the RTC is reliable — ignore BLE time.
    if (s_last_ntp_sync > 0) {
        time_t now;
        time(&now);
        long age = (long)(now - s_last_ntp_sync);
        if (age < NTP_STALE_THRESHOLD) {
            ESP_LOGI(TAG, "Last NTP sync %lds ago — trusting RTC, "
                          "ignoring BLE time", age);
            return;
        }
        ESP_LOGI(TAG, "Last NTP sync %lds ago (>24h) — accepting BLE time",
                 age);
    }

    // Accept BLE time.
    struct timeval tv = { .tv_sec = (time_t)unix_time, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_time_valid = true;

    struct tm t;
    time_t ts = (time_t)unix_time;
    localtime_r(&ts, &t);
    ESP_LOGI(TAG, "Clock set from BLE: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
}

bool time_sync_is_valid(void) {
    return s_time_valid;
}
