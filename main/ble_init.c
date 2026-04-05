#include "ble_init.h"

#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "store/config/ble_store_config.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "gatt_server.h"
#include "wifi_prov.h"

static const char *TAG = "ble";

/// Advertising name — uses nickname if set, else provisioning-aware default.
static const char *device_name(void) {
    return wifi_prov_get_device_name();
}

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

// ── Connection handle accessor ───────────────────────────────────

uint16_t ble_get_conn_handle(void) {
    return s_conn_handle;
}

// ── GAP event handler ────────────────────────────────────────────

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Client connected (handle=%d)", s_conn_handle);

            ble_gap_security_initiate(s_conn_handle);
        } else {
            ESP_LOGW(TAG, "Connection failed, status=%d", event->connect.status);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Client disconnected (reason=%d)",
                 event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_start_advertising();
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU negotiated: %d", event->mtu.value);
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "Subscribe: handle=%d, notify=%d",
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "Encryption established (handle=%d)",
                     event->enc_change.conn_handle);
        } else {
            ESP_LOGW(TAG, "Encryption failed (status=%d), disconnecting",
                     event->enc_change.status);
            ble_gap_terminate(event->enc_change.conn_handle,
                              BLE_ERR_AUTH_FAIL);
        }
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        break;
    }

    return 0;
}

// ── Advertising ──────────────────────────────────────────────────

void ble_start_advertising(void) {
    // Advertisement data: flags + 128-bit service UUID.
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t[]){ gs_svc_uuid };
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    // Scan response: device name.
    const char *name = device_name();
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)name;
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields failed: %d", rc);
        return;
    }

    // Connectable, general-discoverable, advertise forever.
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "Advertising as \"%s\"", name);
}

// ── NimBLE host callbacks ────────────────────────────────────────

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_start_advertising();
}

static void on_reset(int reason) {
    ESP_LOGE(TAG, "NimBLE host reset, reason=%d", reason);
}

static void nimble_host_task(void *param) {
    nimble_port_run();          // blocks until nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ── Public init ──────────────────────────────────────────────────

void ble_init(void) {
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    // Host configuration.
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;

    // Security Manager — Just Works pairing (no display/keyboard).
    ble_hs_cfg.sm_io_cap       = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding      = 1;
    ble_hs_cfg.sm_mitm         = 0;
    ble_hs_cfg.sm_sc           = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_store_config_init();

    // Mandatory GAP / GATT services.
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(device_name());

    // Register our custom services.
    gatt_server_init();
    wifi_prov_gatt_init();

    // Start the NimBLE host FreeRTOS task.
    nimble_port_freertos_init(nimble_host_task);

    ESP_LOGI(TAG, "BLE initialised");
}
