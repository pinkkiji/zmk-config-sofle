/*
 * 右手のUSBシリアル（画像送信用）。段階B1: 通信の確認だけ。
 * 'P' を受け取ると、名前と版を返す。
 * SPDX-License-Identifier: MIT
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(art_usb, LOG_LEVEL_INF);

static const struct device *const art_uart = DEVICE_DT_GET(DT_NODELABEL(art_cdc));

static void out(const char *s) {
    while (*s) {
        uart_poll_out(art_uart, (unsigned char)*s++);
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
    unsigned char ch;
    for (;;) {
        if (uart_poll_in(art_uart, &ch) == 0) {
            if (ch == 'P') {
                out("ESART-RIGHT v0\r\n");
            }
        } else {
            k_msleep(5);
        }
    }
}

K_THREAD_DEFINE(art_usb_tid, 1024, art_usb_thread, NULL, NULL, NULL, 10, 0, 0);
