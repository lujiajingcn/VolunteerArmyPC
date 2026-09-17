# -*- coding: utf-8 -*-
"""VolunteerArmyPC —— 横向渐变「分段条纹」探针

为什么需要这个：
  用「一排不同 alpha 的 draw_rect」叠横向渐变时，会同时引入两种可见缺陷 ——
    1) N 段离散 alpha 在段边界硬跳变（40 段就是 40 条竖纹）；
    2) 相邻矩形为了不留缝常写成 width = 段宽 + 1，重叠的 1px 被合成两次，
       竖纹更明显。
  这两种缺陷在缩略图上很容易被当成「压缩噪点」而忽略，必须客观量出来。

判据：
  取一条水平线带（默认画面中上部、通常是天空这种平坦区域），算逐列平均亮度，
  再看相邻列之间的「硬跳变」。
  · 平滑渐变：跳变基本为 0，只受渲染抖动影响（<1）
  · 分段条纹：出现 N 个 ~Δlum 的尖峰，且**间距规则**

用法：
    python tools/band_probe.py 图.png [--y0 100 --y1 400] [--x0 0] [--x1 0] [--thr 1.2]
       --x1 0 表示到图宽
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from probe_png import read_png  # noqa: E402


def lum(r, i):
    return 0.2126 * r[i] + 0.7152 * r[i + 1] + 0.0722 * r[i + 2]


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 1
    path = a[0]
    y0, y1, x0, x1, thr = 100, 400, 0, 0, 1.2
    for i, t in enumerate(a):
        if t == "--y0":
            y0 = int(a[i + 1])
        elif t == "--y1":
            y1 = int(a[i + 1])
        elif t == "--x0":
            x0 = int(a[i + 1])
        elif t == "--x1":
            x1 = int(a[i + 1])
        elif t == "--thr":
            thr = float(a[i + 1])

    w, h, ch, rows = read_png(path)
    x1 = x1 or w
    y0, y1 = max(0, y0), min(h, y1)
    x0, x1 = max(0, x0), min(w, x1)
    if y1 - y0 < 2 or x1 - x0 < 4:
        print("取样区太小")
        return 1

    # 逐列平均亮度
    col = []
    for x in range(x0, x1):
        s = 0.0
        for y in range(y0, y1):
            s += lum(rows[y], x * ch)
        col.append(s / (y1 - y0))

    jumps = []
    for i in range(1, len(col)):
        d = col[i] - col[i - 1]
        if abs(d) > thr:
            jumps.append((x0 + i, d))

    lo, hi = min(col), max(col)
    print("%s  取样 x[%d,%d) y[%d,%d)" % (os.path.basename(path), x0, x1, y0, y1))
    print("  列均值范围 %.2f .. %.2f  (跨度 %.2f)" % (lo, hi, hi - lo))
    print("  硬跳变(>%.2f) 个数 = %d" % (thr, len(jumps)))
    if jumps:
        mx = max(jumps, key=lambda j: abs(j[1]))
        print("  最大跳变 %.2f @ x=%d" % (mx[1], mx[0]))
        # 间距是否规则 —— 规则就说明是"分段"，不是画面本身的纹理
        if len(jumps) > 2:
            gaps = [jumps[i][0] - jumps[i - 1][0] for i in range(1, len(jumps))]
            gaps_s = sorted(gaps)
            med = gaps_s[len(gaps_s) // 2]
            print("  跳变间距 中位 %d px  (最小 %d / 最大 %d)"
                  % (med, gaps_s[0], gaps_s[-1]))
            near = sum(1 for g in gaps if abs(g - med) <= max(2, med // 6))
            print("  间距接近中位的比例 %.0f%%  → %s"
                  % (100.0 * near / len(gaps),
                     "规则分段（疑似合成条纹）" if near >= 0.7 * len(gaps) else "不规则（可能是画面内容）"))
    else:
        print("  无硬跳变 → 渐变连续")
    return 0


if __name__ == "__main__":
    sys.exit(main())
