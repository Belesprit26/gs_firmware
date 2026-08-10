#pragma once

#include "driver/gpio.h"

/// Initialise the WS2812B status LED (D9) on the given GPIO (LED_1).
/// Non-fatal: if the RMT device can't be created the task simply runs
/// without lighting anything.
void led_init(gpio_num_t pin);

/// FreeRTOS task: drives the status LED. Poll-based (~30 ms tick).
/// Colour = connectivity (white setup · blue BLE · green cloud · amber
/// offline), breathe = relay ON; button press / factory-wipe feedback
/// overrides. Pass NULL as param; never returns.
void led_task(void *param);
