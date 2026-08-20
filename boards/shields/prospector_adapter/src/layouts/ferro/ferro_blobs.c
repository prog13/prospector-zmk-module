#include "ferro_blobs.h"
#include "ferro_palette.h"
#include "../flux/flux_poles.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <math.h>
#include <string.h>
#include <lvgl.h>
#include <zmk/event_manager.h>
#include <zmk/events/wpm_state_changed.h>
#include <zephyr/kernel.h>

#include <zephyr/drivers/display.h>

/* The blobs are a scalar field F(x) = sum_j exp(-d_j^2 / (2 R0^2 max(|q_j|, Q_FLOOR))), one
 * Gaussian per pole with the charge setting its width. It is drawn as an ordered-Bayer halftone
 * on a CHUNK-pixel grid, fully off below ISO and fully on above 2*ISO. Each pole is smeared over
 * its last SMOOTH_WIN_MS of movement before it enters F. */

#define PANEL_W FERRO_BLOBS_WIDTH
#define PANEL_H FERRO_BLOBS_HEIGHT

/* The image is rotated 270, so the devicetree height is the screen's width. */
#define GLASS_W DT_PROP(DT_CHOSEN(zephyr_display), height)
#define GLASS_H DT_PROP(DT_CHOSEN(zephyr_display), width)
BUILD_ASSERT(GLASS_W >= FERRO_BLOBS_WIDTH,
             "ferro needs a panel at least FERRO_BLOBS_WIDTH across after rotation");
BUILD_ASSERT(GLASS_H >= FERRO_BLOBS_HEIGHT,
             "ferro needs a panel at least FERRO_BLOBS_HEIGHT tall after rotation");

/* The field is evaluated on a grid of CHUNK x CHUNK pixel cells. Everything from the splat to the
 * dirty bands works in grid cells; only blit_bands expands back to pixels. */
#define CHUNK 3
#define GRID_W (PANEL_W / CHUNK)
#define GRID_H (PANEL_H / CHUNK)
_Static_assert(GRID_W * CHUNK == PANEL_W && GRID_H * CHUNK == PANEL_H,
               "CHUNK must divide the panel");
/* Pixel to grid is g = p / CHUNK - GRID_OFF; the offset puts integer grid coordinates at cell
 * centres. */
#define GRID_OFF ((CHUNK - 1) * 0.5f / CHUNK)

/* Maps the pole lattice onto the panel: scaled uniformly to fit the width, with the height
 * centred. CROP_Y is how many pixels of lattice fall off the top, and may be negative. BBOX
 * matches CONF_HALF in flux_poles.c. */
#define BBOX_W (LAT_W - 2.0f)
#define BBOX_H (LAT_H - 2.0f)
#define SCALE_PX (PANEL_W / BBOX_W)
#define CROP_Y ((BBOX_H * SCALE_PX - PANEL_H) / 2.0f)
#define GRID_SCALE (SCALE_PX / CHUNK)

#define R0 2.5f /* Blob width scale, in cells: drawn radius ~1.18 R0 sqrt(|q|) */
#define ISO 0.5f
#define Q_FLOOR 0.02f


/* A touch adds one more Gaussian to F, with charge TOUCH_CHARGE * touch_level. It is drawn only;
 * the pole engine never sees it. */
#define TOUCH_CHARGE 1.5f
/* The touch ramps up over TOUCH_RISE_MS, so a held finger makes a bigger droplet than a tap. */
#define TOUCH_RISE_MS 120
#define TOUCH_FALL_MS 700

#define FIELD_RATE_IDLE 0.3f
#define FIELD_RATE_TYPING 8.0f

/* Must stay above TIMER_PERIOD_30HZ * FIELD_RATE_TYPING. Below that, the cap clips the typing
 * rate on every frame and FIELD_RATE_TYPING stops meaning anything. */
#define FIELD_MS_PER_FRAME_MAX 600

/* Caps the dt fed to the decays after a stall. A normal frame at the idle tier is already
 * TIMER_PERIOD_2HZ, so this must not go lower. */
#define DECAY_FRAME_CAP_MS 500

/* ZMK updates WPM once a second, so it arrives as a staircase. A tau shorter than a second
 * would follow the steps instead of smoothing them. */
#define DECAY_TAU_RISE_SEC 1.5f
#define DECAY_TAU_SLOW_FALL_ACTIVE_SEC 6.58f
#define DECAY_TAU_NORMAL_FALL_IDLE_SEC 1.63f

/* Benchmark mode: pins the frame rate and the pole sequence so two builds render the
 * same frames and their timings can be compared. */
#define FERRO_BENCH 0

/* Per-frame timing logs. Keep off in a shipped build: the log volume starves the input thread
 * and touch events get dropped. Independent of FERRO_BENCH, which only pins the load. */
#define FERRO_PERF_LOG 0
#if FERRO_BENCH && !FERRO_PERF_LOG
#undef FERRO_PERF_LOG
#define FERRO_PERF_LOG 1
#endif

#if FERRO_PERF_LOG
#define FERRO_PERF_COUNT(c) ((c)++)
#define FERRO_PERF_SET(f) ((f) = true)
#else
#define FERRO_PERF_COUNT(c) ((void)0)
#define FERRO_PERF_SET(f) ((void)0)
#endif

#define TIMER_PERIOD_30HZ 33
#define TIMER_PERIOD_15HZ 66
#define TIMER_PERIOD_2HZ 500

/* Each pole is drawn as the average of SMOOTH_TAPS snapshots spread over its last SMOOTH_WIN_MS
 * of field time. The touch term is not smoothed; its ramp is already in wall time. */
#define SMOOTH_WIN_MS 1000
#define SMOOTH_TAPS 8
/* Enough engine-step snapshots to cover the window: the oldest tap reaches back
 * (1 - alpha) + WIN/STEP steps. */
#define SMOOTH_RING_N (SMOOTH_WIN_MS / FLUX_POLES_STEP_MS + 2)

#define FIELD_RATE_DEEP (TIMER_PERIOD_15HZ * FIELD_RATE_IDLE / (float)TIMER_PERIOD_2HZ)

/* How long after the last keypress the timer drops to 2Hz. This is the point where nobody is
 * looking at the screen any more, not a decay time. */
#define DEEP_IDLE_AFTER_MS 300000

/* Lookup table for exp(-t), t in [0, EXP_MAX), filled at init. Entry 0 is a guard holding
 * exp(0): the fixed-point splat can round its index to -1, and this makes that harmless
 * instead of needing a clamp. */
#define EXP_LUT_N 2048
#define EXP_MAX 8.0f
#define EXP_IDX_SCALE ((float)EXP_LUT_N / EXP_MAX)
static float exp_lut[EXP_LUT_N + 1];
/* The same table in Q13 for the splat's integer inner loop. (POLE_COUNT + 1) terms *
 * EXP_ONE_Q13 * the Q15 profile maximum still fits in int32. */
#define EXP_ONE_Q13 8192
static uint16_t exp_lut16[EXP_LUT_N + 1];
/* exp(-ROW_CUT_T) in F's fixed-point scale, for the per-tile skip test. Filled at init. */
static int32_t tile_cut_q;

/* ts is t already multiplied by EXP_IDX_SCALE; callers fold the scale into their coefficients. */
static inline float exp_neg(float ts) {
    int i = (int)ts;
    return (uint32_t)i < EXP_LUT_N ? exp_lut[i + 1] : 0.0f;
}

/* Fixed-point fraction bits for the splat's LUT index. Rounding drift over a row is ~0.15 bin;
 * the skip windows allow a full bin for it. */
#define TQ 13

/* A term contributing less than exp(-ROW_CUT_T) everywhere on a row is skipped for that row.
 * Even all terms at that level together cannot move a halftone edge. */
#define ROW_CUT_T 5.52f

struct pole_snap {
    float x, y, q;
};
static struct pole_snap smooth_ring[SMOOTH_RING_N][POLE_COUNT];
static int smooth_head;
static float smooth_alpha;

static void smooth_ring_push(void) {
    const struct flux_pole *p = flux_poles();
    smooth_head = (smooth_head + 1) % SMOOTH_RING_N;
    for (int j = 0; j < POLE_COUNT; j++) {
        smooth_ring[smooth_head][j] = (struct pole_snap){p[j].x, p[j].y, p[j].q};
    }
}

static void smooth_ring_prime(void) {
    for (int i = 0; i < SMOOTH_RING_N; i++) {
        smooth_ring_push();
    }
}

/* Pole j as it was `back` engine steps ago, interpolated between snapshots. */
static struct pole_snap ring_at(int j, float back) {
    back = back < 0.0f ? 0.0f : (back > SMOOTH_RING_N - 2 ? (float)(SMOOTH_RING_N - 2) : back);
    int s = (int)back;
    float fr = back - s;
    const struct pole_snap *a = &smooth_ring[(smooth_head - s + SMOOTH_RING_N) % SMOOTH_RING_N][j];
    const struct pole_snap *b =
        &smooth_ring[(smooth_head - s - 1 + SMOOTH_RING_N) % SMOOTH_RING_N][j];
    return (struct pole_snap){a->x + (b->x - a->x) * fr, a->y + (b->y - a->y) * fr,
                              a->q + (b->q - a->q) * fr};
}

/* A smeared pole is split into a Gaussian across its track times a 1-D profile along it, one
 * profile per term. The first and last bins are always zero so that a clamped out-of-range
 * index reads as far field. */
