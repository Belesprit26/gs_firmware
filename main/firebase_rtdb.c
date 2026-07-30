#include "firebase_rtdb.h"
#include "firebase_auth.h"
#include "firebase_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"

#include "device_state.h"
#include "relay.h"
#include "nvs_store.h"
#include "gatt_server.h"

static const char *TAG = "fb_rtdb";

#define PUSH_INTERVAL_MS  10000
#define SSE_TIMEOUT_MS    5000
#define SSE_DEAD_MS       90000
#define RECONNECT_WAIT_MS 3000
#define BODY_MAX          256
#define SSE_LINE_MAX      1024
#define RESP_BUF_SIZE     512

// Firebase ID tokens are ~1 200 bytes.  The full URL with ?auth=<JWT>
// can reach ~1 400 bytes.  The esp_http_client transmit buffer must
// hold the HTTP request line (which contains the full URL path+query)
// plus all headers.  2 560 bytes covers current and future token sizes.
#define URL_BUF           2048
#define HTTP_TX_BUF       2560

// ── Async request flags ──────────────────────────────────────────

static volatile bool s_pending_live     = false;
static volatile bool s_pending_settings = false;

/// Set by process_sse_event on auth_revoked — makes the SSE loop
/// reconnect with a freshly-refreshed token (same task, no race).
static bool s_auth_reconnect = false;

#define EVENT_QUEUE_DEPTH 8

typedef struct {
    uint8_t type;
    uint8_t temp;
} event_item_t;

static QueueHandle_t s_event_queue;

void firebase_rtdb_init(void)
{
    s_pending_live     = false;
    s_pending_settings = false;
    if (!s_event_queue) {
        s_event_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(event_item_t));
    }
}

void firebase_rtdb_request_live_push(void)
{
    s_pending_live = true;
}

void firebase_rtdb_request_settings_push(void)
{
    s_pending_settings = true;
}

void firebase_rtdb_request_event_push(uint8_t type, uint8_t temp)
{
    event_item_t item = { .type = type, .temp = temp };
    if (xQueueSend(s_event_queue, &item, 0) != pdTRUE) {
        ESP_LOGW("fb_rtdb", "Event queue full — dropped type=%u temp=%u",
                 type, temp);
    }
}

// ── Helpers ──────────────────────────────────────────────────────
// Single static URL buffer — safe because every HTTP call runs
// sequentially on the firebase_task (FIREBASE_STACK, 12 KB).

static char s_url[URL_BUF];

static void build_url(const char *path, const char *token)
{
    snprintf(s_url, sizeof(s_url),
             FIREBASE_DB_URL "/gs/%s/%s.json?auth=%s",
             firebase_auth_get_user_id(), path, token);
}

// ── HTTP response accumulator ────────────────────────────────────

typedef struct { char *buf; int len; int cap; } resp_t;

static esp_err_t on_data(esp_http_client_event_t *evt)
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

static bool http_patch(const char *url, const char *json_body)
{
    char rbuf[RESP_BUF_SIZE];
    resp_t resp = { .buf = rbuf, .len = 0, .cap = sizeof(rbuf) };
    memset(rbuf, 0, sizeof(rbuf));

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_PATCH,
        .event_handler     = on_data,
        .user_data         = &resp,
        .timeout_ms        = 8000,
        .buffer_size_tx    = HTTP_TX_BUF,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, json_body, strlen(json_body));

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK || (status != 200 && status != 204)) {
        ESP_LOGE(TAG, "PATCH → err=%d status=%d", err, status);
        return false;
    }
    return true;
}

static bool http_put(const char *url, const char *json_body)
{
    char rbuf[RESP_BUF_SIZE];
    resp_t resp = { .buf = rbuf, .len = 0, .cap = sizeof(rbuf) };
    memset(rbuf, 0, sizeof(rbuf));

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_PUT,
        .event_handler     = on_data,
        .user_data         = &resp,
        .timeout_ms        = 8000,
        .buffer_size_tx    = HTTP_TX_BUF,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, json_body, strlen(json_body));

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK || (status != 200 && status != 204)) {
        ESP_LOGE(TAG, "PUT → err=%d status=%d", err, status);
        return false;
    }
    return true;
}

