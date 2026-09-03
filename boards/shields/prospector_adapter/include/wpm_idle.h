#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ZMK raises a WPM event only when the value changes, so idle counts from the last event, the drop
 * to 0 included. */

int prospector_wpm_current(void);

uint32_t prospector_wpm_idle_ms(uint32_t now);

/* Whether a nonzero WPM has ever arrived. */
bool prospector_wpm_seen(void);
