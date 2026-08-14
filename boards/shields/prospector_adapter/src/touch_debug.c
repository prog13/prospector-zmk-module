#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <touch_debug.h>

#define TOUCH_NODE DT_NODELABEL(cst816s)

#define READOUT_WIDTH 240
#define READOUT_PERIOD_MS 100

#define ROW_POSITION 0

struct readout_snapshot {
    int tracked;
    int across;
    bool touching;
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
    lv_obj_set_style_text_font(strip, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    label_position = lv_label_create(strip);
    lv_obj_align(label_position, LV_ALIGN_TOP_LEFT, 0, ROW_POSITION);

    label_state = lv_label_create(strip);
    lv_obj_align(label_state, LV_ALIGN_TOP_RIGHT, 0, ROW_POSITION);

    readout_update(NULL);
    lv_timer_create(readout_update, READOUT_PERIOD_MS, NULL);
}