#define PROF_N 640
static uint16_t prof[POLE_COUNT + 1][PROF_N];
/* Maximum of each PROF_BLK-bin block of prof, for the per-tile skip test. */
#define PROF_BLK 32
static uint16_t prof_blk[POLE_COUNT + 1][PROF_N / PROF_BLK];

#define SAMPLE_STEP 4
#define SAMPLE_N ((GRID_W - 1) / SAMPLE_STEP + 1)
/* The splat works in tiles of 8 samples; F is padded to a whole number of tiles. */
#define TILE_N ((SAMPLE_N + 7) / 8)

/* fminf/fmaxf are library calls on the M4F, and the row setup makes about eight per term. */
#define FMINF(a, b) ((a) < (b) ? (a) : (b))
#define FMAXF(a, b) ((a) > (b) ? (a) : (b))

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

static struct k_spinlock contact_lock;
static int16_t contact_x, contact_y; /* Screen pixels, written from the input thread */
/* Also read outside the lock, by timer_cb and contact_poll_cb; only the x/y pair needs it. */
static volatile bool contact_pressed;
static uint32_t contact_changed_at;

static float touch_level = 0.0f; /* 0..1, one envelope for both the rise and the tail */
static float touch_px, touch_py; /* Widget-local pixels */

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);
static lv_timer_t *animation_timer = NULL;
static lv_timer_t *contact_poll_timer = NULL;
static uint32_t last_timer_period = TIMER_PERIOD_30HZ;

/* Which grid cells are lit, as shown on the panel. mask_diff_row updates it and records what
 * changed; blit_bands reads it to compose the pixels it ships. */
#define MASK_W ((GRID_W + 31) / 32)
static uint32_t mask[GRID_H][MASK_W];

/* Bayer thresholds in F's fixed-point scale: cell (x,y) is lit when F > ISO*(1+(2b+1)/128), b
 * being the Bayer value. dith_min is the smallest; F below it lights nothing. */
static int32_t dith_thr[8][8];
static int32_t dith_min;
/* Just above the largest threshold. A whole segment with both ends above it is lit without
 * testing each pixel; the +3 covers the fill loop's >>2 rounding. */
static int32_t dith_sat;

#define FINE_BANDS 64
/* Two bands are merged if the wasted area between them is under this. A display_write has about
 * 0.8ms of fixed cost, the same as sending ~1600 pixels, so merging below that is always a win. */
#define BAND_MERGE_PX (1600 / (CHUNK * CHUNK))
#define BAND_WASTE 4
#define DIFF_GAP 12
static struct band {
    int16_t y1, y2, x1, x2;
} bands[FINE_BANDS];
static int n_bands;

/* Repaint the whole widget on the next frame. Needed whenever colours change without the mask
 * changing (a palette switch), since the diff only tracks mask flips. Starts set so the first
 * frame paints everything. */
static bool force_full = true;
#ifdef FERRO_HOST_HARNESS
bool ferro_host_no_force_full;
#endif

/* The forked st7789v driver counts every panel write. If the count moves when ferro did not
 * write, someone else painted the panel and ferro must repaint everything: the diff never
 * reships cells it believes are already correct. One such write is expected at boot, when LVGL
 * paints the background after ferro's first frame. */
extern uint32_t prospector_panel_writes, prospector_panel_px;
static uint32_t ferro_panel_writes, ferro_panel_px;
static uint32_t panel_writes_seen;
static uint32_t panel_foreign_repairs;

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

ZMK_LISTENER(widget_ferro_blobs, wpm_event_handler);
ZMK_SUBSCRIPTION(widget_ferro_blobs, zmk_wpm_state_changed);

static float envelope_advance(float level, bool pressed, uint32_t ms) {
    float rate = (float)ms / (pressed ? TOUCH_RISE_MS : TOUCH_FALL_MS);

    return level + clampf((pressed ? 1.0f : 0.0f) - level, -rate, rate);
}

static void update_touch(uint32_t now, uint32_t frame_ms) {
    k_spinlock_key_t key = k_spin_lock(&contact_lock);
    bool pressed = contact_pressed;
    int16_t px = contact_x, py = contact_y;
    uint32_t changed_at = contact_changed_at;
    k_spin_unlock(&contact_lock, key);

    if (pressed) {
        struct zmk_widget_ferro_blobs *widget;
        SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
            lv_area_t coords;
            lv_obj_get_coords(widget->obj, &coords);
            touch_px = px - coords.x1;
            touch_py = py - coords.y1;
            break;
        }
    }

    /* If the press or release happened mid-frame, advance the envelope in two parts: the old
     * state up to the change, the new state after it. Otherwise the ramp starts up to a frame
     * late, which is visible at the 2Hz tier. */
    uint32_t after = MIN(now - changed_at, frame_ms);

    touch_level = envelope_advance(touch_level, !pressed, frame_ms - after);
    touch_level = envelope_advance(touch_level, pressed, after);
}

static void mask_set_range(uint32_t *row, int a, int b) {
    int wa = a >> 5, wb = b >> 5;
    uint32_t ma = ~0u << (a & 31);
    uint32_t mb = ~0u >> (31 - (b & 31));
    if (wa == wb) {
        row[wa] |= ma & mb;
        return;
    }
    row[wa] |= ma;
    for (int w = wa + 1; w < wb; w++) {
        row[w] = ~0u;
    }
    row[wb] |= mb;
}

#if FERRO_PERF_LOG
static uint32_t perf_ranges, perf_tile_run, perf_tile_skip;
#endif

/* How much bigger band b's rectangle gets if the given range is added to it. Bands are grouped
 * by this rather than by distance: grouping by distance chains along a curved edge into one
 * huge rectangle. */
static int32_t band_growth(const struct band *b, int y1, int y2, int x1, int x2) {
    int32_t ux1 = x1 < b->x1 ? x1 : b->x1;
    int32_t ux2 = x2 > b->x2 ? x2 : b->x2;
    int32_t uy1 = y1 < b->y1 ? y1 : b->y1;
    int32_t uy2 = y2 > b->y2 ? y2 : b->y2;
    return (ux2 - ux1 + 1) * (uy2 - uy1 + 1) -
           (b->x2 - b->x1 + 1) * (b->y2 - b->y1 + 1);
}

static void add_range(int y, int x1, int x2) {
    FERRO_PERF_COUNT(perf_ranges);

    int best = -1;
    int32_t best_growth = INT32_MAX;
    for (int b = 0; b < n_bands; b++) {
        int32_t growth = band_growth(&bands[b], y, y, x1, x2);
        if (growth < best_growth) {
            best_growth = growth;
            best = b;
        }
    }

    if (n_bands < FINE_BANDS && best_growth > (x2 - x1 + 1) * BAND_WASTE) {
        bands[n_bands] = (struct band){y, y, x1, x2};
        n_bands++;
        return;
    }
    if (y < bands[best].y1) bands[best].y1 = y;
    if (y > bands[best].y2) bands[best].y2 = y;
    if (x1 < bands[best].x1) bands[best].x1 = x1;
    if (x2 > bands[best].x2) bands[best].x2 = x2;
}

/* Ferro draws the labels itself, glyph by glyph onto the fluid. Only pixels with ink are
 * touched; there is no cleared box behind a label. */
#define TXT_BIG_W 176 /* Layer and battery: a whole word of text */
#define TXT_BIG_H 44
#define TXT_SML_W 48 /* One modifier glyph, plus slack for the ±2 of raster margin */
#define TXT_SML_H 44
#define TXT_BIG_N 2
#define TXT_SML_N (FERRO_TEXT_COUNT - TXT_BIG_N)

static uint8_t cov_big[TXT_BIG_N][TXT_BIG_H * (TXT_BIG_W / 2)];
static uint8_t cov_sml[TXT_SML_N][TXT_SML_H * (TXT_SML_W / 2)];
/* First and last inked column of each row, so compose_row only walks the ink and not the whole
 * slot box. lo > hi marks a row with no ink. */
static int16_t ink_big[TXT_BIG_N][TXT_BIG_H][2];
static int16_t ink_sml[TXT_SML_N][TXT_SML_H][2];

static struct text_slot {
    lv_obj_t *obj;
    uint8_t *cov;             /* 4bpp alpha, high nibble first, `pitch` bytes a row */
    int16_t (*ink)[2];        /* per row: first and last inked column, slot-local */
    int16_t wmax, hmax, pitch;
    int16_t x0, y0, w, h;     /* Widget-local physical box of the rasterised run */
    /* Ink colour blended over ground and over blob, per coverage level. Both are needed: with
     * only the ground one, anti-aliased edges over a blob would fade to ground. */
    uint16_t lut_bg[16], lut_fg[16];
    char seen_txt[24];
    int16_t seen_x, seen_y, seen_w;
    uint32_t seen_col;
    uint8_t seen_opa;
    int16_t sx0, sy0, sw, sh; /* The box whose pixels ferro last wrote to the panel */
    bool primed;              /* Slot has been read once; seen_* mean something */
    bool live;                /* Drawn at all: a transparent widget stencils nothing */
    bool force;               /* Ship the whole box next frame */
    bool warned_wide;
} text_slots[FERRO_TEXT_COUNT];

/* Coverage below this counts as no ink. Lower and every glyph grows a faint fringe that the
 * ink extents then have to walk. */
#define TXT_INK_MIN 3 /* of 15, i.e. >40 of 255 */

