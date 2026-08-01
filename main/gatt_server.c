#include "gatt_server.h"

#include <string.h>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "os/os_mbuf.h"

#include "device_state.h"
#include "relay.h"
#include "nvs_store.h"
#include "ble_init.h"
#include "time_sync.h"
#include "event_buffer.h"
#include "firebase_rtdb.h"
#include "firebase_auth.h"
#include "owner_auth.h"
#include "temperature.h"

static const char *TAG = "gatt";

// ── UUID helpers ─────────────────────────────────────────────────
//
// Base UUID: 47530000-7652-4543-b201-c4b801a6c700
// In NimBLE little-endian byte order only the 13th byte changes.

#define GS_UUID128_INIT(id)                                          \
    BLE_UUID128_INIT(0x00, 0xc7, 0xa6, 0x01, 0xb8, 0xc4, 0x01, 0xb2,\
                     0x43, 0x45, 0x52, 0x76, (id), 0x00, 0x53, 0x47)

const ble_uuid128_t gs_svc_uuid         = GS_UUID128_INIT(0x01);
static const ble_uuid128_t uuid_temp    = GS_UUID128_INIT(0x02);
static const ble_uuid128_t uuid_state   = GS_UUID128_INIT(0x03);
static const ble_uuid128_t uuid_limits  = GS_UUID128_INIT(0x04);
static const ble_uuid128_t uuid_timers  = GS_UUID128_INIT(0x05);
static const ble_uuid128_t uuid_info    = GS_UUID128_INIT(0x06);
// 0x07 (legacy telemetry bulk) retired — buffered telemetry is 0x0A.
static const ble_uuid128_t uuid_tsync   = GS_UUID128_INIT(0x08);
static const ble_uuid128_t uuid_events  = GS_UUID128_INIT(0x09);
static const ble_uuid128_t uuid_tbuf    = GS_UUID128_INIT(0x0A);
static const ble_uuid128_t uuid_ack     = GS_UUID128_INIT(0x0B);
static const ble_uuid128_t uuid_devid  = GS_UUID128_INIT(0x0C);
static const ble_uuid128_t uuid_maxon  = GS_UUID128_INIT(0x0D);
static const ble_uuid128_t uuid_oauth  = GS_UUID128_INIT(0x0E);
static const ble_uuid128_t uuid_runst  = GS_UUID128_INIT(0x0F);

// ── Value handles (filled by NimBLE during registration) ─────────

static uint16_t h_temp;
static uint16_t h_state;
static uint16_t h_events;

// ── Access callbacks ─────────────────────────────────────────────

/// Temperature — Read, Notify.
/// Format: int16 LE, °C × 100 (e.g. 4250 = 42.50 °C).
static int on_temp_access(uint16_t conn, uint16_t attr,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    float temp = device_state_get_temperature();
    int16_t raw = (int16_t)(temp * 100.0f);
    os_mbuf_append(ctxt->om, &raw, sizeof(raw));
    return 0;
}

