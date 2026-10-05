/*
 * スクリーンセーバー: 使っていない間（ZMK の idle 状態）は、ヘッダ（バッテリー・接続先）を含めた画面全体で、
 * 斜めのしま模様を流す。同じ画素が出しっぱなしにならないようにして、焼き付きを防ぐ。
 * 使い始める（active に戻る）と、すぐ元の画面に戻る。
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#include "eyelash_art.h"

#if IS_ENABLED(CONFIG_EYELASH_SCREENSAVER)

#define SS_W 160
#define SS_H 68
#define SS_ROW (SS_W / 8)
#define SS_BYTES (8 + SS_ROW * SS_H)
#define SS_TICK_MS 100 /* 1秒に10回 */
#define SS_STRIPE 8 /* しまの幅（画素） */
#define SS_STEP 2   /* 1回で流れる量（画素） */

static lv_obj_t *ss_obj;
static lv_img_dsc_t ss_dsc;
static uint8_t ss_buf[SS_BYTES];
static bool ss_on;
static uint32_t phase;
static struct k_work_delayable tick_work;

static void draw(void) {
    /* パレットは画像と同じ（index0=白 index1=黒の画素） */
    static const uint8_t pal[8] = {0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff};
    memcpy(ss_buf, pal, sizeof(pal));
    memset(ss_buf + 8, 0, SS_ROW * SS_H);
    for (int y = 0; y < SS_H; y++) {
        for (int x = 0; x < SS_W; x++) {
            if ((((uint32_t)(x + y) + phase) / SS_STRIPE) & 1) {
                ss_buf[8 + y * SS_ROW + (x >> 3)] |= 0x80 >> (x & 7);
            }
        }
    }
}

static void tick_cb(struct k_work *work) {
    if (!ss_on || !ss_obj) {
        return;
    }
    phase += SS_STEP;
    draw();
    lv_img_cache_invalidate_src(NULL);
    lv_img_set_src(ss_obj, &ss_dsc);
    lv_obj_invalidate(ss_obj);
    lv_refr_now(NULL);
    k_work_reschedule_for_queue(zmk_display_work_q(), &tick_work, K_MSEC(SS_TICK_MS));
}

static void start_cb(struct k_work *work) {
    if (!ss_obj || ss_on) {
        return;
    }
    ss_on = true;
    eyelash_art_pause(true);
    lv_obj_clear_flag(ss_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ss_obj);
    tick_cb(NULL);
}
static K_WORK_DEFINE(start_work, start_cb);

static void stop_cb(struct k_work *work) {
    if (!ss_obj || !ss_on) {
        return;
    }
    ss_on = false;
    k_work_cancel_delayable(&tick_work);
    lv_obj_add_flag(ss_obj, LV_OBJ_FLAG_HIDDEN);
    eyelash_art_pause(false);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
}
static K_WORK_DEFINE(stop_work, stop_cb);

void eyelash_screensaver_attach(lv_obj_t *screen) {
    k_work_init_delayable(&tick_work, tick_cb);
    ss_dsc.header.cf = LV_IMG_CF_INDEXED_1BIT;
    ss_dsc.header.always_zero = 0;
    ss_dsc.header.reserved = 0;
    ss_dsc.header.w = SS_W;
    ss_dsc.header.h = SS_H;
    ss_dsc.data_size = SS_BYTES;
    ss_dsc.data = ss_buf;
    draw();
    ss_obj = lv_img_create(screen);
    lv_img_set_src(ss_obj, &ss_dsc);
    lv_obj_align(ss_obj, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(ss_obj, LV_OBJ_FLAG_HIDDEN);
}

static int ss_listener(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (!ev || !ss_obj) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->state == ZMK_ACTIVITY_IDLE) {
        k_work_submit_to_queue(zmk_display_work_q(), &start_work);
    } else if (ev->state == ZMK_ACTIVITY_ACTIVE) {
        k_work_submit_to_queue(zmk_display_work_q(), &stop_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(eyelash_ss, ss_listener);
ZMK_SUBSCRIPTION(eyelash_ss, zmk_activity_state_changed);

#else
void eyelash_screensaver_attach(lv_obj_t *screen) { (void)screen; }
#endif
