#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GLB 沿长轴的剖面量测 —— 分出「厚实车体」与「细突出物（炮管/保险杠/天线）」。

为什么需要它
------------
图生3D 出来的载具，包围盒的**长轴尺寸里含炮管**。而引擎侧 `VehicleSpec.len`
是 sim 用来判碰撞与遮挡的**车体盒**（`vehicle_blocked` / `vehicle_hit`），它表示的
应该是"车体占多大地方"，不是"连炮管算多长"。坦克的 76mm 炮管前伸约 1.7 m、
占整体长度的两成：拿"含炮管"的长度当碰撞盒 → "子弹在炮管前的空气里被挡下"；
拿车体当碰撞盒却按"含炮管"去归一化模型 → 坦克被静默缩到实车的七成多。
两个方向都错，所以先量出来再定。

判据（故意做成纯几何：不看名字、不看朝向）
------------------------------------------
沿长轴切 NBIN 片，每片取**另外两轴跨度的较大者**当该片"厚度"。
车体（含履带/车轮）一定厚，炮管/保险杠/天线一定薄。以全局最大厚度的
HULL_FRAC_MIN 为门槛，取**最长的连续达标段** = 车体，两端余下即突出物。

长轴由实测决定，**不猜**：图生3D 的长轴躺在哪个轴逐张不同，假定长轴=X
曾把坦克静默放大 7.03 倍（见 SKILL 的载具小节）。

只读，不改文件。用法：
    python tools/glb_profile.py --all
    python tools/glb_profile.py assets/art/veh/model/veh_tank.glb
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import glb_preview as G   # noqa: E402  复用它的 GLB 解析（含 BIN 块按前 3 字节比那处修正）

ROOT = os.path.dirname(HERE)
MODEL_DIR = os.path.join(ROOT, "assets", "art", "veh", "model")

NBIN = 120              # 沿长轴分的片数：再密就受三角面分布噪声影响
HULL_FRAC_MIN = 0.55    # 片厚度 ≥ 全局最大厚度的这个比例，才算"车体"
BAR = 46


def measure(path):
    js, bin_ = G.glb_read(path)
    pts = G.collect_points(js, bin_)
    name = os.path.basename(path)
    if not pts:
        print("%s : 没有顶点" % name)
        return None

    mn = [min(p[a] for p in pts) for a in range(3)]
    mx = [max(p[a] for p in pts) for a in range(3)]
    size = [mx[a] - mn[a] for a in range(3)]
    long_axis = max(range(3), key=lambda a: size[a])
    other = [a for a in (0, 1, 2) if a != long_axis]

    lo, hi = mn[long_axis], mx[long_axis]
    span = hi - lo

    # 逐片截面：记另外两轴各自的 min/max，再取跨度较大者当"厚度"
    bin_lo = [[None] * NBIN for _ in other]
    for p in pts:
        i = int((p[long_axis] - lo) / span * NBIN)
        i = 0 if i < 0 else (NBIN - 1 if i >= NBIN else i)
        for k, a in enumerate(other):
            bl = bin_lo[k][i]
            if bl is None:
                bin_lo[k][i] = [p[a], p[a]]
            else:
                if p[a] < bl[0]:
                    bl[0] = p[a]
                if p[a] > bl[1]:
                    bl[1] = p[a]

    thick = []
    for i in range(NBIN):
        w = [0.0 if bin_lo[k][i] is None else bin_lo[k][i][1] - bin_lo[k][i][0]
             for k in range(len(other))]
        thick.append(max(w) if w else 0.0)

    # 最长连续达标段 = 车体
    thr = max(thick) * HULL_FRAC_MIN
    best = (0, -1)
    i = 0
    while i < NBIN:
        if thick[i] >= thr:
            j = i
            while j + 1 < NBIN and thick[j + 1] >= thr:
                j += 1
            if j - i > best[1] - best[0]:
                best = (i, j)
            i = j + 1
        else:
            i += 1
    hull_n = best[1] - best[0] + 1
    hull_frac = hull_n / float(NBIN)

    print("%-16s 包围盒 (%.3f, %.3f, %.3f)  长轴 %s = %.3f" % (
        name, size[0], size[1], size[2], "XYZ"[long_axis], span))
    print("    车体 %.1f%% 长轴（余 %.1f%% 是突出物：左 %.1f%% 右 %.1f%%）" % (
        hull_frac * 100.0, (1.0 - hull_frac) * 100.0,
        best[0] / float(NBIN) * 100.0, (NBIN - 1 - best[1]) / float(NBIN) * 100.0))
    peak = max(thick) or 1.0
    for i in range(0, NBIN, 4):
        m = max(thick[i:i + 4])
        n = int(round(m / peak * BAR))
        mark = "#" if m >= thr else "."
        print("    |%s%s|%s %3d%%" % ("#" * n, " " * (BAR - n), mark,
                                      int(round((i + 2) / float(NBIN) * 100.0))))
    return hull_frac, size, long_axis


def main():
    args = sys.argv[1:]
    if not args or args[0] == "--all":
        files = sorted(os.path.join(MODEL_DIR, f) for f in os.listdir(MODEL_DIR)
                       if f.endswith(".glb"))
    else:
        files = args
    for f in files:
        measure(f)
        print("")


if __name__ == "__main__":
    main()
