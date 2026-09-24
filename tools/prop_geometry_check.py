#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""地物 GLB 的**几何验收** —— 按比例判是否退化，并核对归一化口径。

为什么需要它
------------
图生3D / 文生3D 的"任务成功"与"几何可用"是两件事。实测（2026-09-22）四个任务
全部 DONE、各 40 积分、各 5 万面、成品非空，其中两个的几何是塌的：

    prop_pine  世界包围盒 (0.070, 1.196, 0.075)  水平只有高的 6.3%  → 一根杆
    prop_bush  世界包围盒 (1.002, 0.0016, 0.988)  高只有最大维的 0.16% → 一张圆盘

而服务端报的每一条都是好的。所以验收必须**独立量几何**，不能读服务端的结论。

判据为什么必须是**比例**而不是绝对值
------------------------------------
原来引擎侧的闸门是"任一维 ≤ 1e-4 算退化"，而灌丛的高是 0.0016 —— 比阈值大 16 倍，
照样过闸，随后被归一化把 Y 拉 219 倍渲染成一根绿色圆柱。根因是**生成器输出不带单位**：
同一个模型整体放大 1000 倍，绝对阈值给的是**相反**的答案，而"它到底是不是一张饼"不变。

本工具的判据（与 scene_builder.cpp 的 load_prop_proto 一一对应）：
    ① 任何一维 < 最大维的 2%                      → 塌了（饼 / 纸片 / 杆）
    ② by_height 的地物（树）：冠幅 / 树高 < 0.15   → 只剩一根树干
    ③ 半径-高度剖面的锯齿度（2026-09-24 新增）     → "一摞飞盘"式的分层感

判据③为什么必须补（它拦的是"判据①②全过、形状仍然不像树"）
-----------------------------------------------------------
2026-09-23 的针叶树：包围盒 (0.695, 1.179, 0.661)、冠幅/高 0.589 ——
**判据①②都是过的**，可近景 1~2 m 读作"一摞水平飞盘"（枝条被生成成一片片薄盘）。
"塌了"和"分层"是两种不同的坏法，前两条只看得见后者之外的那种。
所以补一条**形状**判据：把高均分 N 层，每层取点到中轴的最大水平距离 r_i，
再看这条曲线的**锯齿**：
    光滑锥体  → r_i 基本单调下降（二阶差分小、符号翻转少）
    层叠圆盘  → r_i 反复起落（二阶差分大、符号翻转多）
两个数（`zigzag` / `flips`）都不靠眼睛，且**是相对判据** —— 跟已知件比，
不要拿绝对值当阈值（理由同下：生成器输出不带单位）。

【口径的真值来源是 C++ 那张表，这里不抄第二份】by_height 与 hw 直接从
scene_builder.cpp 的 kPropArt 正则抽出来。抄一份的后果是"代码改了、验收还按旧口径过"。

量的是什么坐标系
----------------
复用 glb_preview.collect_points()：它按节点层级把变换吃掉，给出**世界坐标**下的点，
与引擎侧 load_glb_root + collect_aabb 看到的是同一个盒子。
（生成器按 Z-up 建网格、节点只做绕 X 转 90°，所以"Y-up 世界里的高"就是点的 Y 跨度 ——
别拿 accessor 的原始 min/max 直接比，那是 Z-up 的。）

只读，不改文件。用法：
    python tools/prop_geometry_check.py                    # 全部已接入的地物
    python tools/prop_geometry_check.py <glb> [<glb> ...]
    python tools/prop_geometry_check.py --include-rejected # 连隔离区一起量
    python tools/prop_geometry_check.py --profile          # 追加打印 40 层的半径曲线
