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
/// except temperature and sensor_ok) are persisted to NVS on every change.
typedef struct {
    float    temperature;          // latest sensor reading (°C), -1 = sensor offline
    bool     relay_on;             // relay GPIO state
    uint8_t  temp_min;             // min allowed temp (°C)
    uint8_t  temp_max;             // max allowed temp (°C)
    bool     auto_reheat;          // auto-ON when temp <= temp_min
    gs_timer_t timers[MAX_TIMERS]; // [0..3] = presets, [4] = custom
    bool     sensor_ok;            // false when DS18B20 is unresponsive
    uint16_t max_on_minutes;       // 0 = disabled, else relay switched OFF after N minutes
    bool     fallback_enabled;     // allow interval mode when the clock is unusable
    uint32_t relay_on_since;       // epoch seconds of the current ON stretch (0 = unknown)
    bool     fallback_active;      // RAM only: interval mode is currently driving the relay
} device_state_t;

/// Initialise the state mutex.  Call once before any other accessor.
void device_state_init(void);

/// Force preset timer hours/minutes to their hardcoded values.
/// Call after loading NVS so user edits to preset times are ignored.
void device_state_enforce_presets(void);

// ── Temperature limit bounds ─────────────────────────────────────
//
// The user's smart setpoints, applied ON TOP OF the geyser's own
// mechanical thermostat by gating mains power: they let the user run
// cooler (and so cheaper) than the factory setting, or hold a normal
// hot-water temperature.  Cutting power when the measured temperature
// exceeds max_t still adds a protective backstop should the geyser's
// own thermostat stick closed — the cut then happens at a safe
// hot-water temperature, far below the TP relief valve (~93 °C) that
// remains the true over-temperature safety device.  TEMP_MAX_CEIL now
// tracks the top of the usual factory-thermostat range rather than
// sitting below it, matching what real geysers do; the backstop still
// fires, just nearer the top of that range.
//
// Enforced at every entry point (BLE, Firebase, NVS load) via
// device_state_clamp_limits(), which applies both the ranges below
// and a minimum hysteresis band (deadband) so min and max can never
// sit close enough to cause relay chatter near the setpoint.

#define TEMP_MIN_FLOOR   5
#define TEMP_MIN_CEIL   50
#define TEMP_MAX_FLOOR  51
#define TEMP_MAX_CEIL   70

/// Minimum gap (°C) enforced between temp_min and temp_max.  Prevents
/// rapid ON/OFF relay cycling when auto-reheat is enabled and the two
/// limits are set almost equal (e.g. min=50, max=51).
#define TEMP_MIN_DEADBAND  5

// ── Max continuous run bounds ────────────────────────────────────
//
// An energy / "left on too long" convenience, not a safety cutoff:
// this controller switches the geyser at the mains and the geyser's
// own mechanical thermostat still regulates temperature.

/// Upper clamp for max_on_minutes (24 h) — mirrors the RTDB rules'
/// 0..1440 range.  0 = disabled remains a legitimate explicit choice
/// and is never overridden by firmware.
#define MAX_ON_CEIL  1440

/// Clamp a (min, max) pair to the ranges above and enforce
/// TEMP_MIN_DEADBAND between them.  Used by every write path so the
/// live state and the persisted NVS values can never diverge.
void device_state_clamp_limits(uint8_t *min, uint8_t *max);

// ── Thread-safe accessors ────────────────────────────────────────

float   device_state_get_temperature(void);
void    device_state_set_temperature(float temp);

bool    device_state_get_relay(void);

/// Sets the relay state AND drives the GPIO atomically (under the
/// state mutex) — call sites must not call relay_set() themselves.
void    device_state_set_relay(bool on);

/// Re-drives the GPIO from the current state under the mutex.  Called
/// periodically by the temperature task to self-heal any desync.
void    device_state_reassert_relay(void);

void    device_state_get_temp_limits(uint8_t *out_min, uint8_t *out_max);
void    device_state_set_temp_limits(uint8_t min, uint8_t max);

bool    device_state_get_auto_reheat(void);
void    device_state_set_auto_reheat(bool enabled);

bool    device_state_get_sensor_ok(void);
void    device_state_set_sensor_ok(bool ok);

uint16_t device_state_get_max_on_minutes(void);
void     device_state_set_max_on_minutes(uint16_t minutes);

// ── Run-window tracking ──────────────────────────────────────────
//
// The elapsed time of the current ON stretch drives the max continuous
// run cutoff and the "time remaining" the app shows.  It is stamped as
// a wall-clock epoch (one small NVS write per ON transition, no wear
// concern) so it SURVIVES A REBOOT — otherwise a unit that restarts
// mid-block silently restarts the window and runs longer than asked.
// Falls back to a RAM tick counter whenever the clock is unusable.

/// Epoch seconds at which the current ON stretch began, or 0 when the
/// relay is off or the clock was invalid at switch-on.
uint32_t device_state_get_relay_on_since(void);
void     device_state_set_relay_on_since(uint32_t epoch);

bool     device_state_get_fallback_enabled(void);
void     device_state_set_fallback_enabled(bool enabled);

/// Whether interval (clock-less) mode is currently driving the relay.
bool     device_state_get_fallback_active(void);
void     device_state_set_fallback_active(bool active);

/// Copy all MAX_TIMERS timers into *out*.
void    device_state_get_timers(gs_timer_t *out);
void    device_state_set_timers(const gs_timer_t *timers);

/// Direct pointer access — caller MUST hold the lock.
void            device_state_lock(void);
void            device_state_unlock(void);
device_state_t *device_state_get_ptr(void);
