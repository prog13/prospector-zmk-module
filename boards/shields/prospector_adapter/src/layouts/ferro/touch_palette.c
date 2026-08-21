#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/dt-bindings/input/cst816s-gesture-codes.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "ferro_blobs.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define TOUCH_NODE DT_NODELABEL(cst816s)

#define PALETTE_CYCLE_INIT_PRIORITY 99

#define REG_MOTION_MASK 0xEC
#define MOTION_EN_DCLICK BIT(0)

static const struct i2c_dt_spec touch_i2c = I2C_DT_SPEC_GET(TOUCH_NODE);

static void palette_gesture(struct input_event *evt, void *user_data) {
    ARG_UNUSED(user_data);

    if (evt->type == INPUT_EV_DEVICE && evt->code == CST816S_GESTURE_CODE_DOUBLE_CLICK) {
        zmk_widget_ferro_blobs_request_palette_next();
    }
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(TOUCH_NODE), palette_gesture, NULL);

/* Enables double-click reporting (gesture 0x0b) on the touch chip. Side effect: single click
 * (0x05) is now reported ~200ms late, which nothing here uses. Read-modify-write so the chip's
 * swipe bits stay as the vendor set them. */
static int palette_cycle_init(void) {
    int ret = i2c_reg_update_byte_dt(&touch_i2c, REG_MOTION_MASK, MOTION_EN_DCLICK,
                                     MOTION_EN_DCLICK);
    if (ret < 0) {
        LOG_WRN("ferro palette: EnDClick write failed (%d); double click will not fire", ret);
    }
    return 0;
}

/* Must run after the touch driver, which initialises in POST_KERNEL. */
SYS_INIT(palette_cycle_init, APPLICATION, PALETTE_CYCLE_INIT_PRIORITY);
