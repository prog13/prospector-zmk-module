#include <lvgl.h>

#include "layer_label.h"
#include "battery_label.h"
#include "ferro_blobs.h"
#include "modifier_indicator.h"
#include "output.h"
#include "ferro_palette.h"

#include <fonts.h>

static struct zmk_widget_layer_label layer_label_widget;
static struct zmk_widget_battery_label battery_label_widget;
static struct zmk_widget_ferro_blobs ferro_blobs_widget;
static struct zmk_widget_modifier_indicator modifier_indicator_widget;
static struct zmk_widget_output output_widget;
static lv_obj_t *screen_obj;

/* Re-applies the palette to the widgets. The blobs themselves need nothing: they read the palette
 * every frame. */
static void reskin(void) {
    lv_obj_set_style_bg_color(screen_obj, lv_color_hex(ferro_palette_active()->ground),
                              LV_PART_MAIN);
    zmk_widget_layer_label_reskin(&layer_label_widget);
    zmk_widget_battery_label_reskin(&battery_label_widget);
    zmk_widget_output_reskin();
    zmk_widget_modifier_indicator_reskin();
}

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);
    screen_obj = screen;
    lv_obj_set_style_bg_color(screen, lv_color_hex(ferro_palette_active()->ground),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, 255, LV_PART_MAIN);

    zmk_widget_ferro_blobs_init(&ferro_blobs_widget, screen);
    lv_obj_align(zmk_widget_ferro_blobs_obj(&ferro_blobs_widget), LV_ALIGN_CENTER, 0, 0);

    zmk_widget_layer_label_init(&layer_label_widget, screen);
    lv_obj_align(zmk_widget_layer_label_obj(&layer_label_widget), LV_ALIGN_TOP_LEFT, 9, 14);

    zmk_widget_output_init(&output_widget, screen);
    lv_obj_align(zmk_widget_output_obj(&output_widget), LV_ALIGN_TOP_LEFT, 4, 52);

    zmk_widget_battery_label_init(&battery_label_widget, screen);
    lv_obj_align(zmk_widget_battery_label_obj(&battery_label_widget), LV_ALIGN_BOTTOM_LEFT, 9, -20);

    zmk_widget_modifier_indicator_init(&modifier_indicator_widget, screen);

    ferro_palette_set_reskin_cb(reskin);

    zmk_widget_ferro_blobs_set_text(FERRO_TEXT_LAYER,
                                    zmk_widget_layer_label_obj(&layer_label_widget));
    zmk_widget_ferro_blobs_set_text(FERRO_TEXT_BATTERY,
                                    zmk_widget_battery_label_obj(&battery_label_widget));

    return screen;
}
