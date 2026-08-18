#pragma once

#include <stdint.h>

/* The field is B(x) = sum_j q_j (x - p_j) / (|x - p_j|^2 + POLE_SIZE^2), and each pole moves in
 * the field of the others. This file knows nothing about drawing. */

#define POLE_COUNT 5
#define POLE_SIZE  2.0f     // Softening, in cells: the pole's apparent width

/* Larger than the visible grid, so a pole can sit off screen and still shape the field on it. */
#define LAT_W 14
#define LAT_H 12
#define LAT_N (LAT_W * LAT_H)
#define VIEW_OFF_X 3
#define VIEW_OFF_Y 3

/* flux_lines.c asserts these match its grid. */
#define VIEW_W 8
#define VIEW_H 6

#define LAT_CX (VIEW_OFF_X + VIEW_W / 2.0f - 0.5f)
#define LAT_CY (VIEW_OFF_Y + VIEW_H / 2.0f - 0.5f)

struct flux_pole {
    float x, y;     // Lattice cells
    float vx, vy;
    float q;        // Signed; the total across all poles is held at zero
};

/* Every rate in flux_poles.c is per step of this length, not per frame. */
#define FLUX_POLES_STEP_MS 50

void flux_poles_init(void);

void flux_poles_step(void);

const struct flux_pole *flux_poles(void);

/* Poles interpolated between the previous step and the current one, alpha in 0..1. */
void flux_poles_sample(float alpha, struct flux_pole *out);
