/*
 * 右手のファーム更新（左手の中継、または USB 経由）。右手専用。
 *   1. 受け取ったファームを、作業領域(0x70000〜0xC0000)に溜める
 *   2. 全体のチェックが合ったら、RAM上の関数で本来の領域(0x26000〜)へ上書きして再起動
 * 上書きの途中で電源が落ちると起動しなくなる。そのときはリセット2回でブートローダーに入れる。
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/crc.h>
#include <zephyr/linker/sections.h>
#include <nrf.h>

#include "eyelash_art.h"

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

#define CODE_START 0x26000u
#define STAGE_START 0x70000u
#define STAGE_END 0xC0000u
#define PAGE 4096u
#define MAX_IMAGE (STAGE_START - CODE_START) /* 現在のファームがこの中に収まっている前提 */

static const struct device *const fdev = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));
static uint32_t img_size, img_crc;
static bool staging;

int art_update_begin(uint32_t size, uint32_t crc) {
    if (size == 0 || size > MAX_IMAGE || !device_is_ready(fdev)) {
        return 1;
    }
    img_size = size;
    img_crc = crc;
    staging = false;
    uint32_t len = (size + PAGE - 1) & ~(PAGE - 1);
    if (flash_erase(fdev, STAGE_START, len) != 0) {
        return 2;
    }
    staging = true;
    return 0;
}

int art_update_data(uint32_t off, const uint8_t *p, uint16_t n) {
    if (!staging || (off & 3) || (n & 3) || off + n > ((img_size + 3) & ~3u)) {
        return 1;
    }
    return flash_write(fdev, STAGE_START + off, p, n) ? 2 : 0;
}

/* 作業領域の中身が、確認用のチェックと合うか */
int art_update_verify(void) {
    if (!staging) {
        return 1;
    }
    return crc32_ieee((const uint8_t *)STAGE_START, img_size) == img_crc ? 0 : 3;
}

/* RAM上で動く。ここから先は、フラッシュ上の関数を一切呼ばない */
static void __ramfunc __noinline apply_ram(uint32_t size) {
    __asm volatile("cpsid i" ::: "memory");
    uint32_t pages = (size + PAGE - 1) / PAGE;
    for (uint32_t i = 0; i < pages; i++) {
        NRF_NVMC->CONFIG = 2; /* 消去 */
        while (NRF_NVMC->READY == 0) {
        }
        NRF_NVMC->ERASEPAGE = CODE_START + i * PAGE;
        while (NRF_NVMC->READY == 0) {
        }
        NRF_NVMC->CONFIG = 1; /* 書き込み */
        while (NRF_NVMC->READY == 0) {
        }
        volatile uint32_t *dst = (volatile uint32_t *)(CODE_START + i * PAGE);
        volatile const uint32_t *src = (volatile const uint32_t *)(STAGE_START + i * PAGE);
        for (uint32_t w = 0; w < PAGE / 4; w++) {
            dst[w] = src[w];
            while (NRF_NVMC->READY == 0) {
            }
        }
        NRF_NVMC->CONFIG = 0;
        while (NRF_NVMC->READY == 0) {
        }
    }
    __asm volatile("dsb" ::: "memory");
    *(volatile uint32_t *)0xE000ED0Cu = (0x5FAu << 16) | (1u << 2); /* SYSRESETREQ */
    __asm volatile("dsb" ::: "memory");
    for (;;) {
    }
}

void art_update_apply(void) { apply_ram(img_size); }

#endif
