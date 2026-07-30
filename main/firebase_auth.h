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

/// Store ONLY the RTDB device ID (persisted to NVS). Used by BLE-only
/// provisioning, which carries no refresh token — without this the
/// firmware never learns its RTDB identity and GATT 0x0C stays empty,
/// so a second phone (iOS especially: CoreBluetooth identifiers are
/// per-phone) derives a different device ID and finds no owner key.
void firebase_auth_set_device_id(const char *device_id);

/// Return a valid Firebase ID token for RTDB REST API calls.
/// Transparently refreshes the token if it has expired.
/// Returns NULL if no credentials are stored, refresh fails, or the
/// failure backoff (30 s → 1 h) is active.
/// The returned pointer is valid until the next call.
const char *firebase_auth_get_id_token(void);

/// Drop the cached ID token so the next get_id_token() refreshes.
/// Call when the server rejects the token (HTTP 401 / auth_revoked)
/// before local expiry — e.g. server-side revocation.
void firebase_auth_invalidate_token(void);

/// Device ID used in RTDB paths (e.g. "a3f9b21c").
/// Derived from the BLE identifier by the app (SHA-256, 8-char hex)
/// and written during provisioning.  Also exposed via GATT 0x0C.
const char *firebase_auth_get_device_id(void);

/// Firebase UID of the owning user (loaded from gs_prov NVS).
const char *firebase_auth_get_user_id(void);

/// True once credentials are loaded and a user ID is available.
bool firebase_auth_is_ready(void);
