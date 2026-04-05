#pragma once

#include <stdbool.h>
#include <stdint.h>

// ── Timer model ──────────────────────────────────────────────────
//
// 4 preset off-peak timers (fixed times, enable/disable only) +
// 1 fully custom timer.  Timers are ON-only: they turn the geyser
// on at the scheduled time.  temp_max is responsible for turning
// the geyser off.

#define PRESET_TIMER_COUNT 4
#define CUSTOM_TIMER_COUNT 1
#define MAX_TIMERS         (PRESET_TIMER_COUNT + CUSTOM_TIMER_COUNT)

/// Preset timer hours/minutes (indices 0..3).
/// Defined in device_state.c.  Times are read-only; users can only
/// enable or disable each preset.
extern const uint8_t PRESET_TIMER_HOURS[PRESET_TIMER_COUNT];
extern const uint8_t PRESET_TIMER_MINUTES[PRESET_TIMER_COUNT];

/// A single scheduled ON timer entry.
typedef struct {
    bool    enabled;
    uint8_t hour;
    uint8_t minute;
} gs_timer_t;

/// Complete device state — the single source of truth.
///
/// All modules read from and write to this via the accessors below,
/// which are protected by a mutex.  The non-volatile fields (everything
/// except temperature) are persisted to NVS on every change.
typedef struct {
    float    temperature;          // latest sensor reading (°C)
    bool     relay_on;             // relay GPIO state
    uint8_t  temp_min;             // min allowed temp (°C)
    uint8_t  temp_max;             // max allowed temp (°C)
    bool     auto_reheat;          // auto-ON when temp <= temp_min
    gs_timer_t timers[MAX_TIMERS]; // [0..3] = presets, [4] = custom
} device_state_t;

/// Initialise the state mutex.  Call once before any other accessor.
void device_state_init(void);

/// Force preset timer hours/minutes to their hardcoded values.
/// Call after loading NVS so user edits to preset times are ignored.
void device_state_enforce_presets(void);

// ── Temperature limit bounds ─────────────────────────────────────
//
// Enforced at every entry point (BLE, Firebase, NVS load).
// The ranges guarantee min < max without an explicit check.

#define TEMP_MIN_FLOOR   5
#define TEMP_MIN_CEIL   50
#define TEMP_MAX_FLOOR  51
#define TEMP_MAX_CEIL   65

// ── Thread-safe accessors ────────────────────────────────────────

float   device_state_get_temperature(void);
void    device_state_set_temperature(float temp);

bool    device_state_get_relay(void);
void    device_state_set_relay(bool on);

void    device_state_get_temp_limits(uint8_t *out_min, uint8_t *out_max);
void    device_state_set_temp_limits(uint8_t min, uint8_t max);

bool    device_state_get_auto_reheat(void);
void    device_state_set_auto_reheat(bool enabled);

/// Copy all MAX_TIMERS timers into *out*.
void    device_state_get_timers(gs_timer_t *out);
void    device_state_set_timers(const gs_timer_t *timers);

/// Direct pointer access — caller MUST hold the lock.
void            device_state_lock(void);
void            device_state_unlock(void);
device_state_t *device_state_get_ptr(void);