// ── Push implementations ─────────────────────────────────────────

void firebase_rtdb_push_live(float temp, bool relay_on)
{
    const char *token = firebase_auth_get_id_token();
    if (!token) return;

    char path[64];
    snprintf(path, sizeof(path), "live/%s", firebase_auth_get_device_id());
    build_url(path, token);

    char body[BODY_MAX];
    snprintf(body, sizeof(body),
             "{\"t\":%.2f,\"on\":%s,\"at\":{\".sv\":\"timestamp\"}}",
             temp, relay_on ? "true" : "false");

    http_patch(s_url, body);
}

void firebase_rtdb_push_stats(int runtime_seconds)
{
    const char *token = firebase_auth_get_id_token();
    if (!token) return;

    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);

    char path[64];
    snprintf(path, sizeof(path), "stats/%s/%04d-%02d-%02d",
             firebase_auth_get_device_id(),
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    build_url(path, token);

    char body[BODY_MAX];
    snprintf(body, sizeof(body),
             "{\"rt\":{\".sv\":{\"increment\":%d}},"
              "\"cy\":{\".sv\":{\"increment\":1}}}",
             runtime_seconds);

    http_patch(s_url, body);
}

void firebase_rtdb_push_boot(void)
{
    const char *token = firebase_auth_get_id_token();
    if (!token) return;

    time_t now;
    time(&now);

    char path[64];
    snprintf(path, sizeof(path), "meta/%s/boot",
             firebase_auth_get_device_id());
    build_url(path, token);

    char body[32];
    snprintf(body, sizeof(body), "%ld", (long)now);

    if (http_put(s_url, body))
        ESP_LOGI(TAG, "Boot timestamp pushed");
    else
        ESP_LOGW(TAG, "Boot timestamp push failed");

    // Field observability: reset reason + firmware version.  Without
    // this, a crash-looping unit is invisible unless a user opens the
    // app in BLE range.  Separate best-effort write on a NEW subpath so
    // the boot push above keeps working even if the rules for /health
    // aren't deployed yet (write is then just denied and logged).
    snprintf(path, sizeof(path), "meta/%s/health",
             firebase_auth_get_device_id());
    build_url(path, token);

    char health[96];
    snprintf(health, sizeof(health),
             "{\"rr\":%d,\"fw\":\"%s\",\"at\":%ld}",
             (int)esp_reset_reason(),
             esp_app_get_description()->version,
             (long)now);

    if (http_put(s_url, health))
        ESP_LOGI(TAG, "Boot health pushed (rr=%d)", (int)esp_reset_reason());
    else
        ESP_LOGW(TAG, "Boot health push failed (rules not deployed yet?)");
}

void firebase_rtdb_push_settings(void)
{
    const char *token = firebase_auth_get_id_token();
    if (!token) return;

    char path[64];
    snprintf(path, sizeof(path), "set/%s", firebase_auth_get_device_id());
    build_url(path, token);

    uint8_t mn, mx;
    device_state_get_temp_limits(&mn, &mx);
    bool ar = device_state_get_auto_reheat();
    bool on = device_state_get_relay();

    gs_timer_t timers[MAX_TIMERS];
    device_state_get_timers(timers);
    int tmask = 0;
    for (int i = 0; i < PRESET_TIMER_COUNT; i++)
        if (timers[i].enabled) tmask |= (1 << i);
    int tcust = timers[PRESET_TIMER_COUNT].enabled
                    ? timers[PRESET_TIMER_COUNT].hour * 60 +
                      timers[PRESET_TIMER_COUNT].minute
                    : 0;

    uint16_t maxon = device_state_get_max_on_minutes();

    char body[BODY_MAX];
    snprintf(body, sizeof(body),
             "{\"on\":%s,\"max\":%u,\"min\":%u,\"ar\":%s,"
             "\"tmask\":%d,\"tcust\":%d,\"maxon\":%u}",
             on ? "true" : "false", mx, mn,
             ar ? "true" : "false", tmask, tcust, maxon);

    if (http_patch(s_url, body))
        ESP_LOGI(TAG, "Settings pushed to RTDB");
    else
        ESP_LOGW(TAG, "Settings push failed");
}

