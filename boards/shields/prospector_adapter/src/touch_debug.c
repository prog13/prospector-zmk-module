#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <touch_debug.h>

#ifdef CONFIG_PROSPECTOR_TOUCH_BRIGHTNESS
#define SHOW_LEVEL
#include <brightness.h>
#endif

#define TOUCH_NODE DT_NODELABEL(cst816s)

#define READOUT_WIDTH 240
#define READOUT_PERIOD_MS 100

#define READOUT_FONT lv_font_montserrat_14
#define ROW_GAP 4

#define BAR_WIDTH 232
#define BAR_HEIGHT 6

struct readout_snapshot {
    int tracked;
    int across;
    bool touching;
#ifdef CONFIG_PROSPECTOR_TOUCH_BRIGHTNESS
    int start_tracked;
    int moved;
#endif
};

static struct readout_snapshot shared;
static struct k_spinlock lock;

static struct {
    int tracked;
    int across;
    bool pressed;
} pending;

static lv_obj_t *label_position;
static lv_obj_t *label_state;

#ifdef SHOW_LEVEL
static lv_obj_t *label_level;
static lv_obj_t *bar_fill;
#endif

#ifdef CONFIG_PROSPECTOR_TOUCH_BRIGHTNESS
static lv_obj_t *label_moved;

void prospector_touch_debug_record(const struct prospector_touch_brightness_drag_state *state,
                                   struct prospector_touch_brightness_drag_report report) {
    k_spinlock_key_t key = k_spin_lock(&lock);

    if (report.pressed) {
        shared.start_tracked = state->start_tracked;
        shared.moved = report.tracked - state->start_tracked;
    } else {
        shared.start_tracked = 0;
        shared.moved = 0;
    }

    k_spin_unlock(&lock, key);
}
#endif

static void touch_debug_report(struct input_event *evt, void *user_data) {
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

    k_spinlock_key_t key = k_spin_lock(&lock);

    shared.touching = pending.pressed;
    shared.tracked = pending.pressed ? pending.tracked : 0;
    shared.across = pending.pressed ? pending.across : 0;

    k_spin_unlock(&lock, key);
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(TOUCH_NODE), touch_debug_report, NULL);

static void readout_update(lv_timer_t *timer) {
    ARG_UNUSED(timer);

    k_spinlock_key_t key = k_spin_lock(&lock);
    struct readout_snapshot now = shared;
    k_spin_unlock(&lock, key);

    lv_label_set_text_fmt(label_position, "X %3d  Y %3d", now.tracked, now.across);
    lv_label_set_text(label_state, now.touching ? "TOUCH" : "IDLE");

#ifdef CONFIG_PROSPECTOR_TOUCH_BRIGHTNESS
    lv_label_set_text_fmt(label_moved, "from %3d  moved %+4d", now.start_tracked, now.moved);
#endif

#ifdef SHOW_LEVEL
    int level = prospector_brightness_get();

    lv_label_set_text_fmt(label_level, "BL %3d", level);
    lv_obj_set_width(bar_fill, level * BAR_WIDTH / PROSPECTOR_BRIGHTNESS_MAX);
#endif
}

void prospector_touch_debug_init(lv_obj_t *parent) {
    lv_obj_t *strip = lv_obj_create(parent);

    lv_obj_set_width(strip, READOUT_WIDTH);
    lv_obj_set_height(strip, LV_SIZE_CONTENT);
    lv_obj_align(strip, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(strip, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(strip, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(strip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(strip, 2, LV_PART_MAIN);
    lv_obj_set_style_text_color(strip, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(strip, &READOUT_FONT, LV_PART_MAIN);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    /* LVGL objects are clickable by default; this strip would otherwise eat touches over the
     * whole band it covers. */
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_CLICKABLE);

    int32_t row = lv_font_get_line_height(&READOUT_FONT) + ROW_GAP;
    int32_t y = 0;
    int32_t y_last = 0;

    label_position = lv_label_create(strip);
    lv_obj_align(label_position, LV_ALIGN_TOP_LEFT, 0, y);

    label_state = lv_label_create(strip);
    lv_obj_align(label_state, LV_ALIGN_TOP_RIGHT, 0, y);

    y_last = y;
    y += row;

#ifdef CONFIG_PROSPECTOR_TOUCH_BRIGHTNESS
    label_moved = lv_label_create(strip);
    lv_obj_align(label_moved, LV_ALIGN_TOP_LEFT, 0, y);

    y_last = y;
    y += row;
#endif

#ifdef SHOW_LEVEL
    label_level = lv_label_create(strip);
    lv_obj_align(label_level, LV_ALIGN_TOP_RIGHT, 0, y_last);

    lv_obj_t *bar_track = lv_obj_create(strip);

    lv_obj_set_size(bar_track, BAR_WIDTH, BAR_HEIGHT);
    lv_obj_align(bar_track, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(bar_track, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_track, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_track, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar_track, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bar_track, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    bar_fill = lv_obj_create(bar_track);

    lv_obj_set_size(bar_fill, 0, BAR_HEIGHT);
    lv_obj_align(bar_fill, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar_fill, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_fill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_fill, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bar_fill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
#endif

    readout_update(NULL);
    lv_timer_create(readout_update, READOUT_PERIOD_MS, NULL);
}
