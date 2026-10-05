/*
 * 左右の画像命令の中継（無線）。
 *   左手(親機): アプリから受けた命令を右手へ送り、右手の応答を待つ
 *   右手(子機): 受け取った命令を、art_usb.c が実行し、応答を左手へ返す
 * ZMK フォークの「リレーイベント」(名前つきの短いデータ)を使う。1回に送れるのは約56バイト。
 * 1つ送るごとに、相手の受け取り確認を待つ。
 *
 * パケット(event_data): [0]=種類 [1]=命令 [2..3]=全体の長さ [4..5]=位置 [6..]=データ
 *   種類: 1=要求(左→右) 2=要求の受け取り確認(右→左) 3=応答(右→左) 4=応答の受け取り確認(左→右)
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/event_manager.h>

#include "eyelash_art.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)

#include <zmk/split/transport/types.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#else
#include <zmk/split/peripheral.h>
#endif

#define TUN_NAME "art"
#define TUN_DATA_MAX ((int)MIN(CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN, 56))
#define TUN_HDR 6
#define TUN_CHUNK (TUN_DATA_MAX - TUN_HDR)
#define TUN_MAX_PAYLOAD 520

enum { T_REQ = 1, T_REQ_ACK = 2, T_RSP = 3, T_RSP_ACK = 4 };

BUILD_ASSERT(CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN >= 24, "relay event data too small for art tunnel");

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void pack(uint8_t *d, uint8_t type, uint8_t cmd, uint16_t total, uint16_t off) {
    d[0] = type;
    d[1] = cmd;
    d[2] = total & 0xff;
    d[3] = total >> 8;
    d[4] = off & 0xff;
    d[5] = off >> 8;
}

/* ---- 送信（役割ごと） ---- */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static int tun_send(const uint8_t *d, uint8_t n) {
    struct zmk_split_relay_event_payload pl;
    memset(&pl, 0, sizeof(pl));
    pl.header.event_data_size = n;
    pl.header.event_type_size = strlen(TUN_NAME);
    strcpy(pl.event_type, TUN_NAME);
    memcpy(pl.event_data, d, n);
    return zmk_split_central_send_relay_event(&pl);
}
#else
static int tun_send(const uint8_t *d, uint8_t n) {
    struct zmk_split_transport_peripheral_event pev;
    memset(&pev, 0, sizeof(pev));
    pev.type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_RELAY_EVENT;
    pev.data.relay_event.header.event_data_size = n;
    strcpy(pev.data.relay_event.event_type, TUN_NAME);
    memcpy(pev.data.relay_event.event_data, d, n);
    return zmk_split_peripheral_report_event(&pev);
}
#endif

/* ================= 左手(親機) ================= */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static K_SEM_DEFINE(req_ack_sem, 0, 1);
static K_SEM_DEFINE(rsp_done_sem, 0, 1);
static volatile uint16_t last_ack_off;
static uint8_t rsp_buf[TUN_MAX_PAYLOAD];
static uint16_t rsp_total, rsp_got;
static uint8_t rsp_cmd;

static void central_on_packet(const uint8_t *d, size_t n) {
    if (n < TUN_HDR) {
        return;
    }
    if (d[0] == T_REQ_ACK) {
        last_ack_off = rd16(d + 4);
        k_sem_give(&req_ack_sem);
    } else if (d[0] == T_RSP) {
        uint16_t total = rd16(d + 2);
        uint16_t off = rd16(d + 4);
        size_t len = n - TUN_HDR;
        if (off == 0) {
            rsp_got = 0;
            rsp_total = total;
            rsp_cmd = d[1];
        }
        if (off == rsp_got && total <= TUN_MAX_PAYLOAD && rsp_got + len <= TUN_MAX_PAYLOAD) {
            memcpy(rsp_buf + rsp_got, d + TUN_HDR, len);
            rsp_got += len;
        }
        uint8_t ack[TUN_HDR];
        pack(ack, T_RSP_ACK, d[1], total, rsp_got);
        tun_send(ack, TUN_HDR);
        if (rsp_got == rsp_total && off + len == total) {
            k_sem_give(&rsp_done_sem);
        }
    }
}

