#include "flux_lines.h"
#include "flux_poles.h"
#include "line_endpoints.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define _USE_MATH_DEFINES
#include <math.h>
#include <lvgl.h>
#include <zmk/event_manager.h>
#include <zmk/events/wpm_state_changed.h>
#include <zephyr/kernel.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define GRID_COLS FLUX_LINES_GRID_COLS
#define GRID_ROWS FLUX_LINES_GRID_ROWS
#define SPACING FLUX_LINES_SPACING
#define CELL_COUNT (GRID_COLS * GRID_ROWS)

BUILD_ASSERT(VIEW_W == GRID_COLS && VIEW_H == GRID_ROWS, "flux view does not match the grid");

#define CROWDING        0.45f
#define CROWDING_EXP    0.4f
#define CROWDING_MIN    0.3f
#define CROWDING_MAX    1.9f
#define CROWDING_YIELD  0.6f
#define FIELD_RATE_IDLE         0.3f
/* Faster and a cell skips orientations: the integer endpoint table quantises direction, coarsely
 * at short lengths. */
#define FIELD_RATE_TYPING       8.0f

/* Must stay above TIMER_PERIOD_30HZ * FIELD_RATE_TYPING, or the cap clips the typing rate every
 * frame. */
#define FIELD_MS_PER_FRAME_MAX  600

/* Must not go below TIMER_PERIOD_2HZ, already a normal frame at the idle tier. */
#define DECAY_FRAME_CAP_MS      500

/* ZMK updates WPM once a second, so a tau under that follows the staircase instead of smoothing
 * it. */
#define DECAY_TAU_RISE_SEC             1.5f
#define DECAY_TAU_SLOW_FALL_ACTIVE_SEC 6.58f
#define DECAY_TAU_NORMAL_FALL_IDLE_SEC 1.63f

/* Sum to 1.0, so full intensity draws a line at LINE_ENDPOINT_LENGTH. */
#define LENGTH_BASE_IDLE        0.65f
#define LENGTH_BASE_ACTIVE      0.35f
#define LENGTH_MIN              0.1f
/* Holds the half-length under SPACING / 2 at LINE_ENDPOINT_LENGTH, so lines from neighbouring
 * cells never meet. */
#define LENGTH_MAX              1.15f

#define OPACITY_BASE_IDLE       0.3f
#define OPACITY_BASE_ACTIVE     0.5f
#define OPACITY_ACTIVE_TRIM     0.15f
#define OPACITY_MIN             0.15f
#define OPACITY_MAX             0.7f

#define TIMER_PERIOD_30HZ       33
#define TIMER_PERIOD_15HZ       66
#define TIMER_PERIOD_2HZ        500

/* Where the timer drops to TIMER_PERIOD_2HZ: under one frame per field step, the lines stop
 * turning smoothly. */
#define DEEP_IDLE_AFTER_MS      300000

static const int16_t grid_cx[GRID_COLS] = {18, 52, 86, 120, 154, 188, 222, 256};
static const int16_t grid_cy[GRID_ROWS] = {18, 52, 86, 120, 154, 188};

static inline int angle_to_index(float angle) {
    int deg = (int)(angle * (180.0f / M_PI));
    deg = deg % 360;
    if (deg < 0) deg += 360;
    return deg / LINE_ENDPOINT_ANGLE_STEP;
}

