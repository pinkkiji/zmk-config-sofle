/*
 * 右手のUSBシリアル: 画像の受信・保存。
 * フレーム: 'E' 'S' cmd len(2,LE) payload crc16(2,LE)
 *   crc16 は cmd..payload が対象。CCITT 0x1021、初期値 0xFFFF、MSB先頭
 * 応答は cmd='a' payload=[元のcmd, 状態]。ping は cmd='p'。
 * コマンド: P=ping B=開始 D=データ E=確定 C=消去 L=描画の記録 R=ブートローダーへ再起動
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>

#include "eyelash_art.h"

LOG_MODULE_REGISTER(art_usb, LOG_LEVEL_INF);

#define MAX_PAYLOAD 520
#define ERASE_PAGE 4096
#define RESET_UF2 0x57 /* Adafruit nRF52 ブートローダー(UF2)へ入る合図 */

enum { ST_OK = 0, ST_BAD_ARGS = 1, ST_FLASH = 2, ST_CRC = 3, ST_STATE = 4, ST_TOO_MANY = 5 };

static const struct device *const art_uart = DEVICE_DT_GET(DT_NODELABEL(art_cdc));
static const struct flash_area *fa;

static bool receiving;
static uint16_t pend_frames;
static uint16_t pend_interval;
static uint32_t pend_crc;
static bool pend_has_dur;
static uint16_t pend_dur[ART_MAX_FRAMES];

