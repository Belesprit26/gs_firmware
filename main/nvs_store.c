#include "nvs_store.h"

#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "nvs";
static const char *NVS_NS = "gs_config";

// ── Load ─────────────────────────────────────────────────────────

void nvs_store_load(device_state_t *state) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        // First boot — no stored config.  Struct defaults are fine.
        ESP_LOGI(TAG, "No saved config (first boot)");
        return;
    }

    uint8_t val;
    if (nvs_get_u8(h, "relay", &val) == ESP_OK) {
        state->relay_on = (val == 1);
    }
    nvs_get_u8(h, "temp_min", &state->temp_min);
    nvs_get_u8(h, "temp_max", &state->temp_max);

    // Auto-reheat setting.
    val = 0;
    if (nvs_get_u8(h, "auto_rh", &val) == ESP_OK) {
        state->auto_reheat = (val == 1);
    }

    // Timer configuration (new format: MAX_TIMERS × 3 bytes).
    uint8_t blob[MAX_TIMERS * 3];
    size_t len = sizeof(blob);
    if (nvs_get_blob(h, "timers2", blob, &len) == ESP_OK &&
        len == sizeof(blob)) {
        for (int i = 0; i < MAX_TIMERS; i++) {
            state->timers[i].enabled = (blob[i * 3] == 1);
            state->timers[i].hour    = blob[i * 3 + 1];
            state->timers[i].minute  = blob[i * 3 + 2];
        }
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Config loaded (relay=%d, limits=[%d,%d], "
                  "auto_reheat=%d, timers loaded)",
             state->relay_on, state->temp_min, state->temp_max,
             state->auto_reheat);
}

// ── Save helpers ─────────────────────────────────────────────────

static bool open_write(nvs_handle_t *out_h) {
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, out_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void commit_and_close(nvs_handle_t h) {
    nvs_commit(h);
    nvs_close(h);
}

void nvs_store_save_relay(bool on) {
    nvs_handle_t h;
    if (!open_write(&h)) return;
    nvs_set_u8(h, "relay", on ? 1 : 0);
    commit_and_close(h);
}

void nvs_store_save_temp_limits(uint8_t min, uint8_t max) {
    nvs_handle_t h;
    if (!open_write(&h)) return;
    nvs_set_u8(h, "temp_min", min);
    nvs_set_u8(h, "temp_max", max);
    commit_and_close(h);
}

void nvs_store_save_auto_reheat(bool enabled) {
    nvs_handle_t h;
    if (!open_write(&h)) return;
    nvs_set_u8(h, "auto_rh", enabled ? 1 : 0);
    commit_and_close(h);
}

void nvs_store_save_timers(const gs_timer_t *timers) {
    uint8_t blob[MAX_TIMERS * 3];
    for (int i = 0; i < MAX_TIMERS; i++) {
        blob[i * 3]     = timers[i].enabled ? 1 : 0;
        blob[i * 3 + 1] = timers[i].hour;
        blob[i * 3 + 2] = timers[i].minute;
    }

    nvs_handle_t h;
    if (!open_write(&h)) return;
    nvs_set_blob(h, "timers2", blob, sizeof(blob));
    commit_and_close(h);
}
