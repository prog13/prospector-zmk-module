#pragma once

#include <lvgl.h>

#ifdef CONFIG_PROSPECTOR_TOUCH_BRIGHTNESS
#include <touch_brightness_drag.h>

void prospector_touch_debug_record(const struct prospector_touch_brightness_drag_state *state,
                                   struct prospector_touch_brightness_drag_report report);
#endif

void prospector_touch_debug_init(lv_obj_t *parent);
