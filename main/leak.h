#pragma once

#include "driver/gpio.h"

/// Initialise the water-leak sensor GPIO (Leak_1). Non-fatal if unwired.
void leak_init(gpio_num_t pin);

/// FreeRTOS task: polls the leak probes (debounced). On a confirmed leak
/// it locks power OFF (the device_state leak-lockout) and fires EVT_LEAK;
/// on a confirmed-dry probe it releases the lockout and fires
/// EVT_LEAK_CLEAR. The lockout hard-blocks any power-on while water is
/// present. Pass NULL as param; never returns.
void leak_task(void *param);