static void text_slots_init(void) {
    for (int i = 0; i < FERRO_TEXT_COUNT; i++) {
        struct text_slot *s = &text_slots[i];
        if (i < TXT_BIG_N) {
            s->cov = cov_big[i];
            s->ink = ink_big[i];
            s->wmax = TXT_BIG_W;
            s->hmax = TXT_BIG_H;
            s->pitch = TXT_BIG_W / 2;
        } else {
            s->cov = cov_sml[i - TXT_BIG_N];
            s->ink = ink_sml[i - TXT_BIG_N];
            s->wmax = TXT_SML_W;
            s->hmax = TXT_SML_H;
            s->pitch = TXT_SML_W / 2;
        }
    }
}

/* Decodes one UTF-8 codepoint. The symbol fonts use private-use codepoints, so labels are not
 * plain ASCII. */
static uint32_t text_utf8_next(const char **p) {
    const uint8_t *c = (const uint8_t *)*p;
    uint32_t cp = *c++;
    int n = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    cp &= n == 3 ? 0x07u : n == 2 ? 0x0Fu : n == 1 ? 0x1Fu : 0xFFu;
    while (n-- > 0 && (*c & 0xC0) == 0x80) {
        cp = (cp << 6) | (*c++ & 0x3F);
    }
    *p = (const char *)c;
    return cp;
}

static void text_build_over(uint16_t *lut, lv_color_t col, uint32_t opa, lv_color_t under) {
    for (int a = 0; a < 16; a++) {
        const uint32_t ea = (uint32_t)(a * 17) * opa / 255;
        const uint32_t r = ((uint32_t)col.red * ea + (uint32_t)under.red * (255 - ea)) / 255;
        const uint32_t g = ((uint32_t)col.green * ea + (uint32_t)under.green * (255 - ea)) / 255;
        const uint32_t b = ((uint32_t)col.blue * ea + (uint32_t)under.blue * (255 - ea)) / 255;
        lut[a] = __builtin_bswap16(lv_color_to_u16(lv_color_make(r, g, b)));
    }
}

static void text_build_lut(struct text_slot *s, lv_color_t col, uint32_t opa,
                           lv_color_t ground) {
    text_build_over(s->lut_bg, col, opa, ground);
    text_build_over(s->lut_fg, col, opa, lv_color_hex(ferro_palette_active()->blob));
}

/* Rasterises the label's text into the slot's coverage buffer, at exactly the position
 * lv_draw_label would have used. */
static void text_rasterise(struct text_slot *s, int ox, int oy) {
    lv_area_t a;
    lv_obj_get_coords(s->obj, &a);
    const lv_font_t *font = lv_obj_get_style_text_font(s->obj, LV_PART_MAIN);
    const char *txt = lv_label_get_text(s->obj);
    if (font == NULL || txt == NULL) {
        s->live = false;
        return;
    }
    const int32_t ls = lv_obj_get_style_text_letter_space(s->obj, LV_PART_MAIN);

    /* The raster box is the label's box plus 2px either side: a glyph with negative ofs_x or a
     * negative letter space can reach past the label's own edge. Coverage writes are clamped to
     * this box. */
    int x0 = a.x1 - ox - 2, y0 = a.y1 - oy;
    int x1 = a.x2 - ox + 2;
    int y1 = a.y1 - oy + font->line_height - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > PANEL_W - 1) x1 = PANEL_W - 1;
    if (y1 > PANEL_H - 1) y1 = PANEL_H - 1;
    if (x1 - x0 + 1 > s->wmax) {
        /* cov is only wmax wide; this clamp is what keeps the writes below in bounds. Warn
         * once: logging every frame starves the input thread. */
        if (!s->warned_wide) {
            s->warned_wide = true;
            LOG_WRN("ferro slot %d: run %d wide, clamped to %d", (int)(s - text_slots),
                    x1 - x0 + 1, s->wmax);
        }
        x1 = x0 + s->wmax - 1;
    }
    if (y1 - y0 + 1 > s->hmax) y1 = y0 + s->hmax - 1;
    if (x1 < x0 || y1 < y0) {
        s->live = false;
        return;
    }

    s->x0 = x0;
    s->y0 = y0;
    s->w = x1 - x0 + 1;
    s->h = y1 - y0 + 1;
    memset(s->cov, 0, (size_t)s->hmax * s->pitch);

    /* Honour the label's text alignment inside its box, as lv_draw_label does. The connection
     * widget's symbol sits centred in a fixed-size box. */
    int pen = a.x1 - ox;
    const lv_text_align_t halign = lv_obj_get_style_text_align(s->obj, LV_PART_MAIN);
    if (halign == LV_TEXT_ALIGN_CENTER || halign == LV_TEXT_ALIGN_RIGHT) {
        int run_w = 0;
        for (const char *c = txt; *c;) {
            const uint32_t cp = text_utf8_next(&c);
            const char *peek = c;
            lv_font_glyph_dsc_t g;
            if (lv_font_get_glyph_dsc(font, &g, cp, *peek ? text_utf8_next(&peek) : 0)) {
                run_w += g.adv_w + ls;
            }
        }
        run_w -= run_w ? ls : 0;
        const int box_w = a.x2 - a.x1 + 1;
        pen += halign == LV_TEXT_ALIGN_CENTER ? (box_w - run_w) / 2 : box_w - run_w;
    }
    int base = a.y1 - oy + font->line_height - font->base_line;
    for (const char *c = txt; *c;) {
        const uint32_t cp = text_utf8_next(&c);
        const char *peek = c;
        lv_font_glyph_dsc_t g;
        /* Pass the next codepoint so adv_w includes kerning. LVGL sized the label with kerning;
         * without it the run comes out wider than the box. */
        if (!lv_font_get_glyph_dsc(font, &g, cp, *peek ? text_utf8_next(&peek) : 0)) {
            continue;
        }
        const lv_font_fmt_txt_dsc_t *fd = g.resolved_font->dsc;
        const uint8_t *bm = fd->glyph_bitmap + fd->glyph_dsc[g.gid.index].bitmap_index;
        const int gx = pen + g.ofs_x, gy = base - g.box_h - g.ofs_y;
        pen += g.adv_w + ls;
        if (fd->bpp != 4) {
            continue; /* Every font on this screen is 4bpp; anything else is a bug */
        }

        for (int r = 0; r < g.box_h; r++) {
            const int py = gy + r;
            if (py < y0 || py > y1) {
                continue;
            }
            uint8_t *dst = s->cov + (py - y0) * s->pitch;
            for (int k = 0; k < g.box_w; k++) {
                const int px = gx + k;
                if (px < x0 || px > x1) {
                    continue;
                }
                const int n = r * g.box_w + k;
                const uint8_t v = (bm[n >> 1] >> ((n & 1) ? 0 : 4)) & 0xF;
                if (!v) {
                    continue;
                }
                const int u = px - x0;
                dst[u >> 1] |= (u & 1) ? v : (uint8_t)(v << 4);
            }
        }
    }

    /* Record the ink extents of each row. Not padded: a pixel without ink must not be claimed. */
    for (int r = 0; r < s->h; r++) {
        const uint8_t *cr = s->cov + r * s->pitch;
        int lo = s->w, hi = -1;
        for (int u = 0; u < s->w; u++) {
            if (((cr[u >> 1] >> ((u & 1) ? 0 : 4)) & 0xF) >= TXT_INK_MIN) {
                if (lo > u) lo = u;
                hi = u;
            }
        }
        s->ink[r][0] = (int16_t)lo;
        s->ink[r][1] = (int16_t)hi;
    }
    s->live = true;
}

/* The ground and blob colours the slot LUTs were built against. When either changes every LUT
 * must be rebuilt, even for slots whose own colour did not move. */
static uint32_t text_ground = 0xFFFFFFFF, text_blob = 0xFFFFFFFF;

