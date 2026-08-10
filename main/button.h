#pragma once

#include <stdint.h>
#include "driver/gpio.h"

/// Initialise the physical button on the given GPIO pin.
/// Configures as input with internal pull-up (active-low: pressed = 0).
void button_init(gpio_num_t pin);

/// FreeRTOS task: polls the button at 20 ms intervals.
///
/// Press behaviour:
///   - Short press  (50 ms – 5 s)  → toggle relay on/off
///   - Medium press (5 s – 10 s)   → ignored (dead zone)
///   - Long press   (≥ 10 s)       → factory reset (NVS wipe + reboot)
///
/// The factory reset fires the instant the 10 s threshold is reached
/// (you don't need to release the button).  The board reboots into
/// "GeyserSwitch-Setup" mode with all config cleared.
void button_task(void *param);

/// Press-duration thresholds. Public so the status LED can track them.
#define BUTTON_SHORT_MAX_MS   5000    // release ≤ this → relay toggle
#define BUTTON_RESET_HOLD_MS  10000   // hold ≥ this → factory reset

/// Milliseconds the button has been held right now, or 0 if released.
/// Single writer (button_task) / single reader (led_task) — no lock.
uint32_t button_held_ms(void);
