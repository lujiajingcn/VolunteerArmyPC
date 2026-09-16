# -*- coding: utf-8 -*-
"""
VolunteerArmyPC —— 两张截图的像素差异定位

用途：把"视图模型到底占了屏幕的哪一块"从肉眼猜测变成数字。
做法：同一 VA_SEED 下拍两张（一张 VA_VM_HIDE=1），相减，差值超阈值的像素即枪。

用法：
    python tools/diff_png.py a.png b.png out.png [--thresh 10]

输出：差异像素数 / 占比 / 归一化 bbox / 每行每列的分布摘要，并把差异涂成品红写到 out.png。
"""
import struct
import sys
import zlib

from crop_png import read_png, write_png


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print("usage: diff_png.py a.png b.png [out.png] [--thresh N]")
        return 1
    pa, pb = args[0], args[1]
    out = args[2] if len(args) > 2 and not args[2].startswith("--") else None
    thresh = 10
    for i, a in enumerate(args):
        if a == "--thresh":
            thresh = int(args[i + 1])

    Wa, Ha, ca, ra = read_png(pa)
    Wb, Hb, cb, rb = read_png(pb)
    if (Wa, Ha) != (Wb, Hb):
        print("尺寸不一致：%dx%d vs %dx%d" % (Wa, Ha, Wb, Hb))
        return 1
    W, H = Wa, Ha

    diff_rows = []
    n = 0
    minx, miny, maxx, maxy = W, H, -1, -1
    col_hist = [0] * W
    row_hist = [0] * H
    for y in range(H):
        sra, srb = ra[y], rb[y]
        row = bytearray(W * 3)
        any_row = False
        for x in range(W):
            o = x * ca
            p = x * cb
            d = abs(sra[o] - srb[p]) + abs(sra[o + 1] - srb[p + 1]) + abs(sra[o + 2] - srb[p + 2])
            q = x * 3
            if d > thresh * 3:
                n += 1
                any_row = True
                col_hist[x] += 1
                if x < minx:
                    minx = x
                if x > maxx:
                    maxx = x
                row[0], row[1], row[2] = 255, 0, 170
            else:
                row[0] = sra[o] // 3
                row[1] = sra[o + 1] // 3
                row[2] = sra[o + 2] // 3
        if any_row:
            if y < miny:
                miny = y
            if y > maxy:
                maxy = y
            row_hist[y] = 1
        diff_rows.append(row)

    tot = W * H
    print("差异像素 %d / %d = %.2f%%" % (n, tot, 100.0 * n / tot))
    if maxx >= 0:
        print("bbox 像素   x[%d..%d] y[%d..%d]" % (minx, maxx, miny, maxy))
        print("bbox 归一化 u[%.3f..%.3f] v[%.3f..%.3f]  (u=x/W, v=y/H)"
              % (minx / W, maxx / W, miny / H, maxy / H))
        print("bbox 屏幕占比 宽 %.1f%%  高 %.1f%%" % (100.0 * (maxx - minx) / W, 100.0 * (maxy - miny) / H))
        # 每 5% 一档的横向分布，用来判断"枪是靠左还是靠右"
        print("横向分布（每 10% 一档，单位：该档差异像素占全部差异的百分比）：")
        band = [0] * 10
        for x in range(W):
            band[min(9, x * 10 // W)] += col_hist[x]
        print("  " + " ".join("%3d%%" % (100 * b // max(1, n)) for b in band))
        print("  档位从左到右依次是 u 0-10% ... 90-100%")
    else:
        print("（两张图完全一致）")

    if out is not None:
        write_png(out, W, H, diff_rows)
        print("wrote %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
