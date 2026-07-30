#include "wifi_prov.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "os/os_mbuf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#include "ble_init.h"
#include "time_sync.h"
#include "firebase_auth.h"
#include "firebase_rtdb.h"
#include "owner_auth.h"

static const char *TAG = "prov";

// ── NVS keys ─────────────────────────────────────────────────────

static const char *NVS_NS       = "gs_prov";
static const char *KEY_SSID     = "ssid";
static const char *KEY_PASS     = "pass";
static const char *KEY_USER_ID  = "user_id";
static const char *KEY_NICKNAME = "nickname";
static const char *KEY_PROV     = "provisioned";

// ── Provisioning state ───────────────────────────────────────────

static prov_status_t s_prov_status = PROV_IDLE;
static uint16_t      h_prov_status;          // GATT notify handle
static char          s_ssid[33]    = {0};    // max WiFi SSID length
static char          s_pass[65]    = {0};    // max WiFi password length
static char          s_user_id[64] = {0};    // Firebase UID
static char          s_nickname[PROV_NICKNAME_MAX + 1] = {0};
static bool          s_wifi_requested = false;  // true if WiFi creds were written this session
static bool          s_prov_in_progress = false; // guards against concurrent prov_connect_task
static portMUX_TYPE  s_prov_mux = portMUX_INITIALIZER_UNLOCKED;

// Firebase auth data received via BLE (0x15)
#define AUTH_REFRESH_MAX 512
#define AUTH_DEVICE_ID_MAX 16
static char s_auth_refresh[AUTH_REFRESH_MAX]    = {0};
static char s_auth_device_id[AUTH_DEVICE_ID_MAX] = {0};
static bool s_auth_received = false;

// Built advertising name: "GeyserSwitch-{nickname}" (max 29 chars + null)
#define ADV_NAME_MAX 30
static char s_adv_name[ADV_NAME_MAX] = {0};

// WiFi connection state — persistent across reconnects
static bool s_wifi_connected = false;
static bool s_wifi_inited    = false;

// Provisioning-time synchronization (blocking wait)
static EventGroupHandle_t s_prov_wifi_events;
#define PROV_CONNECTED_BIT BIT0
#define PROV_FAIL_BIT      BIT1
static int s_retry_count = 0;
#define MAX_RETRIES 5

// Exponential backoff for reconnection after quick retries exhaust
static TimerHandle_t s_backoff_timer  = NULL;
static uint32_t      s_backoff_sec    = 30;
#define BACKOFF_INITIAL_SEC   30
#define BACKOFF_MAX_SEC       1800   // 30 minutes

static void backoff_timer_cb(TimerHandle_t timer) {
    ESP_LOGI("prov", "Backoff retry (next in %lus)", (unsigned long)s_backoff_sec);
    esp_wifi_connect();
}

// ── UUID definitions ─────────────────────────────────────────────
//
// Provisioning service: 47530010-7652-4543-b201-c4b801a6c700
// Uses 0x10–0x14 range to stay clear of control service (0x01–0x07).

#define GS_UUID128_INIT(id)                                          \
    BLE_UUID128_INIT(0x00, 0xc7, 0xa6, 0x01, 0xb8, 0xc4, 0x01, 0xb2,\
                     0x43, 0x45, 0x52, 0x76, (id), 0x00, 0x53, 0x47)

static const ble_uuid128_t prov_svc_uuid     = GS_UUID128_INIT(0x10);
static const ble_uuid128_t uuid_wifi_creds   = GS_UUID128_INIT(0x11);
static const ble_uuid128_t uuid_user_bind    = GS_UUID128_INIT(0x12);
static const ble_uuid128_t uuid_prov_status  = GS_UUID128_INIT(0x13);
static const ble_uuid128_t uuid_dev_nickname = GS_UUID128_INIT(0x14);
static const ble_uuid128_t uuid_auth_data    = GS_UUID128_INIT(0x15);
static const ble_uuid128_t uuid_owner_key    = GS_UUID128_INIT(0x16);

// ── Advertising name builder ─────────────────────────────────────

static void build_adv_name(void) {
    if (s_nickname[0] != '\0') {
        snprintf(s_adv_name, ADV_NAME_MAX, "GeyserSwitch-%s", s_nickname);
    } else if (wifi_prov_is_provisioned()) {
        strncpy(s_adv_name, "GeyserSwitch", ADV_NAME_MAX - 1);
    } else {
        strncpy(s_adv_name, "GeyserSwitch-Setup", ADV_NAME_MAX - 1);
    }
}

