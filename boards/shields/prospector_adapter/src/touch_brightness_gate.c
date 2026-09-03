#include <touch_brightness_gate.h>

void prospector_touch_brightness_gate_reset(struct prospector_touch_brightness_gate_state *state) {
    *state = (struct prospector_touch_brightness_gate_state){0};
}

enum prospector_touch_brightness_gate_event
prospector_touch_brightness_gate_update(struct prospector_touch_brightness_gate_state *state, int x,
                                        bool pressed,
                                        struct prospector_touch_brightness_gate_settings settings) {
    if (pressed && !state->was_pressed) {
        state->born_x = x;
    }
    state->was_pressed = pressed;

    if (!state->armed) {
        const bool born_in_band =
            state->born_x > settings.xmax - settings.band && state->born_x <= settings.xmax;
        if (!pressed || !born_in_band || x > state->born_x - settings.band) {
            return PROSPECTOR_TOUCH_BRIGHTNESS_GATE_NONE;
        }
        state->armed = true;
        state->entry_stroke = true;
        return PROSPECTOR_TOUCH_BRIGHTNESS_GATE_ARMED;
    }

    if (!pressed) {
        state->entry_stroke = false;
        return PROSPECTOR_TOUCH_BRIGHTNESS_GATE_IDLE;
    }

    return state->entry_stroke ? PROSPECTOR_TOUCH_BRIGHTNESS_GATE_NONE
                               : PROSPECTOR_TOUCH_BRIGHTNESS_GATE_DRAG;
}

void prospector_touch_brightness_gate_disarm(struct prospector_touch_brightness_gate_state *state) {
    state->armed = false;
    state->entry_stroke = false;
}
