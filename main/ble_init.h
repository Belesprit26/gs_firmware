#pragma once

#include <stdbool.h>
#include <stdint.h>

/// Initialise the NimBLE stack, register GATT services, and start the
/// host task.  Call once from app_main after NVS and device_state are ready.
void ble_init(void);

/// Start (or restart) BLE advertising.
/// Called automatically on boot and after client disconnect.
void ble_start_advertising(void);

/// Returns the current connection handle, or BLE_HS_CONN_HANDLE_NONE
/// if no client is connected.
uint16_t ble_get_conn_handle(void);

/// True if a BLE client is currently connected.
bool ble_is_connected(void);
