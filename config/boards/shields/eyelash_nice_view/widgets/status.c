/*
 * 左手(親機)の画面。nice!view の標準表示を作り変えたもの。
 *   上の行 : バッテリーと接続の記号（標準どおり）
 *   その下 : 画像（右手と同じ 68x140）。画像の左上に L<レイヤー番号>、右上に C<接続先> を重ねる
 *   切り替えたとき: 画面の中央に、番号を大きく、約1.5秒出す
 * Copyright (c) 2025 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/display.h>
#include "status.h"
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include "eyelash_art.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};

struct layer_status_state {
    zmk_keymap_layer_index_t index;
    const char *label;
};

/* ---- 上の行（標準どおり: 左にバッテリー、右に接続の記号） ---- */
static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_label_dsc_t icon_dsc;
    init_label_dsc(&icon_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);
    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);

    // Fill background
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    // Draw battery
    draw_battery(canvas, state);

    // Draw output status
    char output_text[10] = {};

    switch (state->selected_endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        strcat(output_text, LV_SYMBOL_USB);
        break;
    case ZMK_TRANSPORT_BLE:
        if (state->active_profile_bonded) {
            if (state->active_profile_connected) {
                strcat(output_text, LV_SYMBOL_WIFI);
            } else {
                strcat(output_text, LV_SYMBOL_CLOSE);
            }
        } else {
            strcat(output_text, LV_SYMBOL_SETTINGS);
        }
        break;
    }

    lv_canvas_draw_text(canvas, 0, 0, CANVAS_SIZE, &icon_dsc, output_text);

    // Rotate canvas
    rotate_canvas(canvas, cbuf);
}

/* ---- 画像の左上(L)・右上(C)に重ねる小さな番号 ----
 * 見たままの向きで小さな領域に描き、画面の向き（内部は横向き）に合わせて回して貼る。
 * 見たままの (u,v) の点は、内部では (x = H-1-v, y = u) になる。 */
#define LBL_W 24 /* 見たままの幅 */
#define LBL_H 16 /* 見たままの高さ */
static lv_obj_t *lbl_tmp;
static lv_color_t lbl_tmp_buf[LBL_W * LBL_H];
static lv_obj_t *lbl_l;
static lv_color_t lbl_l_buf[LBL_W * LBL_H];
static lv_obj_t *lbl_c;
static lv_color_t lbl_c_buf[LBL_W * LBL_H];

static void draw_label(lv_obj_t *dest, const char *text) {
    if (!lbl_tmp || !dest) {
        return;
    }
    lv_draw_rect_dsc_t bg_dsc;
    init_rect_dsc(&bg_dsc, LVGL_BACKGROUND);
    lv_draw_label_dsc_t txt_dsc;
    init_label_dsc(&txt_dsc, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);

    /* 背景は白（クロマキー色）。白い部分は透明になり、黒い文字だけが画像に重なる */
    lv_canvas_draw_rect(lbl_tmp, 0, 0, LBL_W, LBL_H, &bg_dsc);
    lv_canvas_draw_text(lbl_tmp, 0, 0, LBL_W, &txt_dsc, text);

    for (int v = 0; v < LBL_H; v++) {
        for (int u = 0; u < LBL_W; u++) {
            lv_canvas_set_px_color(dest, (LBL_H - 1) - v, u, lv_canvas_get_px(lbl_tmp, u, v));
        }
    }
    lv_obj_invalidate(dest);
}

/* ---- 切り替えたときの大きな表示 ---- */
static lv_obj_t *popup_obj;
static struct k_work_delayable popup_hide_work;
static int popup_last_profile = -1;
static int popup_last_layer = -1;

