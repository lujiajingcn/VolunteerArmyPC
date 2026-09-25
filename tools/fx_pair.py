# -*- coding: utf-8 -*-
"""射击光效 —— 成对差分定位

对 sweep/v_fx_diff（开）与 sweep/v_fx_diff_off（关）里**同名**的截图逐对相减，
输出每帧的差异像素数 / bbox / **符号**（开比关亮 or 暗）。

为什么要看符号：光效是**发光体**，开 → 关应该是"变暗"，
即 (off - on) > 0。若差异像素不少但符号是负的，说明差在别处
（比如某帧 HUD 没藏干净、或者交火时刻对不齐），不是光效。

用法：
    python _fx_pair.py <on_dir> <off_dir> [--thresh 10] [--stride 2]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crop_png import read_png  # noqa: E402  （tools/ 下的纯标准库 PNG 读写）


def pair(pa, pb, thresh=10, stride=1):
    Wa, Ha, ca, ra = read_png(pa)
    Wb, Hb, cb, rb = read_png(pb)
    if (Wa, Ha) != (Wb, Hb):
        return None
    W, H = Wa, Ha
    n = 0
    brighter = 0     # 开这张比关那张亮的像素数（tmp 用 off-on）
    sum_delta = 0
    minx, miny, maxx, maxy = W, H, -1, -1
    for y in range(0, H, stride):
        sra, srb = ra[y], rb[y]
        for x in range(0, W, stride):
            o, p = x * ca, x * cb
            da = (sra[o] + sra[o + 1] + sra[o + 2])
            db = (srb[p] + srb[p + 1] + srb[p + 2])
            d = abs(sra[o] - srb[p]) + abs(sra[o + 1] - srb[p + 1]) + abs(sra[o + 2] - srb[p + 2])
            if d > thresh * 3:
                n += 1
                sum_delta += (db - da)
                if db > da:
                    brighter += 1
                if x < minx:
                    minx = x
                if x > maxx:
                    maxx = x
                if y < miny:
                    miny = y
                if y > maxy:
                    maxy = y
    return {
        "n": n, "sampled": ((W + stride - 1) // stride) * ((H + stride - 1) // stride),
        "W": W, "H": H, "bbox": (minx, miny, maxx, maxy),
        "brighter": brighter, "sum_delta": sum_delta,
    }


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print("usage: _fx_pair.py <on_dir> <off_dir> [--thresh N] [--stride N]")
        return 1
    on_dir, off_dir = args[0], args[1]
    thresh, stride = 10, 2
    for i, a in enumerate(args):
        if a == "--thresh":
            thresh = int(args[i + 1])
        if a == "--stride":
            stride = int(args[i + 1])

    names = sorted(f for f in os.listdir(on_dir) if f.startswith("cap_") and f.endswith(".png"))
    if not names:
        print("目录里没有 cap_*.png：%s" % on_dir)
        return 1

    print("on=%s  off=%s  thresh=%d  stride=%d" % (on_dir, off_dir, thresh, stride))
    print("%-14s %9s %9s %7s  %-26s %s" % ("帧", "差异px", "占采样%", "更亮占比", "bbox(x0,y0,x1,y1)", "判定"))
    hit = []
    for nm in names:
        pa = os.path.join(on_dir, nm)
        pb = os.path.join(off_dir, nm)
        if not os.path.exists(pb):
            print("%-14s  （缺对照）" % nm)
            continue
        r = pair(pa, pb, thresh, stride)
        if r is None:
            print("%-14s  尺寸不一致" % nm)
            continue
        pct = 100.0 * r["n"] / max(1, r["sampled"])
        bri = 100.0 * r["brighter"] / max(1, r["n"])
        # 判定：差异够多 且 开比关**暗**（= 关了光效变亮？不对）
        # off - on > 0 ⇒ 开的这张更暗 ⇒ 反常；光效应当让开的那张更亮。
        if r["n"] < 20:
            verdict = "无差异（本帧没光效）"
        elif bri > 80.0:
            verdict = "异常：开的那张更暗"
        else:
            verdict = "有光效"
            hit.append(nm)
        print("%-14s %9d %8.2f%% %6.1f%%  %-26s %s"
              % (nm, r["n"], pct, bri, str(r["bbox"]), verdict))
    print()
    print("有光效的帧（%d 张）：%s" % (len(hit), " ".join(hit) if hit else "（无）"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
