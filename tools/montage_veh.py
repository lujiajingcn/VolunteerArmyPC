# -*- coding: utf-8 -*-
"""把多张「载具」取证图拼成一张对照表 —— 一次看完四辆车的正面与侧面。

【为什么不能只交一张单图】
和图生3D 那批一个道理：单看一辆车时人脑没有参照，
"轮子是不是陷进地里 / 车身是不是偏窄 / 这辆跟那辆是不是同一个风格"全都判不出来。
四辆并排之后，"哪辆明显比别的大一圈""哪辆的车头是反的"会立刻显形。

【与 montage_wpn.py 的差别】那个是**竖排**（三把枪各占一行，看材质亮度差），
本工具是**网格**：载具要同时比"同一辆的正/侧面"和"四辆之间"两个维度。
另外每格都要标中文，所以标签画在**格子自己的顶栏**里，而不是整图下方的图例。

用法（用带 Pillow 的隔离 venv）：
    <venv-python> tools/montage_veh.py 出图.png --cols 2 --w 780 \
        --roi 640,0,2520,1080 \
        sweep/v_veh_jeep/cap_1s.png=吉普·正面 \
        sweep/v_veh_jeep_d90/cap_1s.png=吉普·侧面 ...

- `图片路径=标签`；不写 `=标签` 就用文件名当标签。
  **按第一个 `=` 切**，所以标签里可以再写 `=`（"全部朝镜头 = 全部朝 +X" 这种）。
  早先写成 `rsplit("=", 1)`（按**最后**一个切），标签里一带 `=` 路径就被切坏，
  报"找不到 sweep/xxx.png=四车陈列排（…"这种看着莫名其妙的错 —— 改掉了。
- `--roi x0,y0,x1,y1` 按**整帧像素**取（取证图是 2560×1369）。
  默认 ROI 是四辆车 + 参照兵的经验取景框，换机位就要重取。
- `--cols` 每行几格；`--w` 每格宽度（高度按 ROI 比例自动算）。
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont


def load_font(size):
    """优先微软雅黑（能画中文），拿不到就退回 PIL 内置位图字体。"""
    for p in (r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\simhei.ttf"):
        if os.path.isfile(p):
            try:
                return ImageFont.truetype(p, size)
            except Exception:
                pass
    return ImageFont.load_default()


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 2

    out = args[0]
    rest = args[1:]

    def opt(name, dflt):
        return rest[rest.index(name) + 1] if name in rest else dflt

    cols = int(opt("--cols", 2))
    W = int(opt("--w", 780))
    roi = tuple(int(v) for v in opt("--roi", "640,0,2520,1080").split(","))
    gap = int(opt("--gap", 10))
    bar = int(opt("--bar", 44))

    # 位置参数里形如 a.png=标签 的才是图；选项值天然不含 "="，
    # 但为了稳，还是把 --xxx 及其值都跳过。
    skip = set()
    for i, a in enumerate(rest):
        if a.startswith("--"):
            skip.add(i)
            if i + 1 < len(rest):
                skip.add(i + 1)

    tiles = []
    for i, a in enumerate(rest):
        if i in skip or "=" not in a and not a.lower().endswith(".png"):
            continue
        if "=" in a:
            # 按**第一个** = 切：路径里不会有 =，而标签里很可能有。
            path, label = a.split("=", 1)
        else:
            path, label = a, os.path.basename(a)
        if not os.path.isfile(path):
            print("!! 找不到 %s" % path)
            return 1
        tiles.append((path, label))

    if not tiles:
        print("!! 一个图都没解析到。参数要写成 图片路径=标签")
        return 1

    x0, y0, x1, y1 = roi
    cw, ch = x1 - x0, y1 - y0
    H = int(round(W * ch / float(cw)))
    scale = W / float(cw)

    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * W + (cols + 1) * gap,
                              rows * (H + bar) + (rows + 1) * gap), (24, 24, 26))
    d = ImageDraw.Draw(sheet)
    font = load_font(int(bar * 0.52))
    ascii_only = font.__class__.__name__ == "ImageFont"

    for i, (path, label) in enumerate(tiles):
        im = Image.open(path).convert("RGB")
        if (x0, y0, x1, y1) != (0, 0, im.width, im.height):
            im = im.crop((max(0, x0), max(0, y0),
                          min(im.width, x1), min(im.height, y1)))
        im = im.resize((W, H), Image.LANCZOS)
        r, c = divmod(i, cols)
        cx = gap + c * (W + gap)
        cy = gap + r * (H + bar + gap)
        d.rectangle([cx, cy, cx + W, cy + bar - 2], fill=(38, 38, 42))
        d.text((cx + 10, cy + 8), label if not ascii_only else label.encode(
            "ascii", "replace").decode("ascii"), font=font, fill=(235, 235, 235))
        sheet.paste(im, (cx, cy + bar))

    sheet.save(out)
    print("wrote %s  %dx%d  格 %d（%d 列，每格 %dx%d，ROI %s）"
          % (out, sheet.width, sheet.height, len(tiles), cols, W, H, roi))
    return 0


if __name__ == "__main__":
    sys.exit(main())