static uint16_t crc16_update(uint16_t c, uint8_t b) {
    c ^= (uint16_t)b << 8;
    for (int k = 0; k < 8; k++) {
        c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

static void put(uint8_t b) { uart_poll_out(art_uart, b); }

static void send(uint8_t cmd, const uint8_t *p, uint16_t n) {
    uint16_t crc = 0xFFFF;
    put('E');
    put('S');
    put(cmd);
    crc = crc16_update(crc, cmd);
    put(n & 0xff);
    crc = crc16_update(crc, n & 0xff);
    put(n >> 8);
    crc = crc16_update(crc, n >> 8);
    for (uint16_t i = 0; i < n; i++) {
        put(p[i]);
        crc = crc16_update(crc, p[i]);
    }
    put(crc & 0xff);
    put(crc >> 8);
}

static void ack(uint8_t cmd, uint8_t status) {
    uint8_t p[2] = {cmd, status};
    send('a', p, 2);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int erase_header(void) { return flash_area_erase(fa, 0, ART_HDR_SIZE); }

static void handle(uint8_t cmd, const uint8_t *p, uint16_t n) {
    switch (cmd) {
    case 'P': {
        const struct art_header *h = eyelash_art_header();
        uint8_t r[8] = {1,
                        ART_MAX_FRAMES & 0xff,
                        ART_MAX_FRAMES >> 8,
                        h ? (h->frames & 0xff) : 0,
                        h ? (h->frames >> 8) : 0,
                        h ? (h->interval_ms & 0xff) : 0,
                        h ? (h->interval_ms >> 8) : 0,
                        0};
        send('p', r, sizeof(r));
        break;
    }
    case 'B': {
        if (n < 8) {
            ack(cmd, ST_BAD_ARGS);
            break;
        }
        uint16_t frames = rd16(p);
        if (frames == 0) {
            ack(cmd, ST_BAD_ARGS);
            break;
        }
        if (frames > ART_MAX_FRAMES) {
            ack(cmd, ST_TOO_MANY);
            break;
        }
        /* 8バイトのあとに、フレームごとの表示時間(u16 x frames)が続いてもよい */
        if (n != 8 && n != 8 + 2 * frames) {
            ack(cmd, ST_BAD_ARGS);
            break;
        }
        pend_has_dur = (n != 8);
        for (uint16_t i = 0; pend_has_dur && i < frames; i++) {
            pend_dur[i] = rd16(p + 8 + 2 * i);
        }
        pend_frames = frames;
        pend_interval = rd16(p + 2);
        pend_crc = rd32(p + 4);
        receiving = false;
        /* 先にヘッダを消して表示を内蔵画像に戻す。その後でデータ領域を消す */
        if (erase_header() != 0) {
            ack(cmd, ST_FLASH);
            break;
        }
        eyelash_art_reload_sync();
        size_t bytes = (size_t)frames * ART_FRAME_BYTES;
        size_t erase_len = ((bytes + ERASE_PAGE - 1) / ERASE_PAGE) * ERASE_PAGE;
        if (flash_area_erase(fa, ART_HDR_SIZE, erase_len) != 0) {
            ack(cmd, ST_FLASH);
            break;
        }
        receiving = true;
        ack(cmd, ST_OK);
        break;
    }
    case 'D': {
        if (!receiving) {
            ack(cmd, ST_STATE);
            break;
        }
        if (n < 8) {
            ack(cmd, ST_BAD_ARGS);
            break;
        }
        uint32_t off = rd32(p);
        uint16_t len = n - 4;
        if ((off & 3) || (len & 3) || off + len > (uint32_t)pend_frames * ART_FRAME_BYTES) {
            ack(cmd, ST_BAD_ARGS);
            break;
        }
        if (flash_area_write(fa, ART_HDR_SIZE + off, p + 4, len) != 0) {
            ack(cmd, ST_FLASH);
            break;
        }
        ack(cmd, ST_OK);
        break;
    }
    case 'E': {
        if (!receiving) {
            ack(cmd, ST_STATE);
            break;
        }
        receiving = false;
        const uint8_t *data = (const uint8_t *)(eyelash_art_flash_addr() + ART_HDR_SIZE);
        if (crc32_ieee(data, (size_t)pend_frames * ART_FRAME_BYTES) != pend_crc) {
            ack(cmd, ST_CRC);
            break;
        }
        /* ヘッダ + 表示時間の表を、1回で書く（4バイト境界） */
        static uint8_t hb[sizeof(struct art_header) + ((ART_MAX_FRAMES * 2 + 3) & ~3)];
        memset(hb, 0, sizeof(hb));
        struct art_header h = {
            .magic = ART_MAGIC,
            .version = 1,
            .frames = pend_frames,
            .interval_ms = pend_interval,
            .flags = pend_has_dur ? ART_FLAG_DURATIONS : 0,
            .data_crc32 = pend_crc,
            .pad = 0,
        };
        memcpy(hb, &h, sizeof(h));
        size_t wlen = sizeof(h);
        if (pend_has_dur) {
            memcpy(hb + sizeof(h), pend_dur, (size_t)pend_frames * 2);
            wlen += (((size_t)pend_frames * 2) + 3) & ~(size_t)3;
        }
        if (flash_area_write(fa, 0, hb, wlen) != 0) {
            ack(cmd, ST_FLASH);
            break;
        }
        eyelash_art_reload_sync();
        ack(cmd, ST_OK);
        break;
    }
    case 'C': {
        receiving = false;
        if (erase_header() != 0) {
            ack(cmd, ST_FLASH);
            break;
        }
        eyelash_art_reload_sync();
        ack(cmd, ST_OK);
        break;
    }
    case 'L': {
        uint8_t buf[sizeof(struct art_flush_rec) * 24];
        size_t m = eyelash_art_spy_read(buf, sizeof(buf));
        send('l', buf, (uint16_t)m);
        break;
    }
    case 'T': {
        uint32_t h, s;
        eyelash_art_timing(&h, &s);
        uint8_t r[8] = {h & 0xff, (h >> 8) & 0xff, (h >> 16) & 0xff, h >> 24,
                        s & 0xff, (s >> 8) & 0xff, (s >> 16) & 0xff, s >> 24};
        send('t', r, sizeof(r));
        break;
    }
    case 'R': {
        ack(cmd, ST_OK);
        k_msleep(100); /* 応答がホストに届くのを待つ */
        sys_reboot(RESET_UF2);
        break;
    }
    default:
        ack(cmd, ST_BAD_ARGS);
    }
}

static void art_usb_thread(void *a, void *b, void *c) {
    int ret = usb_enable(NULL);
    if (ret != 0 && ret != -EALREADY) {
        LOG_ERR("usb_enable failed: %d", ret);
        return;
    }
    if (!device_is_ready(art_uart)) {
        LOG_ERR("cdc uart not ready");
        return;
    }
    if (flash_area_open(FIXED_PARTITION_ID(image_partition), &fa) != 0) {
        LOG_ERR("image partition open failed");
        return;
    }

    static uint8_t payload[MAX_PAYLOAD];
    enum { S0, S1, CMD, L0, L1, PAY, C0, C1 } st = S0;
    uint8_t cmd = 0;
    uint16_t len = 0, got = 0, crc = 0, rx_crc = 0;
    unsigned char ch;

    for (;;) {
        if (uart_poll_in(art_uart, &ch) != 0) {
            k_msleep(1);
            continue;
        }
        switch (st) {
        case S0:
            st = (ch == 'E') ? S1 : S0;
            break;
        case S1:
            st = (ch == 'S') ? CMD : ((ch == 'E') ? S1 : S0);
            break;
        case CMD:
            cmd = ch;
            crc = crc16_update(0xFFFF, ch);
            st = L0;
            break;
        case L0:
            len = ch;
            crc = crc16_update(crc, ch);
            st = L1;
            break;
        case L1:
            len |= (uint16_t)ch << 8;
            crc = crc16_update(crc, ch);
            got = 0;
            if (len > MAX_PAYLOAD) {
                st = S0;
            } else {
                st = (len == 0) ? C0 : PAY;
            }
            break;
        case PAY:
            payload[got++] = ch;
            crc = crc16_update(crc, ch);
            if (got == len) {
                st = C0;
            }
            break;
        case C0:
            rx_crc = ch;
            st = C1;
            break;
        case C1:
            rx_crc |= (uint16_t)ch << 8;
            if (rx_crc == crc) {
                handle(cmd, payload, len);
            } else {
                ack(cmd, ST_BAD_ARGS);
            }
            st = S0;
            break;
        }
    }
}

K_THREAD_DEFINE(art_usb_tid, 2048, art_usb_thread, NULL, NULL, NULL, 10, 0, 0);
