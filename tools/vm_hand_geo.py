#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""第一人称手模 GLB 的几何验收 —— 判退化、**实测长轴**、定位拳端、核前臂占比。

为什么不复用 prop_geometry_check.py
-----------------------------------
那套口径的判据②③都建立在 `kPropArt` 表上（by_height / hw 归一化），而**手不在那张表里**：
手是视图模型，接入时的基准是"长轴长度 = 腕横纹→指节 ≈ 0.115 m"这条**跨枪通用的语义**，
不是"归一化到单位原型再压 Y"。把手的 GLB 丢进那个工具只会得到一行
"⚠ 键不在 kPropArt 表里"和一个与实际接入无关的比例。所以同类判据要重写，但**量测复用**。

判据（每条都对着一个**已经踩过**的坑）
--------------------------------------
    ① 任一维 < 最大维的 2%            → 塌成饼/纸片/杆（与 prop 那条同源）
    ② 长轴**实测**报出来              → 见下"长轴躺在哪个轴逐张不同"
    ③ 长轴 / 次长轴 ≤ ARM_MAX         → 又带了一长截前臂
    ④ 最薄维 / 次长维 ≥ FLAT_MIN      → 被压扁（手成了片）
    ⑤ 沿长轴的粗细剖面（**观察项，不是判据**）→ 末层半径收不收敛，旁证"带没带长前臂"

判据⑤为什么**不是**判据（这条是自打脸记下来的）
---------------------------------------------
原设计想用它自动判"哪端是拳"：理由看着很硬 —— 拳端粗、腕端细。
**用 09-21 的已知件一量就否了**：那个件是"握拳 + 一截腕柱"，长轴两端都是柱，
实测 0.226 / 0.244 只差 1.08 倍，而半径剖面的峰值落在长轴 30% 处（拳在中段偏一侧）。
所以它**判不出拳端**，朝向仍然只能靠 `glb_preview.py` 的三视图目视判读 + 渲染验证。
留着它是因为它能答另一个真正有用的问题：**这件是不是又带了一长截前臂**
（末层半径迟迟不收敛 = 腕柱长，是判据③之外的第二道旁证）。
——记这一笔是因为"造一个看着很硬的判据、其实量的是别的东西"正是本工程反复出现的那类错。

判据③为什么存在（这一条是真金白银换来的）
----------------------------------------
09-21 那个模型自带约 40% 前臂，导致**归一化基准选错就会整体放大 1.6 倍**：
按"腕→中指尖 18.5 cm"归一化 → 错；按"腕横纹→指节 0.115 m" → 对。
这两个数看着都对、都能自圆其说，唯一能区分它们的是**模型里到底有没有那截前臂**。
③ 把这件事从"回头看参考图"变成"量一下长轴/次长轴"：
09-21 旧件 (0.723, 0.916, 0.416) → 0.916/0.723 = **1.267**（确实带前臂）。
反过来说，③ 只是**报警**不是判决 —— 带前臂的模型也能用，只要归一化按对的那个数。

【口径的真值来源是 C++ 那张表，这里不抄第二份】
`len_m` / `rot` / `place` 直接从 scene_builder.cpp 的 `kVmHandArt` 正则抽出来 ——
它们同时是"接入时按多少米归一化、转多少度、挪多少"的实际取值。
抄一份的后果是"代码改了、验收还按旧口径过"。

只读，不改文件。用法：
    python tools/vm_hand_geo.py                     # assets/art/vm/model/*.glb
    python tools/vm_hand_geo.py <glb> [<glb> ...]
    python tools/vm_hand_geo.py --profile           # 追加打印 10 层粗细曲线
