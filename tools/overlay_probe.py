# -*- coding: utf-8 -*-
"""VolunteerArmyPC —— 把「压暗层」单独抠出来，判定它是阶梯还是连续渐变

背景知识：
  菜单/简报/结算的背景 = 原始 key art 按 cover 铺满 + 一层横向渐变压暗。
  把截图按同一套 cover 几何反算回素材坐标再相减，画面内容会被消掉，
  剩下的就是压暗层本身。压暗层是阶梯还是平滑，一比就出来。

为什么不能简单找"硬跳变"：
  生成素材自带明显的胶片颗粒（1px 尺度）。直接找跳变会被颗粒淹没 ——
  实测间距中位数 1px，全是噪声，什么也判不出来。所以用**有针对性的周期检验**：
  已知分段数 N，则阶梯的竖边必然落在 x = k·W/N 上。于是
     · 在"预期边界处"测亮度差
     · 在"分段中部"测亮度差（作为对照）
  若真是阶梯，边界处的差值会**系统性**大于中部；平滑渐变则两者相当。
  39 个边界平均下来，信噪比足够。

用法：
    python tools/overlay_probe.py 截图.png 素材.png [--x0 900 --x1 2500]
                                                 [--y0 600 --y1 900] [--bands 40]
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
    if len(a) < 2:
        print(__doc__)
        return 1
    shot, art = a[0], a[1]
    x0, x1, y0, y1, bands = 900, 2500, 600, 900, 40
    for i, t in enumerate(a):
        if t == "--x0":
            x0 = int(a[i + 1])
        elif t == "--x1":
            x1 = int(a[i + 1])
        elif t == "--y0":
            y0 = int(a[i + 1])
        elif t == "--y1":
            y1 = int(a[i + 1])
        elif t == "--bands":
            bands = int(a[i + 1])

    sw, sh, sch, srows = read_png(shot)
    aw, ah, ach, arows = read_png(art)

    k = max(sw / aw, sh / ah)
    ox, oy = (sw - aw * k) * 0.5, (sh - ah * k) * 0.5
    print("截图 %dx%d 素材 %dx%d | cover 缩放 %.4f 偏移 (%.1f,%.1f)"
          % (sw, sh, aw, ah, k, ox, oy))

    x1 = min(x1, sw)
    y1 = min(y1, sh)
    prof = []
    for x in range(x0, x1):
        sx = int((x - ox) / k)
        acc = 0.0
        n = 0
        for y in range(y0, y1, 7):
            sy = int((y - oy) / k)
            if 0 <= sx < aw and 0 <= sy < ah:
                acc += lum(arows[sy], sx * ach) - lum(srows[y], x * sch)
                n += 1
        prof.append(acc / n if n else 0.0)

    lo, hi = min(prof), max(prof)
    print("压暗层造成的亮度损失 %.1f .. %.1f (跨度 %.1f)，取样 x[%d,%d) y[%d,%d)"
          % (lo, hi, hi - lo, x0, x1, y0, y1))

    # 人眼可读的形状：降采样成 40 个数。阶梯会看到"连续相同的值成组出现"。
    m = 40
    step = max(1, len(prof) // m)
    shape = [sum(prof[i:i + step]) / len(prof[i:i + step]) for i in range(0, len(prof) - step, step)]
    print("  剖面形状(%d 点): %s" % (len(shape), " ".join("%.1f" % v for v in shape)))

    # 周期检验：分段边界 vs 分段中部
    # 【注意这里必须用整屏宽度】分段是按 vp_.x * t 排布的（draw_art_bg 里
    # t = i/(bands-1)），所以边界在 x = k * vp_.x / (bands-1) 上，
    # 与"取样区从哪到哪"无关。第一版误用了取样区宽度，检验点全部落在错误位置，
    # 于是把明显的阶梯判成了"连续渐变"。
    period = sw / float(bands - 1)
    W = 9            # 每侧取样宽度（约 1/7 个分段，保证还在平台内）

    def mean_abs(boundary):
        s = n = 0.0
        for kb in range(1, bands - 1):
            cx = int(round(kb * period)) + (0 if boundary else int(period * 0.5))
            if cx - W < 0 or cx + W >= len(prof):
                continue
            l = sum(prof[cx - W:cx]) / W
            r = sum(prof[cx:cx + W]) / W
            s += abs(r - l)
            n += 1
        return (s / n) if n else 0.0

    eb, em = mean_abs(True), mean_abs(False)
    print("  预期边界处平均差 %.3f   分段中部平均差 %.3f   比值 %.2f" % (eb, em, eb / em if em else 0))
    if em <= 0.01:
        print("  → 分段中部几乎无变化：整体是**阶梯**（每个分段内部恒定）")
    elif eb > em * 3.0:
        print("  → 边界差远大于中部差：**阶梯**（分段合成）")
    elif eb > em * 1.6:
        print("  → 边界差略大：存在弱分段痕迹")
    else:
        print("  → 边界与中部相当：**连续渐变**，无分段")
    return 0


if __name__ == "__main__":
    sys.exit(main())
