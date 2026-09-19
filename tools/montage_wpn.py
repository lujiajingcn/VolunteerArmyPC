# -*- coding: utf-8 -*-
"""把多张「第一人称武器」取证图拼成一张对照表 —— 一次看完三把枪。

为什么需要它：判"这把枪的材质对不对"必须**并排**看。单看一张时人脑没有参照，
会把"金属过曝成一块白"当成"本来就该这么亮"；拼起来之后，
木托/弹鼓/圆盘弹匣的亮度差、以及"哪把枪偏黑"会立刻显形。

用法（用带 Pillow 的那个 Python，本机是系统 3.11）：
    "C:/Program Files/Python311/python.exe" tools/montage_wpn.py 出图.png 目录1 目录2 ...
    ... --roi 700,500,2400,1400 --w 900 --label 名称1,名称2,...

每个目录里默认取 cap_1s.png（tools/capture_wpn.sh 的产物）。
不传 --label 时用目录名当标签。中文标签走微软雅黑；
拿不到字体就退回 ASCII 默认字体（此时中文标签会变成方块，但不影响判读图片）。
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont


def load_font(size):
    """优先微软雅黑（能画中文），拿不到就退回 PIL 内置位图字体。"""
    for p in (r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\simhei.ttf"):
        if os.path.isfile(p):
            try:
                return ImageFont.truetype(p, size), True
            except Exception:
                pass
    return ImageFont.load_default(), False


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 1

    out = args[0]
    roi = (700, 500, 2400, 1400)
    W = 900
    labels = None

    srcs = []
    i = 1
    while i < len(args):
        a = args[i]
        if a == "--roi":
            roi = tuple(int(v) for v in args[i + 1].split(","))
            i += 2
            continue
        if a == "--w":
            W = int(args[i + 1])
            i += 2
            continue
        if a == "--label":
            labels = args[i + 1].split(",")
            i += 2
            continue
        srcs.append(a)
        i += 1

    if len(roi) != 4 or roi[2] <= roi[0] or roi[3] <= roi[1]:
        print("--roi 要写 x0,y0,x1,y1 且 x1>x0、y1>y0")
        return 1

    font, cjk = load_font(26)
    BAR = 38
    GAP = 8

    tiles = []
    names = []
    for k, d in enumerate(srcs):
        # 既接受目录（取里面的 cap_1s.png），也接受直接给 png 路径
        p = d if d.lower().endswith(".png") else os.path.join(d, "cap_1s.png")
        if not os.path.isfile(p):
            print("缺 %s（跳过）" % p)
            continue
        im = Image.open(p).convert("RGB")
        # ROI 按图片实际尺寸裁剪，避免换分辨率后越界
        x0 = min(roi[0], im.width - 2)
        y0 = min(roi[1], im.height - 2)
        x1 = min(roi[2], im.width)
        y1 = min(roi[3], im.height)
        im = im.crop((x0, y0, x1, y1))
        h = int(round(im.height * W / im.width))
        tiles.append(im.resize((W, h), Image.LANCZOS))
        if labels is not None and k < len(labels):
            names.append(labels[k])
        else:
            names.append(os.path.basename(d.rstrip("/\\")) or d)

    if not tiles:
        print("没有可用的输入图")
        return 1

    canvas_h = sum(t.height + BAR for t in tiles) + GAP * (len(tiles) - 1)
    canvas = Image.new("RGB", (W, canvas_h), (40, 40, 40))
    draw = ImageDraw.Draw(canvas)
    y = 0
    for t, nm in zip(tiles, names):
        canvas.paste(t, (0, y + BAR))
        txt = nm if cjk else nm.encode("ascii", "replace").decode("ascii")
        draw.text((10, y + 6), txt, fill=(240, 240, 240), font=font)
        y += t.height + BAR + GAP

    canvas.save(out)
    print("wrote %s  %dx%d  ROI=%s  顺序: %s"
          % (out, canvas.width, canvas.height, roi, " / ".join(names)))
    if not cjk:
        print("（没找到中文字体，标签已退化为 ASCII）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
