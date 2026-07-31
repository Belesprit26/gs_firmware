#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/// Initialise the DS18B20 sensor on the given OneWire data pin.
/// Returns ESP_OK on success, or an error if the sensor is not found.
/// The firmware continues to operate without temperature readings
/// if no sensor is attached — BLE and relay control still work.
esp_err_t temperature_init(gpio_num_t data_pin);

/// Read the current temperature in °C.
/// Returns ESP_ERR_INVALID_STATE if no sensor was found during init.
esp_err_t temperature_read(float *out_temp_c);

/// FreeRTOS task: reads the sensor every 10 s, updates device_state,
/// and pushes a BLE notification.  Pass NULL as the task parameter.
void temperature_task(void *param);

/// Seconds the geyser has been continuously ON.  Derived from the
/// persisted wall-clock stamp when the clock is usable (so the window
/// survives a reboot), else from a RAM tick counter.
int temperature_current_on_seconds(void);

/// Tick at which the max continuous run limit last switched the geyser
/// off.  Used by the scheduler to suppress an immediate re-fire when a
/// timer's time coincides with the cutoff.
TickType_t temperature_last_limit_cutoff(void);
