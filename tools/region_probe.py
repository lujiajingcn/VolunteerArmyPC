# -*- coding: utf-8 -*-
"""定点区域取样：把"画面里这块到底吃不吃某盏灯"变成一张对照表。

用法：python tools/region_probe.py 图1 图2 ...  —— 区域表写在下面的 REGIONS。
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_png import read_png, region_stats

# 坐标基于 2560x1369 视口（本机竖屏取证环境）。
# 这些坐标不是目测的 —— 先用 tools/dead_scan.py 扫出死黑在哪几列，
# 再按列段回推物体位置，最后用 tools/view_crop.py 裁出来确认。
# 目测缩略图定坐标已经错过一次（整整偏了一个物体），不要再犯。
REGIONS = [
    ("树干(黑洞柱)",  990,  300, 1050,  900),
    ("石块暗侧",     1240,  780, 1320,  890),
    ("石块顶面",     1230,  740, 1330,  770),
    ("远处碎石",     2380,  560, 2500,  640),
    ("草地",          300, 1050,  500, 1200),
    ("士兵",          330,  460,  420,  540),
]

def main():
    print("%-10s" % "区域" + "".join("%22s" % os.path.basename(os.path.dirname(p)) for p in sys.argv[1:]))
    for name, x0, y0, x1, y1 in REGIONS:
        cells = []
        for p in sys.argv[1:]:
            w, h, ch, rows = read_png(p)
            x1c, y1c = min(x1, w), min(y1, h)
            st = region_stats(rows, ch, x0, y0, x1c, y1c)
            r_, g_, b_ = st["rgb"]
            cells.append("%3.0f,%3.0f,%3.0f(L%3.0f)" % (r_, g_, b_, st["lum"]))
        print("%-10s" % name + "".join("%22s" % c for c in cells))
    return 0

if __name__ == "__main__":
    sys.exit(main())
