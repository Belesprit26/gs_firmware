#include "device_state.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "relay.h"

// ── Preset timer definitions ─────────────────────────────────────

const uint8_t PRESET_TIMER_HOURS[PRESET_TIMER_COUNT]   = { 4,  6, 15, 17 };
const uint8_t PRESET_TIMER_MINUTES[PRESET_TIMER_COUNT] = { 0,  0,  0,  0 };

// ── Singleton state ──────────────────────────────────────────────

static device_state_t s_state = {
    .temperature    = 0.0f,
    .relay_on       = false,
    .temp_min       = 30,
    .temp_max       = 60,
    .auto_reheat    = false,
    .sensor_ok      = true,
    .max_on_minutes = 240,
};

static SemaphoreHandle_t s_mutex;

// ── Init ─────────────────────────────────────────────────────────

void device_state_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);

    // Initialise preset timers to their hardcoded values (disabled).
    for (int i = 0; i < PRESET_TIMER_COUNT; i++) {
        s_state.timers[i].enabled = false;
        s_state.timers[i].hour    = PRESET_TIMER_HOURS[i];
        s_state.timers[i].minute  = PRESET_TIMER_MINUTES[i];
    }
    // Custom timer (index 4) defaults: disabled, 06:00.
    s_state.timers[PRESET_TIMER_COUNT].enabled = false;
    s_state.timers[PRESET_TIMER_COUNT].hour    = 6;
    s_state.timers[PRESET_TIMER_COUNT].minute   = 0;
}

void device_state_enforce_presets(void) {
    device_state_lock();
    for (int i = 0; i < PRESET_TIMER_COUNT; i++) {
        s_state.timers[i].hour   = PRESET_TIMER_HOURS[i];
        s_state.timers[i].minute = PRESET_TIMER_MINUTES[i];
    }
    device_state_unlock();
}

// ── Lock / Unlock ────────────────────────────────────────────────

void device_state_lock(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void device_state_unlock(void) {
    xSemaphoreGive(s_mutex);
}

device_state_t *device_state_get_ptr(void) {
    return &s_state;
}

// ── Temperature ──────────────────────────────────────────────────

float device_state_get_temperature(void) {
    device_state_lock();
    float t = s_state.temperature;
    device_state_unlock();
    return t;
}

void device_state_set_temperature(float temp) {
    device_state_lock();
    s_state.temperature = temp;
    device_state_unlock();
}

// ── Relay ────────────────────────────────────────────────────────

bool device_state_get_relay(void) {
    device_state_lock();
    bool on = s_state.relay_on;
    device_state_unlock();
    return on;
}

void device_state_set_relay(bool on) {
    device_state_lock();
    s_state.relay_on = on;
    // Drive the GPIO under the same mutex so state and pin can never
    // interleave into disagreement across tasks (state=OFF + GPIO=ON
    // would blind the thermostat's auto-OFF and the max-on backstop).
    relay_set(on);
    device_state_unlock();
}

void device_state_reassert_relay(void) {
    device_state_lock();
    relay_set(s_state.relay_on);
    device_state_unlock();
}

// ── Temp limits ──────────────────────────────────────────────────

void device_state_get_temp_limits(uint8_t *out_min, uint8_t *out_max) {
    device_state_lock();
    *out_min = s_state.temp_min;
    *out_max = s_state.temp_max;
    device_state_unlock();
}

void device_state_clamp_limits(uint8_t *min, uint8_t *max) {
    uint8_t mn = *min, mx = *max;

    if (mn < TEMP_MIN_FLOOR) mn = TEMP_MIN_FLOOR;
    if (mn > TEMP_MIN_CEIL)  mn = TEMP_MIN_CEIL;
    if (mx < TEMP_MAX_FLOOR) mx = TEMP_MAX_FLOOR;
    if (mx > TEMP_MAX_CEIL)  mx = TEMP_MAX_CEIL;

    // Enforce the minimum hysteresis band.  Prefer raising max; if max
    // is already at its ceiling, lower min instead.
    if (mx < mn + TEMP_MIN_DEADBAND) {
        mx = mn + TEMP_MIN_DEADBAND;
        if (mx > TEMP_MAX_CEIL) {
            mx = TEMP_MAX_CEIL;
            if (mn > mx - TEMP_MIN_DEADBAND) mn = mx - TEMP_MIN_DEADBAND;
        }
    }

    *min = mn;
    *max = mx;
}

void device_state_set_temp_limits(uint8_t min, uint8_t max) {
    device_state_clamp_limits(&min, &max);

    device_state_lock();
    s_state.temp_min = min;
    s_state.temp_max = max;
    device_state_unlock();
}

// ── Auto-reheat ──────────────────────────────────────────────────

bool device_state_get_auto_reheat(void) {
    device_state_lock();
    bool v = s_state.auto_reheat;
    device_state_unlock();
    return v;
}

void device_state_set_auto_reheat(bool enabled) {
    device_state_lock();
    s_state.auto_reheat = enabled;
    device_state_unlock();
}

// ── Sensor OK ────────────────────────────────────────────────────

bool device_state_get_sensor_ok(void) {
    device_state_lock();
    bool v = s_state.sensor_ok;
    device_state_unlock();
    return v;
}

void device_state_set_sensor_ok(bool ok) {
    device_state_lock();
    s_state.sensor_ok = ok;
    device_state_unlock();
}

// ── Max-on minutes ───────────────────────────────────────────────

uint16_t device_state_get_max_on_minutes(void) {
    device_state_lock();
    uint16_t v = s_state.max_on_minutes;
    device_state_unlock();
    return v;
}

void device_state_set_max_on_minutes(uint16_t minutes) {
    // 0 = disabled is a legitimate, explicit user choice (the app offers
    // "Off"); anything above MAX_ON_CEIL is clamped — mirrors the RTDB
    // rules' 0..1440 range so BLE writes can't exceed what cloud allows.
    if (minutes > MAX_ON_CEIL) minutes = MAX_ON_CEIL;

    device_state_lock();
    s_state.max_on_minutes = minutes;
    device_state_unlock();
}

// ── Timers ───────────────────────────────────────────────────────

void device_state_get_timers(gs_timer_t *out) {
    device_state_lock();
    memcpy(out, s_state.timers, MAX_TIMERS * sizeof(gs_timer_t));
    device_state_unlock();
}

void device_state_set_timers(const gs_timer_t *timers) {
    device_state_lock();
    memcpy(s_state.timers, timers, MAX_TIMERS * sizeof(gs_timer_t));
    device_state_unlock();
}
