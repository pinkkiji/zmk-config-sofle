/*
 * 右手の画像表示。フラッシュの画像（なければ内蔵の custom_art）を出す。
 * 複数フレームなら、一定間隔で切り替える。
 * SPDX-License-Identifier: MIT
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/crc.h>
#include <zmk/display.h>

#include "eyelash_art.h"

LV_IMG_DECLARE(custom_art);

#define ART_FLASH_ADDR DT_REG_ADDR(DT_NODELABEL(image_partition))

static lv_obj_t *art_obj;
static lv_timer_t *anim_timer;
static lv_img_dsc_t frames[ART_MAX_FRAMES];
static uint16_t nframes;
static uint16_t cur;
static K_SEM_DEFINE(reload_done, 0, 1);

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

static void anim_cb(lv_timer_t *t) {
    cur = (cur + 1) % nframes;
    lv_img_set_src(art_obj, &frames[cur]);
}

static void apply(void) {
    if (!art_obj) {
        return;
    }
    if (anim_timer) {
        lv_timer_del(anim_timer);
        anim_timer = NULL;
    }
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
    if (nframes > 1 && h->interval_ms >= 20) {
        anim_timer = lv_timer_create(anim_cb, h->interval_ms, NULL);
    }
}

static void apply_work_cb(struct k_work *work) {
    apply();
    k_sem_give(&reload_done);
}
static K_WORK_DEFINE(apply_work, apply_work_cb);

void eyelash_art_attach(lv_obj_t *img) {
    art_obj = img;
    apply();
}

void eyelash_art_reload_sync(void) {
    k_sem_reset(&reload_done);
    k_work_submit_to_queue(zmk_display_work_q(), &apply_work);
    k_sem_take(&reload_done, K_SECONDS(2));
}
