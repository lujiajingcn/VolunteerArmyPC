# -*- coding: utf-8 -*-
"""裁剪 + 最近邻放大，把细小的光效从 1920 宽的截图里拉出来。

用法：
    python _fx_zoom.py <img> <out.png> <x0> <y0> <x1> <y1> [scale]

为什么需要它：光效只有几十~几千个像素（占画面 0.08%），
直接看 1920×1001 的缩略图根本分辨不出"那是一条曳光弹还是一块白石头"。
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crop_png import read_png, write_png  # noqa: E402


def main():
    a = sys.argv[1:]
    if len(a) < 6:
        print(__doc__)
        return 1
    src, out = a[0], a[1]
    x0, y0, x1, y1 = (int(v) for v in a[2:6])
    scale = int(a[6]) if len(a) > 6 else 3

    W, H, ch, rows = read_png(src)
    x0 = max(0, x0); y0 = max(0, y0)
    x1 = min(x1, W); y1 = min(y1, H)
    sw, sh = (x1 - x0) * scale, (y1 - y0) * scale
    outs = []
    for y in range(y0, y1):
        row = rows[y]
        line = bytearray(sw * 3)
        for x in range(x0, x1):
            o = x * ch
            base = (x - x0) * scale * 3
            for k in range(scale):
                q = base + k * 3
                line[q] = row[o]; line[q + 1] = row[o + 1]; line[q + 2] = row[o + 2]
        for _ in range(scale):
            outs.append(line)
    write_png(out, sw, sh, outs)
    print("wrote %s  %dx%d (源区域 %dx%d @ %d,%d, 放大 %dx)"
          % (out, sw, sh, x1 - x0, y1 - y0, x0, y0, scale))
    return 0


if __name__ == "__main__":
    sys.exit(main())
