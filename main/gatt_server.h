#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "host/ble_uuid.h"

/// The GeyserSwitch primary service UUID (47530001-7652-4543-b201-c4b801a6c700).
/// Exposed so ble_init.c can include it in advertisement data.
extern const ble_uuid128_t gs_svc_uuid;

/// Register the GATT service table with the NimBLE host.
/// Call after ble_svc_gap/gatt_init, before nimble_port_freertos_init.
void gatt_server_init(void);

/// Push a temperature notification to the connected client (if any).
void gatt_server_notify_temperature(float temp_c);

/// Push a geyser-state notification to the connected client (if any).
void gatt_server_notify_state(bool on);

/// Push a device-event notification to the connected client (if any).
/// Format: 2 bytes [event_type, temperature_int].
void gatt_server_notify_event(uint8_t type, uint8_t temp);
