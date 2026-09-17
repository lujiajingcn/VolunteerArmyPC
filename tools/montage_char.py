# -*- coding: utf-8 -*-
"""把 11 张胸像拼成一张联络表，一次看完。

为什么需要它：逐张查看 11 个文件既慢又费上下文，而"头像有没有切歪"
恰恰需要**并排**才容易发现 —— 单看一张时人脑没有参照，会把切歪的脑袋
当成"这张本来就长这样"。拼成一行排列后，尺寸与取景的不一致一眼可见。

用法：
    python tools/montage_char.py
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from probe_png import read_png  # noqa: E402
from prep_char import write_png  # noqa: E402

PORT_DIR = os.path.join(ROOT, "assets", "art", "char", "portrait")
OUT = os.path.join(ROOT, "sweep", "char_portraits.png")

ORDER = [
    "char_leader", "char_rifleman", "char_mg", "char_sniper",
    "char_at", "char_demo", "char_medic", "char_ammo",
    "char_enemy_rifle", "char_enemy_mg", "char_enemy_officer",
]
COLS = 6
GAP = 4
BG = 24


def main():
    tiles = []
    names = []
    for name in ORDER:
        p = os.path.join(PORT_DIR, name + ".png")
        if not os.path.isfile(p):
            print("缺 %s（跳过）" % name)
            continue
        w, h, ch, rows = read_png(p)
        tiles.append((w, h, ch, rows))
        names.append(name)
    if not tiles:
        print("没有任何胸像")
        return 1

    tw = max(t[0] for t in tiles)
    th = max(t[1] for t in tiles)
    cols = min(COLS, len(tiles))
    rows_n = (len(tiles) + cols - 1) // cols
    W = cols * tw + (cols + 1) * GAP
    H = rows_n * th + (rows_n + 1) * GAP

    out = [bytearray([BG] * (W * 3)) for _ in range(H)]
    for i, (w, h, ch, rows) in enumerate(tiles):
        c = i % cols
        r = i // cols
        ox = GAP + c * (tw + GAP)
        oy = GAP + r * (th + GAP)
        for y in range(h):
            src = rows[y]
            dst = out[oy + y]
            for x in range(w):
                si = x * ch
                di = (ox + x) * 3
                dst[di] = src[si]
                dst[di + 1] = src[si + 1]
                dst[di + 2] = src[si + 2]

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    write_png(OUT, W, H, 3, out)
    print("联络表 %dx%d -> %s" % (W, H, OUT))
    print("顺序：" + " ".join(names))
    return 0


if __name__ == "__main__":
    sys.exit(main())
