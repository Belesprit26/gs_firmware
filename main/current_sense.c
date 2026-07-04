// ── STAGED, NOT IN THE BUILD ─────────────────────────────────────
//
// This module is deliberately excluded from main/CMakeLists.txt.
// It targets the current-sensing hardware still under evaluation
// (HARDWARE_ROADMAP.md → "Current sensor evaluation"): element/relay
// failure detection via an SCT-013 CT clamp + SEN0211 module.
// Add it to SRCS (and call current_sense_init from app_main) once the
// CT hardware lands on the production BOM.

#include "current_sense.h"

#include <math.h>

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ct_sense";

// ── Sensor parameters ────────────────────────────────────────────
//
//  DFRobot SEN0211 module + SCT-013-020 CT probe:
//    CT output:       0–1 V AC for 0–20 A  (linear)
//    Module gain:     ~1× (bias at VCC/2, AC passthrough)
//    Module supply:   3.3 V → bias ≈ 1.65 V, output within ESP32 ADC range
//    Connection:      Gravity 3-pin PH2.0 → repurposed UART JST (J5)
//
//  Conversion:  I_rms = V_rms_at_ADC × AMPS_PER_VOLT
//  The CT outputs 1 V RMS at 20 A → 0.05 V/A → 20 A/V.
//  Adjust MODULE_GAIN if calibration against a clamp meter shows
//  consistent over/under-reading.

#define CT_AMPS_PER_VOLT 20.0f
#define MODULE_GAIN      1.0f

// ── ADC sampling ─────────────────────────────────────────────────
//
// 2 full 50 Hz cycles = 40 ms.
// ESP32-C6 oneshot reads take ~25 µs each, giving ~1600 samples
// per window — more than enough for accurate RMS.

#define MAINS_PERIOD_MS  20
#define SAMPLE_CYCLES    2
#define SAMPLE_WINDOW_MS (MAINS_PERIOD_MS * SAMPLE_CYCLES)
#define MAX_SAMPLES      2048

// ── Noise floor ──────────────────────────────────────────────────
// Readings below this threshold (in amps) are clamped to 0.
// Prevents ADC noise from reporting phantom ~0.05 A when idle.

#define NOISE_FLOOR_A    0.15f

// ── Module state ─────────────────────────────────────────────────

static adc_oneshot_unit_handle_t s_adc_handle  = NULL;
static adc_cali_handle_t        s_cali_handle  = NULL;
static adc_channel_t            s_channel;
static int                      s_bias_mv      = 1650;

// ── Helpers ──────────────────────────────────────────────────────

static int raw_to_mv(int raw)
{
    if (s_cali_handle) {
        int mv;
        adc_cali_raw_to_voltage(s_cali_handle, raw, &mv);
        return mv;
    }
    return raw * 3300 / 4095;
}

// ── Init ─────────────────────────────────────────────────────────

esp_err_t current_sense_init(adc_channel_t channel)
{
    s_channel = channel;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) return err;

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten   = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_adc_handle, s_channel, &chan_cfg);
    if (err != ESP_OK) return err;

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .chan     = s_channel,
        .atten   = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable — raw conversion used");
        s_cali_handle = NULL;
    }

    // Measure DC bias at rest (average 64 samples).
    int64_t sum = 0;
    for (int i = 0; i < 64; i++) {
        int raw;
        if (adc_oneshot_read(s_adc_handle, s_channel, &raw) == ESP_OK)
            sum += raw_to_mv(raw);
    }
    s_bias_mv = (int)(sum / 64);

    ESP_LOGI(TAG, "SEN0211 ready  ADC1_CH%d  bias=%d mV  "
             "scale=%.1f A/V  gain=%.1f",
             channel, s_bias_mv, CT_AMPS_PER_VOLT, MODULE_GAIN);
    return ESP_OK;
}

// ── RMS current read ─────────────────────────────────────────────

esp_err_t current_sense_read_rms(float *out_amps)
{
    if (!s_adc_handle) return ESP_ERR_INVALID_STATE;

    double sum_sq = 0.0;
    int    n      = 0;

    TickType_t deadline = xTaskGetTickCount()
                        + pdMS_TO_TICKS(SAMPLE_WINDOW_MS);

    while (xTaskGetTickCount() < deadline && n < MAX_SAMPLES) {
        int raw;
        if (adc_oneshot_read(s_adc_handle, s_channel, &raw) != ESP_OK)
            continue;

        float v_ac = (float)(raw_to_mv(raw) - s_bias_mv) / 1000.0f;
        sum_sq += (double)v_ac * v_ac;
        n++;
    }

    if (n < 20) {
        ESP_LOGW(TAG, "Too few samples (%d)", n);
        return ESP_ERR_INVALID_SIZE;
    }

    float v_rms = sqrtf((float)(sum_sq / n));
    float i_rms = v_rms / MODULE_GAIN * CT_AMPS_PER_VOLT;

    if (i_rms < NOISE_FLOOR_A)
        i_rms = 0.0f;

    *out_amps = i_rms;
    return ESP_OK;
}
