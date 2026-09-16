# -*- coding: utf-8 -*-
"""
VolunteerArmyPC —— 视图模型材质评估

问题背景：判断"枪在屏幕上是不是糊成一团黑"，用肉眼会误判 ——
本机实测过两次把背景草地当成"过亮的枪身"。这个脚本把判断变成数字：

  1. 用 VA_VM_MAT=1 拍的**识别色图**（枪身红/手套绿/袖子蓝/金属白）提取各部件掩码，
     这一步不需要人眼，纯按色相判决；
  2. 把掩码套到**待评估图**（正常材质）上，统计各部件真实显示的亮度分布。

用法：
    python tools/vm_eval.py 识别色图.png 待评估图.png [--u0 0.40] [--label vm8]

判读标准（第一人称武器模型的经验区间）：
    死黑 <20   —— 结构丢失，玩家只看到一块黑影。占比应 <10%
    正常 70-110 —— 目标区间，占比应 >35%
    过曝 >200  —— 高光糊成白块，占比应 <3%
"""
import sys

from crop_png import read_png


def classify(r, g, b):
    """按色相判决识别色；返回部件名或 None。"""
    mx, mn = max(r, g, b), min(r, g, b)
    if r > 100 and r > g + 45 and r > b + 45:
        return "枪身(poly)"
    if g > 100 and g > r + 45 and g > b + 45:
        return "手套(glove)"
    if b > 100 and b > r + 30 and b > g + 20:
        return "袖子(sleeve)"
    if mx > 175 and (mx - mn) < 45:
        return "金属(metal)"
    return None


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 1
    mat_png, eval_png = args[0], args[1]
    u0 = 0.40
    v0 = 0.50
    label = eval_png
    for i, a in enumerate(args):
        if a == "--v0":
            v0 = float(args[i + 1])
        elif a == "--u0":
            u0 = float(args[i + 1])
        elif a == "--label":
            label = args[i + 1]

    W, H, cm, M = read_png(mat_png)
    W2, H2, ce, E = read_png(eval_png)
    if (W, H) != (W2, H2):
        print("尺寸不一致")
        return 1

    buckets = {}   # part -> [lum, ...]
    for y in range(int(v0 * H), H):
        rm, re = M[y], E[y]
        for x in range(int(u0 * W), W):
            om, oe = x * cm, x * ce
            part = classify(rm[om], rm[om + 1], rm[om + 2])
            if part is None:
                continue
            r, g, b = re[oe], re[oe + 1], re[oe + 2]
            buckets.setdefault(part, []).append((r * 299 + g * 587 + b * 114) // 1000)

    if not buckets:
        print("没有识别到任何部件像素 —— 识别色图是否正确（VA_VM_MAT=1）？")
        return 1

    print("评估目标：%s" % label)
    print("扫描范围 u>%.2f v>%.2f" % (u0, v0))
    print("%-14s %8s %7s %7s   %s" % ("部件", "像素", "中位亮", "均值", "亮度分布"))
    allv = []
    for part in sorted(buckets):
        v = sorted(buckets[part])
        allv += v
        n = len(v)
        bins = [(0, 20), (20, 40), (40, 70), (70, 110), (110, 160), (160, 200), (200, 256)]
        names = ["死黑", "很暗", "偏暗", "正常", "偏亮", "亮", "过曝"]
        dist = " ".join("%s%2d%%" % (nm, round(100.0 * sum(1 for t in v if lo <= t < hi) / n))
                        for (lo, hi), nm in zip(bins, names))
        print("%-14s %8d %7d %7.1f   %s" % (part, n, v[n // 2], sum(v) / n, dist))

    v = sorted(allv)
    n = len(v)
    print("-" * 76)
    print("合计 %d px  中位 %d  均值 %.1f  5%%分位 %d  95%%分位 %d" %
          (n, v[n // 2], sum(v) / n, v[n // 20], v[n * 19 // 20]))
    dead = 100.0 * sum(1 for t in v if t < 20) / n
    norm = 100.0 * sum(1 for t in v if 70 <= t < 110) / n
    hot = 100.0 * sum(1 for t in v if t >= 200) / n
    print("死黑 %.1f%%（目标<10）   正常 %.1f%%（目标>35）   过曝 %.1f%%（目标<3）" % (dead, norm, hot))
    return 0


if __name__ == "__main__":
    sys.exit(main())
