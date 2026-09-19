# -*- coding: utf-8 -*-
"""真模型武器的材质亮度评估 —— 用「两档差分」取掩码。

## 为什么不能用 tools/vm_eval.py

vm_eval.py 靠一张 `VA_VM_MAT=1` 拍的**识别色图**（枪身红/手套绿/袖子蓝）来取掩码，
那是给**程序化**枪模用的：它的每块图元都有各自的材质色。
真模型的贴图是照片级的，没有"识别色"可言，这条路直接不通。

## 这个脚本的办法

真模型只有一个可调旋钮：`VA_VM_ART_ALB`。同一场景、同一种子、同一时刻拍两张**只差这个系数**的图，
**两张之差非零的像素必然是枪**（背景一模一样，唯一在变的就是枪身亮度）。

于是掩码 = |A - B| > 阈值。

注意事项：
- 必须**同种子同秒数**拍（用 tools/capture_wpn.sh，它把 VA_SEED 与 VA_CAPTURE 都定死了）。
- 仿真跟墙钟走 → 两次运行的真实耗时略有差异，相机的呼吸/摇摆会带来一点错位，
  所以阈值不能太低。经验值 12：实测能把枪身完整抠出来，且背景几乎不入选。
- 差分掩码对"枪与背景亮度接近的部分"会漏选（差得不够大）—— 这会让统计略偏，
  但不影响"哪一档过曝更少"这种**相对**判断，而这正是它的用途。

用法（用带 Pillow 的 Python，本机是系统 3.11）：
    python tools/vm_art_probe.py 图A.png 图B.png [更多图...] --label A,B [--thr 12] [--roi x0,y0,x1,y1]
    ... --mask i,j      # 显式指定用第 i、j 张（0 起）的差取掩码，默认 0,1

**比多档时务必用同一个掩码**（`--mask`），否则每档各取一次掩码、选出的像素集合不同，
数字之间就不可比了。做法：把**最低档与最高档**放进列表，用 `--mask` 指向它们那一对
（它们的差最大、抠出的轮廓最完整），其余档位在这同一个掩码上统计。

判读标准与 vm_eval.py 同一套（第一人称武器模型的经验区间）：
    死黑 <20   占比应 <10%
    正常 70-110  目标区间，占比应 >35%
    过曝 >200  占比应 <3%

⚠️ **绝对占比不要与 vm_eval.py 的数字直接比**：差分掩码天然偏向"两档差得多的像素"，
也就是偏亮的那些，于是"过曝"占比会被系统性抬高。
它的用途是**相对比较**（同掩码下哪一档过曝更少、哪一档死黑更多）。
"""

import os
import sys

from PIL import Image


def lum(px):
    r, g, b = px
    return (r * 299 + g * 587 + b * 114) // 1000


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 1

    thr = 12
    roi = None
    labels = None
    mask_ij = (0, 1)
    paths = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--thr":
            thr = int(args[i + 1])
            i += 2
            continue
        if a == "--roi":
            roi = tuple(int(v) for v in args[i + 1].split(","))
            i += 2
            continue
        if a == "--label":
            labels = args[i + 1].split(",")
            i += 2
            continue
        if a == "--mask":
            mask_ij = tuple(int(v) for v in args[i + 1].split(","))
            i += 2
            continue
        paths.append(a)
        i += 1

    if len(paths) < 2:
        print("至少给两张图：两档之间的差用来取掩码，剩下的用来统计")
        return 1

    imgs = []
    for p in paths:
        if not os.path.isfile(p):
            print("缺 %s" % p)
            return 1
        imgs.append(Image.open(p).convert("RGB"))

    W, H = imgs[0].size
    for im in imgs[1:]:
        if im.size != (W, H):
            print("尺寸不一致：%s vs %s" % (im.size, (W, H)))
            return 1

    x0, y0, x1, y1 = roi if roi else (0, 0, W, H)
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(W, x1), min(H, y1)

    # 掩码来源由 --mask 指定（默认前两张）。比多档时要指向固定的一对，
    # 否则各档各取一次掩码、像素集合不同，数字之间不可比。
    ia, ib = mask_ij
    if not (0 <= ia < len(imgs) and 0 <= ib < len(imgs)) or ia == ib:
        print("--mask 要给两个不同的合法下标（0 起），当前 %s，共 %d 张图"
              % (str(mask_ij), len(imgs)))
        return 1
    pa = imgs[ia].load()
    pb = imgs[ib].load()

    masks = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            r0, g0, b0 = pa[x, y]
            r1, g1, b1 = pb[x, y]
            if abs(r0 - r1) + abs(g0 - g1) + abs(b0 - b1) > thr:
                masks.append((x, y))

    if not masks:
        print("掩码为空 —— 阈值 %d 太高？或者两张图其实一样？" % thr)
        return 1

    print("掩码：%d 像素（ROI %d×%d 的 %.1f%%），来自 #%d 与 #%d 的差，阈值 %d"
          % (len(masks), x1 - x0, y1 - y0,
             100.0 * len(masks) / max(1, (x1 - x0) * (y1 - y0)), ia, ib, thr))
    print()
    print("%-22s %6s %7s %7s   %s" % ("档位", "中位亮", "均值", "P95", "亮度分布"))
    print("-" * 92)

    bins = [(0, 20), (20, 40), (40, 70), (70, 110), (110, 160), (160, 200), (200, 256)]
    names = ["死黑", "很暗", "偏暗", "正常", "偏亮", "亮", "过曝"]

    for k, im in enumerate(imgs):
        px = im.load()
        v = sorted(lum(px[x, y]) for x, y in masks)
        n = len(v)
        dist = " ".join("%s%3d%%" % (nm, round(100.0 * sum(1 for t in v if lo <= t < hi) / n))
                        for (lo, hi), nm in zip(bins, names))
        lb = labels[k] if (labels and k < len(labels)) else os.path.basename(paths[k])
        print("%-22s %6d %7.1f %7d   %s"
              % (lb, v[n // 2], sum(v) / n, v[n * 95 // 100], dist))
        dead = 100.0 * sum(1 for t in v if t < 20) / n
        norm = 100.0 * sum(1 for t in v if 70 <= t < 110) / n
        hot = 100.0 * sum(1 for t in v if t >= 200) / n
        print("%-22s 死黑 %.1f%%（<10）  正常 %.1f%%（>35）  过曝 %.1f%%（<3）"
              % ("", dead, norm, hot))
    return 0


if __name__ == "__main__":
    sys.exit(main())
