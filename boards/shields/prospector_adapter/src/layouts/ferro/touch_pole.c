#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/util.h>

#include "ferro_blobs.h"

#define TOUCH_NODE DT_NODELABEL(cst816s)


/* The touch chip reports in panel coordinates, which are portrait. The image is rotated 270, so
 * touch X is screen Y and vice versa; the 180 build additionally reverses both axes. */
#define PANEL_NODE DT_CHOSEN(zephyr_display)
#define PANEL_TRACKED_MAX (DT_PROP(PANEL_NODE, width) - 1)   // The screen's vertical axis
#define PANEL_ACROSS_MAX (DT_PROP(PANEL_NODE, height) - 1)   // ...and its horizontal one

static struct {
    int16_t tracked;
    int16_t across;
    bool pressed;
} pending;

static void touch_report(struct input_event *evt, void *user_data) {
    ARG_UNUSED(user_data);

    if (evt->type == INPUT_EV_ABS) {
        switch (evt->code) {
        case INPUT_ABS_X:
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

    int16_t x, y;

    if (IS_ENABLED(CONFIG_PROSPECTOR_ROTATE_DISPLAY_180)) {
        x = PANEL_ACROSS_MAX - pending.across;
        y = pending.tracked;
    } else {
        x = pending.across;
        y = PANEL_TRACKED_MAX - pending.tracked;
    }

    zmk_widget_ferro_blobs_set_contact(x, y, pending.pressed);
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(TOUCH_NODE), touch_report, NULL);
