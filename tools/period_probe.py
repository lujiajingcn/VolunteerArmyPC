# -*- coding: utf-8 -*-
"""VolunteerArmyPC —— 竖纹周期探测（自相关法）

为什么需要它：
  「画面上有规则竖纹」这种判断，肉眼只能给出"大概几十像素一条"，
  而这个精度不足以区分不同的成因：
      · 压暗层用 N 条 draw_rect 叠出来 → 周期 = 屏宽 / (N-1)
      · 素材本身的解码/生成条带      → 周期与压暗层无关
  两者要做的事完全不同（一个改绘制方式，一个处理素材），所以必须先量准周期。

方法：
  取一块**没有文字**的平坦区域，算逐列平均亮度；先用滑动均值去掉大趋势
  （天空到山脊的明暗过渡），再对残差做自相关，找滞后 4..300 px 内的峰值。
  规则竖纹会在"周期"那一档给出明显高于邻域的峰。

用法：
    python tools/period_probe.py 图.png [--x0 900 --x1 2500 --y0 620 --y1 900]
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from probe_png import read_png  # noqa: E402


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 1
    path = a[0]
    x0, x1, y0, y1 = 900, 2500, 620, 900
    for i, t in enumerate(a):
        if t == "--x0":
            x0 = int(a[i + 1])
        elif t == "--x1":
            x1 = int(a[i + 1])
        elif t == "--y0":
            y0 = int(a[i + 1])
        elif t == "--y1":
            y1 = int(a[i + 1])

    w, h, ch, rows = read_png(path)
    x1, y1 = min(x1, w), min(y1, h)
    col = []
    for x in range(x0, x1):
        s = 0.0
        for y in range(y0, y1):
            r = rows[y]
            i = x * ch
            s += 0.2126 * r[i] + 0.7152 * r[i + 1] + 0.0722 * r[i + 2]
        col.append(s / (y1 - y0))

    n = len(col)
    # 去趋势：减去宽度 K 的滑动均值，只留局部起伏
    K = 201
    half = K // 2
    det = []
    for i in range(n):
        lo, hi = max(0, i - half), min(n, i + half + 1)
        m = sum(col[lo:hi]) / (hi - lo)
        det.append(col[i] - m)

    # 自相关（滞后 4..300）
    var = sum(v * v for v in det) / n
    best = []
    for lag in range(4, 301):
        if lag >= n:
            break
        s = 0.0
        for i in range(n - lag):
            s += det[i] * det[i + lag]
        best.append((s / (n - lag) / var, lag))

    print("%s  取样 x[%d,%d) y[%d,%d)  n=%d" % (os.path.basename(path), x0, x1, y0, y1, n))
    print("  去趋势后标准差 %.3f" % (var ** 0.5))
    top = sorted(best, key=lambda t: -t[0])[:8]
    print("  自相关最强的前 8 个滞后：")
    for c, lag in top:
        print("    滞后 %4d px   自相关 %.3f" % (lag, c))
    # 判据必须区分两种"自相关高"，只看峰值/均值会把平滑画面误判成条纹
    # （原始素材就被误判过一次）：
    #   · 小滞后自相关高（<12px）= 相邻像素本来就相似 → 画面**平滑**，不是条纹
    #   · 大滞后处出现**孤立峰**   = 真正的**周期**条纹，峰的位置就是周期
    #   而且周期性条纹的峰往往在滞后 = 周期、2×周期、3×周期…上成组出现，
    #   这个"倍频成组"比单看峰值更可靠。
    LAG_SMOOTH = 12
    far = [t for t in top if t[1] >= LAG_SMOOTH]
    print("  远滞后（≥%dpx）最强：%s"
          % (LAG_SMOOTH, ("滞后 %dpx 自相关 %.3f" % (far[0][1], far[0][0])) if far
             else "无（峰值全在小滞后 = 平滑）"))
    if not far or far[0][0] < 0.15:
        print("  → 无周期性：峰值集中在相邻像素，属于图像自身的平滑相关")
    else:
        p0 = far[0][1]
        # 倍频检验：2×p0、3×p0 附近是否也有较高相关
        def near(lag, tol=6):
            cand = [c for c, l in best if abs(l - lag) <= tol]
            return max(cand) if cand else 0.0
        h2, h3 = near(2 * p0), near(3 * p0)
        print("  主峰 %dpx  倍频 %dpx→%.3f  %dpx→%.3f" % (p0, 2 * p0, h2, 3 * p0, h3))
        if h2 > 0.15 and h3 > 0.15:
            print("  → 存在周期条纹：周期约 %d px，且 2×/3× 处同样成峰" % p0)
        else:
            print("  → 远滞后有孤立峰但无倍频 → 可能是画面里的一段重复纹理，不是规则条纹")
    return 0


if __name__ == "__main__":
    sys.exit(main())