void firebase_rtdb_push_event(uint8_t type, uint8_t temp)
{
    const char *token = firebase_auth_get_id_token();
    if (!token) return;

    char path[64];
    snprintf(path, sizeof(path), "events/%s", firebase_auth_get_device_id());
    build_url(path, token);

    char body[BODY_MAX];
    snprintf(body, sizeof(body),
             "{\"type\":%u,\"temp\":%u,\"at\":{\".sv\":\"timestamp\"}}",
             type, temp);

    if (http_patch(s_url, body))
        ESP_LOGI(TAG, "Event pushed → type=%u temp=%u", type, temp);
    else
        ESP_LOGW(TAG, "Event push failed");
}

// ── SSE (Server-Sent Events) listener ────────────────────────────

static char  sse_evt_type[32];
static char  sse_evt_data[SSE_LINE_MAX];
static int   sse_data_pos;
static char  sse_line[SSE_LINE_MAX];
static int   sse_line_pos;

static void apply_timer_mask(int mask, int custom_minutes)
{
    gs_timer_t timers[MAX_TIMERS];
    device_state_get_timers(timers);

    for (int i = 0; i < PRESET_TIMER_COUNT; i++)
        timers[i].enabled = (mask & (1 << i)) != 0;

    if (custom_minutes > 0) {
        timers[PRESET_TIMER_COUNT].enabled = true;
        timers[PRESET_TIMER_COUNT].hour    = custom_minutes / 60;
        timers[PRESET_TIMER_COUNT].minute  = custom_minutes % 60;
    } else {
        timers[PRESET_TIMER_COUNT].enabled = false;
    }

    device_state_set_timers(timers);
    nvs_store_save_timers(timers);
}

static void apply_full_settings(cJSON *data, bool is_put)
{
    if (!cJSON_IsObject(data)) return;

    cJSON *j;
    bool relay_changed = false;

    if (is_put) {
        j = cJSON_GetObjectItem(data, "on");
        if (cJSON_IsBool(j)) {
            bool want = cJSON_IsTrue(j);
            if (want != device_state_get_relay()) {
                ESP_LOGI(TAG, "Remote toggle → %s", want ? "ON" : "OFF");
                device_state_set_relay(want);
                nvs_store_save_relay(want);
                gatt_server_notify_state(want);
                relay_changed = true;
            }
        }
    }

    cJSON *jmax = cJSON_GetObjectItem(data, "max");
    cJSON *jmin = cJSON_GetObjectItem(data, "min");
    if (cJSON_IsNumber(jmax) || cJSON_IsNumber(jmin)) {
        uint8_t mn, mx;
        device_state_get_temp_limits(&mn, &mx);
        if (cJSON_IsNumber(jmin)) mn = (uint8_t)jmin->valueint;
        if (cJSON_IsNumber(jmax)) mx = (uint8_t)jmax->valueint;
        ESP_LOGI(TAG, "Remote temp limits → min=%u max=%u", mn, mx);
        device_state_set_temp_limits(mn, mx);
        nvs_store_save_temp_limits(mn, mx);
    }

    j = cJSON_GetObjectItem(data, "ar");
    if (cJSON_IsBool(j)) {
        bool ar = cJSON_IsTrue(j);
        ESP_LOGI(TAG, "Remote auto_reheat → %s", ar ? "ON" : "OFF");
        device_state_set_auto_reheat(ar);
        nvs_store_save_auto_reheat(ar);
    }

    int mask = -1, cust = 0;
    j = cJSON_GetObjectItem(data, "tmask");
    if (cJSON_IsNumber(j)) mask = j->valueint;
    j = cJSON_GetObjectItem(data, "tcust");
    if (cJSON_IsNumber(j)) cust = j->valueint;
    if (mask >= 0) {
        ESP_LOGI(TAG, "Remote timers → mask=0x%X cust=%d", mask, cust);
        apply_timer_mask(mask, cust);
    }

    j = cJSON_GetObjectItem(data, "maxon");
    if (cJSON_IsNumber(j)) {
        uint16_t maxon = (uint16_t)j->valueint;
        ESP_LOGI(TAG, "Remote max-on → %u min", maxon);
        device_state_set_max_on_minutes(maxon);
        nvs_store_save_max_on_minutes(device_state_get_max_on_minutes());
    }

    if (relay_changed) {
        firebase_rtdb_push_live(device_state_get_temperature(),
                                device_state_get_relay());
    }
}

