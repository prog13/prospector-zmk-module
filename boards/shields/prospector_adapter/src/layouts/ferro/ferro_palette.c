#include "ferro_palette.h"

#include <zephyr/kernel.h>

static uint8_t active_index = CONFIG_PROSPECTOR_FERRO_PALETTE_INDEX;
static void (*reskin_cb)(void);

BUILD_ASSERT(CONFIG_PROSPECTOR_FERRO_PALETTE_INDEX < FERRO_PALETTE_COUNT,
             "the Kconfig choice maps to an index past the table");

const struct ferro_palette *ferro_palette_active(void) { return &ferro_palettes[active_index]; }

uint8_t ferro_palette_active_index(void) { return active_index; }

void ferro_palette_set_reskin_cb(void (*cb)(void)) { reskin_cb = cb; }

void ferro_palette_set(uint8_t index) {
    if (index >= FERRO_PALETTE_COUNT || index == active_index) {
        return;
    }
    active_index = index;
    if (reskin_cb != NULL) {
        reskin_cb();
    }
}

void ferro_palette_next(void) { ferro_palette_set((active_index + 1) % FERRO_PALETTE_COUNT); }
