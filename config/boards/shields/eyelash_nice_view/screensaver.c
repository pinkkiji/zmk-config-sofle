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

static lv_obj_t *ss_obj;
static lv_img_dsc_t ss_dsc;
static uint8_t ss_buf[SS_BYTES];
static bool ss_on;
static uint32_t phase;
static int bx = 10, by = 10, bdx = 1, bdy = 1; /* 跳ねる四角 */
static struct k_work_delayable tick_work;

static inline void setpx(int x, int y) {
    if (x >= 0 && x < SS_W && y >= 0 && y < SS_H) {
        ss_buf[8 + y * SS_ROW + (x >> 3)] |= 0x80 >> (x & 7);
    }
}

static void draw(void) {
    /* パレットは画像と同じ（index0=白 index1=黒の画素） */
    static const uint8_t pal[8] = {0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff};
    const struct art_cfg *c = eyelash_cfg();
    uint32_t st = c->stripe;
    memcpy(ss_buf, pal, sizeof(pal));
    memset(ss_buf + 8, 0, SS_ROW * SS_H);
    if (c->pattern == 3) { /* 跳ねる四角 */
        const int bw = 28, bh = 20;
        bx += bdx * (int)c->step;
        by += bdy * (int)((c->step * 2 + 2) / 3);
        if (bx < 0) { bx = 0; bdx = -bdx; }
        if (bx + bw > SS_W) { bx = SS_W - bw; bdx = -bdx; }
        if (by < 0) { by = 0; bdy = -bdy; }
        if (by + bh > SS_H) { by = SS_H - bh; bdy = -bdy; }
        for (int y = by; y < by + bh; y++) {
            for (int x = bx; x < bx + bw; x++) {
                setpx(x, y);
            }
        }
        return;
    }
    for (int y = 0; y < SS_H; y++) {
        for (int x = 0; x < SS_W; x++) {
            uint32_t v = (c->pattern == 0) ? (uint32_t)(x + y) : (c->pattern == 1) ? (uint32_t)x : (uint32_t)y;
            if (((v + phase) / st) & 1) {
                ss_buf[8 + y * SS_ROW + (x >> 3)] |= 0x80 >> (x & 7);
            }
        }
    }
}

static void tick_cb(struct k_work *work) {
    if (!ss_on || !ss_obj) {
        return;
    }
    const struct art_cfg *c = eyelash_cfg();
    phase += c->step;
    draw();
    lv_img_cache_invalidate_src(NULL);
    lv_img_set_src(ss_obj, &ss_dsc);
    lv_obj_invalidate(ss_obj);
    lv_refr_now(NULL);
    k_work_reschedule_for_queue(zmk_display_work_q(), &tick_work, K_MSEC(1000 / c->fps));
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
        if (!eyelash_cfg()->ss_enable) {
            return ZMK_EV_EVENT_BUBBLE;
        }
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