static void text_refresh(int ox, int oy) {
#if FERRO_PERF_LOG
    bool shape_changed = false;
#endif
    const lv_color_t ground = lv_obj_get_style_bg_color(lv_screen_active(), LV_PART_MAIN);
    /* Compared separately, not packed into one word: packed, two palettes could collide, and
     * then timer_cb would paint new colours while every LUT and steady cell kept the old. */
    const uint32_t ground24 = ((uint32_t)ground.red << 16) | ((uint32_t)ground.green << 8) |
                              ground.blue;
    const uint32_t blob24 = ferro_palette_active()->blob;
    const bool under_moved = ground24 != text_ground || blob24 != text_blob;

    if (under_moved) {
        force_full = true;
    }

    for (int i = 0; i < FERRO_TEXT_COUNT; i++) {
        struct text_slot *s = &text_slots[i];
        if (s->obj == NULL) {
            continue;
        }
        const char *txt = lv_label_get_text(s->obj);
        if (txt == NULL) {
            continue;
        }
        lv_area_t a;
        lv_obj_get_coords(s->obj, &a);
        const lv_color_t col = lv_obj_get_style_text_color(s->obj, LV_PART_MAIN);
        /* Effective opacity is the product up the parent chain: the connection widget fades its
         * container while the breathing animation drives the symbol's own opa. */
        uint32_t opa = lv_obj_get_style_text_opa(s->obj, LV_PART_MAIN);
        for (lv_obj_t *o = s->obj; o != NULL; o = lv_obj_get_parent(o)) {
            opa = opa * lv_obj_get_style_opa(o, LV_PART_MAIN) / 255;
        }
        const uint32_t col24 = ((uint32_t)col.red << 16) | ((uint32_t)col.green << 8) | col.blue;

        /* Re-rasterise when anything that affects glyph placement changed. The box is included
         * because alignment only resolves on LVGL's first layout pass, after init set the text.
         * Compare against what was last read, never against s->live: a transparent slot would
         * otherwise re-rasterise every frame. */
        const bool shape = !s->primed || a.x1 != s->seen_x || a.y1 != s->seen_y ||
                           a.x2 - a.x1 != s->seen_w ||
                           strncmp(txt, s->seen_txt, sizeof(s->seen_txt) - 1) != 0 ||
                           (opa == 0) != (s->seen_opa == 0);
        if (!shape && col24 == s->seen_col && opa == s->seen_opa && !under_moved) {
            continue;
        }

        s->primed = true;
        strncpy(s->seen_txt, txt, sizeof(s->seen_txt) - 1);
        s->seen_txt[sizeof(s->seen_txt) - 1] = '\0';
        s->seen_x = a.x1;
        s->seen_y = a.y1;
        s->seen_w = a.x2 - a.x1;
        s->seen_col = col24;
        s->seen_opa = (uint8_t)opa;

        if (shape) {
            if (opa == 0) {
                s->live = false;
            } else {
                text_rasterise(s, ox, oy);
            }
            FERRO_PERF_SET(shape_changed);
        }
        text_build_lut(s, col, opa, ground);
        s->force = true;
    }
    text_ground = ground24;
    text_blob = blob24;

#if FERRO_PERF_LOG
    if (shape_changed) {
        /* Logs the inked pixel count so the device can be checked against the host harness. */
        uint32_t px = 0;
        for (int i = 0; i < FERRO_TEXT_COUNT; i++) {
            const struct text_slot *s = &text_slots[i];
            if (!s->live) {
                continue;
            }
            for (int r = 0; r < s->h; r++) {
                if (s->ink[r][1] >= s->ink[r][0]) {
                    px += s->ink[r][1] - s->ink[r][0] + 1;
                }
            }
        }
        LOG_INF("ferro text %d%d%d%d%d%d%d%d ink=%u", text_slots[0].live,
                text_slots[1].live, text_slots[2].live, text_slots[3].live,
                text_slots[4].live, text_slots[5].live, text_slots[6].live,
                text_slots[7].live, px);
    }
#endif
}

static void text_ship_box(int16_t x0, int16_t y0, int16_t w, int16_t h) {
    if (w <= 0 || h <= 0) {
        return;
    }
    const int cx1 = (x0 + w - 1) / CHUNK, cy1 = (y0 + h - 1) / CHUNK;
    for (int cy = y0 / CHUNK; cy <= cy1; cy++) {
        add_range(cy, x0 / CHUNK, cx1);
    }
}

/* Queues a repaint of every slot whose text changed, covering both its old box and its new one.
 * The old box matters because an auto-sized label that shrank leaves stale glyph pixels outside
 * the new box, and the mask diff would never repaint them. */
static void text_force_bands(void) {
    for (int i = 0; i < FERRO_TEXT_COUNT; i++) {
        struct text_slot *s = &text_slots[i];
        if (!s->force) {
            continue;
        }
        s->force = false;
        text_ship_box(s->sx0, s->sy0, s->sw, s->sh);
        if (s->live) {
            text_ship_box(s->x0, s->y0, s->w, s->h);
            s->sx0 = s->x0;
            s->sy0 = s->y0;
            s->sw = s->w;
            s->sh = s->h;
        } else {
            s->sw = s->sh = 0;
        }
    }
}

/* Dirty bands go straight to display_write, bypassing LVGL. LVGL's partial invalidation is
 * broken in this snapshot (lv_obj_area_is_visible clobbers the area), and its pipeline would
 * cost more than the pixels themselves. */
/* Big enough that a typical blob-change band (~5-6k px) goes out as one display_write. */
#define BLIT_BUF_PX (24 * PANEL_W)
#define BLIT_RECTS 96
static uint16_t blit_buf[BLIT_BUF_PX];

struct blit_rect {
    int16_t x1, y1, x2, y2;
};

#ifdef FERRO_HOST_HARNESS
uint64_t ferro_host_box_iters, ferro_host_ink_iters;
#endif

/* Composes one row of the final picture: the halftone fill, then every glyph crossing the row.
 * Both blit_bands and the host harness's reference image use this, so the two cannot drift
 * apart. */
static void compose_row(uint16_t *line, int py, int x1, int w, uint16_t bg, uint16_t fg) {
    const uint32_t *mr = mask[py / CHUNK];
    for (int i = 0; i < w; i++) {
        const int gx = (x1 + i) / CHUNK;
        line[i] = mr[gx >> 5] & (1u << (gx & 31)) ? fg : bg;
    }
    const int x2 = x1 + w - 1;
    for (int si = 0; si < FERRO_TEXT_COUNT; si++) {
        const struct text_slot *ts = &text_slots[si];
        if (!ts->live || py < ts->y0 || py >= ts->y0 + ts->h) {
            continue;
        }
        const int row = py - ts->y0;
        const int lo = ts->ink[row][0], hi = ts->ink[row][1];
#ifdef FERRO_HOST_HARNESS
        {   /* Count what a whole-box walk would have cost, so the harness can measure what the
             * ink extents save. */
            const int bxa = x1 > ts->x0 ? x1 : ts->x0;
            const int bxb = x2 < ts->x0 + ts->w - 1 ? x2 : ts->x0 + ts->w - 1;
            if (bxb >= bxa) ferro_host_box_iters += bxb - bxa + 1;
        }
#endif
        if (lo > hi) {
            continue;
        }
        const int xa = x1 > ts->x0 + lo ? x1 : ts->x0 + lo;
        const int xb = x2 < ts->x0 + hi ? x2 : ts->x0 + hi;
#ifdef FERRO_HOST_HARNESS
        if (xb >= xa) ferro_host_ink_iters += xb - xa + 1;
#endif
        const uint8_t *cov = ts->cov + row * ts->pitch;
        for (int px = xa; px <= xb; px++) {
            const int u = px - ts->x0;
            const uint8_t al = (cov[u >> 1] >> ((u & 1) ? 0 : 4)) & 0xF;
            if (al < TXT_INK_MIN) {
                continue;
            }
            uint16_t *dp = &line[px - x1];
            *dp = (*dp == fg ? ts->lut_fg : ts->lut_bg)[al];
        }
    }
}

BUILD_ASSERT(GLASS_W <= BLIT_BUF_PX, "one border row must fit blit_buf");
static void blit_solid(const struct device *dev, int x, int y, int w, int h, uint16_t c) {
    int chunk = BLIT_BUF_PX / w;
    for (int y0 = y; y0 < y + h; y0 += chunk) {
        int rows = y + h - y0;
        if (rows > chunk) {
            rows = chunk;
        }
        for (int i = 0; i < w * rows; i++) {
            blit_buf[i] = c;
        }
        struct display_buffer_descriptor desc = {
            .buf_size = (uint32_t)w * rows * 2,
            .width = w,
            .pitch = w,
            .height = rows,
        };
        ferro_panel_writes++;
        ferro_panel_px += (uint32_t)w * rows;
        display_write(dev, x, y0, &desc, blit_buf);
    }
}

static void blit_border(const struct device *dev, const lv_area_t *box, uint16_t bg) {
    if (box->y1 > 0) {
        blit_solid(dev, 0, 0, GLASS_W, box->y1, bg);
    }
    if (box->y2 + 1 < GLASS_H) {
        blit_solid(dev, 0, box->y2 + 1, GLASS_W, GLASS_H - box->y2 - 1, bg);
    }
    if (box->x1 > 0) {
        blit_solid(dev, 0, box->y1, box->x1, box->y2 - box->y1 + 1, bg);
    }
    if (box->x2 + 1 < GLASS_W) {
        blit_solid(dev, box->x2 + 1, box->y1, GLASS_W - box->x2 - 1,
                   box->y2 - box->y1 + 1, bg);
    }
}

static void blit_bands(const struct device *dev, int ox, int oy, uint16_t bg, uint16_t fg) {
    static struct blit_rect rects[BLIT_RECTS];
    int n_rects = 0;

    for (int b = 0; b < n_bands && n_rects < BLIT_RECTS; b++) {
        rects[n_rects++] = (struct blit_rect){bands[b].x1 * CHUNK, bands[b].y1 * CHUNK,
                                              bands[b].x2 * CHUNK + CHUNK - 1,
                                              bands[b].y2 * CHUNK + CHUNK - 1};
    }
    /* Sort top to bottom. If the panel's refresh crosses the writes mid-way, the tear is one
     * horizontal seam instead of a patchwork of stale rectangles. */
    for (int i = 1; i < n_rects; i++) {
        struct blit_rect key = rects[i];
        int j = i;
        for (; j > 0 && rects[j - 1].y1 > key.y1; j--) {
            rects[j] = rects[j - 1];
        }
        rects[j] = key;
    }

    for (int r = 0; r < n_rects; r++) {
        int w = rects[r].x2 - rects[r].x1 + 1;
        int chunk = BLIT_BUF_PX / w;
        for (int y0 = rects[r].y1; y0 <= rects[r].y2; y0 += chunk) {
            int rows = rects[r].y2 - y0 + 1;
            if (rows > chunk) {
                rows = chunk;
            }
            for (int dy = 0; dy < rows; dy++) {
                compose_row(&blit_buf[dy * w], y0 + dy, rects[r].x1, w, bg, fg);
            }
            struct display_buffer_descriptor desc = {
                .buf_size = (uint32_t)w * rows * 2,
                .width = w,
                .pitch = w,
                .height = rows,
            };
            ferro_panel_writes++;
            ferro_panel_px += (uint32_t)w * rows;
            display_write(dev, ox + rects[r].x1, oy + y0, &desc, blit_buf);
        }
    }
}

