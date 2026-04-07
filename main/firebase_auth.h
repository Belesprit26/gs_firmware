#pragma once

#include <stdbool.h>

/// Initialise Firebase auth.  Loads the refresh token and device ID
/// from NVS.  Returns true if credentials are available and the
/// module is ready to issue ID tokens.
bool firebase_auth_init(void);

/// Store new credentials received via BLE provisioning.
/// Saves the refresh token and device ID to NVS immediately.
void firebase_auth_set_credentials(const char *refresh_token,
                                   const char *device_id);

/// Return a valid Firebase ID token for RTDB REST API calls.
/// Transparently refreshes the token if it has expired.
/// Returns NULL if no credentials are stored or refresh fails.
/// The returned pointer is valid until the next call.
const char *firebase_auth_get_id_token(void);

/// Device ID used in RTDB paths (e.g. "a3f9b21c").
/// Derived from the BLE identifier by the app (SHA-256, 8-char hex)
/// and written during provisioning.  Also exposed via GATT 0x0C.
const char *firebase_auth_get_device_id(void);

/// Firebase UID of the owning user (loaded from gs_prov NVS).
const char *firebase_auth_get_user_id(void);

/// True once credentials are loaded and a user ID is available.
bool firebase_auth_is_ready(void);
