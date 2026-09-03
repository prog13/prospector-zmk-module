#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

struct zmk_widget_brightness_readout {
    lv_obj_t *obj;
};

int zmk_widget_brightness_readout_init(struct zmk_widget_brightness_readout *widget,
                                       lv_obj_t *parent);
lv_obj_t *zmk_widget_brightness_readout_obj(struct zmk_widget_brightness_readout *widget);
void zmk_widget_brightness_readout_reskin(struct zmk_widget_brightness_readout *widget);

/* Fade the readout in and out. Safe from the input thread. */
void zmk_widget_brightness_readout_show(int level);
void zmk_widget_brightness_readout_hide(void);