/* Band merging. Bands are merged cheapest pair first, for as long as the wasted area of a merge
 * is below the cost of the display_write it saves. Each band caches its cheapest partner; a
 * full rescan after every merge would be cubic. */
static int32_t pair_growth(int i, int j) {
    return band_growth(&bands[i], bands[j].y1, bands[j].y2, bands[j].x1, bands[j].x2) -
           (bands[j].x2 - bands[j].x1 + 1) * (bands[j].y2 - bands[j].y1 + 1);
}

static int32_t fold_g[FINE_BANDS];
static int8_t fold_j[FINE_BANDS];

/* Recomputes band b's cheapest partner after b changed. b may also have become some other
 * band's cheapest partner, so update those too. */
static void fold_rescan(int b) {
    fold_g[b] = INT32_MAX;
    fold_j[b] = -1;
    for (int o = 0; o < n_bands; o++) {
        if (o == b) {
            continue;
        }
        int32_t g = pair_growth(b, o);
        if (g < fold_g[b]) {
            fold_g[b] = g;
            fold_j[b] = o;
        }
        if (g < fold_g[o]) {
            fold_g[o] = g;
            fold_j[o] = b;
        }
    }
}

static void coalesce_bands(void) {
    for (int b = 0; b < n_bands; b++) {
        fold_g[b] = INT32_MAX;
        fold_j[b] = -1;
    }
    for (int i = 0; i < n_bands - 1; i++) {
        for (int j = i + 1; j < n_bands; j++) {
            int32_t g = pair_growth(i, j);
            if (g < fold_g[i]) {
                fold_g[i] = g;
                fold_j[i] = j;
            }
            if (g < fold_g[j]) {
                fold_g[j] = g;
                fold_j[j] = i;
            }
        }
    }
    for (;;) {
        int i = -1;
        int32_t best = BAND_MERGE_PX;
        for (int b = 0; b < n_bands; b++) {
            if (fold_g[b] < best) {
                best = fold_g[b];
                i = b;
            }
        }
        if (i < 0) {
            break;
        }
        int j = fold_j[i];
        if (bands[j].y1 < bands[i].y1) bands[i].y1 = bands[j].y1;
        if (bands[j].y2 > bands[i].y2) bands[i].y2 = bands[j].y2;
        if (bands[j].x1 < bands[i].x1) bands[i].x1 = bands[j].x1;
        if (bands[j].x2 > bands[i].x2) bands[i].x2 = bands[j].x2;
        int last = --n_bands;
        bands[j] = bands[last];
        fold_g[j] = fold_g[last];
        fold_j[j] = fold_j[last];
        for (int b = 0; b < n_bands; b++) {
            if (fold_j[b] == last) {
                fold_j[b] = j;
            }
        }
        if (i == last) {
            i = j;
        }
        fold_rescan(i);
        for (int b = 0; b < n_bands; b++) {
            if (b != i && (fold_j[b] == i || fold_j[b] == j)) {
                fold_rescan(b);
            }
        }
    }
}

/* Stores nrow into the mask and queues a dirty range for every run of cells that changed. */
static void mask_diff_row(int y, const uint32_t *nrow) {
    uint32_t *orow = mask[y];
    int run0 = -1, last = 0;

    for (int w = 0; w < MASK_W; w++) {
        uint32_t x = orow[w] ^ nrow[w];
        orow[w] = nrow[w];
        while (x) {
            int px = w * 32 + __builtin_ctz(x);
            x &= x - 1;
            if (run0 < 0) {
                run0 = px;
            } else if (px > last + 1 + DIFF_GAP) {
                add_range(y, run0, last);
                run0 = px;
            }
            last = px;
        }
    }
    if (run0 >= 0) {
        add_range(y, run0, last);
    }
}

struct term {
    float cx, cy;     /* Track centroid, widget-local pixels */
    float dx, dy;     /* Unit track direction */
    float ka;         /* LUT bins per px^2, mean q over taps */
    float u0, inv_bin; /* Profile window: index = (u - u0) * inv_bin */
    float vr;          /* |v| beyond this reads only the zero far-field bin */
    int pb_max;        /* Bins at or past this are zero (build never wrote them) */
    /* Per-frame constants for the row loop; only Cv and Cu below depend on y. */
    float ka_q;        /* ka * 2^TQ */
    float dyS;         /* dy * SAMPLE_STEP */
    float u_c0, u_c1;  /* u = u_c0 - u_c1 * Cv, both in Q13 bins */
    float dpb;         /* Profile bins per sample */
    float pf_span;     /* piN = pi0 + pf_span */
    float inv_vS, inv_dpb; /* Window inverses; 0 flags a degenerate axis */
    float vend_off;    /* vend = Cv - vend_off */
    int32_t du_i, dpi_i;
};

/* Fills prof[idx] with the along-track profile: the mean over taps of exp(-(u - o_k)^2 tk_k).
 * Each tap is added only over the bins where it is non-zero, stepping the exponent as an
 * integer parabola. PROF_EQ gives it more fraction bits than the LUT index (TQ) needs, which
 * keeps the accumulated rounding under one bin across the widest window. */
#define PROF_EQ (TQ + 6)
static int32_t prof_acc[PROF_N];
static void build_profile(int idx, struct term *t, const float *o, const float *tk, int ntaps) {
    float omin = o[0], omax = o[0], kmin = tk[0];
    for (int k = 1; k < ntaps; k++) {
        omin = o[k] < omin ? o[k] : omin;
        omax = o[k] > omax ? o[k] : omax;
        kmin = tk[k] < kmin ? tk[k] : kmin;
    }
    float reach = sqrtf((float)EXP_LUT_N / kmin);
    float span = (omax - omin) + 2.0f * reach;
    float bin = span / (PROF_N - 2);
    bin = bin > 1.0f ? bin : 1.0f;
    t->u0 = omin - reach - bin;
    t->inv_bin = 1.0f / bin;
    memset(prof[idx], 0, sizeof(prof[idx]));
    memset(prof_blk[idx], 0, sizeof(prof_blk[idx]));
    int imax = (int)(span / bin) + 2;
    imax = imax > PROF_N - 1 ? PROF_N - 1 : imax;
    t->pb_max = imax;
    t->vr = sqrtf((float)EXP_LUT_N / t->ka);
    memset(prof_acc, 0, (size_t)imax * sizeof(prof_acc[0]));
    for (int k = 0; k < ntaps; k++) {
        float rk = sqrtf((float)EXP_LUT_N / tk[k]);
        int i0 = (int)((o[k] - rk - t->u0) * t->inv_bin);
        int i1 = (int)((o[k] + rk - t->u0) * t->inv_bin) + 1;
        i0 = i0 < 1 ? 1 : i0;
        i1 = i1 > imax - 1 ? imax - 1 : i1;
        float d0 = t->u0 + (float)i0 * bin - o[k];
        float kq = tk[k] * (float)(1 << PROF_EQ);
        int32_t e = (int32_t)(kq * d0 * d0);
        int32_t de = (int32_t)(kq * bin * (2.0f * d0 + bin));
        const int32_t dde = (int32_t)(2.0f * kq * bin * bin);
        for (int i = i0; i <= i1; i++) {
            int32_t bi = e >> PROF_EQ;
            bi = bi < 0 ? 0 : (bi > EXP_LUT_N - 1 ? EXP_LUT_N - 1 : bi);
            prof_acc[i] += exp_lut16[bi + 1];
            e += de;
            de += dde;
        }
    }
    const float w = 32767.0f / (8192.0f * (float)ntaps);
    for (int i = 1; i < imax; i++) {
        uint16_t pq = (uint16_t)((float)prof_acc[i] * w + 0.5f);
        prof[idx][i] = pq;
        if (pq > prof_blk[idx][i / PROF_BLK]) {
            prof_blk[idx][i / PROF_BLK] = pq;
        }
    }
}

/* Stage timing inside ferro_update, read from the Cortex-M DWT cycle counter. */
#ifdef FERRO_HOST_HARNESS
/* No DWT on the host. */
static uint32_t ferro_dwt_dummy[3];
#define FERRO_DWT_CYCCNT (ferro_dwt_dummy[0])
#define FERRO_DWT_CTRL (ferro_dwt_dummy[1])
#define FERRO_DEMCR (ferro_dwt_dummy[2])
#else
#define FERRO_DWT_CYCCNT (*(volatile uint32_t *)0xE0001004)
#define FERRO_DWT_CTRL (*(volatile uint32_t *)0xE0001000)
#define FERRO_DEMCR (*(volatile uint32_t *)0xE000EDFC)
#endif
#if FERRO_PERF_LOG
static uint32_t perf_stage[5];
static uint32_t perf_mark_last;
#endif
static inline void ferro_perf_mark(int stage) {
#if FERRO_PERF_LOG
    uint32_t now = FERRO_DWT_CYCCNT;
    if (stage >= 0) {
        perf_stage[stage] += now - perf_mark_last;
    }
    perf_mark_last = now;
#else
    (void)stage;
#endif
}