// ── Persistent WiFi event handler ────────────────────────────────
//
// Registered once, never unregistered.  Tracks connection state
// globally and auto-reconnects on disconnect.

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_wifi_connected = false;

            if (s_retry_count < MAX_RETRIES) {
                s_retry_count++;
                ESP_LOGI(TAG, "WiFi retry %d/%d", s_retry_count, MAX_RETRIES);
                esp_wifi_connect();
            } else {
                if (s_prov_wifi_events)
                    xEventGroupSetBits(s_prov_wifi_events, PROV_FAIL_BIT);

                // Start exponential backoff: 30s → 60s → 120s → 300s → 1800s
                if (!s_backoff_timer) {
                    s_backoff_timer = xTimerCreate(
                        "wifi_bo", pdMS_TO_TICKS(s_backoff_sec * 1000),
                        pdFALSE, NULL, backoff_timer_cb);
                }
                if (s_backoff_timer) {
                    xTimerChangePeriod(s_backoff_timer,
                        pdMS_TO_TICKS(s_backoff_sec * 1000), 0);
                    xTimerStart(s_backoff_timer, 0);
                    ESP_LOGW(TAG, "WiFi retries exhausted — backoff %lus",
                             (unsigned long)s_backoff_sec);
                    if (s_backoff_sec < BACKOFF_MAX_SEC) {
                        s_backoff_sec *= 2;
                        if (s_backoff_sec > BACKOFF_MAX_SEC)
                            s_backoff_sec = BACKOFF_MAX_SEC;
                    }
                }
                s_retry_count = 0;
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_backoff_sec = BACKOFF_INITIAL_SEC;
        if (s_backoff_timer) xTimerStop(s_backoff_timer, 0);
        s_wifi_connected = true;
        // Start SNTP on EVERY IP acquisition (idempotent), not just the
        // provisioning success path — otherwise a device that boots
        // faster than the router (e.g. after a power outage) reconnects
        // via backoff but never syncs its clock, silently killing all
        // schedule timers until reboot.
        time_sync_start_sntp();
        if (s_prov_wifi_events)
            xEventGroupSetBits(s_prov_wifi_events, PROV_CONNECTED_BIT);
    }
}

// ── WiFi init + connect ──────────────────────────────────────────

static void wifi_stack_init(void) {
    if (s_wifi_inited) return;
    s_wifi_inited = true;

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, NULL);
}

static bool wifi_connect(const char *ssid, const char *pass) {
    s_retry_count = 0;
    wifi_stack_init();

    // Already connected to the requested network — skip reconnection.
    if (s_wifi_connected) {
        wifi_config_t cur = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK &&
            strcmp((char *)cur.sta.ssid, ssid) == 0) {
            ESP_LOGI(TAG, "Already connected to \"%s\"", ssid);
            return true;
        }
    }

    // Stop the driver so esp_wifi_start() will fire STA_START → connect.
    // esp_wifi_stop() does NOT post STA_DISCONNECTED, avoiding spurious
    // reconnect attempts from the event handler.
    esp_wifi_stop();
    s_wifi_connected = false;

    s_prov_wifi_events = xEventGroupCreate();

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();

    ESP_LOGI(TAG, "Connecting to \"%s\"...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_prov_wifi_events,
        PROV_CONNECTED_BIT | PROV_FAIL_BIT,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(15000));

    EventGroupHandle_t tmp = s_prov_wifi_events;
    s_prov_wifi_events = NULL;
    vEventGroupDelete(tmp);

    if (bits & PROV_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        return true;
    }

    ESP_LOGW(TAG, "WiFi connection failed");
    return false;
}

// ── NVS helpers ──────────────────────────────────────────────────

static void save_provisioning(bool with_wifi) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_str(h, KEY_USER_ID, s_user_id);
    nvs_set_str(h, KEY_NICKNAME, s_nickname);

    if (with_wifi) {
        nvs_set_str(h, KEY_SSID, s_ssid);
        nvs_set_str(h, KEY_PASS, s_pass);
    }

    nvs_set_u8(h, KEY_PROV, 1);
    nvs_commit(h);
    nvs_close(h);

    // Store Firebase auth credentials if received via BLE, and
    // start the Firebase sync task if it wasn't already running.
    if (s_auth_received) {
        bool was_ready = firebase_auth_is_ready();
        firebase_auth_set_credentials(s_auth_refresh, s_auth_device_id);

        if (!was_ready && s_wifi_connected && firebase_auth_is_ready()) {
            xTaskCreate(firebase_task, "firebase", 12288, NULL, 2, NULL);
            ESP_LOGI(TAG, "Firebase task started after provisioning");
        }
    }

    build_adv_name();

    ESP_LOGI(TAG, "Provisioning saved to NVS (wifi=%s, auth=%s, name=%s)",
             with_wifi ? "yes" : "no",
             s_auth_received ? "yes" : "no",
             s_adv_name);
}