static void apply_partial(const char *path, cJSON *data)
{
    if (strcmp(path, "/on") == 0 && cJSON_IsBool(data)) {
        bool want = cJSON_IsTrue(data);
        if (want != device_state_get_relay()) {
            ESP_LOGI(TAG, "Remote toggle → %s", want ? "ON" : "OFF");
            device_state_set_relay(want);
            nvs_store_save_relay(want);
            gatt_server_notify_state(want);
            firebase_rtdb_push_live(device_state_get_temperature(),
                                    device_state_get_relay());
        }
    } else if (strcmp(path, "/max") == 0 && cJSON_IsNumber(data)) {
        uint8_t mn, mx;
        device_state_get_temp_limits(&mn, &mx);
        mx = (uint8_t)data->valueint;
        ESP_LOGI(TAG, "Remote max temp → %u", mx);
        device_state_set_temp_limits(mn, mx);
        nvs_store_save_temp_limits(mn, mx);
    } else if (strcmp(path, "/min") == 0 && cJSON_IsNumber(data)) {
        uint8_t mn, mx;
        device_state_get_temp_limits(&mn, &mx);
        mn = (uint8_t)data->valueint;
        ESP_LOGI(TAG, "Remote min temp → %u", mn);
        device_state_set_temp_limits(mn, mx);
        nvs_store_save_temp_limits(mn, mx);
    } else if (strcmp(path, "/ar") == 0 && cJSON_IsBool(data)) {
        bool ar = cJSON_IsTrue(data);
        ESP_LOGI(TAG, "Remote auto_reheat → %s", ar ? "ON" : "OFF");
        device_state_set_auto_reheat(ar);
        nvs_store_save_auto_reheat(ar);
    } else if (strcmp(path, "/tmask") == 0 && cJSON_IsNumber(data)) {
        int mask = data->valueint;
        ESP_LOGI(TAG, "Remote timer mask → 0x%X", mask);
        gs_timer_t timers[MAX_TIMERS];
        device_state_get_timers(timers);
        for (int i = 0; i < PRESET_TIMER_COUNT; i++)
            timers[i].enabled = (mask & (1 << i)) != 0;
        device_state_set_timers(timers);
        nvs_store_save_timers(timers);
    } else if (strcmp(path, "/tcust") == 0 && cJSON_IsNumber(data)) {
        int mins = data->valueint;
        ESP_LOGI(TAG, "Remote custom timer → %d min", mins);
        gs_timer_t timers[MAX_TIMERS];
        device_state_get_timers(timers);
        if (mins > 0) {
            timers[PRESET_TIMER_COUNT].enabled = true;
            timers[PRESET_TIMER_COUNT].hour    = mins / 60;
            timers[PRESET_TIMER_COUNT].minute  = mins % 60;
        } else {
            timers[PRESET_TIMER_COUNT].enabled = false;
        }
        device_state_set_timers(timers);
        nvs_store_save_timers(timers);
    } else if (strcmp(path, "/maxon") == 0 && cJSON_IsNumber(data)) {
        uint16_t maxon = (uint16_t)data->valueint;
        ESP_LOGI(TAG, "Remote max-on → %u min", maxon);
        device_state_set_max_on_minutes(maxon);
        nvs_store_save_max_on_minutes(device_state_get_max_on_minutes());
    }
}

