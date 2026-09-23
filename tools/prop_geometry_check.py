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
"""
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
    }


def check(path, table):
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
    oks = [check(p, table) for p in paths]
    print("合计 %d 个：可用 %d，退化 %d" % (len(oks), sum(oks), len(oks) - sum(oks)))
    return 0 if all(oks) else 2


if __name__ == "__main__":
    sys.exit(main())
