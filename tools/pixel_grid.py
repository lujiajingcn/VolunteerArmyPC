# -*- coding: utf-8 -*-
"""
VolunteerArmyPC —— 局部 ASCII 亮度图

用途：回答"这个位置上到底画了东西没有"。HUD 的准星、命中标记这类元素只有十几像素，
缩略图上一眼看不出来，肉眼看原图又要靠猜。这里把一小块区域按亮度打成字符画，
有东西就是明显更亮的一撮，没有就是一片均匀。

字符： . <25（死黑）  : <60  - <110  = <160  + <205  # <245  @ >=245

用法：
    python tools/pixel_grid.py 图.png cx,cy [半径] [步长]
    例：python tools/pixel_grid.py cap_45s.png 1280,684 40 2
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_png import read_png  # noqa: E402

RAMP = ".:-=+#@"


def ch(lum):
    i = 0
    for t in (25, 60, 110, 160, 205, 245):
        if lum >= t:
            i += 1
    return RAMP[i]


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__)
        return 1
    path = a[0]
    cx, cy = (int(v) for v in a[1].split(","))
    r = int(a[2]) if len(a) > 2 else 40
    st = int(a[3]) if len(a) > 3 else 2

    w, h, chn, rows = read_png(path)
    print("%s  中心(%d,%d) 半径 %d 步长 %d" % (os.path.basename(path), cx, cy, r, st))
    hdr = "      " + "".join("%d" % ((x // 100) % 10) for x in range(cx - r, cx + r + 1, st))
    print(hdr)
    for y in range(cy - r, cy + r + 1, st):
        if y < 0 or y >= h:
            continue
        rr = rows[y]
        line = []
        for x in range(cx - r, cx + r + 1, st):
            i = x * chn
            lum = 0.2126 * rr[i] + 0.7152 * rr[i + 1] + 0.0722 * rr[i + 2]
            line.append(ch(lum))
        mark = "<--" if y == cy else ""
        print("%5d %s %s" % (y, "".join(line), mark))
    return 0


if __name__ == "__main__":
    sys.exit(main())