/* 右手へ命令を送って、応答を待つ。戻り値: 0=成功 1=送信失敗(右手につながっていない等) 2=時間切れ */
int art_tunnel_request(uint8_t cmd, const uint8_t *p, uint16_t n, uint8_t *out_cmd, uint8_t *out,
                       uint16_t *out_n, int timeout_ms) {
    k_sem_reset(&req_ack_sem);
    k_sem_reset(&rsp_done_sem);
    rsp_got = 0;
    rsp_total = 0;

    uint16_t off = 0;
    do {
        uint16_t len = MIN((uint16_t)TUN_CHUNK, (uint16_t)(n - off));
        uint8_t pkt[TUN_DATA_MAX];
        pack(pkt, T_REQ, cmd, n, off);
        memcpy(pkt + TUN_HDR, p + off, len);

        bool acked = false;
        for (int attempt = 0; attempt < 5 && !acked; attempt++) {
            if (tun_send(pkt, TUN_HDR + len) != 0) {
                k_msleep(30);
                continue;
            }
            if (k_sem_take(&req_ack_sem, K_MSEC(800)) == 0 && last_ack_off >= off + len) {
                acked = true;
            }
        }
        if (!acked) {
            return (off == 0) ? 1 : 2;
        }
        off += len;
    } while (off < n);

    if (k_sem_take(&rsp_done_sem, K_MSEC(timeout_ms)) != 0) {
        return 2;
    }
    *out_cmd = rsp_cmd;
    memcpy(out, rsp_buf, rsp_total);
    *out_n = rsp_total;
    return 0;
}

#else /* ================= 右手(子機) ================= */

static K_SEM_DEFINE(rsp_ack_sem, 0, 1);
static volatile uint16_t last_rsp_ack_off;
static uint8_t req_buf[TUN_MAX_PAYLOAD];
static uint16_t req_total, req_got;
static uint8_t req_cmd;
static volatile bool req_ready;

static void peripheral_on_packet(const uint8_t *d, size_t n) {
    if (n < TUN_HDR) {
        return;
    }
    if (d[0] == T_REQ) {
        uint16_t total = rd16(d + 2);
        uint16_t off = rd16(d + 4);
        size_t len = n - TUN_HDR;
        if (off == 0 && !req_ready) {
            req_got = 0;
            req_total = total;
            req_cmd = d[1];
        }
        if (!req_ready && off == req_got && total <= TUN_MAX_PAYLOAD && req_got + len <= TUN_MAX_PAYLOAD) {
            memcpy(req_buf + req_got, d + TUN_HDR, len);
            req_got += len;
        }
        uint8_t ack[TUN_HDR];
        pack(ack, T_REQ_ACK, d[1], total, req_got);
        tun_send(ack, TUN_HDR);
        if (!req_ready && req_got == req_total && off + len == total) {
            req_ready = true;
        }
    } else if (d[0] == T_RSP_ACK) {
        last_rsp_ack_off = rd16(d + 4);
        k_sem_give(&rsp_ack_sem);
    }
}

/* art_usb.c のスレッドから呼ぶ: 完成した要求があれば取り出す */
bool art_tunnel_take_request(uint8_t *cmd, const uint8_t **p, uint16_t *n) {
    if (!req_ready) {
        return false;
    }
    *cmd = req_cmd;
    *p = req_buf;
    *n = req_total;
    return true;
}

void art_tunnel_request_done(void) {
    req_got = 0;
    req_total = 0;
    req_ready = false;
}

/* 応答を左手へ返す（art_usb.c の出力先として使う） */
void art_tunnel_send_response(uint8_t cmd, const uint8_t *p, uint16_t n) {
    uint16_t off = 0;
    do {
        uint16_t len = MIN((uint16_t)TUN_CHUNK, (uint16_t)(n - off));
        uint8_t pkt[TUN_DATA_MAX];
        pack(pkt, T_RSP, cmd, n, off);
        memcpy(pkt + TUN_HDR, p + off, len);
        bool acked = false;
        k_sem_reset(&rsp_ack_sem);
        for (int attempt = 0; attempt < 5 && !acked; attempt++) {
            if (tun_send(pkt, TUN_HDR + len) != 0) {
                k_msleep(30);
                continue;
            }
            if (k_sem_take(&rsp_ack_sem, K_MSEC(800)) == 0 && last_rsp_ack_off >= off + len) {
                acked = true;
            }
        }
        if (!acked) {
            return;
        }
        off += len;
    } while (off < n);
}

#endif

/* ---- 受信（共通） ---- */
static int art_tunnel_listener(const zmk_event_t *eh) {
    struct zmk_relay_event_received *ev = as_zmk_relay_event_received(eh);
    if (!ev || strcmp(ev->event_name, TUN_NAME) != 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    central_on_packet(ev->event_data, ev->event_data_size);
#else
    peripheral_on_packet(ev->event_data, ev->event_data_size);
#endif
    return ZMK_EV_EVENT_HANDLED;
}

ZMK_LISTENER(art_tunnel, art_tunnel_listener);
ZMK_SUBSCRIPTION(art_tunnel, zmk_relay_event_received);

#endif /* CONFIG_ZMK_SPLIT_RELAY_EVENT */