/// Geyser state — Read, Write, Notify.
/// Format: uint8 — 0x00 = off, 0x01 = on.
static int on_state_access(uint16_t conn, uint16_t attr,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t val = device_state_get_relay() ? 0x01 : 0x00;
        os_mbuf_append(ctxt->om, &val, sizeof(val));
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (!owner_auth_gate_ok())
            return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

        uint8_t val;
        if (OS_MBUF_PKTLEN(ctxt->om) != 1)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        os_mbuf_copydata(ctxt->om, 0, sizeof(val), &val);

        bool on = (val == 0x01);
        device_state_set_relay(on);   // drives the GPIO under the state mutex
        nvs_store_save_relay(on);

        ESP_LOGI(TAG, "Relay → %s", on ? "ON" : "OFF");

        gatt_server_notify_state(on);
        firebase_rtdb_request_settings_push();
        firebase_rtdb_request_live_push();
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/// Temperature limits — Read, Write.
/// Format: [uint8 min, uint8 max, uint8 auto_reheat].
/// Write accepts 2 bytes (legacy) or 3 bytes (with auto_reheat).
static int on_limits_access(uint16_t conn, uint16_t attr,
                            struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t min, max;
        device_state_get_temp_limits(&min, &max);
        uint8_t buf[3] = { min, max, device_state_get_auto_reheat() ? 1 : 0 };
        os_mbuf_append(ctxt->om, buf, sizeof(buf));
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (!owner_auth_gate_ok())
            return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len < 2 || len > 3)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

        uint8_t buf[3] = {0};
        os_mbuf_copydata(ctxt->om, 0, len, buf);

        device_state_set_temp_limits(buf[0], buf[1]);
        nvs_store_save_temp_limits(buf[0], buf[1]);

        if (len >= 3) {
            bool ar = (buf[2] == 1);
            device_state_set_auto_reheat(ar);
            nvs_store_save_auto_reheat(ar);
            ESP_LOGI(TAG, "Temp limits → [%d, %d] °C, auto-reheat=%s",
                     buf[0], buf[1], ar ? "on" : "off");
        } else {
            ESP_LOGI(TAG, "Temp limits → [%d, %d] °C", buf[0], buf[1]);
        }
        firebase_rtdb_request_settings_push();
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/// Timer configuration — Read, Write.
/// Format: MAX_TIMERS × 4 bytes [is_preset, enabled, hour, minute].
/// On write, preset timer times are ignored (only enabled flag changes).
static int on_timers_access(uint16_t conn, uint16_t attr,
                            struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        gs_timer_t timers[MAX_TIMERS];
        device_state_get_timers(timers);
        for (int i = 0; i < MAX_TIMERS; i++) {
            uint8_t entry[4] = {
                (i < PRESET_TIMER_COUNT) ? 1 : 0,  // is_preset
                timers[i].enabled ? 1 : 0,
                timers[i].hour,
                timers[i].minute,
            };
            os_mbuf_append(ctxt->om, entry, sizeof(entry));
        }
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (!owner_auth_gate_ok())
            return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len % 4 != 0)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

        uint8_t count = len / 4;
        if (count > MAX_TIMERS) count = MAX_TIMERS;

        uint8_t buf[MAX_TIMERS * 4];
        os_mbuf_copydata(ctxt->om, 0, count * 4, buf);

        gs_timer_t timers[MAX_TIMERS];
        device_state_get_timers(timers);   // start from current state

        for (int i = 0; i < count; i++) {
            // Update enabled flag for all timers.
            timers[i].enabled = (buf[i * 4 + 1] == 1);

            // Only update time for non-preset (custom) timers.
            if (i >= PRESET_TIMER_COUNT) {
                timers[i].hour   = buf[i * 4 + 2];
                timers[i].minute = buf[i * 4 + 3];
            }
        }

        device_state_set_timers(timers);
        nvs_store_save_timers(timers);

        ESP_LOGI(TAG, "Timers updated (%d entries)", count);
        firebase_rtdb_request_settings_push();
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/// Device info — Read only.
/// Format: UTF-8 firmware version string, taken from the running
/// image's embedded app description (driven by version.txt / PROJECT_VER)
/// so the reported version always matches the actual running build.
static int on_info_access(uint16_t conn, uint16_t attr,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    const char *ver = esp_app_get_description()->version;
    os_mbuf_append(ctxt->om, ver, strlen(ver));
    return 0;
}

/// Time sync — Write only.
/// Format: uint32 LE — Unix timestamp (seconds since epoch).
static int on_time_sync(uint16_t conn, uint16_t attr,
                        struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    if (!owner_auth_gate_ok())
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

    if (OS_MBUF_PKTLEN(ctxt->om) != 4)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint32_t unix_time;
    os_mbuf_copydata(ctxt->om, 0, sizeof(unix_time), &unix_time);

    time_sync_set_from_ble(unix_time);
    return 0;
}

/// Device events — Read (buffered), Notify (real-time).
/// Read: N × 6 bytes [type, temp, ts0, ts1, ts2, ts3].
/// Notify payload: 2 bytes [type, temp].
static int on_events_access(uint16_t conn, uint16_t attr,
                            struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    buffered_event_t events[EVENT_BUF_MAX];
    uint8_t count = event_buffer_get_events(events, EVENT_BUF_MAX);

    for (int i = 0; i < count; i++) {
        uint8_t entry[6];
        entry[0] = events[i].type;
        entry[1] = events[i].temp;
        entry[2] = (uint8_t)(events[i].timestamp);
        entry[3] = (uint8_t)(events[i].timestamp >> 8);
        entry[4] = (uint8_t)(events[i].timestamp >> 16);
        entry[5] = (uint8_t)(events[i].timestamp >> 24);
        os_mbuf_append(ctxt->om, entry, sizeof(entry));
    }

    ESP_LOGI(TAG, "Events read: %d entries", count);
    return 0;
}

/// Telemetry buffer — Read.
/// N × 10 bytes [temp_i16_le, relay, min, max, auto_reheat, ts_u32_le].
static int on_tbuf_access(uint16_t conn, uint16_t attr,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    buffered_telemetry_t telem[TELEM_BUF_MAX];
    uint8_t count = event_buffer_get_telemetry(telem, TELEM_BUF_MAX);

    for (int i = 0; i < count; i++) {
        uint8_t entry[10];
        // temp_raw as int16 LE
        entry[0] = (uint8_t)(telem[i].temp_raw);
        entry[1] = (uint8_t)(telem[i].temp_raw >> 8);
        entry[2] = telem[i].relay_on;
        entry[3] = telem[i].min_temp;
        entry[4] = telem[i].max_temp;
        entry[5] = telem[i].auto_reheat;
        // timestamp as uint32 LE
        entry[6] = (uint8_t)(telem[i].timestamp);
        entry[7] = (uint8_t)(telem[i].timestamp >> 8);
        entry[8] = (uint8_t)(telem[i].timestamp >> 16);
        entry[9] = (uint8_t)(telem[i].timestamp >> 24);
        os_mbuf_append(ctxt->om, entry, sizeof(entry));
    }

    ESP_LOGI(TAG, "Telemetry buffer read: %d entries", count);
    return 0;
}

/// Buffer acknowledge — Write.
/// Any byte clears both event and telemetry buffers.
static int on_ack_access(uint16_t conn, uint16_t attr,
                         struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    if (!owner_auth_gate_ok())
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

    event_buffer_clear();
    ESP_LOGI(TAG, "Buffer acknowledge — buffers cleared");
    return 0;
}

/// Max-on timer — Read, Write (encrypted).
/// Format: uint16 LE — minutes (0 = disabled).
static int on_maxon_access(uint16_t conn, uint16_t attr,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint16_t val = device_state_get_max_on_minutes();
        uint8_t buf[2] = { (uint8_t)val, (uint8_t)(val >> 8) };
        os_mbuf_append(ctxt->om, buf, sizeof(buf));
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (!owner_auth_gate_ok())
            return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;

        if (OS_MBUF_PKTLEN(ctxt->om) != 2)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

        uint8_t buf[2];
        os_mbuf_copydata(ctxt->om, 0, sizeof(buf), buf);
        uint16_t minutes = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);

        device_state_set_max_on_minutes(minutes);
        // Persist the CLAMPED value so NVS and live state never diverge.
        nvs_store_save_max_on_minutes(device_state_get_max_on_minutes());
        firebase_rtdb_request_settings_push();

        ESP_LOGI(TAG, "Max-on timer → %u min (%s)",
                 minutes, minutes == 0 ? "disabled" : "enabled");
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/// Owner auth — Read (challenge), Write (response), encrypted.
/// Read:  16-byte random nonce, regenerated on every read.
/// Write: 32-byte HMAC-SHA256(ownerKey, nonce). A correct response
/// marks this connection owner-unlocked until disconnect; a wrong one
/// is rejected and consumes the nonce (re-read to retry).
static int on_oauth_access(uint16_t conn, uint16_t attr,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t nonce[OWNER_NONCE_LEN];
        owner_auth_get_nonce(nonce);
        os_mbuf_append(ctxt->om, nonce, sizeof(nonce));
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len != OWNER_HMAC_LEN)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

        uint8_t resp[OWNER_HMAC_LEN];
        os_mbuf_copydata(ctxt->om, 0, sizeof(resp), resp);

        if (!owner_auth_try_unlock(resp, sizeof(resp)))
            return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/// Run status — Read (encrypted), Write (encrypted, owner-gated).
///
/// Read format (7 bytes, little-endian):
///   uint32 elapsed_on_seconds   0 when the geyser is off
///   uint16 max_on_minutes       echo, so the app can show "time left"
///                               from a single read (0 = no limit set)
///   uint8  flags                bit0 interval-mode active
///                               bit1 clock usable
///                               bit2 sensor ok
///                               bit3 interval fallback enabled
///
/// Write format (1 byte): 0x00 disables the clock-less interval
/// fallback, 0x01 enables it (persisted).
///
/// Deliberately BLE-only: the app needs it while connected, and the
/// interval-mode flag is only ever set when the device has no usable
/// clock — which in practice means no WiFi and therefore no cloud
/// anyway.  Keeping it off the RTDB `live` node avoids coupling this
/// firmware to a security-rules deploy.  Encrypted like 0x0D — it
/// echoes the same setting plus occupancy-adjacent run data.
static int on_runst_access(uint16_t conn, uint16_t attr,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (!owner_auth_gate_ok())
            return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
        uint8_t val;
        if (OS_MBUF_PKTLEN(ctxt->om) != 1)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        os_mbuf_copydata(ctxt->om, 0, sizeof(val), &val);

        bool enabled = (val == 0x01);
        device_state_set_fallback_enabled(enabled);
        nvs_store_save_fallback_enabled(enabled);
        ESP_LOGI(TAG, "Interval fallback %s", enabled ? "enabled" : "disabled");
        return 0;
    }

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    uint32_t elapsed = 0;
    if (device_state_get_relay()) {
        int s = temperature_current_on_seconds();
        if (s > 0) elapsed = (uint32_t)s;
    }
    uint16_t max_on = device_state_get_max_on_minutes();

    uint8_t flags = 0;
    if (device_state_get_fallback_active())  flags |= 0x01;
    if (time_sync_is_valid())                flags |= 0x02;
    if (device_state_get_sensor_ok())        flags |= 0x04;
    if (device_state_get_fallback_enabled()) flags |= 0x08;

    uint8_t buf[7] = {
        (uint8_t)(elapsed),       (uint8_t)(elapsed >> 8),
        (uint8_t)(elapsed >> 16), (uint8_t)(elapsed >> 24),
        (uint8_t)(max_on),        (uint8_t)(max_on >> 8),
        flags,
    };
    os_mbuf_append(ctxt->om, buf, sizeof(buf));
    return 0;
}

