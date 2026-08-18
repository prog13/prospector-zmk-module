#include "flux_poles.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif
#include <zephyr/random/random.h>

/* Poles are pulled back inside an ellipse of these half-axes, centred on the lattice. */
#define CONF_HALF_X (LAT_W / 2.0f - 1.0f)
#define CONF_HALF_Y (LAT_H / 2.0f - 1.0f)

#define Q_MIN        0.35f      // Init floor only; a running pole may drain through zero
#define Q_MAX        1.60f
#define INIT_RADIUS  5.5f

#define ADVECT       6.0f
#define CORE         2.2f
#define CORE_DIST    1.1f
#define CONFINE      0.06f
#define DAMP         0.001f
#define STIR         0.04f

/* Negative: charge flows from the weaker pole to the stronger, not towards zero. */
#define TRICKLE     (-2.0f)
/* Per step: rescale by the square root of any FLUX_POLES_STEP_MS change, not linearly; the
 * walk's variance is what accumulates per unit time. */
#define JITTER       0.00316f
#define CRIT_DIST    1.30f

/* In field time. A distance threshold instead would tie the merge rate to the step size. */
#define REFRACTORY_MS 150000u
/* Above 1, so the drained pole flips polarity rather than emptying. */
#define DUMP_OVER    1.8f
/* In field time. Much longer and a merge is nearly always in progress, draining every |q|
 * towards zero. */
#define DUMP_MS      6000.0f

#define FORCE_SCALE  60.0f
#define VEL_SCALE    0.00006f
#define TRICKLE_RATE 0.002f

#define LORENZ_SIGMA    10.0f
#define LORENZ_RHO      28.0f
#define LORENZ_BETA     2.6666667f
#define LORENZ_SUBSTEPS 3
#define LORENZ_DT       0.00008f

static struct flux_pole poles[POLE_COUNT];
static struct flux_pole poles_prev[POLE_COUNT];
static uint32_t cooldown_ms[POLE_COUNT][POLE_COUNT];
static float lorenz_x = 0.9f, lorenz_y = 0.2f, lorenz_z = 25.1f;

/* One slot, so two merges can never overlap. */
static struct {
    int small, big;
    float per_step, left;
} drain;

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Uniform in -0.5..0.5. */
static inline float rand_signed(void) {
    return (float)((sys_rand32_get() >> 8) & 0xFFFFFF) / (float)0x1000000 - 0.5f;
}

static void step_lorenz(void) {
    float dt = LORENZ_DT;

    for (int s = 0; s < LORENZ_SUBSTEPS; s++) {
        float dx = LORENZ_SIGMA * (lorenz_y - lorenz_x);
        float dy = lorenz_x * (LORENZ_RHO - lorenz_z) - lorenz_y;
        float dz = lorenz_x * lorenz_y - LORENZ_BETA * lorenz_z;
        lorenz_x += dx * dt;
        lorenz_y += dy * dt;
        lorenz_z += dz * dt;
    }
}

static float drive(int axis) {
    float v = axis == 0 ? lorenz_x / 16.0f
            : axis == 1 ? lorenz_y / 22.0f
                        : (lorenz_z - 24.0f) / 12.0f;
    return clampf(v, -1.0f, 1.0f);
}

void flux_poles_init(void) {
    memset(poles, 0, sizeof(poles));

    for (int j = 0; j < POLE_COUNT; j++) {
        float a = (rand_signed() + 0.5f) * 2.0f * (float)M_PI;
        float r = INIT_RADIUS * sqrtf(rand_signed() + 0.5f);   // Uniform over the disc

        poles[j].x = LAT_CX + cosf(a) * r;
        poles[j].y = LAT_CY + sinf(a) * r;
        poles[j].vx = poles[j].vy = 0.0f;
        poles[j].q = ((j & 1) ? -1.0f : 1.0f) * (Q_MIN + (rand_signed() + 0.5f) * (Q_MAX - Q_MIN));
    }

    /* The Q_MAX clamp can leave a residue, so the total is not exactly zero. */
    float total = 0.0f;
    for (int j = 0; j < POLE_COUNT; j++) {
        total += poles[j].q;
    }
    poles[POLE_COUNT - 1].q = clampf(poles[POLE_COUNT - 1].q - total, -Q_MAX, Q_MAX);

    memcpy(poles_prev, poles, sizeof(poles));
    memset(cooldown_ms, 0, sizeof(cooldown_ms));
    memset(&drain, 0, sizeof(drain));
}

