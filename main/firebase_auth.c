#include "firebase_auth.h"
#include "firebase_config.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "fb_auth";

static SemaphoreHandle_t s_mutex;

#define NVS_NS_PROV     "gs_prov"
#define KEY_REFRESH_TK  "refresh_tk"
#define KEY_DEVICE_ID   "device_id"
#define KEY_USER_ID     "user_id"

#define MAX_REFRESH_LEN 512
#define MAX_ID_TOKEN    2048
#define MAX_DEVICE_ID   16
#define MAX_USER_ID     64
#define RESP_BUF_SIZE   4096

static char s_refresh[MAX_REFRESH_LEN];
static char s_id_token[MAX_ID_TOKEN];
static char s_device_id[MAX_DEVICE_ID];
static char s_user_id[MAX_USER_ID];
static time_t s_expiry;
static bool s_ready;

// ── HTTP response accumulator ────────────────────────────────────

typedef struct {
    char *buf;
    int   len;
    int   cap;
} resp_t;

static esp_err_t on_http_data(esp_http_client_event_t *evt)
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

// ── Token refresh ────────────────────────────────────────────────

/// Failure backoff: a revoked refresh token (user changed password /
/// deleted account) previously caused a blocking 10 s HTTPS POST every
/// few seconds, forever.  Failures now back off 30 s → 1 h; HTTP 400
/// (invalid_grant = token revoked, permanent) goes straight to the
/// hourly ceiling.
static int    s_refresh_fails = 0;
static time_t s_retry_after   = 0;

static void note_refresh_failure(bool permanent)
{
    time_t now;
    time(&now);

    if (permanent) {
        s_retry_after = now + 3600;
        ESP_LOGW(TAG, "Refresh token rejected (revoked?) — dormant 1 h");
        return;
    }
    if (s_refresh_fails < 7) s_refresh_fails++;
    int backoff = 30 << (s_refresh_fails - 1);   // 30s → 32min
    s_retry_after = now + backoff;
    ESP_LOGW(TAG, "Refresh failed (%d) — next attempt in %ds",
             s_refresh_fails, backoff);
}

static bool do_refresh(void)
{
    if (s_refresh[0] == '\0') return false;

    char post[MAX_REFRESH_LEN + 64];
    snprintf(post, sizeof(post),
             "grant_type=refresh_token&refresh_token=%s", s_refresh);

    char body[RESP_BUF_SIZE];
    resp_t resp = { .buf = body, .len = 0, .cap = sizeof(body) };
    memset(body, 0, sizeof(body));

    esp_http_client_config_t cfg = {
        .url            = FIREBASE_TOKEN_URL,
        .method         = HTTP_METHOD_POST,
        .event_handler  = on_http_data,
        .user_data      = &resp,
        .timeout_ms     = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type",
                               "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(c, post, strlen(post));

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Refresh failed: err=%d status=%d", err, status);
        note_refresh_failure(status == 400);
        return false;
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) { ESP_LOGE(TAG, "JSON parse failed"); return false; }

    cJSON *jtok = cJSON_GetObjectItem(json, "id_token");
    cJSON *jref = cJSON_GetObjectItem(json, "refresh_token");
    cJSON *jexp = cJSON_GetObjectItem(json, "expires_in");

    if (!cJSON_IsString(jtok) || !cJSON_IsString(jref)) {
        ESP_LOGE(TAG, "Missing fields in token response");
        cJSON_Delete(json);
        return false;
    }

    strncpy(s_id_token, jtok->valuestring, sizeof(s_id_token) - 1);

    if (strcmp(s_refresh, jref->valuestring) != 0) {
        strncpy(s_refresh, jref->valuestring, sizeof(s_refresh) - 1);
        nvs_handle_t h;
        if (nvs_open(NVS_NS_PROV, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, KEY_REFRESH_TK, s_refresh);
            nvs_commit(h);
            nvs_close(h);
        }
    }

    int secs = cJSON_IsString(jexp) ? atoi(jexp->valuestring) : 3600;
    time_t now;
    time(&now);
    s_expiry = now + secs - 300;            // refresh 5 min early

    cJSON_Delete(json);
    s_refresh_fails = 0;
    s_retry_after   = 0;
    ESP_LOGI(TAG, "ID token refreshed (expires in %ds)", secs);
    return true;
}

