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

/* 描画の記録（LVGL → 画面ドライバーの転送1回ごと） */
struct art_flush_rec {
    int16_t x1, y1, x2, y2;
    uint32_t start_us;
    uint32_t dur_us;
};
/* 記録を out に詰める（最大 max バイト）。読んだ分は消える。戻り値は書いたバイト数 */
size_t eyelash_art_spy_read(uint8_t *out, size_t max);
/* 描画時間の測定: 絵を隠した場合と出した場合の lv_refr_now の所要時間(us)。表示キューで実行し、完了を待つ */
void eyelash_art_timing(uint32_t *hidden_us, uint32_t *shown_us);
