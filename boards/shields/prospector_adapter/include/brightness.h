#pragma once

#include <stdint.h>

/* This header is compiled without Zephyr by tests/run.sh. */

#define PROSPECTOR_BRIGHTNESS_MIN 1
#define PROSPECTOR_BRIGHTNESS_MAX 100

#ifndef CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR

/* level is a percentage, clamped to PROSPECTOR_BRIGHTNESS_MIN..MAX. */
void prospector_brightness_set(int level);

uint8_t prospector_brightness_get(void);

#endif
