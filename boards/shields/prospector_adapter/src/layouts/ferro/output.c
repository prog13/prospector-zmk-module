#include "output.h"

#include "ferro_palette.h"
#include "ferro_blobs.h"

#include <zmk/display.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/event_manager.h>
#include <zmk/endpoints.h>
#include <zmk/ble.h>

#include <fonts.h>
#include <symbols.h>

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

#define PROFILE_DISPLAY_TIMEOUT K_SECONDS(3)

#define NUM_ACTIVE       (ferro_palette_active()->bright)
#define NUM_INACTIVE     (ferro_palette_active()->dim)
#define SYM_SENDING      (ferro_palette_active()->bright)
#define SYM_CONNECTED    (ferro_palette_active()->bright)
#define SYM_SEARCHING    (ferro_palette_active()->dim)
#define SYM_UNPAIRED     (ferro_palette_active()->dim)

static void profile_display_timeout_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(profile_display_timeout_work, profile_display_timeout_handler);
static void set_symbol_opa(void *obj, int32_t opa);
static void stop_breathing_anim(lv_obj_t *obj);
static void update_output_widget(struct zmk_widget_output *widget, uint8_t profile_index);

static uint8_t active_profile_index = 0;
static enum zmk_transport active_transport = ZMK_TRANSPORT_USB;
static bool output_visible = false;

static void set_output_visible(bool visible) {
    output_visible = visible;

    struct zmk_widget_output *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        zmk_widget_ferro_blobs_fade(widget->container, visible);
        /* Stop the breathing animation while hidden. It nudges ferro every LVGL tick and would
         * keep waking the renderer for a widget nobody can see. */
        if (visible) {
            update_output_widget(widget, active_profile_index);
        } else {
            stop_breathing_anim(widget->links_label);
        }
    }
}

static void profile_display_timeout_handler(struct k_work *work) {
    if (active_transport != ZMK_TRANSPORT_BLE) {
        set_output_visible(false);
    }
}

static void set_symbol_opa(void *obj, int32_t opa) {
    lv_obj_set_style_opa(obj, opa, LV_PART_MAIN);
    zmk_widget_ferro_blobs_text_dirty();
}

static void start_breathing_anim(lv_obj_t *obj) {
    lv_anim_delete(obj, NULL);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, 155, 255);
    lv_anim_set_duration(&anim, 500);
    lv_anim_set_exec_cb(&anim, set_symbol_opa);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_playback_duration(&anim, 500);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);
}

static void stop_breathing_anim(lv_obj_t *obj) {
    lv_anim_delete(obj, NULL);
    lv_obj_remove_local_style_prop(obj, LV_STYLE_OPA, LV_PART_MAIN);
}

static void update_output_widget(struct zmk_widget_output *widget, uint8_t profile_index) {
    char profile_text[4];
    snprintf(profile_text, sizeof(profile_text), "%d", profile_index);
    lv_label_set_text(widget->profile_label, profile_text);

    bool is_connected = zmk_ble_profile_is_connected(profile_index);
    bool is_open = zmk_ble_profile_is_open(profile_index);
    bool is_ble_active = (active_transport == ZMK_TRANSPORT_BLE);

    lv_label_set_text(widget->links_label, SYMBOL_WAVES_UP);
    stop_breathing_anim(widget->links_label);

    if (is_connected) {
        if (is_ble_active) {
            lv_obj_set_style_text_font(widget->links_label, &Symbols_Bold_26, LV_PART_MAIN);
            lv_obj_set_style_text_color(widget->links_label, lv_color_hex(SYM_SENDING), LV_PART_MAIN);
            lv_obj_set_style_text_color(widget->profile_label, lv_color_hex(NUM_ACTIVE), LV_PART_MAIN);
            lv_obj_set_style_translate_y(widget->links_label, 2, LV_PART_MAIN);
        } else {
            lv_obj_set_style_text_font(widget->links_label, &Symbols_Regular_28, LV_PART_MAIN);
            lv_obj_set_style_text_color(widget->links_label, lv_color_hex(SYM_CONNECTED), LV_PART_MAIN);
            lv_obj_set_style_text_color(widget->profile_label, lv_color_hex(NUM_INACTIVE), LV_PART_MAIN);
            lv_obj_set_style_translate_y(widget->links_label, 0, LV_PART_MAIN);
        }
    } else {
        lv_obj_set_style_text_font(widget->links_label, &Symbols_Regular_28, LV_PART_MAIN);
        lv_obj_set_style_translate_y(widget->links_label, 0, LV_PART_MAIN);
        lv_obj_set_style_text_color(widget->profile_label, lv_color_hex(NUM_INACTIVE), LV_PART_MAIN);
        if (is_open) {
            lv_obj_set_style_text_color(widget->links_label, lv_color_hex(SYM_UNPAIRED), LV_PART_MAIN);
        } else {
            lv_obj_set_style_text_color(widget->links_label, lv_color_hex(SYM_SEARCHING), LV_PART_MAIN);
            start_breathing_anim(widget->links_label);
        }
    }

    /* Ferro draws these labels, not LVGL, so lv_obj_invalidate would have no effect. */
    zmk_widget_ferro_blobs_text_dirty();
}

