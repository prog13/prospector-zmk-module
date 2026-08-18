#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

#define FLUX_LINES_GRID_COLS 8
#define FLUX_LINES_GRID_ROWS 6
#define FLUX_LINES_SPACING 34
#define FLUX_LINES_GRID_OFFSET 18

#define FLUX_LINES_WIDTH 274
#define FLUX_LINES_HEIGHT 206

struct zmk_widget_flux_lines {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *layer_label;
    lv_obj_t *battery_label;
};

int zmk_widget_flux_lines_init(struct zmk_widget_flux_lines *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_flux_lines_obj(struct zmk_widget_flux_lines *widget);

void zmk_widget_flux_lines_set_labels(struct zmk_widget_flux_lines *widget,
                                      lv_obj_t *layer_label,
                                      lv_obj_t *battery_label);

void zmk_widget_flux_lines_set_cell_excluded(int col, int row, bool excluded);
