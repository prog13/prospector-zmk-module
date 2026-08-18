#include <lvgl.h>

#include "layer_label.h"
#include "battery_label.h"
#include "flux_lines.h"
#include "modifier_indicator.h"
#include "output.h"

#include <fonts.h>

static struct zmk_widget_layer_label layer_label_widget;
static struct zmk_widget_battery_label battery_label_widget;
static struct zmk_widget_flux_lines flux_lines_widget;
static struct zmk_widget_modifier_indicator modifier_indicator_widget;
static struct zmk_widget_output output_widget;

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, 255, LV_PART_MAIN);

    zmk_widget_flux_lines_init(&flux_lines_widget, screen);
    lv_obj_set_size(zmk_widget_flux_lines_obj(&flux_lines_widget), 274, 206);
    lv_obj_align(zmk_widget_flux_lines_obj(&flux_lines_widget), LV_ALIGN_CENTER, 0, 0);

    zmk_widget_layer_label_init(&layer_label_widget, screen);
    lv_obj_align(zmk_widget_layer_label_obj(&layer_label_widget), LV_ALIGN_TOP_LEFT, 9, 14);

    zmk_widget_output_init(&output_widget, screen);
    lv_obj_align(zmk_widget_output_obj(&output_widget), LV_ALIGN_TOP_LEFT, 4, 52);

    zmk_widget_battery_label_init(&battery_label_widget, screen);
    lv_obj_align(zmk_widget_battery_label_obj(&battery_label_widget), LV_ALIGN_BOTTOM_LEFT, 9, -20);

    zmk_widget_modifier_indicator_init(&modifier_indicator_widget, screen);

    zmk_widget_flux_lines_set_labels(&flux_lines_widget,
                                        zmk_widget_layer_label_obj(&layer_label_widget),
                                        zmk_widget_battery_label_obj(&battery_label_widget));

    return screen;
}