// ── Notify helper ────────────────────────────────────────────────

static void notify_prov_status(prov_status_t status) {
    s_prov_status = status;
    uint16_t conn = ble_get_conn_handle();
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;

    uint8_t val = (uint8_t)status;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&val, sizeof(val));
    if (om) {
        ble_gatts_notify_custom(conn, h_prov_status, om);
    }
}

// ── Provisioning task (runs on a separate FreeRTOS task) ─────────

static void prov_connect_task(void *param) {
    bool with_wifi = (bool)(uintptr_t)param;

    if (with_wifi) {
        notify_prov_status(PROV_CONNECTING);

        if (wifi_connect(s_ssid, s_pass)) {
            time_sync_start_sntp();

            notify_prov_status(PROV_WIFI_OK);
            save_provisioning(true);
            notify_prov_status(PROV_COMPLETE);
            ESP_LOGI(TAG, "Full provisioning complete (user=%s, nick=%s)",
                     s_user_id, s_nickname);
        } else {
            notify_prov_status(PROV_WIFI_FAIL);
        }
    } else {
        save_provisioning(false);
        notify_prov_status(PROV_BLE_ONLY_OK);
        ESP_LOGI(TAG, "BLE-only provisioning complete (user=%s, nick=%s)",
                 s_user_id, s_nickname);
    }

    s_prov_in_progress = false;
    vTaskDelete(NULL);
}

// ── GATT callbacks ───────────────────────────────────────────────

/// WiFi credentials — Read (SSID only), Write (SSID\0password).
/// Owner-gated in BOTH directions once provisioned: the write for
/// obvious reasons, the read because the SSID leaks the home network
/// name to any paired stranger.
static int on_wifi_creds(uint16_t conn, uint16_t attr,
                         struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (!owner_auth_gate_ok())
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        // Return stored SSID only (never expose the password).
        os_mbuf_append(ctxt->om, s_ssid, strlen(s_ssid));
        return 0;
    }

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 3 || len > sizeof(s_ssid) + sizeof(s_pass))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    char buf[sizeof(s_ssid) + sizeof(s_pass)];
    memset(buf, 0, sizeof(buf));
    os_mbuf_copydata(ctxt->om, 0, len, buf);

    // Find the null separator.
    char *sep = memchr(buf, '\0', len);
    if (sep == NULL || sep == buf)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    strncpy(s_ssid, buf, sizeof(s_ssid) - 1);
    strncpy(s_pass, sep + 1, sizeof(s_pass) - 1);
    s_wifi_requested = true;

    ESP_LOGI(TAG, "WiFi creds received: SSID=\"%s\"", s_ssid);
    return 0;
}

