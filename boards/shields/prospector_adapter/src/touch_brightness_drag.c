#include <touch_brightness_drag.h>

static const struct prospector_touch_brightness_drag_result no_change = {.changed = false,
                                                                         .level = 0};

static int clamp_level(int level) {
    if (level < PROSPECTOR_BRIGHTNESS_MIN) {
        return PROSPECTOR_BRIGHTNESS_MIN;
    }
    if (level > PROSPECTOR_BRIGHTNESS_MAX) {
        return PROSPECTOR_BRIGHTNESS_MAX;
    }

    return level;
}

void prospector_touch_brightness_drag_reset(struct prospector_touch_brightness_drag_state *state,
                                            int level) {
    state->start_tracked = 0;
    state->start_level = level;
    state->last_level = level;
    state->touching = false;
}

struct prospector_touch_brightness_drag_result
prospector_touch_brightness_drag_update(struct prospector_touch_brightness_drag_state *state,
                                        struct prospector_touch_brightness_drag_report report,
                                        struct prospector_touch_brightness_drag_settings settings) {
    if (!report.pressed) {
        state->touching = false;
        return no_change;
    }

    if (!state->touching) {
        state->touching = true;
        state->start_tracked = report.tracked;
        state->start_level = state->last_level;
        return no_change;
    }

    int moved = report.tracked - state->start_tracked;
    int level = clamp_level(state->start_level +
                            (settings.inverted ? -moved : moved) * 100 / settings.travel);

    if (level == state->last_level) {
        return no_change;
    }
    state->last_level = level;

    struct prospector_touch_brightness_drag_result result = {.changed = true, .level = level};

    return result;
}
