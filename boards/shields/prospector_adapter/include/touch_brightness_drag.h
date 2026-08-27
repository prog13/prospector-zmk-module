#pragma once

#include <stdbool.h>

#include <brightness.h>

/* This header is compiled without Zephyr by tests/run.sh. */

struct prospector_touch_brightness_drag_report {
    int tracked;
    int across;
    bool pressed;
};

struct prospector_touch_brightness_drag_settings {
    /* Finger travel, in panel counts, for the full brightness range. Must be non-zero: it is a
     * divisor. */
    int travel;
    bool inverted;
};

struct prospector_touch_brightness_drag_state {
    int start_tracked;
    int start_level;
    /* Survives a release; the next drag starts from it. */
    int last_level;
    bool touching;
};

struct prospector_touch_brightness_drag_result {
    bool changed;
    /* Valid only when changed is true. */
    int level;
};

/* level is the current brightness; the first drag starts from it. */
void prospector_touch_brightness_drag_reset(struct prospector_touch_brightness_drag_state *state,
                                            int level);

/* Feed every report in. The position in a release report is ignored. */
struct prospector_touch_brightness_drag_result
prospector_touch_brightness_drag_update(struct prospector_touch_brightness_drag_state *state,
                                        struct prospector_touch_brightness_drag_report report,
                                        struct prospector_touch_brightness_drag_settings settings);
