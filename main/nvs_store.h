#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "device_state.h"

/// Load all persisted fields into *state*.
/// Safe to call on first boot — missing keys are silently skipped,
/// leaving the struct defaults intact.
void nvs_store_load(device_state_t *state);

/// Persist individual fields.  Each writes only the changed key
/// to minimise flash wear.
/// Persists the relay state AND the current run-window start stamp
/// (device_state_get_relay_on_since()), which belong together.
void nvs_store_save_relay(bool on);

/// Persists the interval-fallback opt-in.
void nvs_store_save_fallback_enabled(bool enabled);
void nvs_store_save_temp_limits(uint8_t min, uint8_t max);
void nvs_store_save_auto_reheat(bool enabled);

/// Save all MAX_TIMERS timers.
void nvs_store_save_timers(const gs_timer_t *timers);

void nvs_store_save_max_on_minutes(uint16_t minutes);
