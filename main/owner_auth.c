#include "owner_auth.h"

#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "mbedtls/md.h"

#include "wifi_prov.h"

static const char *TAG = "owner";

// Same NVS namespace as the rest of provisioning, so a factory reset
// or wifi_prov_reset() (nvs_erase_all on "gs_prov") also clears the
// owner key — exactly the semantics we want for ownership transfer.
#define NVS_NS_PROV  "gs_prov"
#define KEY_OWNER_SK "owner_sk"

static uint8_t s_key[OWNER_KEY_LEN];
static bool    s_has_key;

static uint8_t s_nonce[OWNER_NONCE_LEN];
static bool    s_nonce_valid;
static bool    s_unlocked;

// Cross-implementation check: the app's Dart HMAC must match mbedtls.
// Shared test vector (RFC 4231 case 2):
//   key   = "Jefe"
//   data  = "what do ya want for nothing?"
//   HMAC-SHA256 = 5bdcc146bf60754e6a042426089575c7
//                 5a003f089d2739839dec58b964ec3843
// The same vector is asserted in the app repo
// (test/core/ble/owner_auth_codec_test.dart).

// ── Init / key management ────────────────────────────────────────

void owner_auth_init(void)
{
    s_has_key     = false;
    s_nonce_valid = false;
    s_unlocked    = false;

    nvs_handle_t h;
    if (nvs_open(NVS_NS_PROV, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No provisioning namespace — no owner key");
        return;
    }

    size_t len = sizeof(s_key);
    esp_err_t err = nvs_get_blob(h, KEY_OWNER_SK, s_key, &len);
    nvs_close(h);

    if (err == ESP_OK && len == OWNER_KEY_LEN) {
        s_has_key = true;
        ESP_LOGI(TAG, "Owner key loaded — BLE writes require unlock");
    } else {
        ESP_LOGI(TAG, "No owner key stored — BLE writes open");
    }
}

bool owner_auth_has_key(void)
{
    return s_has_key;
}

bool owner_auth_set_key(const uint8_t *key, size_t len)
{
    if (len != OWNER_KEY_LEN) return false;

    nvs_handle_t h;
    if (nvs_open(NVS_NS_PROV, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed — key not stored");
        return false;
    }
    esp_err_t err = nvs_set_blob(h, KEY_OWNER_SK, key, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(err));
        return false;
    }

    memcpy(s_key, key, OWNER_KEY_LEN);
    s_has_key = true;

    // Whoever sets the key evidently holds it — unlock this session so
    // the just-provisioned phone can operate without a redundant
    // challenge round-trip.
    s_unlocked = true;

    ESP_LOGI(TAG, "Owner key stored — session unlocked");
    return true;
}

// ── Challenge-response ───────────────────────────────────────────

void owner_auth_get_nonce(uint8_t out[OWNER_NONCE_LEN])
{
    esp_fill_random(s_nonce, OWNER_NONCE_LEN);
    s_nonce_valid = true;
    memcpy(out, s_nonce, OWNER_NONCE_LEN);
}

bool owner_auth_try_unlock(const uint8_t *resp, size_t len)
{
    // Consume the nonce regardless of outcome: every attempt needs a
    // fresh challenge, which also rules out replay.
    bool nonce_ok = s_nonce_valid;
    s_nonce_valid = false;

    if (!s_has_key || !nonce_ok || len != OWNER_HMAC_LEN) {
        ESP_LOGW(TAG, "Unlock rejected (key=%d nonce=%d len=%u)",
                 s_has_key, nonce_ok, (unsigned)len);
        return false;
    }

    uint8_t expected[OWNER_HMAC_LEN];
    const mbedtls_md_info_t *md =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_hmac(md, s_key, OWNER_KEY_LEN,
                        s_nonce, OWNER_NONCE_LEN, expected) != 0) {
        ESP_LOGE(TAG, "HMAC computation failed");
        return false;
    }

    // Constant-time compare.
    uint8_t diff = 0;
    for (int i = 0; i < OWNER_HMAC_LEN; i++) {
        diff |= expected[i] ^ resp[i];
    }

    if (diff != 0) {
        ESP_LOGW(TAG, "Unlock failed — wrong key");
        return false;
    }

    s_unlocked = true;
    ESP_LOGI(TAG, "Owner unlock OK");
    return true;
}

// ── Session / gate ───────────────────────────────────────────────

bool owner_auth_is_unlocked(void)
{
    return s_unlocked;
}

void owner_auth_reset_session(void)
{
    s_unlocked    = false;
    s_nonce_valid = false;
}

bool owner_auth_gate_ok(void)
{
    // Setup mode: an unprovisioned device has no owner to protect.
    if (!wifi_prov_is_provisioned()) return true;

    // Provisioned by a pre-owner-lock image: stay open (documented;
    // no such units shipped — this is a belt-and-braces fallback).
    if (!s_has_key) return true;

    return s_unlocked;
}
