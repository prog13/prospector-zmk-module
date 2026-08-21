#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

/* The panel is 284 wide; this is trimmed to a multiple of CHUNK. */
#define FERRO_BLOBS_WIDTH 282
#define FERRO_BLOBS_HEIGHT 240

#define FERRO_BLOBS_GRID_SPACING 34
#define FERRO_BLOBS_GRID_OFFSET 18

/* Labels registered in these slots are hidden from LVGL and drawn by ferro instead. */
enum ferro_text_slot {
    FERRO_TEXT_LAYER,
    FERRO_TEXT_BATTERY,
    FERRO_TEXT_OUT_LINK, /* the connection widget's two labels */
    FERRO_TEXT_OUT_PROFILE,
    FERRO_TEXT_MOD_0, /* four modifier slots, FERRO_TEXT_MOD_0 + i */
    FERRO_TEXT_COUNT = FERRO_TEXT_MOD_0 + 4,
};

struct zmk_widget_ferro_blobs {
    sys_snode_t node;
    lv_obj_t *obj;
};

int zmk_widget_ferro_blobs_init(struct zmk_widget_ferro_blobs *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_ferro_blobs_obj(struct zmk_widget_ferro_blobs *widget);

/* Hands a label to ferro. Its text, font, colour and opacity are re-read every frame, so the
 * caller keeps styling it as usual. */
void zmk_widget_ferro_blobs_set_text(enum ferro_text_slot slot, lv_obj_t *label);

/* Wakes the render timer after a label change; the label itself is re-read on the next frame. */
void zmk_widget_ferro_blobs_text_dirty(void);

/* Requests the next palette. Safe from the input thread. */
void zmk_widget_ferro_blobs_request_palette_next(void);

/* Where the screen is being touched, in screen pixels. Safe from the input thread. On release
 * pass the last position; the droplet drains from there. */
void zmk_widget_ferro_blobs_set_contact(int16_t screen_x, int16_t screen_y, bool pressed);
