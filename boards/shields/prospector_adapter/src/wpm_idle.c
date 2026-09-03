#include <wpm_idle.h>

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/wpm_state_changed.h>

static uint32_t last_event_ms;
static int current_wpm;
static bool seen;

static int wpm_idle_handler(const zmk_event_t *eh) {
    const struct zmk_wpm_state_changed *ev = as_zmk_wpm_state_changed(eh);
    if (ev == NULL || ev->state > 300) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    last_event_ms = k_uptime_get_32();
    current_wpm = ev->state;
    if (current_wpm > 0) {
        seen = true;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(wpm_idle, wpm_idle_handler);
ZMK_SUBSCRIPTION(wpm_idle, zmk_wpm_state_changed);

int prospector_wpm_current(void) { return current_wpm; }

uint32_t prospector_wpm_idle_ms(uint32_t now) { return now - last_event_ms; }

bool prospector_wpm_seen(void) { return seen; }