static void ferro_update(void) {
    ferro_perf_mark(-1);
    static bool first_frame = true;
    if (first_frame) {
        LOG_INF("ferro: first frame");
        first_frame = false;
    }

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
#if FERRO_BENCH
    intensity = 0.5f;
#endif

    update_touch(now, frame_ms);

    static float field_accum_ms = 0.0f;
    const float rate_floor =
        last_timer_period == TIMER_PERIOD_2HZ ? FIELD_RATE_DEEP : FIELD_RATE_IDLE;
    const float field_rate = rate_floor + intensity * (FIELD_RATE_TYPING - rate_floor);

#if FERRO_BENCH
    /* Advance by a fixed amount per frame, not by wall time, so both builds under comparison
     * see the same pole positions on the same frame. Otherwise a slower build gets more field
     * time per frame, more dirty area, and slows down further. */
    field_accum_ms += TIMER_PERIOD_30HZ * field_rate;
#else
    field_accum_ms += MIN(frame_ms * field_rate, (float)FIELD_MS_PER_FRAME_MAX);
#endif
    while (field_accum_ms >= FLUX_POLES_STEP_MS) {
        flux_poles_step();
        smooth_ring_push();
        field_accum_ms -= FLUX_POLES_STEP_MS;
    }
    smooth_alpha = field_accum_ms / FLUX_POLES_STEP_MS;

    struct zmk_widget_ferro_blobs *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        lv_area_t widget_coords;
        lv_obj_get_coords(widget->obj, &widget_coords);
        text_refresh(widget_coords.x1, widget_coords.y1);
        break;
    }
    n_bands = 0;
    ferro_perf_mark(0);

    struct term terms[POLE_COUNT + 1];
    int n_terms = 0;

    /* Convert each pole to grid coordinates here; nothing after this knows about pixels. */
    const float px_k = EXP_IDX_SCALE / (2.0f * R0 * R0 * GRID_SCALE * GRID_SCALE);
    const float tap_back = ((float)SMOOTH_WIN_MS / (SMOOTH_TAPS - 1)) / FLUX_POLES_STEP_MS;
    for (int j = 0; j < POLE_COUNT; j++) {
        float tx[SMOOTH_TAPS], ty[SMOOTH_TAPS], tk[SMOOTH_TAPS], o[SMOOTH_TAPS];
        float cx = 0, cy = 0, qbar = 0;
        for (int k = 0; k < SMOOTH_TAPS; k++) {
            struct pole_snap s = ring_at(j, (1.0f - smooth_alpha) + k * tap_back);
            tx[k] = (s.x - (LAT_CX - BBOX_W / 2.0f)) * GRID_SCALE - GRID_OFF;
            ty[k] = (s.y - (LAT_CY - BBOX_H / 2.0f)) * GRID_SCALE - CROP_Y / (float)CHUNK -
                    GRID_OFF;
            float q = fabsf(s.q);
            q = q > Q_FLOOR ? q : Q_FLOOR;
            tk[k] = px_k / q;
            cx += tx[k] * (1.0f / SMOOTH_TAPS);
            cy += ty[k] * (1.0f / SMOOTH_TAPS);
            qbar += q * (1.0f / SMOOTH_TAPS);
        }
        float ddx = tx[0] - tx[SMOOTH_TAPS - 1], ddy = ty[0] - ty[SMOOTH_TAPS - 1];
        float dl = sqrtf(ddx * ddx + ddy * ddy);
        struct term *t = &terms[n_terms];
        t->cx = cx;
        t->cy = cy;
        t->dx = dl > 1e-6f ? ddx / dl : 1.0f;
        t->dy = dl > 1e-6f ? ddy / dl : 0.0f;
        t->ka = px_k / qbar;
        for (int k = 0; k < SMOOTH_TAPS; k++) {
            o[k] = (tx[k] - cx) * t->dx + (ty[k] - cy) * t->dy;
        }
        build_profile(n_terms, t, o, tk, SMOOTH_TAPS);
        n_terms++;
    }

    /* Add the touch term. Below Q_FLOOR it is skipped: the fixed-point splat cannot represent a
     * narrower Gaussian, and the ramp spends less than a frame down there anyway. */
    float touch_q = TOUCH_CHARGE * touch_level;
    if (touch_q > Q_FLOOR) {
        struct term *t = &terms[n_terms];
        float o0 = 0.0f, k0 = px_k / touch_q;
        t->cx = touch_px / (float)CHUNK - GRID_OFF;
        t->cy = touch_py / (float)CHUNK - GRID_OFF;
        t->dx = 1.0f;
        t->dy = 0.0f;
        t->ka = k0;
        build_profile(n_terms, t, &o0, &k0, 1);
        n_terms++;
    }

    for (int t = 0; t < n_terms; t++) {
        struct term *tm = &terms[t];
        const float S = SAMPLE_STEP;
        const float row_end = (float)((SAMPLE_N - 1) * SAMPLE_STEP);
        tm->ka_q = tm->ka * (float)(1 << TQ);
        tm->dyS = tm->dy * S;
        tm->u_c0 = tm->ka_q * tm->dyS * tm->dyS;
        tm->u_c1 = 2.0f * tm->ka_q * tm->dyS;
        tm->du_i = (int32_t)(2.0f * tm->u_c0);
        tm->dpb = tm->dx * S * tm->inv_bin;
        tm->dpi_i = (int32_t)(tm->dpb * 256.0f);
        tm->pf_span = tm->dx * row_end * tm->inv_bin;
        tm->inv_vS = tm->dy != 0.0f ? 1.0f / tm->dyS : 0.0f;
        tm->inv_dpb = tm->dpb != 0.0f ? 1.0f / tm->dpb : 0.0f;
        tm->vend_off = tm->dy * row_end;
    }
    ferro_perf_mark(1);

    for (int y = 0; y < GRID_H; y++) {
        uint32_t mrow[MASK_W] = {0};

        /* Field samples along this row, one per SAMPLE_STEP cells. */
        int32_t F[TILE_N * 8];
        struct live_term {
            int32_t tv, u, du;  /* Across exponent: a row parabola in Q13 LUT bins */
            int32_t pi, dpi;    /* Profile index, Q8 bins, clamped at both ends */
            const uint16_t *pr;
            const uint16_t *blk; /* Block maxima of pr, for the faintness cull */
            int n0, n1;         /* Active sample window; state is taken at n0 */
        } live[POLE_COUNT + 1];
        int n_row = 0;
        int umin = SAMPLE_N, umax = -1;
        for (int t = 0; t < n_terms; t++) {
            const struct term *tm = &terms[t];
            float ry = y - tm->cy;
            /* Row cull, in float. This must run before anything is cast to fixed point: a term
             * that survives it has its minimum exponent within the LUT, which bounds the row's
             * end values to ~184k bins, a third of int32 in Q13. Without the cull a touch
             * coordinate off the panel or a runaway pole would overflow the fixed-point path. */
            float Cv = tm->dy * tm->cx + tm->dx * ry;
            float vend = Cv - tm->vend_off;
            float vmin = (Cv > 0.0f) != (vend > 0.0f) ? 0.0f
                         : FMINF(fabsf(Cv), fabsf(vend));
            float ev_min = tm->ka * vmin * vmin;
            if (ev_min > ROW_CUT_T * EXP_IDX_SCALE || ev_min > (float)EXP_LUT_N) {
                continue;
            }
            float Cu = tm->dy * ry - tm->dx * tm->cx;
            float pi0 = (Cu - tm->u0) * tm->inv_bin;
            float piN = pi0 + tm->pf_span;
            if ((pi0 < 0.0f && piN < 0.0f) ||
                (pi0 > PROF_N - 1 && piN > PROF_N - 1)) {
                continue;
            }
            /* Narrow the term to the samples where it reads non-zero LUT bins. Outside that
             * window it adds exactly zero, so skipping it changes nothing. Both windows include
             * a bin of margin for fixed-point drift. */
            float n_lo = 0.0f, n_hi = (float)(SAMPLE_N - 1);
            if (tm->dy != 0.0f) {
                float a = (Cv - tm->vr) * tm->inv_vS, b = (Cv + tm->vr) * tm->inv_vS;
                n_lo = FMAXF(n_lo, FMINF(a, b));
                n_hi = FMINF(n_hi, FMAXF(a, b));
            }
            if (tm->dpb != 0.0f) {
                float a = (-1.0f - pi0) * tm->inv_dpb;
                float b = ((float)tm->pb_max + 1.0f - pi0) * tm->inv_dpb;
                n_lo = FMAXF(n_lo, FMINF(a, b));
                n_hi = FMINF(n_hi, FMAXF(a, b));
            } else if (pi0 < -1.0f || pi0 > (float)tm->pb_max + 1.0f) {
                continue;
            }
            if (n_lo > n_hi) {
                continue;
            }
            const int n0 = (int)n_lo;
            /* Start the fixed-point parabola at n0 directly rather than stepping from sample 0.
             * Cv0 is a value on this row, so the overflow bound from the cull still holds. */
            float Cv0 = Cv - tm->dyS * (float)n0;
            live[n_row].tv = (int32_t)(tm->ka_q * Cv0 * Cv0);
            live[n_row].u = (int32_t)(tm->u_c0 - tm->u_c1 * Cv0);
            live[n_row].du = tm->du_i;
            live[n_row].pi = (int32_t)((pi0 + (float)n0 * tm->dpb) * 256.0f);
            live[n_row].dpi = tm->dpi_i;
            live[n_row].pr = prof[t];
            live[n_row].blk = prof_blk[t];
            live[n_row].n0 = n0;
            live[n_row].n1 = (int)n_hi;
            if (n0 < umin) umin = n0;
            if (live[n_row].n1 > umax) umax = live[n_row].n1;
            n_row++;
        }
        /* Only clear and scan the part of the row any term touches, plus one guard sample each
         * side. Everything outside is zero, so the result equals a full-row scan. */
        const int w0 = umin > 0 ? umin - 1 : 0;
        const int w1 = umax + 1 > SAMPLE_N - 1 ? SAMPLE_N - 1 : umax + 1;
        if (n_row > 0) {
            memset(F + w0, 0, (size_t)(w1 - w0 + 1) * sizeof(F[0]));
            const uint16_t *lut = exp_lut16 + 1;
            for (int t = 0; t < n_row; t++) {
                int32_t tv = live[t].tv, u = live[t].u, pi = live[t].pi;
                const int32_t du = live[t].du, dpi = live[t].dpi;
                const uint16_t *pr = live[t].pr;
                const uint16_t *blk = live[t].blk;
                int32_t *Fp = F + live[t].n0;
                int left = live[t].n1 - live[t].n0 + 1;
                while (left > 0) {
                    const int c = left < 8 ? left : 8;
                    /* Skip this tile if the term cannot exceed exp(-ROW_CUT_T) anywhere in it.
                     * The bound is the across factor at the parabola's minimum over the tile
                     * times the profile's block maximum over the bins the tile can read. Both
                     * over-estimate, so nothing visible is ever skipped. */
                    const int32_t ue = u + (c - 1) * du;
                    int32_t tmin =
                        u >= 0 ? tv
                               : (ue <= 0 ? tv + (c - 1) * u + ((c - 1) * (c - 2) / 2) * du : 0);
                    int32_t bi = tmin >> TQ;
                    bi = bi < 0 ? 0 : (bi > EXP_LUT_N - 1 ? EXP_LUT_N - 1 : bi);
                    int32_t pa = pi >> 8, pz = (pi + (c - 1) * dpi) >> 8;
                    int32_t plo = pa < pz ? pa : pz, phi = pa > pz ? pa : pz;
                    plo = plo < 0 ? 0 : (plo > PROF_N - 1 ? PROF_N - 1 : plo);
                    phi = phi < 0 ? 0 : (phi > PROF_N - 1 ? PROF_N - 1 : phi);
                    uint32_t pm = blk[plo / PROF_BLK];
                    if (blk[phi / PROF_BLK] > pm) {
                        pm = blk[phi / PROF_BLK];
                    }
                    if ((uint32_t)lut[bi] * pm < (uint32_t)tile_cut_q) {
                        /* Advance the parabola over the tile in closed form. Gives the same
                         * values as stepping, so the overflow bound still holds. */
                        tv += c * u + ((c * (c - 1)) / 2) * du;
                        u = ue + du;
                        pi += c * dpi;
                        Fp += c;
                        left -= c;
                        FERRO_PERF_COUNT(perf_tile_skip);
                        continue;
                    }
                    FERRO_PERF_COUNT(perf_tile_run);
                    for (int k = 0; k < c; k++) {
                        int32_t bs = tv >> TQ;
                        bs = bs > EXP_LUT_N - 1 ? EXP_LUT_N - 1 : bs; /* -1: guard bin */
                        int32_t pb = pi >> 8;
                        pb = pb < 0 ? 0 : (pb > PROF_N - 1 ? PROF_N - 1 : pb); /* zero bins */
                        *Fp++ += (int32_t)((uint32_t)lut[bs] * pr[pb]);
                        tv += u;
                        u += du;
                        pi += dpi;
                    }
                    left -= c;
                }
            }
            ferro_perf_mark(2);
            /* Halftone fill. F is interpolated linearly between samples, one add per cell, and
             * each cell is compared with its Bayer threshold. A segment with both ends below the
             * smallest threshold lights nothing and is skipped whole; one with both ends above
             * the largest is lit whole. The last segment may extend past the final sample, so
             * its skip test uses the interpolated value at xe rather than F[i+1]. */
            const int32_t *thr = dith_thr[y & 7];
            const int px_end = w1 == SAMPLE_N - 1 ? GRID_W - 1 : w1 * SAMPLE_STEP;
            int x = w0 * SAMPLE_STEP;
            for (int i = w0; i < w1; i++) {
                const int xe = i + 1 < w1 ? x + SAMPLE_STEP - 1 : px_end;
                const int32_t fe = F[i] + (((F[i + 1] - F[i]) >> 2) * (xe - x));
                if (F[i] <= dith_min && fe <= dith_min) {
                    x = xe + 1;
                    continue;
                }
                if (F[i] > dith_sat && F[i + 1] > dith_sat && xe - x == SAMPLE_STEP - 1) {
                    mask_set_range(mrow, x, xe);
                    x = xe + 1;
                    continue;
                }
                int32_t fa = F[i];
                const int32_t d4 = (F[i + 1] - fa) >> 2;
                for (; x <= xe; x++) {
                    if (fa > thr[x & 7]) {
                        mrow[x >> 5] |= 1u << (x & 31);
                    }
                    fa += d4;
                }
            }
        }
        mask_diff_row(y, mrow);
        ferro_perf_mark(3);
    }
    text_force_bands();
    if (force_full) {
        force_full = false;
#ifdef FERRO_HOST_HARNESS
        /* The host harness disables the full repaint to prove its stale-pixel test would catch
         * a missing one. */
        if (!ferro_host_no_force_full)
#endif
        {
            n_bands = 1;
            bands[0] = (struct band){0, GRID_H - 1, 0, GRID_W - 1};
        }
    }
    coalesce_bands();
    ferro_perf_mark(4);
}

