#pragma once

#include <stdbool.h>
#include "driver/gpio.h"

/// Configure the relay GPIO as output.
void relay_init(gpio_num_t pin);

/// Drive the relay.  true = closed (power on), false = open (power off).
void relay_set(bool on);
