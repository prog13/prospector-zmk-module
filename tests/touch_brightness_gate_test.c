/* Screen x runs 0..XMAX left to right; the edge band is the rightmost BAND columns. */

#include <stdbool.h>
#include <stdio.h>

#include <touch_brightness_gate.h>

#define XMAX 283
#define BAND 20

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

#define NONE PROSPECTOR_TOUCH_BRIGHTNESS_GATE_NONE
#define ARMED PROSPECTOR_TOUCH_BRIGHTNESS_GATE_ARMED
#define DRAG PROSPECTOR_TOUCH_BRIGHTNESS_GATE_DRAG
#define IDLE PROSPECTOR_TOUCH_BRIGHTNESS_GATE_IDLE

struct step {
    int x;
    bool pressed;
    bool disarm; /* The idle timeout fired before this report */
};

#define PRESS(x)                                                                                   \
    { (x), true, false }
#define RELEASE(x)                                                                                 \
    { (x), false, false }
#define TIMEOUT_THEN_PRESS(x)                                                                      \
    { (x), true, true }

static const char *const names[] = {"none", "armed", "drag", "idle"};

static int failures;

static void run(const char *name, const struct step *steps, int count,
                const enum prospector_touch_brightness_gate_event *want) {
    struct prospector_touch_brightness_gate_state state;
    const struct prospector_touch_brightness_gate_settings settings = {.xmax = XMAX, .band = BAND};
    enum prospector_touch_brightness_gate_event got[32];

    prospector_touch_brightness_gate_reset(&state);

    bool ok = true;
    for (int i = 0; i < count; i++) {
        if (steps[i].disarm) {
            prospector_touch_brightness_gate_disarm(&state);
        }
        got[i] = prospector_touch_brightness_gate_update(&state, steps[i].x, steps[i].pressed,
                                                          settings);
        ok = ok && got[i] == want[i];
    }

    if (ok) {
        printf("ok   - %s\n", name);
        return;
    }

    failures++;
    printf("FAIL - %s\n       want [", name);
    for (int i = 0; i < count; i++) {
        printf("%s%s", i ? " " : "", names[want[i]]);
    }
    printf("]\n        got [");
    for (int i = 0; i < count; i++) {
        printf("%s%s", i ? " " : "", names[got[i]]);
    }
    printf("]\n");
}

#define EXPECT(name, steps, want)                                                                  \
    do {                                                                                           \
        _Static_assert(ARRAY_LEN(steps) == ARRAY_LEN(want), "one event per step");                 \
        run(name, steps, ARRAY_LEN(steps), want);                                                  \
    } while (0)

int main(void) {
    {
        static const struct step steps[] = {PRESS(100), PRESS(80), PRESS(60), RELEASE(60)};
        static const enum prospector_touch_brightness_gate_event want[] = {NONE, NONE, NONE, NONE};
        EXPECT("a contact away from the edge never arms", steps, want);
    }

    {
        static const struct step steps[] = {PRESS(280), PRESS(270), PRESS(260), PRESS(240)};
        static const enum prospector_touch_brightness_gate_event want[] = {NONE, NONE, ARMED, NONE};
        EXPECT("a swipe in from the edge arms once it has travelled a band inward", steps, want);
    }

    {
        static const struct step steps[] = {PRESS(280), PRESS(265), RELEASE(265), PRESS(200),
                                            PRESS(150)};
        static const enum prospector_touch_brightness_gate_event want[] = {NONE, NONE, NONE, NONE,
                                                                            NONE};
        EXPECT("a tap in the band that lifts short of a band's travel does not arm", steps, want);
    }

    {
        static const struct step steps[] = {PRESS(4600), PRESS(200), PRESS(100), RELEASE(100)};
        static const enum prospector_touch_brightness_gate_event want[] = {NONE, NONE, NONE, NONE};
        EXPECT("a glitched off-panel birth does not count as the edge", steps, want);
    }

    {
        static const struct step steps[] = {PRESS(5), PRESS(30), PRESS(60), RELEASE(60)};
        static const enum prospector_touch_brightness_gate_event want[] = {NONE, NONE, NONE, NONE};
        EXPECT("a swipe in from the left edge does not arm", steps, want);
    }

    {
        static const struct step steps[] = {PRESS(283), PRESS(250), PRESS(200), PRESS(100),
                                            RELEASE(100)};
        static const enum prospector_touch_brightness_gate_event want[] = {NONE, ARMED, NONE, NONE,
                                                                            IDLE};
        EXPECT("the arming stroke never drags, and lifting it idles", steps, want);
    }

    {
        static const struct step steps[] = {PRESS(283), PRESS(250), RELEASE(250), PRESS(50),
                                            PRESS(60),  PRESS(70),  RELEASE(70),  PRESS(200),
                                            RELEASE(200)};
        static const enum prospector_touch_brightness_gate_event want[] = {
            NONE, ARMED, IDLE, DRAG, DRAG, DRAG, IDLE, DRAG, IDLE};
        EXPECT("once armed, later contacts drag anywhere", steps, want);
    }

    {
        static const struct step steps[] = {PRESS(283), PRESS(250), RELEASE(250),
                                            TIMEOUT_THEN_PRESS(50), PRESS(60), RELEASE(60)};
        static const enum prospector_touch_brightness_gate_event want[] = {NONE, ARMED, IDLE,
                                                                            NONE, NONE,  NONE};
        EXPECT("the idle timeout disarms until the next swipe", steps, want);
    }

    {
        static const struct step steps[] = {PRESS(283), PRESS(250), RELEASE(250),
                                            TIMEOUT_THEN_PRESS(283), PRESS(250), RELEASE(250),
                                            PRESS(50), PRESS(60)};
        static const enum prospector_touch_brightness_gate_event want[] = {
            NONE, ARMED, IDLE, NONE, ARMED, IDLE, DRAG, DRAG};
        EXPECT("a fresh swipe re-arms after the timeout", steps, want);
    }

    if (failures) {
        printf("\n%d case%s failed\n", failures, failures == 1 ? "" : "s");
        return 1;
    }

    printf("\nall cases passed\n");

    return 0;
}
