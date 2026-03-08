#pragma once

#include <stdbool.h>
#include <stdint.h>

/// Create the internal command queue.  Call once from app_main()
/// before any task that might call the request functions.
void firebase_rtdb_init(void);

// ── Direct push (firebase_task context only) ─────────────────────

void firebase_rtdb_push_live(float temp, bool relay_on);
void firebase_rtdb_push_stats(int runtime_seconds);
void firebase_rtdb_push_boot(void);
void firebase_rtdb_push_settings(void);
void firebase_rtdb_push_event(uint8_t type, uint8_t temp);
void firebase_rtdb_sync_relay(bool on);

// ── Async requests (safe from ANY task or BLE callback) ──────────
// These just set flags; the firebase_task picks them up within
// ~500 ms and executes the HTTP call on its own 8 KB stack.

void firebase_rtdb_request_live_push(void);
void firebase_rtdb_request_settings_push(void);
void firebase_rtdb_request_event_push(uint8_t type, uint8_t temp);

/// Main Firebase task — push telemetry and listen for remote
/// settings changes via SSE.  Pass NULL as param.
/// Create with xTaskCreate; never returns.
void firebase_task(void *param);
