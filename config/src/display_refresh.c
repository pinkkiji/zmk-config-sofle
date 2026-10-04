#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <lvgl.h>
#include <zmk/display.h>

static void refresh_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(refresh_work, refresh_cb);

static void refresh_cb(struct k_work *work) {
    if (zmk_display_is_initialized()) {
        lv_obj_invalidate(lv_screen_active());
    }
    k_work_reschedule_for_queue(zmk_display_work_q(), &refresh_work,
                                K_MSEC(CONFIG_EYELASH_DISPLAY_REFRESH_MS));
}

static int refresh_init(void) {
    k_work_schedule_for_queue(zmk_display_work_q(), &refresh_work, K_SECONDS(2));
    return 0;
}

SYS_INIT(refresh_init, APPLICATION, 99);
