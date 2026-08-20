/* Opening any CDC port at 1200 baud reboots into the UF2 bootloader (the Arduino convention), so
 * the dongle can be reflashed without pressing its button. */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/uart/cdc_acm.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/sys/reboot.h>

static void enter_bootloader(struct k_work *work) {
    ARG_UNUSED(work);
    bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
    sys_reboot(SYS_REBOOT_WARM);
}

static K_WORK_DEFINE(dfu_touch_work, enter_bootloader);

static void dfu_touch_rate_cb(const struct device *dev, uint32_t rate) {
    ARG_UNUSED(dev);
    if (rate == 1200) {
        k_work_submit(&dfu_touch_work);
    }
}

#define DFU_TOUCH_HOOK(node) cdc_acm_dte_rate_callback_set(DEVICE_DT_GET(node), dfu_touch_rate_cb);

static int dfu_touch_init(void) {
    DT_FOREACH_STATUS_OKAY(zephyr_cdc_acm_uart, DFU_TOUCH_HOOK)
    return 0;
}

SYS_INIT(dfu_touch_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
