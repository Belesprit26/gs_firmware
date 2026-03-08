#include "event_buffer.h"

#include <time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "evtbuf";

// ── Event ring buffer ────────────────────────────────────────────

static buffered_event_t s_events[EVENT_BUF_MAX];
static uint8_t s_evt_head  = 0;   // next write position
static uint8_t s_evt_count = 0;   // valid entries

// ── Telemetry ring buffer ────────────────────────────────────────

static buffered_telemetry_t s_telem[TELEM_BUF_MAX];
static uint8_t s_tel_head  = 0;
static uint8_t s_tel_count = 0;

static SemaphoreHandle_t s_mutex;

// ── Init ─────────────────────────────────────────────────────────

void event_buffer_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);
    ESP_LOGI(TAG, "Event buffer ready (events=%d, telemetry=%d)",
             EVENT_BUF_MAX, TELEM_BUF_MAX);
}

// ── Events ───────────────────────────────────────────────────────

void event_buffer_push_event(uint8_t type, uint8_t temp) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    time_t now;
    time(&now);

    s_events[s_evt_head].type      = type;
    s_events[s_evt_head].temp      = temp;
    s_events[s_evt_head].timestamp = (uint32_t)now;

    s_evt_head = (s_evt_head + 1) % EVENT_BUF_MAX;
    if (s_evt_count < EVENT_BUF_MAX) s_evt_count++;

    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Event pushed: type=0x%02x temp=%d°C (buf=%d)",
             type, temp, s_evt_count);
}

uint8_t event_buffer_get_events(buffered_event_t *out, uint8_t max_count) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    uint8_t n = (s_evt_count < max_count) ? s_evt_count : max_count;
    // Start from oldest entry in the circular buffer.
    uint8_t start = (s_evt_head + EVENT_BUF_MAX - s_evt_count) % EVENT_BUF_MAX;

    for (uint8_t i = 0; i < n; i++) {
        out[i] = s_events[(start + i) % EVENT_BUF_MAX];
    }

    xSemaphoreGive(s_mutex);
    return n;
}

// ── Telemetry ────────────────────────────────────────────────────

void event_buffer_push_telemetry(int16_t temp_raw, uint8_t relay,
                                  uint8_t min_t, uint8_t max_t,
                                  uint8_t auto_rh) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    time_t now;
    time(&now);

    s_telem[s_tel_head].temp_raw    = temp_raw;
    s_telem[s_tel_head].relay_on    = relay;
    s_telem[s_tel_head].min_temp    = min_t;
    s_telem[s_tel_head].max_temp    = max_t;
    s_telem[s_tel_head].auto_reheat = auto_rh;
    s_telem[s_tel_head].timestamp   = (uint32_t)now;

    s_tel_head = (s_tel_head + 1) % TELEM_BUF_MAX;
    if (s_tel_count < TELEM_BUF_MAX) s_tel_count++;

    xSemaphoreGive(s_mutex);
}

uint8_t event_buffer_get_telemetry(buffered_telemetry_t *out, uint8_t max_count) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    uint8_t n = (s_tel_count < max_count) ? s_tel_count : max_count;
    uint8_t start = (s_tel_head + TELEM_BUF_MAX - s_tel_count) % TELEM_BUF_MAX;

    for (uint8_t i = 0; i < n; i++) {
        out[i] = s_telem[(start + i) % TELEM_BUF_MAX];
    }

    xSemaphoreGive(s_mutex);
    return n;
}

// ── Clear ────────────────────────────────────────────────────────

void event_buffer_clear(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_evt_head  = 0;
    s_evt_count = 0;
    s_tel_head  = 0;
    s_tel_count = 0;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Buffers cleared (app acknowledged)");
}