static inline float clampf(float val, float min_val, float max_val) {
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

static inline float blend_to(float from, float to, float t) {
    return from + (to - from) * t;
}

typedef struct {
    float current_value;
    float at_stop_value;
    float target_value;
} decay_param_t;

static inline decay_param_t compute_decay_param(
    decay_param_t state,
    int current_wpm,
    uint32_t idle_ms,
    uint32_t decay_ms,
    float dt
) {
    decay_param_t result = state;

    if (current_wpm > 0) {
        result.target_value = fminf(1.0f, current_wpm / 60.0f);
        result.at_stop_value = result.current_value;
    } else if (idle_ms < decay_ms) {
        float progress = (float)idle_ms / decay_ms;
        result.target_value = result.at_stop_value * (1.0f - progress);
    } else {
        result.target_value = 0.0f;
    }

    float tau;
    if (current_wpm > 0 && result.target_value > result.current_value) {
        tau = DECAY_TAU_RISE_SEC;
    } else if (current_wpm > 0) {
        tau = DECAY_TAU_SLOW_FALL_ACTIVE_SEC;
    } else {
        tau = DECAY_TAU_NORMAL_FALL_IDLE_SEC;
    }

    result.current_value = blend_to(result.current_value, result.target_value,
                                    1.0f - expf(-dt / tau));
    return result;
}

static float intensity = 0.0f;
static uint32_t last_keypress_time = 0;
static int current_wpm = 0;
static bool animation_started = false;

static uint8_t line_endpoint_idx[CELL_COUNT];
static float line_length_scale[CELL_COUNT];
static lv_opa_t line_opacity[CELL_COUNT];

static float field_mag[CELL_COUNT];

static uint64_t label_excluded_cells = 0;
static uint64_t modifier_excluded_cells = 0;

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);
static lv_timer_t *animation_timer = NULL;
static uint32_t last_timer_period = TIMER_PERIOD_30HZ;

