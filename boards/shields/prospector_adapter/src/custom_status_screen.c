#include <lvgl.h>

#ifdef CONFIG_PROSPECTOR_TOUCH_DEBUG
#include <touch_debug.h>

/* Frees zmk_display_status_screen() for the wrapper below; the layout's status_screen.c
 * defines it. */
#define zmk_display_status_screen prospector_layout_status_screen
#endif

#if defined(CONFIG_PROSPECTOR_STATUS_SCREEN_CLASSIC)
#include "layouts/classic/status_screen.c"
#elif defined(CONFIG_PROSPECTOR_STATUS_SCREEN_RADII)
#include "layouts/radii/status_screen.c"
#elif defined(CONFIG_PROSPECTOR_STATUS_SCREEN_FIELD)
#include "layouts/field/status_screen.c"
#elif defined(CONFIG_PROSPECTOR_STATUS_SCREEN_FLUX)
#include "layouts/flux/status_screen.c"
#elif defined(CONFIG_PROSPECTOR_STATUS_SCREEN_OPERATOR)
#include "layouts/operator/status_screen.c"
#else
#error "No status screen layout selected"
#endif

#ifdef CONFIG_PROSPECTOR_TOUCH_DEBUG
#undef zmk_display_status_screen

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = prospector_layout_status_screen();

    prospector_touch_debug_init(screen);

    return screen;
}
#endif
