/*
 * 本体の設定（アプリから変えられる）。フラッシュ（Zephyr settings）に保存して、起動時に読み込む。
 *   スクリーンセーバーの入り切り・始まるまでの時間・模様・流れる速さ、深い眠りまでの時間
 * 通信（art_usb.c）: 'Y'=読む → 'y' [12バイト]、'Z'=書く（同じ12バイト）
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zmk/activity.h>

#include "eyelash_art.h"

static struct art_cfg cur = {
    .ver = 1,
    .ss_enable = 1,
    .idle_s = 30,
    .sleep_min = 60,
    .pattern = 0,
    .stripe = 8,
    .fps = 10,
    .step = 2,
};

static void sanitize(struct art_cfg *c) {
    c->ver = 1;
    c->ss_enable = c->ss_enable ? 1 : 0;
    if (c->idle_s < 3) {
        c->idle_s = 3;
    }
    if (c->idle_s > 3600) {
        c->idle_s = 3600;
    }
    if (c->sleep_min > 1440) {
        c->sleep_min = 1440;
    }
    if (c->pattern > 3) {
        c->pattern = 0;
    }
    if (c->stripe < 2) {
        c->stripe = 2;
    }
    if (c->stripe > 32) {
        c->stripe = 32;
    }
    if (c->fps < 1) {
        c->fps = 1;
    }
    if (c->fps > 15) {
        c->fps = 15;
    }
    if (c->step < 1) {
        c->step = 1;
    }
    if (c->step > 8) {
        c->step = 8;
    }
}

/* ZMK の「使っていない」判定と、眠りの時間に反映する */
static void apply_activity(void) {
    zmk_activity_set_idle_ms((uint32_t)cur.idle_s * 1000u);
    zmk_activity_set_sleep_ms((uint32_t)cur.sleep_min * 60u * 1000u); /* 0 なら眠らない */
}

const struct art_cfg *eyelash_cfg(void) { return &cur; }

int eyelash_cfg_set(const struct art_cfg *c) {
    struct art_cfg n = *c;
    sanitize(&n);
    cur = n;
    apply_activity();
    return settings_save_one("eyelash/cfg", &cur, sizeof(cur));
}

static int cfg_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, "cfg") != 0 || len != sizeof(cur)) {
        return -ENOENT;
    }
    struct art_cfg n;
    if (read_cb(cb_arg, &n, sizeof(n)) != sizeof(n)) {
        return -EIO;
    }
    sanitize(&n);
    cur = n;
    apply_activity();
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(eyelash, "eyelash", NULL, cfg_set, NULL, NULL);
