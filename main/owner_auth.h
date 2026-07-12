#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ── BLE owner-lock ───────────────────────────────────────────────
//
// Proves that a connected phone belongs to the owning household
// account before it may write control or provisioning characteristics.
//
// At provisioning the app generates a random 32-byte key, writes it
// to the device (0x16, encrypted link) and stores it in the account's
// cloud scope. Any phone signed into the same account fetches the key
// and unlocks via challenge-response on characteristic 0x0E:
//
//   read  0x0E → 16-byte random nonce (fresh per read)
//   write 0x0E → HMAC-SHA256(key, nonce); match unlocks THIS
//                connection until disconnect
//
// The key itself never crosses BLE again after provisioning.
//
// Threading: all calls run on the NimBLE host task (GATT callbacks)
// except owner_auth_init(), which runs in app_main before BLE starts.
// No locking is needed.

#define OWNER_KEY_LEN    32
#define OWNER_NONCE_LEN  16
#define OWNER_HMAC_LEN   32

/// Load the owner key from NVS (if provisioned earlier). Call once
/// from app_main after NVS init, before ble_init().
void owner_auth_init(void);

/// True if an owner key is stored.
bool owner_auth_has_key(void);

/// Store a new owner key (exactly OWNER_KEY_LEN bytes) to NVS + RAM.
/// The writing connection becomes owner-unlocked (whoever sets the
/// key has just proven they hold it). Returns false on NVS failure.
bool owner_auth_set_key(const uint8_t *key, size_t len);

/// Generate + return a fresh challenge nonce for this connection.
void owner_auth_get_nonce(uint8_t out[OWNER_NONCE_LEN]);

/// Verify HMAC-SHA256(key, nonce). Consumes the nonce either way.
/// On success the current connection is marked owner-unlocked.
bool owner_auth_try_unlock(const uint8_t *resp, size_t len);

/// True if the current connection has passed the owner unlock.
bool owner_auth_is_unlocked(void);

/// Clear unlock state + pending nonce. Call on connect AND disconnect.
void owner_auth_reset_session(void);

/// Unified write-gate: returns true when the write should be allowed —
/// device unprovisioned (setup mode), no key stored (legacy image),
/// or this connection is owner-unlocked.
bool owner_auth_gate_ok(void);
