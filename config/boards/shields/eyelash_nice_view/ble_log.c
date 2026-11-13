/*
 * 無線の接続の記録。つながった・切れた（理由の番号つき）・暗号化・接続の設定の変更を、本体の中にためる。
 * 左手では「PC への接続（周辺機器の役）」と「右手への接続（中央の役）」の両方が、右手では「左手への接続」が残る。
 * 電源を切ると消える（スリープから起きたあとの分が残る）。アプリの「接続の記録」から、'Q' で読む。
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

#include "eyelash_art.h"

#if IS_ENABLED(CONFIG_BT)

BUILD_ASSERT(sizeof(struct art_ble_ev) == 16, "art_ble_ev must be 16 bytes");

#define LOG_N 64

static struct art_ble_ev ring[LOG_N];
static uint32_t head; /* 書いた総数 */
static struct k_spinlock lk;

static void put(uint8_t type, struct bt_conn *conn, uint16_t p1, uint16_t p2, uint16_t p3) {
    struct art_ble_ev e;
    memset(&e, 0, sizeof(e));
    e.t_ms = k_uptime_get_32();
    e.type = type;
    struct bt_conn_info info;
    if (conn && bt_conn_get_info(conn, &info) == 0) {
        e.role = info.role;
    }
    const bt_addr_le_t *dst = conn ? bt_conn_get_dst(conn) : NULL;
    if (dst) {
        e.peer = (uint16_t)(dst->a.val[0] | (dst->a.val[1] << 8));
    }
    e.p1 = p1;
    e.p2 = p2;
    e.p3 = p3;
    k_spinlock_key_t k = k_spin_lock(&lk);
    ring[head % LOG_N] = e;
    head++;
    k_spin_unlock(&lk, k);
}

static void on_connected(struct bt_conn *c, uint8_t err) {
    struct bt_conn_info info;
    uint16_t interval = 0;
    uint16_t timeout = 0;
    if (bt_conn_get_info(c, &info) == 0) {
        interval = info.le.interval;
        timeout = info.le.timeout;
    }
    put(1, c, err, interval, timeout);
}

static void on_disconnected(struct bt_conn *c, uint8_t reason) { put(2, c, reason, 0, 0); }

static void on_security(struct bt_conn *c, bt_security_t level, enum bt_security_err err) {
    put(3, c, (uint16_t)level, (uint16_t)err, 0);
}

static void on_params(struct bt_conn *c, uint16_t interval, uint16_t latency, uint16_t timeout) {
    put(4, c, interval, latency, timeout);
}

BT_CONN_CB_DEFINE(eyelash_ble_log_cb) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
    .security_changed = on_security,
    .le_param_updated = on_params,
};

/* out: [件数 u8][今の時刻 u32 LE][記録 16バイト x 件数（古い順）]。戻り値は書いたバイト数 */
size_t eyelash_ble_log_read(uint8_t *out, size_t max) {
    if (max < 5) {
        return 0;
    }
    k_spinlock_key_t k = k_spin_lock(&lk);
    uint32_t total = head;
    uint32_t have = total < LOG_N ? total : LOG_N;
    uint32_t fit = (uint32_t)((max - 5) / sizeof(struct art_ble_ev));
    uint32_t n = have < fit ? have : fit;
    uint32_t now = k_uptime_get_32();
    out[0] = (uint8_t)n;
    out[1] = now & 0xff;
    out[2] = (now >> 8) & 0xff;
    out[3] = (now >> 16) & 0xff;
    out[4] = (now >> 24) & 0xff;
    for (uint32_t i = 0; i < n; i++) {
        memcpy(out + 5 + i * sizeof(struct art_ble_ev), &ring[(total - n + i) % LOG_N], sizeof(struct art_ble_ev));
    }
    k_spin_unlock(&lk, k);
    return 5 + n * sizeof(struct art_ble_ev);
}

#else
size_t eyelash_ble_log_read(uint8_t *out, size_t max) {
    (void)out;
    (void)max;
    return 0;
}
#endif