static int wpm_event_handler(const zmk_event_t *eh) {
    const struct zmk_wpm_state_changed *ev = as_zmk_wpm_state_changed(eh);
    if (ev) {
        uint32_t now = k_uptime_get_32();
        int new_wpm = ev->state;

        if (now < 10000) {
            return ZMK_EV_EVENT_BUBBLE;
        }

        if (new_wpm > 300) {
            return ZMK_EV_EVENT_BUBBLE;
        }

        current_wpm = new_wpm;
        if (current_wpm > 0) {
            last_keypress_time = now;
            animation_started = true;
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(widget_flux_lines, wpm_event_handler);
ZMK_SUBSCRIPTION(widget_flux_lines, zmk_wpm_state_changed);

/* lv_obj_get_coords is screen-absolute while grid_cx is relative to the widget, hence the
 * subtraction. */
static int label_columns(lv_obj_t *label, lv_obj_t *widget_obj) {
    lv_area_t label_coords, widget_coords;
    lv_obj_get_coords(label, &label_coords);
    lv_obj_get_coords(widget_obj, &widget_coords);

    int label_right = label_coords.x2 + 1 - widget_coords.x1;

    for (int col = 0; col < GRID_COLS; col++) {
        int cell_left = grid_cx[col] - SPACING / 2;
        if (label_right <= cell_left) {
            return col;
        }
    }
    return GRID_COLS;
}

static void update_label_excluded_cells(void) {
    label_excluded_cells = 0;

    struct zmk_widget_flux_lines *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        if (widget->layer_label) {
            int cols = label_columns(widget->layer_label, widget->obj);
            for (int col = 0; col < cols; col++) {
                label_excluded_cells |= (1ULL << col);
            }
        }

        if (widget->battery_label) {
            int cols = label_columns(widget->battery_label, widget->obj);
            for (int col = 0; col < cols; col++) {
                label_excluded_cells |= (1ULL << (5 * GRID_COLS + col));
            }
        }
        break;
    }
}

static void label_size_changed_cb(lv_event_t *e) {
    update_label_excluded_cells();

    struct zmk_widget_flux_lines *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        lv_obj_invalidate(widget->obj);
    }
}

static uint32_t perf_update_us = 0;
static uint32_t perf_draw_us = 0;
static uint32_t perf_frame_count = 0;
static uint32_t perf_tick_count = 0;
static uint32_t perf_draw_us_sum = 0;

static void flux_update(void) {
    uint32_t start = k_cycle_get_32();

    uint32_t now = k_uptime_get_32();
    uint32_t idle_ms = now - last_keypress_time;

    const uint32_t INTENSITY_DECAY_MS = CONFIG_PROSPECTOR_ANIMATION_INTENSITY_DECAY_SEC * 1000;

    static float intensity_at_stop = 0.0f;

    static uint32_t last_frame_time = 0;
    uint32_t frame_ms = last_frame_time != 0 ? now - last_frame_time : TIMER_PERIOD_30HZ;
    last_frame_time = now;

    const float decay_dt = MIN(frame_ms, DECAY_FRAME_CAP_MS) / 1000.0f;

    decay_param_t intensity_state = {intensity, intensity_at_stop, 0};
    intensity_state = compute_decay_param(intensity_state, current_wpm, idle_ms,
                                          INTENSITY_DECAY_MS, decay_dt);
    intensity = intensity_state.current_value;
    intensity_at_stop = intensity_state.at_stop_value;

    static float field_accum_ms = 0.0f;
    const float field_rate = FIELD_RATE_IDLE + intensity * (FIELD_RATE_TYPING - FIELD_RATE_IDLE);

    field_accum_ms += MIN(frame_ms * field_rate, (float)FIELD_MS_PER_FRAME_MAX);
    while (field_accum_ms >= FLUX_POLES_STEP_MS) {
        flux_poles_step();
        field_accum_ms -= FLUX_POLES_STEP_MS;
    }

    /* At idle a step lands only every few frames; drawing the raw step positions would make the
     * field move in jerks. */
    struct flux_pole poles[POLE_COUNT];
    flux_poles_sample(field_accum_ms / FLUX_POLES_STEP_MS, poles);

    const float soft2 = POLE_SIZE * POLE_SIZE;
    float mag_sum = 0.0f;

    for (int y = 0; y < LAT_H; y++) {
        for (int x = 0; x < LAT_W; x++) {
            float bx = 0.0f, by = 0.0f;

            for (int j = 0; j < POLE_COUNT; j++) {
                float dx = x - poles[j].x;
                float dy = y - poles[j].y;
                float w = poles[j].q / (dx * dx + dy * dy + soft2);
                bx += dx * w;
                by += dy * w;
            }

            float mag = sqrtf(bx * bx + by * by);
            mag_sum += mag;

            int col = x - VIEW_OFF_X;
            int row = y - VIEW_OFF_Y;
            if (col < 0 || col >= GRID_COLS || row < 0 || row >= GRID_ROWS) {
                continue;
            }

            int line_idx = row * GRID_COLS + col;

            field_mag[line_idx] = mag;
            line_endpoint_idx[line_idx] = (uint8_t)angle_to_index(atan2f(by, bx));
        }
    }

    /* The view's mean would rescale every line on screen as a pole approaches the window, rather
     * than lengthen the ones near it. */
    const float mag_mean = mag_sum > 0.0f ? mag_sum / LAT_N : 1.0f;
    const float base_scale = LENGTH_BASE_IDLE + intensity * LENGTH_BASE_ACTIVE;
    const float base_opa = clampf(OPACITY_BASE_IDLE + intensity * OPACITY_BASE_ACTIVE -
                                      OPACITY_ACTIVE_TRIM * intensity,
                                  OPACITY_MIN, OPACITY_MAX);
    const lv_opa_t opa = (lv_opa_t)(base_opa * 255.0f);

    const float crowding = CROWDING * (1.0f - CROWDING_YIELD * intensity);

    for (int line_idx = 0; line_idx < CELL_COUNT; line_idx++) {
        float crowd = 1.0f + crowding * (powf(field_mag[line_idx] / mag_mean, CROWDING_EXP) - 1.0f);
        crowd = clampf(crowd, CROWDING_MIN, CROWDING_MAX);

        line_length_scale[line_idx] = clampf(base_scale * crowd, LENGTH_MIN, LENGTH_MAX);
        line_opacity[line_idx] = opa;
    }

    uint32_t elapsed = k_cycle_get_32() - start;
    perf_update_us = k_cyc_to_us_floor32(elapsed);
}

static void draw_cb(lv_event_t *e) {
    uint32_t start = k_cycle_get_32();
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);

    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);

    int32_t obj_x1 = obj_coords.x1;
    int32_t obj_y1 = obj_coords.y1;

    uint64_t excluded = label_excluded_cells | modifier_excluded_cells;

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_white();
    line_dsc.width = 2;
    line_dsc.round_start = 0;
    line_dsc.round_end = 0;

    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            int line_idx = row * GRID_COLS + col;

            if (excluded & (1ULL << line_idx)) {
                continue;
            }

            int cx = grid_cx[col];
            int cy = grid_cy[row];

            uint8_t idx = line_endpoint_idx[line_idx];
            int8_t dx_base = line_endpoints[idx][0];
            int8_t dy_base = line_endpoints[idx][1];
            float scale = line_length_scale[line_idx];
            int16_t dx = (int16_t)(dx_base * scale);
            int16_t dy = (int16_t)(dy_base * scale);

            line_dsc.opa = line_opacity[line_idx];
            line_dsc.p1.x = obj_x1 + cx - dx;
            line_dsc.p1.y = obj_y1 + cy - dy;
            line_dsc.p2.x = obj_x1 + cx + dx;
            line_dsc.p2.y = obj_y1 + cy + dy;
            lv_draw_line(layer, &line_dsc);
        }
    }

    uint32_t elapsed = k_cycle_get_32() - start;
    perf_draw_us = k_cyc_to_us_floor32(elapsed);

    perf_frame_count++;
    perf_draw_us_sum += perf_draw_us;
    if (perf_frame_count >= 30) {
        LOG_DBG("perf: update=%uus draw=%uus period=%ums wpm=%d int=%d%% "
                "draws=%u ticks=%u draw_sum=%uus",
                perf_update_us, perf_draw_us, last_timer_period, current_wpm,
                (int)(intensity * 100.0f), perf_frame_count, perf_tick_count, perf_draw_us_sum);
        perf_frame_count = 0;
        perf_tick_count = 0;
        perf_draw_us_sum = 0;
    }
}

