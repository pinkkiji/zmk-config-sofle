/* 右手の画像（フラッシュ保存）の共通定義。SPDX-License-Identifier: MIT */
#pragma once

#include <lvgl.h>
#include <stdint.h>
#include <stdbool.h>

#define ART_W 140
#define ART_H 68
#define ART_FRAME_BYTES 1232 /* パレット8 + 18バイト x 68行 */
#define ART_HDR_SIZE 4096    /* 先頭1ページがヘッダ */
#define ART_PART_SIZE 0x2C000
#define ART_MAX_FRAMES ((ART_PART_SIZE - ART_HDR_SIZE) / ART_FRAME_BYTES) /* 142 */
#define ART_MAGIC 0x4d495345u                                             /* 'ESIM' */

struct art_header {
    uint32_t magic;
    uint16_t version;
    uint16_t frames;
    uint16_t interval_ms;
    uint16_t reserved;
    uint32_t data_crc32;
    uint32_t pad; /* 16バイトに揃える */
};

/* 表示スレッドから: 画像オブジェクトを登録して、現在の画像を表示する */
void eyelash_art_attach(lv_obj_t *img);
/* 他スレッドから: 表示の更新を依頼して、完了を待つ */
void eyelash_art_reload_sync(void);
/* 保存済みで有効な画像のヘッダ（なければ NULL） */
const struct art_header *eyelash_art_header(void);
/* フラッシュ上の画像領域の先頭アドレス（メモリマップ） */
uint32_t eyelash_art_flash_addr(void);