static int endpoint_changed_listener(const zmk_event_t *eh) {
    const struct zmk_endpoint_changed *event = as_zmk_endpoint_changed(eh);
    if (event) {
        struct zmk_endpoint_instance selected = zmk_endpoint_get_selected();
        active_transport = selected.transport;

        if (active_transport == ZMK_TRANSPORT_BLE) {
            k_work_cancel_delayable(&profile_display_timeout_work);
            if (!output_visible) {
                set_output_visible(true);
            }
        } else {
            if (!output_visible) {
                set_output_visible(true);
            }
            k_work_reschedule(&profile_display_timeout_work, PROFILE_DISPLAY_TIMEOUT);
        }

        struct zmk_widget_output *widget;
        SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
            update_output_widget(widget, active_profile_index);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int ble_active_profile_changed_listener(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *event = as_zmk_ble_active_profile_changed(eh);
    if (event) {
        active_profile_index = event->index;

        set_output_visible(true);
        k_work_reschedule(&profile_display_timeout_work, PROFILE_DISPLAY_TIMEOUT);

        struct zmk_widget_output *widget;
        SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
            update_output_widget(widget, active_profile_index);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(widget_output_endpoint, endpoint_changed_listener);
ZMK_SUBSCRIPTION(widget_output_endpoint, zmk_endpoint_changed);

ZMK_LISTENER(widget_output_profile, ble_active_profile_changed_listener);
ZMK_SUBSCRIPTION(widget_output_profile, zmk_ble_active_profile_changed);

int zmk_widget_output_init(struct zmk_widget_output *widget, lv_obj_t *parent) {
    widget->container = lv_obj_create(parent);
    lv_obj_set_size(widget->container, 68, 34);
    lv_obj_set_style_border_width(widget->container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(widget->container, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->container, LV_OPA_TRANSP, LV_PART_MAIN);

    widget->links_label = lv_label_create(widget->container);
    lv_obj_set_size(widget->links_label, 30, 30);
    lv_obj_set_style_text_font(widget->links_label, &Symbols_Regular_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(widget->links_label, lv_color_hex(NUM_INACTIVE), LV_PART_MAIN);
    lv_obj_set_style_text_align(widget->links_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(widget->links_label, 8, 3);

    widget->profile_label = lv_label_create(widget->container);
    lv_obj_set_style_text_font(widget->profile_label, &FG_Medium_26, LV_PART_MAIN);
    lv_obj_set_style_text_color(widget->profile_label, lv_color_hex(NUM_INACTIVE), LV_PART_MAIN);
    lv_obj_align(widget->profile_label, LV_ALIGN_RIGHT_MID, -5, 4);

    if (sys_slist_is_empty(&widgets)) {
        active_profile_index = zmk_ble_active_profile_index();
        struct zmk_endpoint_instance selected = zmk_endpoint_get_selected();
        active_transport = selected.transport;

        if (active_transport != ZMK_TRANSPORT_BLE) {
            lv_obj_set_style_opa(widget->container, 0, LV_PART_MAIN);
        } else {
            output_visible = true;
        }
    }

    zmk_widget_ferro_blobs_set_text(FERRO_TEXT_OUT_LINK, widget->links_label);
    zmk_widget_ferro_blobs_set_text(FERRO_TEXT_OUT_PROFILE, widget->profile_label);

    update_output_widget(widget, active_profile_index);

    sys_slist_append(&widgets, &widget->node);

    return 0;
}

lv_obj_t *zmk_widget_output_obj(struct zmk_widget_output *widget) {
    return widget->container;
}

void zmk_widget_output_reskin(void) {
    struct zmk_widget_output *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        lv_obj_set_style_bg_color(widget->container, lv_color_hex(ferro_palette_active()->ground),
                                  LV_PART_MAIN);
        update_output_widget(widget, active_profile_index);
    }
}
