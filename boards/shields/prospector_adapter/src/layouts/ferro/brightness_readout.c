#include "brightness_readout.h"

#include <stdio.h>
#include <zmk/display.h>

#include <fonts.h>

#include "ferro_blobs.h"
#include "ferro_palette.h"

/* A fixed box: an auto-sized label re-centres on LVGL's next layout pass, a frame behind ferro. */
#define READOUT_W 160

static lv_obj_t *label;
/* The level to show, or 0 to hide. Written from the input thread, read on the display queue. */
static atomic_t requested;
static bool shown;

static void sync_handler(struct k_work *work) {
    ARG_UNUSED(work);
    const int level = atomic_get(&requested);
    if (label == NULL) {
        return;
    }
    if (level > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", level);
        lv_label_set_text(label, buf);
        zmk_widget_ferro_blobs_text_dirty();
    }
    if ((level > 0) != shown) {
        shown = level > 0;
        zmk_widget_ferro_blobs_fade(label, shown);
    }
}
static K_WORK_DEFINE(sync_work, sync_handler);

static void request(int level) {
    atomic_set(&requested, level);
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &sync_work);
    }
}

void zmk_widget_brightness_readout_show(int level) { request(level); }

void zmk_widget_brightness_readout_hide(void) { request(0); }

int zmk_widget_brightness_readout_init(struct zmk_widget_brightness_readout *widget,
                                       lv_obj_t *parent) {
    widget->obj = lv_label_create(parent);
    label = widget->obj;

    lv_obj_set_width(label, READOUT_W);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &FR_Regular_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(ferro_palette_active()->bright), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);

    /* Whatever was requested while the display was still coming up applies now. */
    k_work_submit_to_queue(zmk_display_work_q(), &sync_work);

    return 0;
}

lv_obj_t *zmk_widget_brightness_readout_obj(struct zmk_widget_brightness_readout *widget) {
    return widget->obj;
}

void zmk_widget_brightness_readout_reskin(struct zmk_widget_brightness_readout *widget) {
    lv_obj_set_style_text_color(widget->obj, lv_color_hex(ferro_palette_active()->bright),
                                LV_PART_MAIN);
}
