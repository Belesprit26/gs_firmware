#pragma once

#include "esp_err.h"
#include "hal/adc_types.h"

/// Initialise the ADC channel for CT-based current sensing.
/// Measures the DC bias at rest — call before mains load is on,
/// or ensure no current is flowing during init.
esp_err_t current_sense_init(adc_channel_t channel);

/// Sample the ADC over 2 full 50 Hz mains cycles (40 ms) and
/// return the computed RMS primary current in amps.
esp_err_t current_sense_read_rms(float *out_amps);