static void timer_cb(lv_timer_t *timer) {
    perf_tick_count++;
    uint32_t now = k_uptime_get_32();
    uint32_t idle_ms = now - last_keypress_time;

    uint32_t target_period;
    if (current_wpm > 0) {
        target_period = TIMER_PERIOD_30HZ;
    } else if (animation_started && idle_ms < DEEP_IDLE_AFTER_MS) {
        target_period = TIMER_PERIOD_15HZ;
    } else {
        target_period = TIMER_PERIOD_2HZ;
    }

    if (target_period != last_timer_period) {
        lv_timer_set_period(animation_timer, target_period);
        last_timer_period = target_period;
    }

    flux_update();

    struct zmk_widget_flux_lines *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        lv_obj_invalidate(widget->obj);
    }
}

int zmk_widget_flux_lines_init(struct zmk_widget_flux_lines *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    widget->layer_label = NULL;
    widget->battery_label = NULL;

    lv_obj_remove_style_all(widget->obj);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0);

    lv_obj_add_event_cb(widget->obj, draw_cb, LV_EVENT_DRAW_MAIN, widget);

    sys_slist_append(&widgets, &widget->node);

    if (animation_timer == NULL) {
        flux_poles_init();
        animation_timer = lv_timer_create(timer_cb, TIMER_PERIOD_30HZ, NULL);
    }

    return 0;
}

lv_obj_t *zmk_widget_flux_lines_obj(struct zmk_widget_flux_lines *widget) {
    return widget->obj;
}

void zmk_widget_flux_lines_set_labels(struct zmk_widget_flux_lines *widget,
                                      lv_obj_t *layer_label,
                                      lv_obj_t *battery_label) {
    widget->layer_label = layer_label;
    widget->battery_label = battery_label;

    if (layer_label) {
        lv_obj_add_event_cb(layer_label, label_size_changed_cb, LV_EVENT_SIZE_CHANGED, NULL);
    }
    if (battery_label) {
        lv_obj_add_event_cb(battery_label, label_size_changed_cb, LV_EVENT_SIZE_CHANGED, NULL);
    }

    update_label_excluded_cells();
}

void zmk_widget_flux_lines_set_cell_excluded(int col, int row, bool excluded) {
    if (col < 0 || col >= GRID_COLS || row < 0 || row >= GRID_ROWS) {
        return;
    }
    uint64_t mask = 1ULL << (row * GRID_COLS + col);
    if (excluded) {
        modifier_excluded_cells |= mask;
    } else {
        modifier_excluded_cells &= ~mask;
    }
}