static void process_sse_event(void)
{
    if (strcmp(sse_evt_type, "put") != 0 &&
        strcmp(sse_evt_type, "patch") != 0) {
        if (strcmp(sse_evt_type, "auth_revoked") == 0) {
            ESP_LOGW(TAG, "Auth revoked — re-authenticating");
            firebase_auth_invalidate_token();
            s_auth_reconnect = true;   // break the SSE loop for a fresh token
        }
        return;
    }

    cJSON *json = cJSON_Parse(sse_evt_data);
    if (!json) return;

    cJSON *jpath = cJSON_GetObjectItem(json, "path");
    cJSON *jdata = cJSON_GetObjectItem(json, "data");

    if (cJSON_IsString(jpath) && jdata) {
        const char *p = jpath->valuestring;
        bool is_put = (strcmp(sse_evt_type, "put") == 0);
        if (strcmp(p, "/") == 0)
            apply_full_settings(jdata, is_put);
        else
            apply_partial(p, jdata);
    }

    cJSON_Delete(json);
}

static void sse_handle_line(const char *line)
{
    if (strncmp(line, "event:", 6) == 0) {
        const char *v = line + 6;
        while (*v == ' ') v++;
        strncpy(sse_evt_type, v, sizeof(sse_evt_type) - 1);
        sse_evt_type[sizeof(sse_evt_type) - 1] = '\0';
    } else if (strncmp(line, "data:", 5) == 0) {
        const char *v = line + 5;
        while (*v == ' ') v++;
        int vlen = strlen(v);
        if (sse_data_pos > 0 && sse_data_pos < SSE_LINE_MAX - 1)
            sse_evt_data[sse_data_pos++] = '\n';
        int space = SSE_LINE_MAX - 1 - sse_data_pos;
        int copy  = vlen < space ? vlen : space;
        if (copy > 0) {
            memcpy(sse_evt_data + sse_data_pos, v, copy);
            sse_data_pos += copy;
        }
        sse_evt_data[sse_data_pos] = '\0';
    } else if (line[0] == '\0') {
        if (sse_evt_type[0] && sse_data_pos > 0)
            process_sse_event();
        sse_evt_type[0] = '\0';
        sse_data_pos = 0;
        sse_evt_data[0] = '\0';
    }
}

static void sse_feed(const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        char ch = data[i];
        if (ch == '\n') {
            sse_line[sse_line_pos] = '\0';
            sse_handle_line(sse_line);
            sse_line_pos = 0;
        } else if (ch != '\r') {
            if (sse_line_pos < SSE_LINE_MAX - 1)
                sse_line[sse_line_pos++] = ch;
        }
    }
}

// ── Process async requests from other tasks ──────────────────────

static void process_pending(void)
{
    if (s_pending_settings) {
        s_pending_settings = false;
        firebase_rtdb_push_settings();
    }
    if (s_pending_live) {
        s_pending_live = false;
        firebase_rtdb_push_live(device_state_get_temperature(),
                                device_state_get_relay());
    }

    event_item_t evt;
    while (xQueueReceive(s_event_queue, &evt, 0) == pdTRUE) {
        firebase_rtdb_push_event(evt.type, evt.temp);
    }
}

// ── Main task ────────────────────────────────────────────────────