// ── Public API ───────────────────────────────────────────────────

bool firebase_auth_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        assert(s_mutex);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    memset(s_refresh,   0, sizeof(s_refresh));
    memset(s_id_token,  0, sizeof(s_id_token));
    memset(s_device_id, 0, sizeof(s_device_id));
    memset(s_user_id,   0, sizeof(s_user_id));
    s_expiry = 0;
    s_ready  = false;

    nvs_handle_t h;
    if (nvs_open(NVS_NS_PROV, NVS_READONLY, &h) != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return false;
    }

    size_t len;

    len = sizeof(s_refresh);
    if (nvs_get_str(h, KEY_REFRESH_TK, s_refresh, &len) != ESP_OK) {
        nvs_close(h);
        xSemaphoreGive(s_mutex);
        return false;
    }

    len = sizeof(s_device_id);
    nvs_get_str(h, KEY_DEVICE_ID, s_device_id, &len);

    len = sizeof(s_user_id);
    nvs_get_str(h, KEY_USER_ID, s_user_id, &len);

    nvs_close(h);

    s_ready = (s_refresh[0] != '\0' && s_user_id[0] != '\0');
    ESP_LOGI(TAG, "Init: ready=%d  device=\"%s\"  user=\"%.8s...\"",
             s_ready, s_device_id, s_user_id);

    xSemaphoreGive(s_mutex);
    return s_ready;
}

void firebase_auth_set_credentials(const char *refresh_token,
                                   const char *device_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    strncpy(s_refresh,   refresh_token, sizeof(s_refresh)   - 1);
    strncpy(s_device_id, device_id,     sizeof(s_device_id) - 1);

    nvs_handle_t h;
    if (nvs_open(NVS_NS_PROV, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, KEY_REFRESH_TK, s_refresh);
        nvs_set_str(h, KEY_DEVICE_ID,  s_device_id);

        if (s_user_id[0] == '\0') {
            size_t len = sizeof(s_user_id);
            nvs_get_str(h, KEY_USER_ID, s_user_id, &len);
        }

        nvs_commit(h);
        nvs_close(h);
    }

    s_ready = (s_refresh[0] != '\0' && s_user_id[0] != '\0');
    ESP_LOGI(TAG, "Credentials stored  device=\"%s\"  ready=%d",
             s_device_id, s_ready);

    xSemaphoreGive(s_mutex);
}

void firebase_auth_set_device_id(const char *device_id)
{
    if (!device_id || device_id[0] == '\0') return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    strncpy(s_device_id, device_id, sizeof(s_device_id) - 1);
    s_device_id[sizeof(s_device_id) - 1] = '\0';

    nvs_handle_t h;
    if (nvs_open(NVS_NS_PROV, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, KEY_DEVICE_ID, s_device_id);
        nvs_commit(h);
        nvs_close(h);
    }

    ESP_LOGI(TAG, "Device ID stored: \"%s\"", s_device_id);

    xSemaphoreGive(s_mutex);
}

const char *firebase_auth_get_id_token(void)
{
    if (!s_ready) return NULL;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    time_t now;
    time(&now);

    if (s_id_token[0] == '\0' || now >= s_expiry) {
        // Honour the failure backoff — don't hammer securetoken with a
        // blocking POST on every caller retry.
        if (now < s_retry_after) {
            xSemaphoreGive(s_mutex);
            return NULL;
        }
        if (!do_refresh()) {
            xSemaphoreGive(s_mutex);
            return NULL;
        }
    }

    xSemaphoreGive(s_mutex);
    return s_id_token;
}

void firebase_auth_invalidate_token(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_id_token[0] = '\0';
    s_expiry      = 0;
    s_retry_after = 0;   // server said 401 — retry the refresh now
    xSemaphoreGive(s_mutex);
    ESP_LOGW(TAG, "ID token invalidated — will refresh on next use");
}

const char *firebase_auth_get_device_id(void)
{
    return s_device_id;
}

const char *firebase_auth_get_user_id(void)
{
    return s_user_id;
}

bool firebase_auth_is_ready(void)
{
    return s_ready;
}
