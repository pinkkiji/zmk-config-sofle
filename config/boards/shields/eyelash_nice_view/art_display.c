/*
 * 右手の画像表示。フラッシュの画像（なければ内蔵の custom_art）を出す。
 * 複数フレームなら、一定間隔で切り替える。
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/crc.h>
#include <zmk/display.h>

#include "eyelash_art.h"

LV_IMG_DECLARE(custom_art);

#define ART_FLASH_ADDR DT_REG_ADDR(DT_NODELABEL(image_partition))

static lv_obj_t *art_obj;
static struct k_work_delayable anim_work;
static int64_t anim_next_ms;
static uint16_t anim_interval_ms;
static lv_img_dsc_t frames[ART_MAX_FRAMES];
static uint16_t nframes;
static uint16_t cur;
static K_SEM_DEFINE(reload_done, 0, 1);

/* ---- 描画の記録 ---- */
#define SPY_N 24
static struct art_flush_rec spy[SPY_N];
static uint32_t spy_head; /* 次に書く位置（通し番号） */
static uint32_t spy_tail; /* 次に読む位置（通し番号） */
typedef void (*flush_cb_t)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *);
static flush_cb_t orig_flush;

static void flush_spy(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *c) {
    uint32_t t0 = k_cyc_to_us_floor32(k_cycle_get_32());
    orig_flush(drv, a, c);
    uint32_t t1 = k_cyc_to_us_floor32(k_cycle_get_32());
    struct art_flush_rec *r = &spy[spy_head % SPY_N];
    r->x1 = a->x1;
    r->y1 = a->y1;
    r->x2 = a->x2;
    r->y2 = a->y2;
    r->start_us = t0;
    r->dur_us = t1 - t0;
    spy_head++;
}

static void install_spy(void) {
    static bool done;
    if (done) {
        return;
    }
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp) {
        return;
    }
    orig_flush = disp->driver->flush_cb;
    disp->driver->flush_cb = flush_spy;
    done = true;
}

size_t eyelash_art_spy_read(uint8_t *out, size_t max) {
    size_t n = 0;
    if (spy_head - spy_tail > SPY_N) {
        spy_tail = spy_head - SPY_N;
    }
    while (spy_tail != spy_head && n + sizeof(struct art_flush_rec) <= max) {
        memcpy(out + n, &spy[spy_tail % SPY_N], sizeof(struct art_flush_rec));
        n += sizeof(struct art_flush_rec);
        spy_tail++;
    }
    return n;
}

uint32_t eyelash_art_flash_addr(void) { return ART_FLASH_ADDR; }

const struct art_header *eyelash_art_header(void) {
    const struct art_header *h = (const struct art_header *)ART_FLASH_ADDR;
    if (h->magic != ART_MAGIC || h->version != 1 || h->frames == 0 || h->frames > ART_MAX_FRAMES) {
        return NULL;
    }
    const uint8_t *data = (const uint8_t *)(ART_FLASH_ADDR + ART_HDR_SIZE);
    if (crc32_ieee(data, (size_t)h->frames * ART_FRAME_BYTES) != h->data_crc32) {
        return NULL;
    }
    return h;
}

/* 表示キュー上の時刻管理つきタイマー（LVGL のタイマーは転送中に遅れるため使わない） */
static void anim_cb(struct k_work *work) {
    if (nframes < 2 || !art_obj) {
        return;
    }
    cur = (cur + 1) % nframes;
    lv_img_set_src(art_obj, &frames[cur]);
    lv_refr_now(NULL); /* 次の tick を待たず、すぐ描く */
    anim_next_ms += anim_interval_ms;
    int64_t now = k_uptime_get();
    int64_t d = anim_next_ms - now;
    if (d < 0) {
        anim_next_ms = now;
        d = 0;
    }
    k_work_reschedule_for_queue(zmk_display_work_q(), &anim_work, K_MSEC(d));
}

static void apply(void) {
    if (!art_obj) {
        return;
    }
    k_work_cancel_delayable(&anim_work);
    lv_img_cache_invalidate_src(NULL);

    const struct art_header *h = eyelash_art_header();
    if (!h) {
        nframes = 0;
        lv_img_set_src(art_obj, &custom_art);
        return;
    }
    nframes = h->frames;
    const uint8_t *base = (const uint8_t *)(ART_FLASH_ADDR + ART_HDR_SIZE);
    for (uint16_t i = 0; i < nframes; i++) {
        frames[i].header.cf = LV_IMG_CF_INDEXED_1BIT;
        frames[i].header.always_zero = 0;
        frames[i].header.reserved = 0;
        frames[i].header.w = ART_W;
        frames[i].header.h = ART_H;
        frames[i].data_size = ART_FRAME_BYTES;
        frames[i].data = base + (size_t)i * ART_FRAME_BYTES;
    }
    cur = 0;
    lv_img_set_src(art_obj, &frames[0]);
    if (nframes > 1) {
        anim_interval_ms = h->interval_ms >= 20 ? h->interval_ms : 20;
        anim_next_ms = k_uptime_get() + anim_interval_ms;
        k_work_reschedule_for_queue(zmk_display_work_q(), &anim_work, K_MSEC(anim_interval_ms));
    }
}

static void apply_work_cb(struct k_work *work) {
    apply();
    k_sem_give(&reload_done);
}
static K_WORK_DEFINE(apply_work, apply_work_cb);

void eyelash_art_attach(lv_obj_t *img) {
    k_work_init_delayable(&anim_work, anim_cb);
    art_obj = img;
    install_spy();
    apply();
}

void eyelash_art_reload_sync(void) {
    k_sem_reset(&reload_done);
    k_work_submit_to_queue(zmk_display_work_q(), &apply_work);
    k_sem_take(&reload_done, K_SECONDS(2));
}
