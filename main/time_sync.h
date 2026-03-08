#pragma once

#include <stdbool.h>
#include <stdint.h>

/// Initialise the time synchronisation module.
/// Call once from app_main before any other time_sync function.
void time_sync_init(void);

/// Start SNTP time synchronisation.
/// Call when WiFi connects and has Internet access.
/// Safe to call multiple times — only starts once.
void time_sync_start_sntp(void);

/// Stop SNTP synchronisation.
/// Call when WiFi disconnects.  The RTC continues to run.
void time_sync_stop_sntp(void);

/// Set the system clock from a BLE phone timestamp.
/// @param unix_time  Seconds since Unix epoch (UTC).
///
/// Decision logic (called once per BLE connection cycle):
///   - If NTP synced within the last 24 h → ignored (RTC is reliable).
///   - If NTP never synced, or last sync > 24 h ago → accepted.
/// This ensures that WiFi/NTP time is preferred when available, but
/// the board can still keep time in BLE-only deployments.
void time_sync_set_from_ble(uint32_t unix_time);

/// Returns true if the system has a valid time source
/// (NTP or BLE sync).  Timers/schedulers should check this before
/// acting on the current wall-clock time.
bool time_sync_is_valid(void);