/* Runs at 30Hz regardless of tier. timer_cb only re-picks its rate when it fires, so at the 2Hz
 * tier a touch or a foreign panel write would wait up to half a second; this fires the animation
 * timer as soon as one appears. */
static void contact_poll_cb(lv_timer_t *timer) {
    if (prospector_panel_writes != panel_writes_seen) {
        lv_timer_ready(animation_timer);
        return;
    }
    if (!contact_pressed || last_timer_period == TIMER_PERIOD_30HZ) {
        return;
    }

    lv_timer_set_period(animation_timer, TIMER_PERIOD_30HZ);
    last_timer_period = TIMER_PERIOD_30HZ;
    lv_timer_ready(animation_timer);
}

/* Bench log. When comparing two builds, check that ranges and tiles run match first; if they do
 * not, the builds rendered different frames and the timings are not comparable. */
#if FERRO_PERF_LOG
static void ferro_perf_window(uint32_t now, uint32_t upd_dt, uint32_t blit_dt) {
    static uint32_t panel_writes_at_window, panel_px_at_window;
    static uint32_t ferro_writes_at_window, ferro_px_at_window;
    static uint32_t upd_sum, upd_max, blit_sum, upd_n;
    static uint32_t win_start, band_sum, band_max, blit_max;
    upd_sum += upd_dt;
    blit_sum += blit_dt;
    if (blit_dt > blit_max) blit_max = blit_dt;
    if (upd_dt > upd_max) upd_max = upd_dt;
    band_sum += n_bands;
    if ((uint32_t)n_bands > band_max) band_max = n_bands;
    if (win_start == 0) {
        win_start = now;
        panel_writes_at_window = prospector_panel_writes;
        panel_px_at_window = prospector_panel_px;
        ferro_writes_at_window = ferro_panel_writes;
        ferro_px_at_window = ferro_panel_px;
    }
    if (++upd_n == 60) {
        uint32_t win_ms = k_uptime_get_32() - win_start;
        static uint32_t win_i;
        win_i++;
        LOG_INF("ferro bench w=%u fps=%u.%02u bands avg=%u.%02u max=%u ranges=%u blit max=%u us",
                win_i, 60000 / win_ms, (6000000 / win_ms) % 100, band_sum / 60,
                (band_sum * 100 / 60) % 100, band_max, perf_ranges / 60, blit_max * 31);
        win_start = 0;
        band_sum = band_max = blit_max = 0;
        LOG_INF("ferro upd avg=%u max=%u blit avg=%u us | foreign: repairs=%u win w=%d px=%d",
                (upd_sum * 31) / 60, upd_max * 31, (blit_sum * 31) / 60,
                panel_foreign_repairs,
                (int)((prospector_panel_writes - panel_writes_at_window) -
                      (ferro_panel_writes - ferro_writes_at_window)),
                (int)((prospector_panel_px - panel_px_at_window) -
                      (ferro_panel_px - ferro_px_at_window)));
        LOG_INF("ferro stp=%u prf=%u spl=%u ext=%u fld=%u us | tiles run=%u skip=%u",
                perf_stage[0] / (60 * 64), perf_stage[1] / (60 * 64), perf_stage[2] / (60 * 64),
                perf_stage[3] / (60 * 64), perf_stage[4] / (60 * 64), perf_tile_run / 60,
                perf_tile_skip / 60);
        memset(perf_stage, 0, sizeof(perf_stage));
        perf_tile_run = perf_tile_skip = perf_ranges = 0;
        upd_sum = upd_max = blit_sum = upd_n = 0;
    }
}
#else
static inline void ferro_perf_window(uint32_t now, uint32_t upd_dt, uint32_t blit_dt) {
    (void)now;
    (void)upd_dt;
    (void)blit_dt;
}
#endif

/* The border (panel area outside the widget) as last painted. It is repainted only when the
 * widget box or the ground colour changes. bg is 32-bit so the initial value cannot equal any
 * real RGB565 colour. */
