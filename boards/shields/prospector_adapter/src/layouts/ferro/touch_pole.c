#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "ferro_blobs.h"

#ifdef CONFIG_PROSPECTOR_TOUCH_BRIGHTNESS
#include <brightness.h>
#include <touch_brightness_drag.h>
#include <touch_brightness_gate.h>

#include "brightness_readout.h"
#endif

#define TOUCH_NODE DT_NODELABEL(cst816s)


/* The touch chip reports in panel coordinates, which are portrait. The image is rotated 270, so
 * touch X is screen Y and vice versa; the 180 build additionally reverses both axes. */
#define PANEL_NODE DT_CHOSEN(zephyr_display)
#define PANEL_TRACKED_MAX (DT_PROP(PANEL_NODE, width) - 1)   // The screen's vertical axis
#define PANEL_ACROSS_MAX (DT_PROP(PANEL_NODE, height) - 1)   // ...and its horizontal one
#define SCREEN_XMAX PANEL_ACROSS_MAX

#define EDGE_BAND 20
#define EDGE_TIMEOUT_MS 2000

static struct {
    int16_t tracked;
    int16_t across;
    bool pressed;
} pending;

#ifdef CONFIG_PROSPECTOR_TOUCH_BRIGHTNESS
static const struct prospector_touch_brightness_drag_settings settings =
    PROSPECTOR_TOUCH_BRIGHTNESS_DRAG_SETTINGS_DEFAULT;
static const struct prospector_touch_brightness_gate_settings gate_settings = {
    .xmax = SCREEN_XMAX,
    .band = EDGE_BAND,
};

/* Both touched from the input thread only. */
static struct prospector_touch_brightness_drag_state drag;
static struct prospector_touch_brightness_gate_state gate;
/* The timeout runs on the system work queue, so it leaves a flag for the input thread to act on
 * rather than reaching into the gate. */
static atomic_t timed_out;

static void edge_timeout(struct k_work *work) {
    ARG_UNUSED(work);
    atomic_set(&timed_out, 1);
    zmk_widget_brightness_readout_hide();
}
static K_WORK_DELAYABLE_DEFINE(edge_timeout_work, edge_timeout);

static void edge_report(int16_t x, bool pressed) {
    if (pressed && !gate.was_pressed) {
        /* Synchronous: a timeout already running must finish before this press is judged. */
        struct k_work_sync sync;
        k_work_cancel_delayable_sync(&edge_timeout_work, &sync);
    }
    if (atomic_clear(&timed_out)) {
        prospector_touch_brightness_gate_disarm(&gate);
    }

    const struct prospector_touch_brightness_drag_report report = {
        .tracked = pending.tracked, .across = pending.across, .pressed = pressed};

    switch (prospector_touch_brightness_gate_update(&gate, x, pressed, gate_settings)) {
    case PROSPECTOR_TOUCH_BRIGHTNESS_GATE_ARMED:
        prospector_touch_brightness_drag_reset(&drag, prospector_brightness_get());
        zmk_widget_brightness_readout_show(prospector_brightness_get());
        break;
    case PROSPECTOR_TOUCH_BRIGHTNESS_GATE_IDLE:
        prospector_touch_brightness_drag_update(&drag, report, settings);
        k_work_reschedule(&edge_timeout_work, K_MSEC(EDGE_TIMEOUT_MS));
        break;
    case PROSPECTOR_TOUCH_BRIGHTNESS_GATE_DRAG: {
        const struct prospector_touch_brightness_drag_result result =
            prospector_touch_brightness_drag_update(&drag, report, settings);
        if (result.changed) {
            prospector_brightness_set(result.level);
            zmk_widget_brightness_readout_show(result.level);
        }
        break;
    }
    case PROSPECTOR_TOUCH_BRIGHTNESS_GATE_NONE:
        break;
    }
}
#endif

static void touch_report(struct input_event *evt, void *user_data) {
    ARG_UNUSED(user_data);

    /* See docs/touch-chip.md, INPUT_EV_DEVICE. */
    if (evt->type == INPUT_EV_DEVICE) {
        return;
    }

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

    if (IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_FIELD_POLE)) {
        zmk_widget_ferro_blobs_set_contact(x, y, pending.pressed);
    }
#ifdef CONFIG_PROSPECTOR_TOUCH_BRIGHTNESS
    edge_report(x, pending.pressed);
#endif
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(TOUCH_NODE), touch_report, NULL);
