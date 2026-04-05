#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/// Maximum user-chosen suffix length (e.g. "LivingRoom" in "GeyserSwitch-LivingRoom").
#define PROV_NICKNAME_MAX 16

/// Event bits for WiFi connection state.
#define WIFI_EVT_CONNECTED BIT0

/// Provisioning status codes — reported via BLE notification.
typedef enum {
    PROV_IDLE           = 0,  // Waiting for credentials
    PROV_CONNECTING     = 1,  // Connecting to WiFi
    PROV_WIFI_OK        = 2,  // WiFi connected, IP obtained
    PROV_WIFI_FAIL      = 3,  // WiFi connection failed
    PROV_COMPLETE       = 4,  // Fully provisioned
    PROV_ERROR          = 5,  // Unexpected error
    PROV_BLE_ONLY_OK    = 6,  // BLE-only provisioning complete (no WiFi)
} prov_status_t;

/// Returns true if the device has been provisioned (user bound in NVS).
bool wifi_prov_is_provisioned(void);

/// Returns true if WiFi credentials have been stored.
bool wifi_prov_has_wifi(void);

/// Returns true if WiFi STA is currently connected (has IP).
bool wifi_prov_is_connected(void);

/// Event group for WiFi state — wait on WIFI_EVT_CONNECTED to block
/// until WiFi is available.
EventGroupHandle_t wifi_prov_event_group(void);

/// Register the provisioning GATT service.
/// Call alongside gatt_server_init(), before nimble_port_freertos_init().
void wifi_prov_gatt_init(void);

/// If WiFi credentials are stored, connect to the WiFi network.
/// Call from app_main after NVS is loaded.  Non-blocking — starts
/// the WiFi station in the background and returns immediately.
void wifi_prov_start_wifi(void);

/// Get the device advertising name.
/// Returns "GeyserSwitch-{nickname}" if nickname is set,
/// "GeyserSwitch-Setup" if not provisioned,
/// "GeyserSwitch" as default.
const char *wifi_prov_get_device_name(void);

/// Load the device nickname from NVS into memory.
/// Call once from app_main after NVS is ready.
void wifi_prov_load_nickname(void);

/// Erase provisioning data only (WiFi, auth, nickname).
/// Device config (relay, limits, timers) is preserved.
/// Not currently called — reserved for future re-provisioning flow
/// (e.g. GATT-triggered re-pair without full factory reset).
void wifi_prov_reset(void);
