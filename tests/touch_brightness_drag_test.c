/* Expected levels are derived by hand from the travel default. */

#include <stdbool.h>
#include <stdio.h>

#include <touch_brightness_drag.h>

#ifndef CONFIG_PROSPECTOR_TOUCH_BRIGHTNESS_TRAVEL
#error "Define CONFIG_PROSPECTOR_TOUCH_BRIGHTNESS_TRAVEL from Kconfig; tests/run.sh does it."
#endif

#define DEFAULT_TRAVEL CONFIG_PROSPECTOR_TOUCH_BRIGHTNESS_TRAVEL

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define MAX_LEVELS   32

#define PRESS(t, o)                                                                                \
    { .tracked = (t), .across = (o), .pressed = true }
#define RELEASE(t, o)                                                                              \
    { .tracked = (t), .across = (o), .pressed = false }

static int failures;

static void run(const char *name, int start_level, int travel, bool inverted,
                const struct prospector_touch_brightness_drag_report *reports, int report_count,
                const int *want, int want_count) {
    struct prospector_touch_brightness_drag_state state;
    struct prospector_touch_brightness_drag_settings settings = {.travel = travel,
                                                                 .inverted = inverted};
    int got[MAX_LEVELS];
    int got_count = 0;

    prospector_touch_brightness_drag_reset(&state, start_level);

    for (int i = 0; i < report_count; i++) {
        struct prospector_touch_brightness_drag_result result =
            prospector_touch_brightness_drag_update(&state, reports[i], settings);

        /* Keep counting past MAX_LEVELS so a case that emits too many levels fails instead of
         * being silently truncated. */
        if (result.changed) {
            if (got_count < MAX_LEVELS) {
                got[got_count] = result.level;
            }
            got_count++;
        }
    }

    bool ok = got_count == want_count && want_count <= MAX_LEVELS;
    for (int i = 0; ok && i < want_count; i++) {
        ok = got[i] == want[i];
    }

    if (ok) {
        printf("ok   - %s\n", name);
        return;
    }

    failures++;
    printf("FAIL - %s\n       want [", name);
    for (int i = 0; i < want_count; i++) {
        printf("%s%d", i ? " " : "", want[i]);
    }
    printf("]\n        got [");
    for (int i = 0; i < got_count && i < MAX_LEVELS; i++) {
        printf("%s%d", i ? " " : "", got[i]);
    }
    printf("]\n");
}

#define EXPECT_LEVELS(name, start, travel, inverted, reports, want)                                \
    run(name, start, travel, inverted, reports, ARRAY_LEN(reports), want, ARRAY_LEN(want))

#define EXPECT_NO_CHANGE(name, start, travel, inverted, reports)                                   \
    run(name, start, travel, inverted, reports, ARRAY_LEN(reports), NULL, 0)

int main(void) {
    {
        static const struct prospector_touch_brightness_drag_report reports[] = {
            PRESS(100, 100),
            RELEASE(100, 100),
        };
        EXPECT_NO_CHANGE("a tap changes nothing", 50, 180, false, reports);
    }

    {
        /* The first press sets the origin and changes nothing. */
        static const struct prospector_touch_brightness_drag_report reports[] = {
            PRESS(100, 100),
            PRESS(118, 100),
            PRESS(136, 100),
            PRESS(154, 100),
        };
        static const int want[] = {60, 70, 80};
        EXPECT_LEVELS("an upward drag brightens", 50, 180, false, reports, want);

        static const int want_inverted[] = {40, 30, 20};
        EXPECT_LEVELS("inverted, the same drag dims", 50, 180, true, reports, want_inverted);
    }

    {
        static const struct prospector_touch_brightness_drag_report reports[] = {
            PRESS(100, 100),
            PRESS(100, 140),
            PRESS(100, 180),
            RELEASE(100, 180),
        };
        EXPECT_NO_CHANGE("a purely sideways drag changes nothing", 50, 180, false, reports);
    }

    {
        /* There is no explicit deadband; integer division to whole percent is what swallows small
         * movements. */
        static const struct prospector_touch_brightness_drag_report reports[] = {
            PRESS(100, 0),
            PRESS(102, 0),
            PRESS(104, 0),
            RELEASE(104, 0),
        };
        EXPECT_NO_CHANGE("movement under one level's worth changes nothing", 50, DEFAULT_TRAVEL,
                         false, reports);
    }

    {
        static const struct prospector_touch_brightness_drag_report reports[] = {
            PRESS(0, 0), PRESS(57, 0), PRESS(114, 0), PRESS(171, 0), PRESS(229, 0),
            RELEASE(229, 0),
            PRESS(0, 0), PRESS(57, 0), PRESS(114, 0), PRESS(171, 0), PRESS(229, 0),
        };
        static const int want[] = {14, 28, 41, 55, 68, 82, 95, PROSPECTOR_BRIGHTNESS_MAX};
        EXPECT_LEVELS("two swipes span the range at the default travel",
                      PROSPECTOR_BRIGHTNESS_MIN, DEFAULT_TRAVEL, false, reports, want);
    }

    {
        static const struct prospector_touch_brightness_drag_report reports[] = {
            PRESS(100, 100), PRESS(160, 100), PRESS(220, 100), PRESS(260, 100),
            PRESS(254, 100), PRESS(200, 100), PRESS(140, 100),
        };
        static const int want[] = {PROSPECTOR_BRIGHTNESS_MAX, 99};
        EXPECT_LEVELS("an overshoot holds at the end until the finger comes back", 90,
                      DEFAULT_TRAVEL, false, reports, want);
    }

    {
        /* Drags far enough that the unclamped level goes well below zero. */
        static const struct prospector_touch_brightness_drag_report reports[] = {
            PRESS(200, 100), PRESS(150, 100), PRESS(100, 100), PRESS(50, 100), PRESS(0, 100),
        };
        static const int want[] = {PROSPECTOR_BRIGHTNESS_MIN};
        EXPECT_LEVELS("dragging down stops at the minimum, not at zero", 20, 180, false, reports,
                      want);
    }

    {
        static const struct prospector_touch_brightness_drag_report reports[] = {
            PRESS(100, 100), PRESS(136, 100), PRESS(172, 100), RELEASE(0, 0),
            PRESS(50, 50),   PRESS(86, 50),   PRESS(122, 50),
        };
        static const int want[] = {30, 50, 70, 90};
        EXPECT_LEVELS("a second drag continues from the first, whatever the release reported", 10,
                      180, false, reports, want);
    }

    if (failures) {
        printf("\n%d case%s failed\n", failures, failures == 1 ? "" : "s");
        return 1;
    }

    printf("\nall cases passed\n");

    return 0;
}