"""
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import glb_preview as G   # noqa: E402

ROOT = os.path.dirname(HERE)
PROP_MODEL_DIR = os.path.join(ROOT, "assets", "art", "prop", "model")
REJECTED_DIR = os.path.join(ROOT, "sweep", "gen3d_prop", "rejected")
SCENE_BUILDER = os.path.join(ROOT, "src", "node", "scene_builder.cpp")

SLENDER_FRAC = 0.02    # ① 任一维 < 最大维的这个比例 → 退化
CROWN_MIN = 0.15       # ② by_height 的冠幅 / 高 低于这个 → 只剩树干
SLICES = 40            # ③ 半径剖面的分层数


def read_prop_art_table():
    """从 scene_builder.cpp 的 kPropArt 抽 {键: (by_height, hw)}。

    【为什么正则抽 C++ 而不是在这里写一份】这张表是"什么口径"的唯一真值，
    验收与实现必须同源。抄第二份就一定会出现"代码改了、验收还按旧口径判"。
    表很短、格式固定（见 src/node/scene_builder.cpp 的 kPropArt），够用。
    """
    src = open(SCENE_BUILDER, encoding="utf-8").read()
    m = re.search(r"kPropArt\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        raise SystemExit("在 %s 里找不到 kPropArt 表" % SCENE_BUILDER)
    out = {}
    for key, by_h, hw in re.findall(
            r'\{\s*"([A-Za-z0-9_]+)"\s*,\s*(true|false)\s*,\s*([0-9.]+)f\s*\}', m.group(1)):
        out[key] = (by_h == "true", float(hw))
    if not out:
        raise SystemExit("kPropArt 表解析出来是空的 —— 表的写法可能变了")
    return out


def measure(path):
    js, bin_ = G.glb_read(path)
    pts = G.collect_points(js, bin_)
    if not pts:
        return None
    mn = [min(p[a] for p in pts) for a in range(3)]
    mx = [max(p[a] for p in pts) for a in range(3)]
    return {
        "size": [mx[a] - mn[a] for a in range(3)],
        "min": mn,
        "max": mx,
        "npts": len(pts),
        "pts": pts,
    }


def profile_stats(pts, slices=SLICES):
    """按高度分层量"半径-高度"剖面，量化"一摞飞盘"式的分层感（判据③）。

    【为什么需要它】判据①②只拦"几何塌了"，拦不住"几何达标、形状仍然不像树"。
    2026-09-23 那棵针叶树就是这种情况：包围盒 (0.695, 1.179, 0.661)、冠幅/高 0.589，
    两条判据全过，而近景 1~2 m 读作"一摞水平飞盘"（枝条被生成成一片片薄盘）。
    **"塌了"与"分层"是两种不同的坏法**，前者看绝对比例、后者只能看形状。

    做法：把 Y 均分 N 层，每层取所有点到**中轴**（包围盒 X/Z 中心）的最大水平距离 r_i：
        光滑锥体  → r_i 基本单调下降   → 二阶差分小、符号翻转少
        层叠圆盘  → r_i 反复起落       → 二阶差分大、符号翻转多

    两个指标都**没有绝对阈值**，是相对判据 —— 拿它跟已知件比（理由与判据①②相同：
    生成器输出不带单位，同一个模型整体缩放会改变绝对值而改变不了锯齿形状）：
        zigzag  = Σ|r_{i-1} − 2r_i + r_{i+1}| / (2·(N−2)·r_max)   归一化锯齿度
        flips   = 一阶差分 Δr 的符号翻转次数（跨过 0 才算）        "起伏几次"
        mono    = Δr ≤ 0 的占比                                     越接近 1 越像锥体
    """
    ys = [p[1] for p in pts]
    y0, y1 = min(ys), max(ys)
    h = y1 - y0
    if h <= 1e-9:
        return None
    cx = (min(p[0] for p in pts) + max(p[0] for p in pts)) / 2.0
    cz = (min(p[2] for p in pts) + max(p[2] for p in pts)) / 2.0
    buckets = [[0.0, 0] for _ in range(slices)]   # [最大半径, 点数]
    for p in pts:
        k = int((p[1] - y0) / h * slices)
        if k >= slices:
            k = slices - 1
        r = math.hypot(p[0] - cx, p[2] - cz)
        if r > buckets[k][0]:
            buckets[k][0] = r
        buckets[k][1] += 1
    r_i = [b[0] for b in buckets]
    r_max = max(r_i)
    if r_max <= 1e-9:
        return None
    # 极少数空层（点全落在别的层里）：用左右邻居的较大值补，避免凭空造出一个 0
    # 把剖面砸出一个假锯齿 —— 那正是这条判据最怕的假信号。
    empty = sum(1 for b in buckets if b[1] == 0)
    for i in range(slices):
        if buckets[i][1] == 0:
            nb = [r_i[j] for j in (i - 1, i + 1) if 0 <= j < slices and buckets[j][1] > 0]
            r_i[i] = max(nb) if nb else r_max * 0.5
    d = [r_i[i + 1] - r_i[i] for i in range(slices - 1)]
    flips = sum(1 for i in range(len(d) - 1) if d[i] * d[i + 1] < 0)
    sec = sum(abs(r_i[i - 1] - 2 * r_i[i] + r_i[i + 1]) for i in range(1, slices - 1))
    return {
        "r_max": r_max,
        "h": h,
        "crown_over_h": 2.0 * r_max / h,
        "zigzag": sec / (2.0 * (slices - 2) * r_max),
        "flips": flips,
        "mono": sum(1 for x in d if x <= 0) / max(1, len(d)),
        "empty": empty,
        "curve": r_i,
    }


def check(path, table, show_profile=False):
    name = os.path.basename(path).replace(".glb", "")
    mb = os.path.getsize(path) / 1048576.0
    r = measure(path)
    print("=== %s  (%.2f MB)" % (name, mb))
    if r is None:
        print("    ❌ 没有顶点 —— 空网格\n")
        return False

    sx, sy, sz = r["size"]
    big = max(r["size"])
    print("    世界包围盒 X=%.4f  Y=%.4f  Z=%.4f   （顶点 %d 个）" % (sx, sy, sz, r["npts"]))

    # 复刻 load_prop_proto 的朝向对齐：长轴躺 Z 上就绕 Y 转 90°，让长轴落到 X
    align = (sz > sx)
    hw_meas = max(sx, sz) if not align else max(sz, sx)
    hy_meas = sy
    print("    朝向对齐：%s    水平尺度（长轴）=%.4f  高=%.4f  高/宽=%.3f"
          % ("绕Y转90°（长轴在Z）" if align else "不用转（长轴在X）",
             hw_meas, hy_meas, hy_meas / max(hw_meas, 1e-9)))

    ok = True
    # ① 任一维 < 最大维的 2%
    if min(r["size"]) < big * SLENDER_FRAC:
        print("    ❌ 判据①：最小维 %.4f < 最大维的 %.0f%%（%.4f）→ 塌成饼 / 纸片 / 杆"
              % (min(r["size"]), SLENDER_FRAC * 100, big * SLENDER_FRAC))
        ok = False
    else:
        print("    ✅ 判据①：最小维 %.4f ≥ 最大维的 %.0f%%"
              % (min(r["size"]), SLENDER_FRAC * 100))

    # ② by_height 的地物要求冠幅 / 高
    if name in table:
        by_h, hw = table[name]
        print("    口径（取自 scene_builder.cpp 的 kPropArt）：by_height=%s，hw=%.2f"
              % ("true" if by_h else "false", hw))
        if by_h:
            crown = hw_meas / max(hy_meas, 1e-9)
            if crown < CROWN_MIN:
                print("    ❌ 判据②：冠幅/高 = %.3f < %.2f → 只剩一根树干"
                      % (crown, CROWN_MIN))
                ok = False
            else:
                print("    ✅ 判据②：冠幅/高 = %.3f ≥ %.2f" % (crown, CROWN_MIN))
        else:
            print("    ℹ 归一化后会被压到 高/宽 = hw = %.2f（原样测出 %.3f，口径会覆盖）"
                  % (hw, hy_meas / max(hw_meas, 1e-9)))
    else:
        print("    ⚠ 键 %r 不在 kPropArt 表里 —— 引擎侧会走回退路径，不会渲染真模型" % name)

    # ③ 半径-高度剖面（形状判据，只报数）
    ps = profile_stats(r["pts"])
    if ps is not None:
        print("    📐 剖面：冠幅/高=%.3f 锯齿度 zigzag=%.4f 起伏 flips=%d 单调占比=%.2f%s"
              % (ps["crown_over_h"], ps["zigzag"], ps["flips"], ps["mono"],
                 "（空层 %d，已用邻居补）" % ps["empty"] if ps["empty"] else ""))
        print("       参考：光滑锥体 zigzag 应明显小于层叠圆盘；"
              "本条**没有绝对阈值**，看的是与已知件的相对高低")
        if show_profile:
            print("       40 层半径（自下而上，按 r_max=%.3f 归一化）：" % ps["r_max"])
            print("       " + " ".join("%.2f" % (x / ps["r_max"]) for x in ps["curve"]))

    print("    → %s\n" % ("✅ 可用" if ok else "❌ 退化，不接进游戏"))
    return ok


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    table = read_prop_art_table()
    paths = list(args)
    if not paths:
        d = PROP_MODEL_DIR
        paths = [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith(".glb")]
        if "--include-rejected" in flags and os.path.isdir(REJECTED_DIR):
            paths += [os.path.join(REJECTED_DIR, f)
                      for f in sorted(os.listdir(REJECTED_DIR)) if f.endswith(".glb")]
    if not paths:
        print("没有可量的 GLB（%s 是空的）" % PROP_MODEL_DIR)
        return 1
    print("kPropArt 表（真值来源：src/node/scene_builder.cpp）：%s\n"
          % "  ".join("%s(by_h=%s,hw=%.2f)" % (k, v[0], v[1]) for k, v in sorted(table.items())))
    show_profile = "--profile" in flags
    oks = [check(p, table, show_profile) for p in paths]
    print("合计 %d 个：可用 %d，退化 %d" % (len(oks), sum(oks), len(oks) - sum(oks)))
    return 0 if all(oks) else 2


if __name__ == "__main__":
    sys.exit(main())
