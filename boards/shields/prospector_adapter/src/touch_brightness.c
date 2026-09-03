#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/util.h>

#include <brightness.h>
#include <touch_brightness_drag.h>

#ifdef CONFIG_PROSPECTOR_TOUCH_DEBUG
#include <touch_debug.h>
#endif

#define TOUCH_NODE DT_NODELABEL(cst816s)

#define TOUCH_BRIGHTNESS_INIT_PRIORITY 99

static const struct prospector_touch_brightness_drag_settings settings =
    PROSPECTOR_TOUCH_BRIGHTNESS_DRAG_SETTINGS_DEFAULT;

static struct prospector_touch_brightness_drag_state state;
static struct prospector_touch_brightness_drag_report pending;
static volatile bool ready;

static void touch_report(struct input_event *evt, void *user_data) {
    ARG_UNUSED(user_data);

    if (evt->type == INPUT_EV_ABS) {
        switch (evt->code) {
        case INPUT_ABS_X:
            /* Panel X is the screen's vertical axis. */
            pending.tracked = evt->value;
            break;
        case INPUT_ABS_Y:
            pending.across = evt->value;
            break;
        }
    } else if (evt->type == INPUT_EV_KEY && evt->code == INPUT_BTN_TOUCH) {
        pending.pressed = evt->value != 0;
    }

    if (!evt->sync) {
        return;
    }

    /* The touch driver starts reporting in POST_KERNEL, before touch_brightness_init has run. */
    if (!ready) {
        return;
    }

    struct prospector_touch_brightness_drag_result result =
        prospector_touch_brightness_drag_update(&state, pending, settings);

#ifdef CONFIG_PROSPECTOR_TOUCH_DEBUG
    prospector_touch_debug_record(&state, pending);
#endif

    if (result.changed) {
        prospector_brightness_set(result.level);
    }
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(TOUCH_NODE), touch_report, NULL);

static int touch_brightness_init(void) {
    prospector_touch_brightness_drag_reset(&state, prospector_brightness_get());
    /* The input callback runs on another thread; it must not see ready before state is written. */
    barrier_dmem_fence_full();
    ready = true;

    return 0;
}

BUILD_ASSERT(CONFIG_APPLICATION_INIT_PRIORITY < TOUCH_BRIGHTNESS_INIT_PRIORITY,
             "Brightness would initialise before brightness.c has set the level.");
SYS_INIT(touch_brightness_init, APPLICATION, TOUCH_BRIGHTNESS_INIT_PRIORITY);