static struct {
    lv_area_t box;
    uint32_t bg;
} border_shipped = {.bg = 0xFFFFFFFF};

static void timer_cb(lv_timer_t *timer) {
    if (prospector_panel_writes != panel_writes_seen) {
        force_full = true;
        panel_foreign_repairs++;
    }

    uint32_t now = k_uptime_get_32();
    uint32_t idle_ms = now - last_keypress_time;

    uint32_t target_period;
    if (current_wpm > 0 || contact_pressed || touch_level > 0.0f) {
        target_period = TIMER_PERIOD_30HZ;
    } else if (animation_started && idle_ms < DEEP_IDLE_AFTER_MS) {
        target_period = TIMER_PERIOD_15HZ;
    } else {
        target_period = TIMER_PERIOD_2HZ;
    }

#if FERRO_BENCH
    target_period = TIMER_PERIOD_30HZ;
#endif

    if (target_period != last_timer_period) {
        lv_timer_set_period(animation_timer, target_period);
        last_timer_period = target_period;
    }

    uint32_t t0 = k_cycle_get_32();
    ferro_update();
    uint32_t t1 = k_cycle_get_32();

    uint16_t fg = __builtin_bswap16(lv_color_to_u16(lv_color_hex(ferro_palette_active()->blob)));
    uint16_t bg = __builtin_bswap16(lv_color_to_u16(lv_obj_get_style_bg_color(lv_screen_active(), LV_PART_MAIN)));

    struct zmk_widget_ferro_blobs *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        lv_area_t coords;
        lv_obj_get_coords(widget->obj, &coords);
        const struct device *panel = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
        if (border_shipped.bg != bg || memcmp(&border_shipped.box, &coords, sizeof(coords))) {
            blit_border(panel, &coords, bg);
            border_shipped.box = coords;
            border_shipped.bg = bg;
        }
        blit_bands(panel, coords.x1, coords.y1, bg, fg);
        break;
    }

    panel_writes_seen = prospector_panel_writes;

    ferro_perf_window(now, t1 - t0, k_cycle_get_32() - t1);
}

#ifdef FERRO_HOST_HARNESS
/* Host harness only. Composes the whole widget from the current mask and glyph rasters. The
 * harness checks that the panel, built up from dirty bands, equals this; that verifies the dirty
 * tracking, not the rendering itself. */
void ferro_host_reference(uint16_t *out, int stride) {
    const uint16_t fg =
        __builtin_bswap16(lv_color_to_u16(lv_color_hex(ferro_palette_active()->blob)));
    const uint16_t bg = __builtin_bswap16(
        lv_color_to_u16(lv_obj_get_style_bg_color(lv_screen_active(), LV_PART_MAIN)));
    for (int y = 0; y < PANEL_H; y++) {
        compose_row(out + (size_t)y * stride, y, 0, PANEL_W, bg, fg);
    }
}

/* Independent implementation of ferro_host_reference that walks whole slot boxes instead of
 * ink extents. The harness diffs the two. */
void ferro_host_reference_slow(uint16_t *out, int stride) {
    const uint16_t fg =
        __builtin_bswap16(lv_color_to_u16(lv_color_hex(ferro_palette_active()->blob)));
    const uint16_t bg = __builtin_bswap16(
        lv_color_to_u16(lv_obj_get_style_bg_color(lv_screen_active(), LV_PART_MAIN)));

    for (int y = 0; y < PANEL_H; y++) {
        uint16_t *line = out + (size_t)y * stride;
        const uint32_t *mr = mask[y / CHUNK];
        for (int x = 0; x < PANEL_W; x++) {
            const int gx = x / CHUNK;
            line[x] = mr[gx >> 5] & (1u << (gx & 31)) ? fg : bg;
        }
        for (int si = 0; si < FERRO_TEXT_COUNT; si++) {
            const struct text_slot *ts = &text_slots[si];
            if (!ts->live || y < ts->y0 || y >= ts->y0 + ts->h) {
                continue;
            }
            const uint8_t *cov = ts->cov + (y - ts->y0) * ts->pitch;
            for (int u = 0; u < ts->w; u++) {
                const int x = ts->x0 + u;
                if (x < 0 || x >= PANEL_W) {
                    continue;
                }
                const uint8_t al = (cov[u >> 1] >> ((u & 1) ? 0 : 4)) & 0xF;
                if (al < TXT_INK_MIN) {
                    continue;
                }
                line[x] = (line[x] == fg ? ts->lut_fg : ts->lut_bg)[al];
            }
        }
    }
}

void ferro_host_slot_boxes(void) {
    for (int i = 0; i < FERRO_TEXT_COUNT; i++) {
        const struct text_slot *s = &text_slots[i];
        LOG_INF("slot %d live=%d box=%d,%d %dx%d shipped=%d,%d %dx%d txt='%s'", i, s->live,
                s->x0, s->y0, s->w, s->h, s->sx0, s->sy0, s->sw, s->sh, s->seen_txt);
    }
}
#endif

int zmk_widget_ferro_blobs_init(struct zmk_widget_ferro_blobs *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(widget->obj);
    lv_obj_set_size(widget->obj, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0);

    sys_slist_append(&widgets, &widget->node);

    if (animation_timer == NULL) {
        exp_lut[0] = 1.0f;
        for (int i = 0; i < EXP_LUT_N; i++) {
            exp_lut[i + 1] = expf(-i * (EXP_MAX / EXP_LUT_N));
        }
        /* The splat clamps out-of-range indices to this bin instead of branching, so it must be
         * exactly zero. */
        exp_lut[EXP_LUT_N] = 0.0f;
        for (int i = 0; i <= EXP_LUT_N; i++) {
            exp_lut16[i] = (uint16_t)(exp_lut[i] * EXP_ONE_Q13 + 0.5f);
        }
        tile_cut_q = (int32_t)(expf(-ROW_CUT_T) * 32767.0f * EXP_ONE_Q13);
        /* 8x8 Bayer matrix built as 4*bay4 + bay2. Thresholds are stored in F's own fixed-point
         * scale so the fill loop is a single compare per cell. */
        static const uint8_t bay2[2][2] = {{0, 2}, {3, 1}};
        static const uint8_t bay4[4][4] = {
            {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
        const int32_t iso_q = (int32_t)(ISO * 32767.0f) * EXP_ONE_Q13;
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 8; j++) {
                int b = 4 * bay4[i & 3][j & 3] + bay2[i >> 2][j >> 2];
                dith_thr[i][j] = iso_q + (int32_t)(((int64_t)iso_q * (2 * b + 1)) / 128);
            }
        }
        dith_min = dith_thr[0][0]; /* b=0 lands at [0][0] in this construction */
        dith_sat = dith_thr[7][0] + 3; /* b=63 */
        text_slots_init();
#if FERRO_PERF_LOG
        FERRO_DEMCR |= 1u << 24;  /* TRCENA */
        FERRO_DWT_CTRL |= 1u;     /* CYCCNTENA */
#endif
        flux_poles_init();
        smooth_ring_prime();
        /* Stop LVGL painting anything: ferro is the only writer to the panel. Layout still runs,
         * since lv_obj_mark_layout_as_dirty does not go through invalidation. LVGL counts these
         * calls, so this must run exactly once. */
        lv_display_enable_invalidation(lv_display_get_default(), false);
        animation_timer = lv_timer_create(timer_cb, TIMER_PERIOD_30HZ, NULL);
    }
    if (IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_FIELD_POLE) && contact_poll_timer == NULL) {
        contact_poll_timer = lv_timer_create(contact_poll_cb, TIMER_PERIOD_30HZ, NULL);
    }

    return 0;
}

lv_obj_t *zmk_widget_ferro_blobs_obj(struct zmk_widget_ferro_blobs *widget) {
    return widget->obj;
}

/* Hides the label and its parents up to the screen. A hidden object is still laid out, so ferro
 * can keep reading its box, and its opacity still animates, so fades still work. It is the
 * disabled invalidation in init that actually stops LVGL painting, not the hidden flag. */
void zmk_widget_ferro_blobs_set_text(enum ferro_text_slot slot, lv_obj_t *label) {
    if (slot < 0 || slot >= FERRO_TEXT_COUNT) {
        return;
    }
    text_slots[slot].obj = label;
    text_slots[slot].live = false;
    text_slots[slot].primed = false;

    /* Not lv_screen_active(): ZMK loads the status screen only after zmk_display_status_screen()
     * returns, so during init the active screen is a different object and the walk would run
     * past ours to the root. */
    lv_obj_t *screen = lv_obj_get_screen(label);
    for (lv_obj_t *o = label; o != NULL && o != screen; o = lv_obj_get_parent(o)) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Just wakes the timer. text_refresh re-reads every slot each frame anyway; without this a
 * modifier press at the 2Hz tier would wait for the next 2Hz tick to show. */
void zmk_widget_ferro_blobs_text_dirty(void) {
    if (animation_timer != NULL) {
        lv_timer_ready(animation_timer);
    }
}

void zmk_widget_ferro_blobs_set_contact(int16_t screen_x, int16_t screen_y, bool pressed) {
    k_spinlock_key_t key = k_spin_lock(&contact_lock);

    if (pressed != contact_pressed) {
        contact_changed_at = k_uptime_get_32();
    }
    contact_pressed = pressed;
    if (pressed) {
        contact_x = screen_x;
        contact_y = screen_y;
    }

    k_spin_unlock(&contact_lock, key);
}
