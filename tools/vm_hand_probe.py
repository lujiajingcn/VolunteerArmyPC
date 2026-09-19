# -*- coding: utf-8 -*-
"""VolunteerArmyPC —— 把"枪模画面上这块到底是谁、多亮"拆成可比较的三行数字。

用法：
    python tools/vm_hand_probe.py --on 正常图.png --hide 藏枪模图.png [--id 识别色图.png]
                                  [--label 莫辛] [--roi x0,y0,x1,y1]

三张图都用 `tools/capture_wpn.sh` 在**同一外观、同一种子、同一秒**拍：
    --on    VA_VM_HIDE 没设的常规图
    --hide  VA_VM_HIDE=1（整个枪模连阴影一起藏掉）
    --id    VA_VM_MAT=1（给程序化部件上识别色，用来分离手套/袖子）

【为什么要三张而不是一张】
"枪身现在多亮"这个问题，看图答不了：真模型没有识别色可用，而枪身、手套、
袖子在正常材质下都是灰褐色系，靠目测划分边界在本工程已经错过一次。
三张图的三角关系能把边界**算**出来：
    VM 掩码   = |on − hide| 非零        → 枪 + 手套 + 袖子（消融差分，无阈值歧义）
    手套掩码  = id 图上"淡黄绿"         → 见下
    袖子掩码  = id 图上"正蓝"
    枪身掩码  = VM 掩码 − 手套 − 袖子
三个掩码互不重叠，各自在 **on** 图上取亮度，于是"调完手有没有把枪带亮/带曝"
就变成同一口径下可比较的数字。

【识别色判据必须从实测渲染色反推，不能照材质常量写】
`id_mat()` 是 unshaded 的，按说 `Color(0,1,0)` 该逐位渲染成 (0,255,0)。**并不是** ——
色调映射（VA_TONEMAP，默认 aces）仍然作用在它之后，实测：
    glove `Color(0,1,0)` → RGB(192,240,112)（淡黄绿，g−r 只有 48）
    sleeve `Color(0,0,1)` → RGB(16,0,240)  （基本没变）
按常量写的 `g > r+60` 会漏掉 97% 的手套像素（实测 947 px vs 真值 34304 px，
差 36 倍），而且这个错误的方向恰好是"让人以为手没画出来"—— 差点因此推翻
量准的握持点。所以判据取 g > r+30 且 g > b+60（实测余量 48 / 128）。
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_png import read_png  # noqa: E402


def luma(r, g, b):
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def is_glove(r, g, b):
    return g > r + 30 and g > b + 60 and g > 120


def is_sleeve(r, g, b):
    return b > r + 40 and b > g + 40 and b > 120


def load(path):
    w, h, ch, rows = read_png(path)
    return w, h, ch, rows


def argval(name, default=None):
    if name in sys.argv:
        return sys.argv[sys.argv.index(name) + 1]
    return default


def main():
    onp = argval("--on")
    hidep = argval("--hide")
    idp = argval("--id")
    label = argval("--label", "?")
    roi = argval("--roi")
    if onp is None or hidep is None:
        print(__doc__)
        return 1

    w, h, ch_on, onr = load(onp)
    w2, h2, ch_h, hr = load(hidep)
    if (w, h) != (w2, h2):
        print("尺寸不一致：%s vs %s —— 必须同一视口拍的" % ((w, h), (w2, h2)))
        return 1

    x0, y0, x1, y1 = 0, 0, w, h
    if roi:
        x0, y0, x1, y1 = [int(v) for v in roi.split(",")]

    idr = None
    if idp is not None:
        w3, h3, ch_i, idr = load(idp)
        if (w3, h3) != (w, h):
            print("识别色图尺寸不一致")
            return 1

    TH = 12   # 消融差分阈值：抖动/抗锯齿噪声底实测 <5
    buckets = {"枪身": [], "手套": [], "袖子": []}
    for y in range(y0, y1):
        row_on = onr[y]
        row_h = hr[y]
        row_id = idr[y] if idr is not None else None
        for x in range(x0, x1):
            o = row_on[x * ch_on:x * ch_on + 3]
            b = row_h[x * ch_h:x * ch_h + 3]
            if abs(o[0] - b[0]) + abs(o[1] - b[1]) + abs(o[2] - b[2]) < TH:
                continue
            key = "枪身"
            if row_id is not None:
                i = row_id[x * ch_i:x * ch_i + 3]
                if is_glove(i[0], i[1], i[2]):
                    key = "手套"
                elif is_sleeve(i[0], i[1], i[2]):
                    key = "袖子"
            buckets[key].append(tuple(o))

    total = w * h
    print("外观 %s   ROI %d,%d,%d,%d   （掩码：|on−hide| ≥ %d；识别色见文件头）"
          % (label, x0, y0, x1, y1, TH))
    print("  %-6s %8s %10s %10s %8s %8s %8s" %
          ("部件", "像素", "占比", "中位L", "p10L", "死黑%", "过曝%"))
    for k in ("枪身", "手套", "袖子"):
        v = buckets[k]
        if not v:
            print("  %-6s %8d" % (k, 0))
            continue
        v.sort(key=lambda c: luma(*c))
        n = len(v)
        med = v[n // 2]
        p10 = v[max(0, int(0.10 * n))]
        dark = sum(1 for c in v if luma(*c) < 20)
        over = sum(1 for c in v if luma(*c) > 200)
        rm, gm, bm = med
        print("  %-6s %8d %9.2f%% %6.0f RGB(%3d,%3d,%3d) %6.0f %7.1f%% %7.1f%%"
              % (k, n, 100.0 * n / total, luma(*med), rm, gm, bm,
                 luma(*p10), 100.0 * dark / n, 100.0 * over / n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