/// User binding — Write only.
/// Format: UTF-8 Firebase User UID string.
/// Writing this triggers the provisioning sequence.
/// If WiFi creds were written first → full provisioning (WiFi + BLE).
/// If no WiFi creds → BLE-only provisioning.
///
/// THE HIJACK FIX: on an unprovisioned device this is open (first
/// owner claims it). Once provisioned, re-binding requires an
/// owner-unlocked connection — a stranger cannot rebind the geyser to
/// their account. Ownership transfer = physical factory reset
/// (button, 10 s) back to setup mode.
static int on_user_bind(uint16_t conn, uint16_t attr,
                        struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    if (!owner_auth_gate_ok()) {
        ESP_LOGW(TAG, "Rebind rejected — connection not owner-unlocked");
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len >= sizeof(s_user_id))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    memset(s_user_id, 0, sizeof(s_user_id));
    os_mbuf_copydata(ctxt->om, 0, len, s_user_id);

    ESP_LOGI(TAG, "User binding: uid=\"%s\", wifi=%s",
             s_user_id, s_wifi_requested ? "yes" : "no");

    portENTER_CRITICAL(&s_prov_mux);
    if (s_prov_in_progress) {
        portEXIT_CRITICAL(&s_prov_mux);
        ESP_LOGW(TAG, "Provisioning already in progress — rejecting");
        return BLE_ATT_ERR_UNLIKELY;
    }
    s_prov_in_progress = true;
    portEXIT_CRITICAL(&s_prov_mux);

    bool with_wifi = s_wifi_requested && s_ssid[0] != '\0';
    if (xTaskCreate(prov_connect_task, "prov", 4096,
                    (void *)(uintptr_t)with_wifi, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create provisioning task");
        s_prov_in_progress = false;
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    return 0;
}

/// Provisioning status — Read, Notify.
/// Format: uint8 — see prov_status_t enum.
static int on_prov_status(uint16_t conn, uint16_t attr,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    uint8_t val = (uint8_t)s_prov_status;
    os_mbuf_append(ctxt->om, &val, sizeof(val));
    return 0;
}

/// Firebase auth data — Write only.
/// Format: "refreshToken\0deviceId" (null-separated).
/// Written by the app after it exchanges the custom token.
static int on_auth_data(uint16_t conn, uint16_t attr,
                        struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    if (!owner_auth_gate_ok())
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 3 || len >= AUTH_REFRESH_MAX + AUTH_DEVICE_ID_MAX)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    char buf[AUTH_REFRESH_MAX + AUTH_DEVICE_ID_MAX];
    memset(buf, 0, sizeof(buf));
    os_mbuf_copydata(ctxt->om, 0, len, buf);

    char *sep = memchr(buf, '\0', len);
    if (sep == NULL || sep == buf)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    memset(s_auth_refresh, 0, sizeof(s_auth_refresh));
    memset(s_auth_device_id, 0, sizeof(s_auth_device_id));
    strncpy(s_auth_refresh, buf, sizeof(s_auth_refresh) - 1);
    strncpy(s_auth_device_id, sep + 1, sizeof(s_auth_device_id) - 1);
    s_auth_received = true;

    ESP_LOGI(TAG, "Auth data received: device=\"%s\", token_len=%d",
             s_auth_device_id, (int)strlen(s_auth_refresh));
    return 0;
}

/// Device nickname — Read, Write.
/// Format: UTF-8, max PROV_NICKNAME_MAX (16) chars.
/// Stored in NVS, used for advertising name: "GeyserSwitch-{nickname}".
static int on_dev_nickname(uint16_t conn, uint16_t attr,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, s_nickname, strlen(s_nickname));
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (!owner_auth_gate_ok())
            return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len > PROV_NICKNAME_MAX)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

        memset(s_nickname, 0, sizeof(s_nickname));
        os_mbuf_copydata(ctxt->om, 0, len, s_nickname);

        // Rebuild advertising name.
        build_adv_name();

        ESP_LOGI(TAG, "Device nickname: \"%s\" → adv name \"%s\"",
                 s_nickname, s_adv_name);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/// Owner key — Write only.
/// Format: exactly 32 random bytes, generated by the app at
/// provisioning and stored in the account's cloud scope so every
/// phone signed into the household account can unlock via 0x0E.
/// Open on an unprovisioned device (first provisioning sets it);
/// once provisioned, rewriting (rotation) requires owner unlock.
static int on_owner_key(uint16_t conn, uint16_t attr,
                        struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    if (!owner_auth_gate_ok())
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != OWNER_KEY_LEN)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t key[OWNER_KEY_LEN];
    os_mbuf_copydata(ctxt->om, 0, len, key);

    if (!owner_auth_set_key(key, len))
        return BLE_ATT_ERR_INSUFFICIENT_RES;

    return 0;
}

// ── GATT service table ───────────────────────────────────────────

static const struct ble_gatt_svc_def prov_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &prov_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {   // 0x11 — WiFi credentials (encrypted: contains password)
                .uuid       = &uuid_wifi_creds.u,
                .access_cb  = on_wifi_creds,
                .flags      = BLE_GATT_CHR_F_READ_ENC
                            | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {   // 0x12 — User binding (encrypted: triggers provisioning)
                .uuid       = &uuid_user_bind.u,
                .access_cb  = on_user_bind,
                .flags      = BLE_GATT_CHR_F_WRITE_ENC,
            },
            {   // 0x13 — Provisioning status
                .uuid       = &uuid_prov_status.u,
                .access_cb  = on_prov_status,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &h_prov_status,
            },
            {   // 0x14 — Device nickname (encrypted)
                .uuid       = &uuid_dev_nickname.u,
                .access_cb  = on_dev_nickname,
                .flags      = BLE_GATT_CHR_F_READ_ENC
                            | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {   // 0x15 — Firebase auth data (encrypted: contains tokens)
                .uuid       = &uuid_auth_data.u,
                .access_cb  = on_auth_data,
                .flags      = BLE_GATT_CHR_F_WRITE_ENC,
            },
            {   // 0x16 — Owner key (encrypted; write at provisioning/rotation)
                .uuid       = &uuid_owner_key.u,
                .access_cb  = on_owner_key,
                .flags      = BLE_GATT_CHR_F_WRITE_ENC,
            },
            { 0 }, // sentinel
        },
    },
    { 0 }, // sentinel
};

