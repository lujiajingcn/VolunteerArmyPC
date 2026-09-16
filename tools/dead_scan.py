# -*- coding: utf-8 -*-
"""
VolunteerArmyPC —— 死黑行列扫描

pixel_stats.py 只给一个总数，dead_mask.py 给出形状但还得靠眼睛量像素。
这个脚本补上中间那一环：把死黑按列（再按行）做直方图，自动合并成连续段并报出宽度，
于是「那根黑柱有多宽、在第几列、占屏宽百分之几」变成一个可以直接贴进结论的数字。

本机已经因为"照着缩略图目测坐标"取错过区域（取样点整整偏了一个物体），
所以凡是涉及"这块黑东西在哪"的问题，一律让脚本先给出坐标，再拿去取样。

用法：
    python tools/dead_scan.py 图.png                # 列向 + 行向
    python tools/dead_scan.py 图.png --y0 160 --y1 1150 --th 25
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_png import read_png  # noqa: E402


def segments(vals, step, gap=8, th=0.5):
    """把 [(坐标, 占比)] 里超过 th 的连续样本合并成段。"""
    hot = [(v, p) for v, p in vals if p > th]
    if not hot:
        return []
    out = []
    s = hot[0][0]
    prev = hot[0][0]
    for v, _ in hot[1:]:
        if v - prev > gap * step:
            out.append((s, prev))
            s = v
        prev = v
    out.append((s, prev))
    return out


def profile(w, h, ch, rows, axis, y0, y1, x0, x1, step, th):
    vals = []
    lo, hi = (x0, x1) if axis == "col" else (y0, y1)
    for v in range(lo, hi, step):
        d = n = 0
        span = range(y0, y1, step) if axis == "col" else range(x0, x1, step)
        for u in span:
            if axis == "col":
                i = v * ch
                r = rows[u]
            else:
                i = u * ch
                r = rows[v]
            lum = 0.2126 * r[i] + 0.7152 * r[i + 1] + 0.0722 * r[i + 2]
            n += 1
            if lum < th:
                d += 1
        vals.append((v, 100.0 * d / max(n, 1)))
    return vals


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 1
    path = a[0]

    def opt(name, default):
        return type(default)(a[a.index(name) + 1]) if name in a else default

    y0, y1 = opt("--y0", 0), opt("--y1", 10 ** 9)
    x0, x1 = opt("--x0", 0), opt("--x1", 10 ** 9)
    th = opt("--th", 25.0)
    step = opt("--step", 4)

    w, h, ch, rows = read_png(path)
    y1, x1 = min(y1, h), min(x1, w)
    print("%s  %dx%d  采样步长 %d  死黑阈值 %.0f  扫描窗口 x[%d,%d) y[%d,%d)"
          % (os.path.basename(path), w, h, step, th, x0, x1, y0, y1))

    for axis, label, full in (("col", "列", w), ("row", "行", h)):
        # 统计主轴时，另一轴的范围要收进窗口内，否则列向会把窗口外的行也算进来
        vals = profile(w, h, ch, rows, axis, y0, y1, x0, x1, step, th)
        segs = segments(vals, step)
        peak = max(vals, key=lambda t: t[1]) if vals else (0, 0.0)
        print("\n[%s向] 峰值在 %s=%d，该%s死黑占比 %.1f%%；连续段（占比>50%%）："
              % (label, label, peak[0], label, peak[1]))
        if not segs:
            print("    （无）")
        for s, e in segs:
            print("    %s %5d .. %5d   长约 %4d px  （占总%s长 %.1f%%）"
                  % (label, s, e, e - s + step, label, 100.0 * (e - s + step) / full))
    return 0


if __name__ == "__main__":
    sys.exit(main())