static void popup_hide_cb(struct k_work *work) {
    if (popup_obj) {
        lv_obj_add_flag(popup_obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void popup_show(struct zmk_widget_status *widget, const char *text) {
    if (!popup_obj) {
        return;
    }
    lv_draw_rect_dsc_t fg_dsc;
    init_rect_dsc(&fg_dsc, LVGL_FOREGROUND);
    lv_draw_rect_dsc_t bg_dsc;
    init_rect_dsc(&bg_dsc, LVGL_BACKGROUND);
    lv_draw_label_dsc_t big_dsc;
    init_label_dsc(&big_dsc, LVGL_FOREGROUND, &lv_font_montserrat_48, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(popup_obj, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &fg_dsc); // 枠
    lv_canvas_draw_rect(popup_obj, 2, 2, CANVAS_SIZE - 4, CANVAS_SIZE - 4, &bg_dsc);
    lv_canvas_draw_text(popup_obj, 0, 6, CANVAS_SIZE, &big_dsc, text);
    rotate_canvas(popup_obj, widget->cbuf2);

    lv_obj_clear_flag(popup_obj, LV_OBJ_FLAG_HIDDEN);
    k_work_reschedule_for_queue(zmk_display_work_q(), &popup_hide_work, K_MSEC(1500));
}

/* ---- バッテリー ---- */
static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

    widget->state.battery = state.level;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    return (struct battery_status_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

/* ---- 出力（接続先） ---- */
static void set_output_status(struct zmk_widget_status *widget,
                              const struct output_status_state *state) {
    widget->state.selected_endpoint = state->selected_endpoint;
    widget->state.active_profile_index = state->active_profile_index;
    widget->state.active_profile_connected = state->active_profile_connected;
    widget->state.active_profile_bonded = state->active_profile_bonded;

    draw_top(widget->obj, widget->cbuf, &widget->state);

    char t[8];
    snprintf(t, sizeof(t), "C%d", state->active_profile_index + 1);
    draw_label(lbl_c, t);
    if (popup_last_profile >= 0 && state->active_profile_index != popup_last_profile) {
        popup_show(widget, t);
    }
    popup_last_profile = state->active_profile_index;
}

static void output_status_update_cb(struct output_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_output_status(widget, &state); }
}

static struct output_status_state output_status_get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoints_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
#endif

/* ---- レイヤー ---- */
static void set_layer_status(struct zmk_widget_status *widget, struct layer_status_state state) {
    widget->state.layer_index = state.index;
    widget->state.layer_label = state.label;

    draw_top(widget->obj, widget->cbuf, &widget->state);

    char t[8];
    snprintf(t, sizeof(t), "L%d", (int)state.index);
    draw_label(lbl_l, t);
    if (popup_last_layer >= 0 && (int)state.index != popup_last_layer) {
        popup_show(widget, t);
    }
    popup_last_layer = (int)state.index;
}

static void layer_status_update_cb(struct layer_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_layer_status(widget, state); }
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state){
        .index = index, .label = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index))};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_status_get_state)

ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

/* ---- 組み立て ---- */
int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);

    /* 上の行（内部の x=92..159。バッテリー等は x>=143 の列）。最初の子にする */
    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    /* 画像（右手と同じ。内部の x=3..142）。実機で見えない列(x<3)を避けて3pxずらす */
    lv_obj_t *art = lv_img_create(widget->obj);
    eyelash_art_attach(art);
    lv_obj_align(art, LV_ALIGN_TOP_LEFT, 3, 0);

    /* 画像の左上(L)・右上(C)に重ねる番号。見たままの上端(Y=17)に接する。
     * 内部の座標: x = 160 - 17 - LBL_H = 127, y = 見たままの左端(X) */
    lbl_tmp = lv_canvas_create(widget->obj);
    lv_canvas_set_buffer(lbl_tmp, lbl_tmp_buf, LBL_W, LBL_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_flag(lbl_tmp, LV_OBJ_FLAG_HIDDEN);
    lbl_l = lv_canvas_create(widget->obj);
    lv_canvas_set_buffer(lbl_l, lbl_l_buf, LBL_H, LBL_W, LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED);
    lv_obj_align(lbl_l, LV_ALIGN_TOP_LEFT, 160 - 17 - LBL_H, 0);
    lbl_c = lv_canvas_create(widget->obj);
    lv_canvas_set_buffer(lbl_c, lbl_c_buf, LBL_H, LBL_W, LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED);
    lv_obj_align(lbl_c, LV_ALIGN_TOP_LEFT, 160 - 17 - LBL_H, 68 - LBL_W);

    /* 切り替えの大きな表示（画面の中央。内部の x=45..112）。普段は隠す */
    lv_obj_t *popup = lv_canvas_create(widget->obj);
    lv_obj_align(popup, LV_ALIGN_TOP_LEFT, 45, 0);
    lv_canvas_set_buffer(popup, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);
    popup_obj = popup;
    lv_obj_add_flag(popup, LV_OBJ_FLAG_HIDDEN);
    k_work_init_delayable(&popup_hide_work, popup_hide_cb);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_output_status_init();
    widget_layer_status_init();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