/// Stored device ID — Read only, unencrypted.
/// Format: UTF-8 string (e.g. "a3f9b21c").
/// Returns the RTDB device ID stored in NVS during provisioning.
/// Unencrypted so it can be read before pairing (iOS re-pair scenario).
static int on_devid_access(uint16_t conn, uint16_t attr,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    const char *id = firebase_auth_get_device_id();
    if (id && id[0] != '\0') {
        os_mbuf_append(ctxt->om, id, strlen(id));
    }
    return 0;
}

// ── Owner-lock summary ───────────────────────────────────────────
//
// Writes that control the geyser or change provisioning require the
// connection to be owner-unlocked once the device is provisioned
// (owner_auth.c). Unlock = HMAC-SHA256(ownerKey, nonce) on 0x0E.
//
//   Gated writes:  0x03 relay, 0x04 limits, 0x05 timers, 0x08 time,
//                  0x0B ack, 0x0D max-on, 0x11 wifi (+SSID read),
//                  0x12 bind, 0x14 nickname, 0x15 auth, 0x16 key.
//   Open (unauth): 0x02 temp, 0x03 read, 0x06 info, 0x09 events,
//                  0x0A telem buffer, 0x0C device id, 0x13 status.
//
// The owner key is set once at provisioning (0x16). While the device
// is unprovisioned everything is open so the first owner can claim it.