void firebase_task(void *param)
{
    ESP_LOGI(TAG, "Firebase task started — waiting for auth");

    while (!firebase_auth_is_ready()) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    while (!firebase_auth_get_id_token()) {
        ESP_LOGW(TAG, "Waiting for valid ID token...");
        process_pending();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    firebase_rtdb_push_boot();
    firebase_rtdb_push_settings();
    firebase_rtdb_push_live(device_state_get_temperature(),
                            device_state_get_relay());

    // Exponential backoff on connection failures: 3 s → 5 min.  A flat
    // 3 s retry meant ~29k TLS handshakes/day (heap churn + radio) when
    // RTDB was unreachable for an extended period.
    uint32_t backoff_ms = RECONNECT_WAIT_MS;
    #define SSE_BACKOFF_MAX_MS 300000

    for (;;) {
        const char *token = firebase_auth_get_id_token();
        if (!token) {
            ESP_LOGW(TAG, "No token — retrying in %lu ms",
                     (unsigned long)backoff_ms);
            process_pending();
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms *= 2;
            if (backoff_ms > SSE_BACKOFF_MAX_MS) backoff_ms = SSE_BACKOFF_MAX_MS;
            continue;
        }

        char path[64];
        snprintf(path, sizeof(path), "set/%s",
                 firebase_auth_get_device_id());
        build_url(path, token);

        esp_http_client_config_t sse_cfg = {
            .url               = s_url,
            .method            = HTTP_METHOD_GET,
            .timeout_ms        = SSE_TIMEOUT_MS,
            .buffer_size       = 1024,
            .buffer_size_tx    = HTTP_TX_BUF,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t sse = esp_http_client_init(&sse_cfg);
        esp_http_client_set_header(sse, "Accept", "text/event-stream");

        esp_err_t err = esp_http_client_open(sse, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SSE open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(sse);
            process_pending();
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms *= 2;
            if (backoff_ms > SSE_BACKOFF_MAX_MS) backoff_ms = SSE_BACKOFF_MAX_MS;
            continue;
        }

        int content_len = esp_http_client_fetch_headers(sse);
        int status = esp_http_client_get_status_code(sse);
        if (status != 200) {
            ESP_LOGE(TAG, "SSE HTTP %d (content_len=%d)", status, content_len);
            esp_http_client_close(sse);
            esp_http_client_cleanup(sse);
            // 401 = server rejected a locally-unexpired token (e.g.
            // revoked server-side).  Previously we retried the SAME
            // token until local expiry — up to ~55 min of dead cloud.
            if (status == 401) {
                firebase_auth_invalidate_token();
            }
            process_pending();
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms *= 2;
            if (backoff_ms > SSE_BACKOFF_MAX_MS) backoff_ms = SSE_BACKOFF_MAX_MS;
            continue;
        }

        ESP_LOGI(TAG, "SSE connected — listening for settings");
        backoff_ms = RECONNECT_WAIT_MS;   // healthy connection — reset
        sse_line_pos = 0;
        sse_evt_type[0] = '\0';
        sse_data_pos = 0;
        sse_evt_data[0] = '\0';

        TickType_t last_push  = xTaskGetTickCount();
        TickType_t last_data  = xTaskGetTickCount();
        TickType_t last_stats = xTaskGetTickCount();
        int relay_on_accum_s  = 0;
        bool connected = true;

        while (connected) {
            char buf[256];
            int read = esp_http_client_read(sse, buf, sizeof(buf) - 1);

            if (read > 0) {
                buf[read] = '\0';
                sse_feed(buf, read);
                last_data = xTaskGetTickCount();
            } else {
                TickType_t since = xTaskGetTickCount() - last_data;
                if (since > pdMS_TO_TICKS(SSE_DEAD_MS)) {
                    ESP_LOGW(TAG, "SSE no data for %ds — reconnecting",
                             SSE_DEAD_MS / 1000);
                    connected = false;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            process_pending();

            if (s_auth_reconnect) {
                s_auth_reconnect = false;
                connected = false;
                break;
            }

            TickType_t now = xTaskGetTickCount();
            if ((now - last_push) >= pdMS_TO_TICKS(PUSH_INTERVAL_MS)) {
                float temp = device_state_get_temperature();
                bool  on   = device_state_get_relay();
                firebase_rtdb_push_live(temp, on);
                last_push = now;

                if (on) {
                    relay_on_accum_s += PUSH_INTERVAL_MS / 1000;
                }
            }

            #define STATS_INTERVAL_MS 3600000
            if ((now - last_stats) >= pdMS_TO_TICKS(STATS_INTERVAL_MS)) {
                if (relay_on_accum_s > 0) {
                    firebase_rtdb_push_stats(relay_on_accum_s);
                    ESP_LOGI(TAG, "Stats pushed: %ds relay ON this hour",
                             relay_on_accum_s);
                }
                relay_on_accum_s = 0;
                last_stats = now;
            }
        }

        if (relay_on_accum_s > 0) {
            firebase_rtdb_push_stats(relay_on_accum_s);
            ESP_LOGI(TAG, "Flushed %ds accumulated stats before reconnect",
                     relay_on_accum_s);
            relay_on_accum_s = 0;
        }

        esp_http_client_close(sse);
        esp_http_client_cleanup(sse);
        ESP_LOGI(TAG, "SSE disconnected — will reconnect");
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_WAIT_MS));
    }
}
