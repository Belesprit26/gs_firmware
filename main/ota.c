#include "ota.h"
#include "firebase_config.h"
#include "firebase_auth.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_state.h"

static const char *TAG = "ota";

// ── Tunables ─────────────────────────────────────────────────────

/// Cancel the pending rollback after this long even without network,
/// so an offline / BLE-only device doesn't revert on the next reboot.
#define OTA_HEALTH_GRACE_MS   (3 * 60 * 1000)
/// First update check after boot, then every OTA_CHECK_INTERVAL_MS.
#define OTA_FIRST_CHECK_MS    (60 * 1000)
#define OTA_CHECK_INTERVAL_MS (6 * 60 * 60 * 1000)
#define OTA_HTTP_TIMEOUT_MS   15000
/// Force the post-update reboot after this long even if the relay is
/// still ON, so an update can never be deferred indefinitely.
#define REBOOT_DEFER_MAX_MS   (30 * 60 * 1000)

#define MANIFEST_MAX          1024
// The RTDB URL carries the full ID token (~1.2 KB) in the query string.
#define URL_MAX               2048
#define HTTP_TX_BUF           2560

// ── Manifest fetch ───────────────────────────────────────────────

typedef struct { char *buf; int len; int cap; } resp_t;

static esp_err_t manifest_on_data(esp_http_client_event_t *evt)
{
    resp_t *r = (resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && r) {
        int avail = r->cap - r->len - 1;
        int copy  = evt->data_len < avail ? evt->data_len : avail;
        if (copy > 0) {
            memcpy(r->buf + r->len, evt->data, copy);
            r->len += copy;
            r->buf[r->len] = '\0';
        }
    }
    return ESP_OK;
}

/// Fetch /firmware/latest from RTDB into *out* (NUL-terminated JSON).
/// Returns true on HTTP 200 with a non-empty body.
static bool fetch_manifest(char *out, int out_cap)
{
    const char *token = firebase_auth_get_id_token();
    if (!token) return false;

    char url[URL_MAX];
    snprintf(url, sizeof(url),
             FIREBASE_DB_URL "/firmware/latest.json?auth=%s", token);

    resp_t resp = { .buf = out, .len = 0, .cap = out_cap };
    out[0] = '\0';

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .event_handler     = manifest_on_data,
        .user_data         = &resp,
        .timeout_ms        = OTA_HTTP_TIMEOUT_MS,
        .buffer_size_tx    = HTTP_TX_BUF,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Manifest fetch failed: err=%d status=%d", err, status);
        return false;
    }
    return resp.len > 0;
}

// ── Semantic version comparison ──────────────────────────────────

static void parse_ver(const char *s, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    while (*s && (*s < '0' || *s > '9')) s++;   // skip a leading 'v' etc.
    sscanf(s, "%d.%d.%d", &out[0], &out[1], &out[2]);
}