"""
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import glb_preview as G   # noqa: E402

ROOT = os.path.dirname(HERE)
VM_MODEL_DIR = os.path.join(ROOT, "assets", "art", "vm", "model")
REJECTED_DIR = os.path.join(ROOT, "sweep", "gen3d_vm", "rejected")
SCENE_BUILDER = os.path.join(ROOT, "src", "node", "scene_builder.cpp")

SLENDER_FRAC = 0.02    # ① 任一维 < 最大维的这个比例 → 退化
ARM_MAX = 1.45         # ③ 长轴 / 次长轴 超过它 → 大概率又带了一长截前臂
FLAT_MIN = 0.30        # ④ 最薄维 / 次长维 低于它 → 被压扁
LAYERS = 10            # ⑤ 沿长轴的层数


def read_hand_table():
    """从 scene_builder.cpp 的 kVmHandArt 抽 {键: (len_m, rot, place, borrow)}。

    表里现在只有 vm_hand_r / vm_hand_l 两行，格式固定：
        { "vm_hand_r", "右手（扳机手）", 0.115f, { 4.1f, 20.2f, 48.5f }, { … }, nullptr },
    """
    src = open(SCENE_BUILDER, encoding="utf-8").read()
    m = re.search(r"kVmHandArt\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        raise SystemExit("在 %s 里找不到 kVmHandArt 表" % SCENE_BUILDER)
    out = {}
    for key, lab, ln, rot, place, borrow in re.findall(
            r'\{\s*"([A-Za-z0-9_]+)"\s*,\s*"([^"]*)"\s*,\s*([0-9.]+)f\s*,'
            r'\s*\{([^}]*)\}\s*,\s*\{([^}]*)\}\s*,\s*(nullptr|"[A-Za-z0-9_]+")\s*\}',
            m.group(1), re.S):
        out[key] = (float(ln),
                    " ".join(rot.split()),
                    " ".join(place.split()),
                    None if borrow == "nullptr" else borrow.strip('"'))
    if not out:
        raise SystemExit("kVmHandArt 表解析出来是空的 —— 表的写法可能变了")
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
        "min": mn, "max": mx,
        "npts": len(pts), "pts": pts,
    }


def thickness_profile(pts, axis, layers=LAYERS):
    """沿 `axis`（0/1/2）均分 layers 层，量每层"到该层形心的最大水平距离"。

    返回 (半径列表, 各层点数)。**粗端 = 拳、细端 = 腕**。
    取"到本层形心的最大距离"而不是"到全局中轴"：手是弯的，
    全局中轴会把手腕那截细柱算得偏粗，反而分不出两端。
    """
    u = [p[axis] for p in pts]
    u0, u1 = min(u), max(u)
    span = u1 - u0
    if span <= 1e-9:
        return None, None
    other = [a for a in range(3) if a != axis]
    buckets = [[] for _ in range(layers)]
    for p in pts:
        k = int((p[axis] - u0) / span * layers)
        if k >= layers:
            k = layers - 1
        buckets[k].append(p)
    rs, ns = [], []
    for b in buckets:
        ns.append(len(b))
        if not b:
            rs.append(0.0)
            continue
        c = [sum(p[a] for p in b) / len(b) for a in other]
        rs.append(max(math.hypot(q[other[0]] - c[0], q[other[1]] - c[1]) for q in b))
    # 空层用邻居补（与 prop 工具同一条理由：空层会凭空造出一个 0 把曲线砸出假锯齿）
    for i in range(layers):
        if ns[i] == 0:
            nb = [rs[j] for j in (i - 1, i + 1)
                  if 0 <= j < layers and ns[j] > 0]
            rs[i] = max(nb) if nb else (max(rs) if max(rs) > 0 else 1.0)
    return rs, ns


def check(path, table, show_profile=False):
    name = os.path.basename(path).replace(".glb", "")
    mb = os.path.getsize(path) / 1048576.0
    r = measure(path)
    print("=== %s  (%.2f MB)" % (name, mb))
    if r is None:
        print("    ❌ 没有顶点 —— 空网格\n")
        return False

    sx, sy, sz = r["size"]
    order = sorted(range(3), key=lambda a: r["size"][a])   # 短→长
    short, mid, long_ = order[0], order[1], order[2]
    AX = "XYZ"
    big = r["size"][long_]
    print("    世界包围盒 X=%.4f  Y=%.4f  Z=%.4f   （顶点 %d 个）" % (sx, sy, sz, r["npts"]))

    ok = True
    # ① 退化
    if min(r["size"]) < big * SLENDER_FRAC:
        print("    ❌ 判据①：最小维 %.4f < 最大维的 %.0f%%（%.4f）→ 塌成饼 / 纸片 / 杆"
              % (min(r["size"]), SLENDER_FRAC * 100, big * SLENDER_FRAC))
        ok = False
    else:
        print("    ✅ 判据①：最小维 %.4f ≥ 最大维的 %.0f%%"
              % (min(r["size"]), SLENDER_FRAC * 100))

    # ② 长轴**实测**（不假定是 Y！载具那轮的教训：长轴躺在哪个轴逐张不同，
    #    假定错了不报错、只静默转坏 —— 这里必须把实测结果打出来给对轴用）
    print("    ② 长轴实测在 **%s**（%.4f）  次长 %s=%.4f  最薄 %s=%.4f"
          % (AX[long_], big, AX[mid], r["size"][mid], AX[short], r["size"][short]))

    # ③ 前臂占比
    arm = big / max(r["size"][mid], 1e-9)
    if arm > ARM_MAX:
        print("    ❌ 判据③：长轴/次长轴 = %.3f > %.2f → 又带了一长截前臂；"
              "归一化基准要按「腕横纹→指节」而不是「腕→中指尖」" % (arm, ARM_MAX))
        ok = False
    else:
        print("    ✅ 判据③：长轴/次长轴 = %.3f ≤ %.2f（前臂占比正常）" % (arm, ARM_MAX))

    # ④ 被压扁
    flat = r["size"][short] / max(r["size"][mid], 1e-9)
    if flat < FLAT_MIN:
        print("    ❌ 判据④：最薄维/次长维 = %.3f < %.2f → 手被压成了片"
              % (flat, FLAT_MIN))
        ok = False
    else:
        print("    ✅ 判据④：最薄维/次长维 = %.3f ≥ %.2f（有厚度）" % (flat, FLAT_MIN))

    # ⑤ 长轴两端的粗细 —— 【观察项，**不是**判据，别拿它定朝向】
    #
    # 原设计以为"拳端必然粗、腕端必然细"，于是能自动判出"哪端是拳"。
    # **用 09-21 的已知件一量就否掉了**：那个件是"握拳 + 一截腕柱"，
    # 长轴两端**都是柱**，实测两端半径 0.226 / 0.244（只差 1.08 倍），
    # 而半径剖面的**峰值落在长轴 30% 处**（拳在中段偏一侧），不在任何一端。
    # 结论：这条曲线**判不出哪端是拳**，朝向仍然只能靠三视图目视判读 + 渲染验证。
    # 保留它是因为它能答另一个有用的问题：**这件是不是又带了一长截前臂**
    # （末层半径迟迟不收敛 = 腕柱很长，判据③之外的第二道旁证）。
    rs, ns = thickness_profile(r["pts"], long_)
    if rs:
        head, tail = sum(rs[:2]) / 2.0, sum(rs[-2:]) / 2.0
        rmax = max(rs)
        peak = rs.index(rmax)
        print("    ⑤ 沿 %s 的粗细（观察项，不能定朝向）：− 端 %.4f，+ 端 %.4f（%.2f 倍）；"
              "峰值在长轴 %.0f%% 处"
              % (AX[long_], head, tail, max(head, tail) / max(min(head, tail), 1e-9),
                 100.0 * peak / max(1, len(rs) - 1)))
        print("       ⚠ 两端等粗 / 峰值在中段 = 「握拳 + 长腕柱」的典型剖面 —— "
              "**判不出拳在哪端**，朝向必须靠三视图判读；末层半径 %.2f 说明腕柱%s"
              % (rs[-1] / max(rmax, 1e-9),
                 "很长（归一化基准要小心）" if rs[-1] / max(rmax, 1e-9) > 0.45 else "不长"))
        if show_profile:
            print("       10 层半径（自 − 端到 + 端，按最大值归一化）：")
            print("       " + " ".join("%.2f" % (x / rmax) for x in rs))
            print("       各层点数：" + " ".join("%d" % n for n in ns))

    # 与已标定的接入口径对照 —— 直接把"该不该改 len_m / rot"摆出来
    if name in table:
        ln_m, rot, place, borrow = table[name]
        print("    已标定口径（取自 scene_builder.cpp 的 kVmHandArt）："
              "len_m=%.4f m  rot=%s  place=%s%s"
              % (ln_m, rot, place, "" if borrow is None else "  借 %s" % borrow))
        print("      → 本件长轴 %.4f m ÷ len_m %.4f = **%.2f 倍**（= 接进去时的单位原型尺度）"
              % (big, ln_m, big / max(ln_m, 1e-9)))
        print("        换件后若不改 len_m，手会按这个倍数被放大/缩小")
    else:
        print("    ⚠ 键 %r 不在 kVmHandArt 表里 —— 引擎侧不会有这份模型的接入记录"
              "（比对件 / 隔离件属正常）" % name)

    print("    → %s\n" % ("✅ 几何可用" if ok else "❌ 不接进游戏"))
    return ok


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    table = read_hand_table()
    paths = list(args)
    if not paths:
        paths = [os.path.join(VM_MODEL_DIR, f)
                 for f in sorted(os.listdir(VM_MODEL_DIR)) if f.endswith(".glb")]
        if "--include-rejected" in flags and os.path.isdir(REJECTED_DIR):
            paths += [os.path.join(REJECTED_DIR, f)
                      for f in sorted(os.listdir(REJECTED_DIR)) if f.endswith(".glb")]
    if not paths:
        print("没有可量的 GLB（%s 是空的）" % VM_MODEL_DIR)
        return 1
    print("kVmHandArt 表（真值来源：src/node/scene_builder.cpp）：%s\n"
          % "  ".join("%s(len=%.3f)" % (k, v[0]) for k, v in sorted(table.items())))
    show_profile = "--profile" in flags
    oks = [check(p, table, show_profile) for p in paths]
    print("合计 %d 个：可用 %d，不通过 %d" % (len(oks), sum(oks), len(oks) - sum(oks)))
    return 0 if all(oks) else 2


if __name__ == "__main__":
    sys.exit(main())