// ── Public API ───────────────────────────────────────────────────

bool wifi_prov_is_provisioned(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    uint8_t val = 0;
    nvs_get_u8(h, KEY_PROV, &val);
    nvs_close(h);
    return val == 1;
}

bool wifi_prov_has_wifi(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    char ssid[33] = {0};
    size_t len = sizeof(ssid);
    esp_err_t err = nvs_get_str(h, KEY_SSID, ssid, &len);
    nvs_close(h);
    return err == ESP_OK && ssid[0] != '\0';
}

void wifi_prov_load_nickname(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        build_adv_name();
        return;
    }

    // Load all provisioning data so it's available before BLE starts.
    size_t len;

    len = sizeof(s_nickname);
    nvs_get_str(h, KEY_NICKNAME, s_nickname, &len);

    len = sizeof(s_ssid);
    nvs_get_str(h, KEY_SSID, s_ssid, &len);

    len = sizeof(s_user_id);
    nvs_get_str(h, KEY_USER_ID, s_user_id, &len);

    nvs_close(h);

    build_adv_name();

    // Set initial provisioning status based on what's stored.
    if (wifi_prov_is_provisioned()) {
        s_prov_status = (s_ssid[0] != '\0') ? PROV_COMPLETE : PROV_BLE_ONLY_OK;
    }

    ESP_LOGI(TAG, "Loaded: nick=\"%s\" → \"%s\", ssid=\"%s\", status=%d",
             s_nickname, s_adv_name, s_ssid, s_prov_status);
}

const char *wifi_prov_get_device_name(void) {
    if (s_adv_name[0] != '\0') return s_adv_name;
    // If load hasn't happened yet, just return default.
    return wifi_prov_is_provisioned() ? "GeyserSwitch" : "GeyserSwitch-Setup";
}

void wifi_prov_gatt_init(void) {
    int rc;
    rc = ble_gatts_count_cfg(prov_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(prov_svcs);
    assert(rc == 0);
    ESP_LOGI(TAG, "Provisioning GATT service registered");
}

void wifi_prov_start_wifi(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t ssid_len = sizeof(s_ssid);
    size_t pass_len = sizeof(s_pass);
    nvs_get_str(h, KEY_SSID, s_ssid, &ssid_len);
    nvs_get_str(h, KEY_PASS, s_pass, &pass_len);

    size_t uid_len = sizeof(s_user_id);
    nvs_get_str(h, KEY_USER_ID, s_user_id, &uid_len);
    nvs_close(h);

    ESP_LOGI(TAG, "Reconnecting WiFi: SSID=\"%s\", user=\"%s\"", s_ssid, s_user_id);

    portENTER_CRITICAL(&s_prov_mux);
    if (s_prov_in_progress) {
        portEXIT_CRITICAL(&s_prov_mux);
        ESP_LOGW(TAG, "Provisioning already in progress — skipping reconnect");
        return;
    }
    s_prov_in_progress = true;
    portEXIT_CRITICAL(&s_prov_mux);

    if (xTaskCreate(prov_connect_task, "wifi_rc", 4096,
                    (void *)(uintptr_t)true, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WiFi reconnect task");
        s_prov_in_progress = false;
    }
}

void wifi_prov_reset(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);

    ble_store_clear();

    memset(s_ssid, 0, sizeof(s_ssid));
    memset(s_pass, 0, sizeof(s_pass));
    memset(s_user_id, 0, sizeof(s_user_id));
    memset(s_nickname, 0, sizeof(s_nickname));
    memset(s_adv_name, 0, sizeof(s_adv_name));
    memset(s_auth_refresh, 0, sizeof(s_auth_refresh));
    memset(s_auth_device_id, 0, sizeof(s_auth_device_id));
    s_prov_status = PROV_IDLE;
    s_wifi_requested = false;
    s_auth_received = false;
    s_prov_in_progress = false;

    ESP_LOGI(TAG, "Provisioning data erased (including BLE bonds)");
}