/// True if `cand` is a strictly higher semantic version than `cur`.
static bool version_is_newer(const char *cand, const char *cur)
{
    int a[3], b[3];
    parse_ver(cand, a);
    parse_ver(cur, b);
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

// ── Relay-safe reboot ────────────────────────────────────────────

static void reboot_when_safe(void)
{
    ESP_LOGI(TAG, "Update staged — deferring reboot until relay is OFF");
    TickType_t start = xTaskGetTickCount();
    while (device_state_get_relay()) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(REBOOT_DEFER_MAX_MS)) {
            ESP_LOGW(TAG, "Relay still ON after defer cap — rebooting anyway");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGW(TAG, "Rebooting into updated firmware");
    esp_restart();
}

// ── Image hash verification ──────────────────────────────────────

/// Hash the written OTA image and compare it to the manifest's
/// lowercase-hex sha256. Runs AFTER esp_https_ota_finish(): ESP-IDF
/// writes the image header last (so an interrupted flash isn't
/// bootable), so the partition only equals the published .bin once
/// finish() has run. Integrity only — a forged manifest can set a
/// matching hash; see SECURE_BOOT.md B1 for signature authenticity.
static bool image_sha256_ok(const esp_partition_t *part, int image_len,
                            const char *expected_hex)
{
    if (!part || image_len <= 0) {
        ESP_LOGW(TAG, "sha256: no partition / unknown length — skipping check");
        return true;   // don't block a good update on a lookup quirk
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);   // 0 = SHA-256, not SHA-224

    static uint8_t buf[512];
    bool ok = true;
    for (int off = 0; off < image_len; ) {
        int chunk = image_len - off;
        if (chunk > (int)sizeof(buf)) chunk = sizeof(buf);
        if (esp_partition_read(part, off, buf, chunk) != ESP_OK) {
            ok = false;
            break;
        }
        mbedtls_sha256_update(&ctx, buf, chunk);
        off += chunk;
    }

    uint8_t digest[32];
    if (ok) mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    if (!ok) return false;

    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return strcasecmp(hex, expected_hex) == 0;
}

// ── Perform the update ───────────────────────────────────────────

static void do_update(const char *bin_url, const char *expected_sha256)
{
    ESP_LOGI(TAG, "Starting OTA download");

    esp_http_client_config_t http_cfg = {
        .url               = bin_url,
        .timeout_ms        = OTA_HTTP_TIMEOUT_MS,
        .buffer_size_tx    = HTTP_TX_BUF,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        return;
    }

    // Reject an image that isn't strictly newer than what we run — guards
    // against a stale or mis-set manifest re-flashing the same/older build.
    esp_app_desc_t new_desc;
    if (esp_https_ota_get_img_desc(handle, &new_desc) == ESP_OK) {
        const esp_app_desc_t *cur = esp_app_get_description();
        if (!version_is_newer(new_desc.version, cur->version)) {
            ESP_LOGW(TAG, "Image %s not newer than %s — aborting",
                     new_desc.version, cur->version);
            esp_https_ota_abort(handle);
            return;
        }
        ESP_LOGI(TAG, "Downloading %s → %s", cur->version, new_desc.version);
    }

    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        vTaskDelay(pdMS_TO_TICKS(1));   // yield to lower-priority tasks
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "OTA download incomplete: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        return;
    }

    // Capture the target slot + downloaded length while the handle is still
    // valid — the integrity check below needs them after finish() frees it.
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    int image_len = esp_https_ota_get_image_len_read(handle);

    err = esp_https_ota_finish(handle);   // validates image, writes header, sets boot
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED)
            ESP_LOGE(TAG, "OTA image failed validation — not applied");
        else
            ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        return;
    }

    // Integrity gate: hash the now-complete image against the manifest sha256.
    // finish() already flipped the boot pointer, so on a mismatch we point it
    // back at the running image — the bad slot is simply never booted (and is
    // overwritten by the next update). Catches a corrupted download or a
    // swapped storage object; not a signature — that is B1 in SECURE_BOOT.md.
    if (expected_sha256 && strlen(expected_sha256) == 64) {
        if (!image_sha256_ok(update_part, image_len, expected_sha256)) {
            ESP_LOGE(TAG, "OTA sha256 mismatch — reverting to current image");
            esp_ota_set_boot_partition(esp_ota_get_running_partition());
            return;
        }
        ESP_LOGI(TAG, "OTA sha256 verified");
    }

    ESP_LOGI(TAG, "OTA image written and validated");
    reboot_when_safe();   // does not return on success
}

// ── Update check ─────────────────────────────────────────────────

static void check_for_update(void)
{
    char body[MANIFEST_MAX];
    if (!fetch_manifest(body, sizeof(body))) return;

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        ESP_LOGW(TAG, "Manifest parse failed");
        return;
    }

    cJSON *jver = cJSON_GetObjectItem(json, "version");
    cJSON *jurl = cJSON_GetObjectItem(json, "url");
    cJSON *jsha = cJSON_GetObjectItem(json, "sha256");

    if (cJSON_IsString(jver) && cJSON_IsString(jurl)) {
        const char *cur = esp_app_get_description()->version;
        if (version_is_newer(jver->valuestring, cur)) {
            ESP_LOGI(TAG, "Update available: %s (running %s)",
                     jver->valuestring, cur);
            // jurl/jsha point into `json`; esp_http_client copies the URL and
            // do_update reboots on success, so keeping json alive is fine.
            const char *sha = cJSON_IsString(jsha) ? jsha->valuestring : NULL;
            do_update(jurl->valuestring, sha);
        } else {
            ESP_LOGI(TAG, "Firmware up to date (running %s, latest %s)",
                     cur, jver->valuestring);
        }
    }

    cJSON_Delete(json);
}

// ── Rollback confirmation ────────────────────────────────────────

static void confirm_running_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "Running image confirmed healthy — rollback cancelled");
        } else {
            ESP_LOGW(TAG, "Failed to mark app valid");
        }
    }
}

// ── Task ─────────────────────────────────────────────────────────

void ota_task(void *param)
{
    // 1. Rollback confirmation.  Prefer confirming once network + auth
    //    work (proves a genuinely good build); fall back to a grace
    //    period so an offline / BLE-only device still cancels the
    //    pending rollback instead of reverting on the next reboot.
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        if (firebase_auth_is_ready() && firebase_auth_get_id_token() != NULL) {
            confirm_running_image();
            break;
        }
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(OTA_HEALTH_GRACE_MS)) {
            ESP_LOGW(TAG, "No network within grace period — confirming boot anyway");
            confirm_running_image();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    // 2. First check shortly after boot, then on a long interval.
    vTaskDelay(pdMS_TO_TICKS(OTA_FIRST_CHECK_MS));
    for (;;) {
        if (firebase_auth_is_ready() && firebase_auth_get_id_token() != NULL) {
            check_for_update();
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_INTERVAL_MS));
    }
}