static void step_motion(void) {
    const float soft2 = POLE_SIZE * POLE_SIZE;
    float fx[POLE_COUNT] = {0}, fy[POLE_COUNT] = {0};

    for (int i = 0; i < POLE_COUNT; i++) {
        for (int j = 0; j < POLE_COUNT; j++) {
            if (i == j) {
                continue;
            }

            float dx = poles[i].x - poles[j].x, dy = poles[i].y - poles[j].y;
            float d2 = dx * dx + dy * dy;
            float w = poles[i].q * poles[j].q / (d2 + soft2);

            fx[i] += dx * w * ADVECT;
            fy[i] += dy * w * ADVECT;

            if (d2 < CORE_DIST * CORE_DIST) {
                float d = sqrtf(d2);
                float push = CORE * (CORE_DIST - d) * FORCE_SCALE;

                fx[i] += (d > 0.001f ? dx / d : 1.0f) * push;
                fy[i] += (d > 0.001f ? dy / d : 0.0f) * push;
            }
        }
    }

    for (int j = 0; j < POLE_COUNT; j++) {
        /* One wind for all would slide the whole arrangement around and never break up a settled
         * packing. */
        float wind_x = STIR * drive(j % 3) * FORCE_SCALE;
        float wind_y = STIR * drive((j + 1) % 3) * FORCE_SCALE;

        float ex = (poles[j].x - LAT_CX) / CONF_HALF_X;
        float ey = (poles[j].y - LAT_CY) / CONF_HALF_Y;
        float ed = sqrtf(ex * ex + ey * ey);
        float pull = ed > 1.0f ? CONFINE * (ed - 1.0f) * FORCE_SCALE / ed : 0.0f;

        fx[j] += wind_x - ex * pull;
        fy[j] += wind_y - ey * pull;

        poles[j].vx = (poles[j].vx + fx[j] * VEL_SCALE) * (1.0f - DAMP);
        poles[j].vy = (poles[j].vy + fy[j] * VEL_SCALE) * (1.0f - DAMP);
        poles[j].x += poles[j].vx;
        poles[j].y += poles[j].vy;
    }
}

/* Moves up to `want` of charge from a to b, stopping where either pole would leave +-Q_MAX. */
static void transfer(int a, int b, float want) {
    if (want == 0.0f) {
        return;
    }

    float room_a = want > 0 ? poles[a].q + Q_MAX : Q_MAX - poles[a].q;
    float room_b = want > 0 ? Q_MAX - poles[b].q : poles[b].q + Q_MAX;
    float amt = fminf(fabsf(want), fminf(fmaxf(room_a, 0.0f), fmaxf(room_b, 0.0f)));

    if (amt <= 0.0f) {
        return;
    }

    amt = want > 0 ? amt : -amt;
    poles[a].q -= amt;
    poles[b].q += amt;
}

static void step_charge(void) {
    float mean = 0.0f;

    for (int j = 0; j < POLE_COUNT; j++) {
        poles[j].q += rand_signed() * JITTER;
        mean += poles[j].q;
    }
    mean /= POLE_COUNT;

    /* A non-zero total makes the whole field point one way far from the poles. */
    for (int j = 0; j < POLE_COUNT; j++) {
        poles[j].q = clampf(poles[j].q - mean, -Q_MAX, Q_MAX);
    }

    /* A merge runs to completion even if the two poles have since drifted apart; the amount was
     * decided when they touched. */
    if (drain.left != 0.0f) {
        float slice = fabsf(drain.per_step) < fabsf(drain.left) ? drain.per_step : drain.left;

        transfer(drain.small, drain.big, slice);
        drain.left -= slice;
        if (fabsf(drain.left) < 0.001f) {
            drain.left = 0.0f;
        }
    }

    const float soft2 = POLE_SIZE * POLE_SIZE;

    for (int i = 0; i < POLE_COUNT; i++) {
        for (int j = i + 1; j < POLE_COUNT; j++) {
            float dx = poles[i].x - poles[j].x, dy = poles[i].y - poles[j].y;
            float d2 = dx * dx + dy * dy;

            transfer(i, j, TRICKLE * (poles[i].q - poles[j].q) / (d2 + soft2) *
                               TRICKLE_RATE);

            if (cooldown_ms[i][j] > 0) {
                cooldown_ms[i][j] = cooldown_ms[i][j] > FLUX_POLES_STEP_MS
                                        ? cooldown_ms[i][j] - FLUX_POLES_STEP_MS
                                        : 0;
            }

            /* Averaging the two instead of draining the smaller into the larger would take both
             * towards zero. */
            if (drain.left == 0.0f && cooldown_ms[i][j] == 0 && d2 < CRIT_DIST * CRIT_DIST) {
                int small = fabsf(poles[i].q) <= fabsf(poles[j].q) ? i : j;

                drain.small = small;
                drain.big = small == i ? j : i;
                drain.left = poles[small].q * DUMP_OVER;
                drain.per_step = drain.left * (FLUX_POLES_STEP_MS / DUMP_MS);
                cooldown_ms[i][j] = REFRACTORY_MS;
            }
        }
    }
}

void flux_poles_step(void) {
    memcpy(poles_prev, poles, sizeof(poles));
    step_lorenz();
    step_motion();
    step_charge();
}

void flux_poles_sample(float alpha, struct flux_pole *out) {
    for (int j = 0; j < POLE_COUNT; j++) {
        out[j].x = poles_prev[j].x + (poles[j].x - poles_prev[j].x) * alpha;
        out[j].y = poles_prev[j].y + (poles[j].y - poles_prev[j].y) * alpha;
        out[j].q = poles_prev[j].q + (poles[j].q - poles_prev[j].q) * alpha;
    }
}

const struct flux_pole *flux_poles(void) {
    return poles;
}
