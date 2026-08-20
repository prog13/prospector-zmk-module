#pragma once

#include <stdbool.h>
#include <stdint.h>

/* All colours sRGB. blob and dim are computed from ground and bright by the generator that
 * writes ferro_palette_table.c; do not hand-tune them. */
struct ferro_palette {
    const char *name;
    uint32_t ground; /* empty field, and the screen background the fluid reads its bg from */
    uint32_t blob;
    uint32_t dim;    /* the idle half of every two-tone indicator */
    uint32_t bright; /* all text and icons */
};

#define FERRO_PALETTE_COUNT 6

extern const struct ferro_palette ferro_palettes[FERRO_PALETTE_COUNT];

const struct ferro_palette *ferro_palette_active(void);
uint8_t ferro_palette_active_index(void);

void ferro_palette_set(uint8_t index);
void ferro_palette_next(void);

/* Called after every palette switch; the layout re-applies its colours in it. */
void ferro_palette_set_reskin_cb(void (*cb)(void));
