#!/usr/bin/env python3
"""二値PNGを、LVGL の1ビットインデックス画像(C配列)に変換する。

使い方:  python tools/gen_art.py 入力.png 出力.c
- 入力は、キーボードで見たままの縦向き 68x137（幅68・高さ137）。上が画面の上。
  画面の上端20pxは、バッテリー/接続の表示なので、画像はその下の領域になる。
  画面の下端3pxは実機で見えないため、領域は137行。内部では下に白3行を足して140行にする。
  （互換のため 68x140 も受け付ける）
- 純粋な白と黒だけの画像。灰色があれば、数を表示して、中止する。
- 内部の向き（横140x縦68）へ、時計回り90度に回して変換する。
- 出力は ZMK の nice!view 用。index0=黒、index1=白（標準の art.c と同じ並び）。
"""
import sys
from PIL import Image

W, H = 140, 68      # 内部（ZMK/LVGL）の向き
VW, VH = 68, 140    # 内部で扱う縦向き（下3行は見えない領域）
VISIBLE_H = 137     # 実際に見える高さ


def convert(src, dst):
    im = Image.open(src)
    if im.size == (VW, VISIBLE_H):
        padded = Image.new(im.mode if im.mode in ("1", "L") else "L", (VW, VH), 255 if im.mode != "1" else 1)
        padded.paste(im if im.mode in ("1", "L") else im.convert("L"), (0, 0))
        im = padded
    if im.size != (VW, VH):
        sys.exit(f"サイズが違います: {im.size}（幅{VW} x 高さ{VISIBLE_H} が必要）")
    # 見たままの縦向き -> 内部の横向き（時計回り90度）。実機で確認済みの向き
    im = im.convert("L").transpose(Image.ROTATE_270)
    px = im
    data = list(px.get_flattened_data()) if hasattr(px, 'get_flattened_data') else list(px.getdata())
    gray = sum(1 for v in data if v not in (0, 255))
    if gray:
        sys.exit(f"白と黒以外の画素が {gray} 個あります。二値の画像にしてください")
    rowbytes = (W + 7) // 8
    out = bytearray()
    for y in range(H):
        row = bytearray(rowbytes)
        for x in range(W):
            if data[y * W + x] == 255:  # 白 = index1
                row[x // 8] |= 0x80 >> (x % 8)
        out += row
    lines = []
    for i in range(0, len(out), 15):
        lines.append("        " + ", ".join(f"0x{b:02x}" for b in out[i:i + 15]) + ",")
    body = "\n".join(lines)
    c = f"""/* 自動生成: tools/gen_art.py（編集しない） */
#include <lvgl.h>

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t custom_art_map[] = {{
        0x00, 0x00, 0x00, 0xff, /*Color of index 0*/
        0xff, 0xff, 0xff, 0xff, /*Color of index 1*/

{body}
}};

const lv_img_dsc_t custom_art = {{
    .header.cf = LV_IMG_CF_INDEXED_1BIT,
    .header.always_zero = 0,
    .header.reserved = 0,
    .header.w = {W},
    .header.h = {H},
    .data_size = {8 + len(out)},
    .data = custom_art_map,
}};
"""
    open(dst, "w", encoding="utf-8", newline="\n").write(c)
    print(f"OK: {dst}（{len(out)} バイト）")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    convert(sys.argv[1], sys.argv[2])
