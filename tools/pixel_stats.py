# -*- coding: utf-8 -*-
"""
VolunteerArmyPC —— 全图统计探针（死黑 / 过曝 / 均值 / 洋红占比）

为什么需要它：
「这张图看着是不是太黑」这句判断，肉眼在不同显示器上给出不同答案，而且
本机已经不止一次在「肉眼认几何体 / 肉眼判亮度」上出错。这里把三个客观量固定下来：

    死黑%  —— 亮度 < 25 的像素占比。背光面塌成纯黑时这个数会明显抬升。
              参考系：晴天基准 7.98%；把环境光拉到可用档位应当降到 1% 量级。
    过曝%  —— 亮度 > 245 的像素占比。环境光加过头、或雾/辉光失控时会飙起来。
    均值   —— 全图平均 RGB。用来确认「改一处没有把整张画面拉亮/拉暗」。
    洋红%  —— R>G+14 且 B>G+14 的像素占比。专门用来判定环境光路径是否接通：
              把 ambient_light_color 换成 (1,0,1) 之后，凡是「只被环境光照亮」
              的表面都会染上洋红，这个数就会抬起来；如果纹丝不动，说明那条路
              根本没接上（而不是「量太小」）。

用法：
    python tools/pixel_stats.py 图1.png [图2.png ...]
    python tools/pixel_stats.py --diff 图A.png 图B.png      # 逐像素差的分箱统计
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_png import read_png  # noqa: E402


def stats(path, step=2):
    w, h, ch, rows = read_png(path)
    n = dead = over = mag = 0
    sr = sg = sb = 0.0
    for y in range(0, h, step):
        r = rows[y]
        for x in range(0, w, step):
            i = x * ch
            R, G, B = r[i], r[i + 1], r[i + 2]
            n += 1
            lum = 0.2126 * R + 0.7152 * G + 0.0722 * B
            if lum < 25.0:
                dead += 1
            if lum > 245.0:
                over += 1
            if R > G + 14 and B > G + 14:
                mag += 1
            sr += R
            sg += G
            sb += B
    return {
        "dead": 100.0 * dead / n,
        "over": 100.0 * over / n,
        "mag": 100.0 * mag / n,
        "mean": (sr / n, sg / n, sb / n),
    }


def diff(a, b, step=2):
    wa, ha, ca, ra = read_png(a)
    wb, hb, cb, rb = read_png(b)
    if (wa, ha) != (wb, hb):
        print("尺寸不同：%s=%dx%d %s=%dx%d" % (a, wa, ha, b, wb, hb))
        return
    n = 0
    buckets = [0] * 6          # 差值分箱：0 / 1-4 / 5-15 / 16-40 / 41-90 / >90
    worst = 0
    for y in range(0, ha, step):
        la, lb = ra[y], rb[y]
        for x in range(0, wa, step):
            ia, ib = x * ca, x * cb
            d = max(abs(la[ia] - lb[ib]), abs(la[ia + 1] - lb[ib + 1]), abs(la[ia + 2] - lb[ib + 2]))
            n += 1
            if d == 0:
                buckets[0] += 1
            elif d <= 4:
                buckets[1] += 1
            elif d <= 15:
                buckets[2] += 1
            elif d <= 40:
                buckets[3] += 1
            elif d <= 90:
                buckets[4] += 1
            else:
                buckets[5] += 1
            if d > worst:
                worst = d
    names = ["完全相同", "1~4", "5~15", "16~40", "41~90", ">90"]
    print("对比 %s  ←→  %s" % (os.path.basename(a), os.path.basename(b)))
    for nm, c in zip(names, buckets):
        print("   %-8s %7.2f%%" % (nm, 100.0 * c / n))
    print("   最大单像素差 %d" % worst)


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    if args[0] == "--diff":
        diff(args[1], args[2])
        return 0
    base = None
    for p in args:
        s = stats(p)
        tag = os.path.basename(os.path.dirname(p)) + "/" + os.path.basename(p)
        tail = ""
        if base is not None:
            tail = "   Δ死黑 %+5.2f  Δ过曝 %+5.2f" % (s["dead"] - base["dead"], s["over"] - base["over"])
        print("%-34s 死黑 %5.2f%%  过曝 %5.2f%%  均值 RGB(%3.0f,%3.0f,%3.0f)  洋红 %5.2f%%%s"
              % (tag, s["dead"], s["over"], s["mean"][0], s["mean"][1], s["mean"][2], s["mag"], tail))
        if base is None:
            base = s
    return 0


if __name__ == "__main__":
    sys.exit(main())