// ── GATT service table ───────────────────────────────────────────

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gs_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {   // 0x02 — Temperature
                .uuid       = &uuid_temp.u,
                .access_cb  = on_temp_access,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &h_temp,
            },
            {   // 0x03 — Geyser state (encrypted: controls relay)
                .uuid       = &uuid_state.u,
                .access_cb  = on_state_access,
                .flags      = BLE_GATT_CHR_F_READ_ENC
                            | BLE_GATT_CHR_F_WRITE_ENC
                            | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &h_state,
            },
            {   // 0x04 — Temperature limits + auto-reheat (encrypted)
                .uuid       = &uuid_limits.u,
                .access_cb  = on_limits_access,
                .flags      = BLE_GATT_CHR_F_READ_ENC
                            | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {   // 0x05 — Timer configuration (encrypted)
                .uuid       = &uuid_timers.u,
                .access_cb  = on_timers_access,
                .flags      = BLE_GATT_CHR_F_READ_ENC
                            | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {   // 0x06 — Device info
                .uuid       = &uuid_info.u,
                .access_cb  = on_info_access,
                .flags      = BLE_GATT_CHR_F_READ,
            },
            {   // 0x08 — Time sync (phone → ESP, encrypted)
                .uuid       = &uuid_tsync.u,
                .access_cb  = on_time_sync,
                .flags      = BLE_GATT_CHR_F_WRITE_ENC,
            },
            {   // 0x09 — Device Events (buffered + real-time notify)
                .uuid       = &uuid_events.u,
                .access_cb  = on_events_access,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &h_events,
            },
            {   // 0x0A — Telemetry Buffer (read missed snapshots)
                .uuid       = &uuid_tbuf.u,
                .access_cb  = on_tbuf_access,
                .flags      = BLE_GATT_CHR_F_READ,
            },
            {   // 0x0B — Buffer Acknowledge (clear both buffers, encrypted)
                .uuid       = &uuid_ack.u,
                .access_cb  = on_ack_access,
                .flags      = BLE_GATT_CHR_F_WRITE_ENC,
            },
            {   // 0x0C — Stored Device ID (unencrypted, for iOS re-pair)
                .uuid       = &uuid_devid.u,
                .access_cb  = on_devid_access,
                .flags      = BLE_GATT_CHR_F_READ,
            },
            {   // 0x0D — Max continuous run timer (encrypted)
                .uuid       = &uuid_maxon.u,
                .access_cb  = on_maxon_access,
                .flags      = BLE_GATT_CHR_F_READ_ENC
                            | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {   // 0x0E — Owner auth: nonce read / HMAC unlock (encrypted)
                .uuid       = &uuid_oauth.u,
                .access_cb  = on_oauth_access,
                .flags      = BLE_GATT_CHR_F_READ_ENC
                            | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {   // 0x0F — Run status + interval-fallback opt-out (encrypted)
                .uuid       = &uuid_runst.u,
                .access_cb  = on_runst_access,
                .flags      = BLE_GATT_CHR_F_READ_ENC
                            | BLE_GATT_CHR_F_WRITE_ENC,
            },
            { 0 }, // sentinel
        },
    },
    { 0 }, // sentinel
};

// ── Public API ───────────────────────────────────────────────────

void gatt_server_init(void) {
    int rc;
    rc = ble_gatts_count_cfg(gatt_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(gatt_svcs);
    assert(rc == 0);
}

void gatt_server_notify_temperature(float temp_c) {
    uint16_t conn = ble_get_conn_handle();
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;

    int16_t raw = (int16_t)(temp_c * 100.0f);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&raw, sizeof(raw));
    if (om) {
        ble_gatts_notify_custom(conn, h_temp, om);
    }
}

void gatt_server_notify_state(bool on) {
    uint16_t conn = ble_get_conn_handle();
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;

    uint8_t val = on ? 0x01 : 0x00;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&val, sizeof(val));
    if (om) {
        ble_gatts_notify_custom(conn, h_state, om);
    }
}

void gatt_server_notify_event(uint8_t type, uint8_t temp) {
    uint16_t conn = ble_get_conn_handle();
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;

    uint8_t buf[2] = { type, temp };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, sizeof(buf));
    if (om) {
        ble_gatts_notify_custom(conn, h_events, om);
    }
}
