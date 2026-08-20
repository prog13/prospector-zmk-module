#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

struct zmk_widget_modifier_indicator {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *mod_containers[4];
    lv_obj_t *mod_labels[4];
};

void zmk_widget_modifier_indicator_reskin(void);

int zmk_widget_modifier_indicator_init(struct zmk_widget_modifier_indicator *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_modifier_indicator_obj(struct zmk_widget_modifier_indicator *widget);
