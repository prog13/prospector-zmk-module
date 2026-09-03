#pragma once

#include <stdbool.h>

/* This header is compiled without Zephyr by tests/run.sh. */

/* Gates the brightness drag behind a swipe in from the right screen edge; once armed, every contact
 * drags until the caller disarms. */

struct prospector_touch_brightness_gate_settings {
    int xmax; /* Rightmost on-panel screen x; anything beyond is a glitched report */
    int band; /* The edge band's width, and the inward travel that arms */
};

struct prospector_touch_brightness_gate_state {
    bool armed;
    bool was_pressed;
    bool entry_stroke; /* The arming contact is still down; it shows the level but never drags */
    int born_x;
};

enum prospector_touch_brightness_gate_event {
    PROSPECTOR_TOUCH_BRIGHTNESS_GATE_NONE,
    PROSPECTOR_TOUCH_BRIGHTNESS_GATE_ARMED, /* This report armed the gate: show the level */
    PROSPECTOR_TOUCH_BRIGHTNESS_GATE_DRAG,
    PROSPECTOR_TOUCH_BRIGHTNESS_GATE_IDLE,  /* Armed and lifted: feed the release, start the timeout */
};

void prospector_touch_brightness_gate_reset(struct prospector_touch_brightness_gate_state *state);

/* Feed every report in. x is the screen-horizontal coordinate. */
enum prospector_touch_brightness_gate_event
prospector_touch_brightness_gate_update(struct prospector_touch_brightness_gate_state *state, int x,
                                        bool pressed,
                                        struct prospector_touch_brightness_gate_settings settings);

void prospector_touch_brightness_gate_disarm(struct prospector_touch_brightness_gate_state *state);
