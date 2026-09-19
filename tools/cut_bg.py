# -*- coding: utf-8 -*-
"""VolunteerArmyPC —— 把参考图抠成「白底、单物体、居中留边」的规范图。

【为什么需要这一步】
图生3D吃的是「一个物体 + 干净背景」。本项目能拿到的年代武器参考图是三来源：
  ① 维基共享资源的白底产品图（Mosin / PPSh）—— 拿过来裁一裁就行；
  ② 博物馆实拍（DP-27 摆在石地板上、还带投影）—— 必须把地板与投影去掉。
直接拿 ② 去生成，地板纹路会被当成"物体的一部分"烘进网格。

【为什么不用 rembg / numpy】
本机 pip 通道不通（`pypi.tuna` 也装不上 numpy），所以这里只用 Pillow 的
纯 C 实现 + 自己写的一遍洪泛填充，不引入新依赖。

【做法】三步，每步都对应一个具体的坑：
  1. **二值化**：亮度 < thresh 的算物体。DP-27 那张的石地板在 150~200、
     投影 130~170、枪身 20~90 —— 阈值取 110 就能同时排掉地板**和投影**
     （投影比地板暗，但比枪亮得多，这个间隙就是分界线）。
  2. **从四边灌水**：把与画面边缘连通的"疑似背景"整块标掉。
     只按亮度判是不够的：枪身上有反光高光（亮度能到 200+），
     只按亮度会把高光挖成洞；而灌水只吃"连到外边"的那一块，高光留得住。
  3. **闭运算 + 羽化**：二值边缘是硬的，直接用会出现一圈锯齿；
     先用 MaxFilter 吃掉孤立噪点、MinFilter 收回一点，再高斯模糊当 alpha。

【已知取舍】
- 枪身内部**封闭**的亮区（例如扳机护圈里的背景）灌水到不了，
  会被算成物体 —— 对武器剪影来说基本无害，且比"挖出几个洞"安全。
- 细长件（DP-27 的两脚架腿）在闭运算里可能被啃掉一点，所以核只开 3。

用法：
    python tools/cut_bg.py <输入> <输出> [--thresh 110] [--margin 0.06] [--side 1600]
不带 --thresh 就只做"裁到内容外框 + 补白边 + 缩放"，用于本来就是白底的图。

依赖 Pillow（唯一一个非纯 Python 的工具，与 slim_glb.py 同一个隔离 venv）：
    C:/Users/<user>/.workbuddy/binaries/python/envs/default/Scripts/python.exe
"""
import argparse
import sys

from PIL import Image, ImageDraw, ImageFilter


def _largest_component(mask):
    """只留最大的一块白。地板纹理里的暗斑也算"物体"，不剔掉就会撑满外框、
    并在成品里留下一串贴在白底上的杂色小点。"""
    w, h = mask.size
    px = mask.load()
    # 质心附近找一个白像素当种子：武器横跨画面，质心必定落在它身上
    xs = ys = n = 0
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            if px[x, y]:
                xs += x; ys += y; n += 1
    if n == 0:
        return mask
    cx, cy = xs // n, ys // n
    seed = None
    for r in range(0, max(w, h), 4):
        for dy in (-r, 0, r):
            for dx in (-r, 0, r):
                x, y = cx + dx, cy + dy
                if 0 <= x < w and 0 <= y < h and px[x, y]:
                    seed = (x, y); break
            if seed:
                break
        if seed:
            break
    if seed is None:
        return mask
    ImageDraw.floodfill(mask, seed, 200, thresh=0)
    return mask.point(lambda v: 255 if v == 200 else 0)


def cut_object(im, thresh):
    """把亮度 < thresh 的物体留下，其余（与边缘连通的背景）涂白。"""
    g = im.convert("L")
    bw = g.point(lambda v: 0 if v < thresh else 255)

    w, h = bw.size
    filled = bw.copy()
    corners = [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]
    for xy in corners:
        # 只在"角落本来就是亮（疑似背景）"时才灌，否则会把物体本身灌掉
        if filled.getpixel(xy) == 255:
            ImageDraw.floodfill(filled, xy, 128, thresh=0)

    # 128 = 连到外边的背景；0 = 物体；255 = 对象内部封闭的亮区（算物体）
    obj = filled.point(lambda v: 0 if v == 128 else 255)

    # 开运算先去掉细碎白点，再只留最大连通块（核开 3 是为了不啃掉两脚架那种细件）
    obj = obj.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.MaxFilter(3))
    obj = _largest_component(obj)

    # 闭运算：把物体内部的针孔补上
    obj = obj.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.MinFilter(3))

    # 羽化当 alpha：硬边缘直接用会在贴图里留下一圈台阶
    alpha = obj.filter(ImageFilter.GaussianBlur(1.2))
    return alpha


def content_alpha(im):
    """不做抠底时，直接把"非白"当 alpha（用于本来就是白底的图）。"""
    g = im.convert("L")
    return g.point(lambda v: 0 if v >= 247 else 255)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--thresh", type=int, default=0,
                    help="亮度阈值；给了才抠底，不给只裁边补白")
    ap.add_argument("--margin", type=float, default=0.055,
                    help="四周留白占长边的比例")
    ap.add_argument("--side", type=int, default=1600, help="长边目标像素")
    ap.add_argument("--square", action="store_true",
                    help="输出正方形画布（物体居中，四周留白）。"
                         "为什么需要：武器侧视图的长宽比高达 3.7:1，而图生3D 的预处理"
                         "若把输入按**中心裁剪**成正方形，枪的首尾会被直接切掉、"
                         "成品变成一截短棒 —— 那是要花积分才能发现的失败。"
                         "补齐成方图之后，无论对端是「等比缩放+留白」还是「中心裁剪」，"
                         "枪都不会被裁到；若对端本来就按内容外框裁边，这一步无副作用。")
    ap.add_argument("--erase", action="append", default=[],
                    metavar="X0,Y0,X1,Y1",
                    help="再擦掉一块，坐标用**原图**像素（可重复）。擦在 alpha 上、"
                         "发生在裁边之前，所以外框会跟着收紧。"
                         "用于亮度阈值治不掉的残留 —— 例如 DP-27 那张，"
                         "阈值取 110 能保住两脚架、却会把地面投影一起吃进来，"
                         "投影与枪身同为中性灰、靠颜色分不开，只能定点擦。"
                         "注意别用成品的坐标：成品尺寸随 --side 变，对不上。")
    a = ap.parse_args()

    im = Image.open(a.src).convert("RGB")
    alpha = cut_object(im, a.thresh) if a.thresh > 0 else content_alpha(im)

    for spec in a.erase:
        try:
            ex0, ey0, ex1, ey1 = (int(v) for v in spec.split(","))
        except ValueError:
            print("!! --erase 参数格式应为 X0,Y0,X1,Y1，收到 %r" % spec)
            return 2
        ImageDraw.Draw(alpha).rectangle([ex0, ey0, ex1, ey1], fill=0)
        print("   擦除（原图坐标）[%d,%d,%d,%d]" % (ex0, ey0, ex1, ey1))

    bbox = alpha.getbbox()
    if bbox is None:
        print("!! %s 抠完之后没有内容了（阈值 %d 太高？）" % (a.src, a.thresh))
        return 1
    x0, y0, x1, y1 = bbox

    # 先把物体贴到一块干净画布上，再统一裁边留白 —— 直接裁原图会把背景色带进来
    obj = Image.new("RGBA", (x1 - x0, y1 - y0), (0, 0, 0, 0))
    obj.paste(im.crop(bbox), (0, 0), alpha.crop(bbox))

    ow, oh = obj.size
    k = a.side / max(ow, oh)
    nw, nh = max(1, int(round(ow * k))), max(1, int(round(oh * k)))
    obj = obj.resize((nw, nh), Image.LANCZOS)

    mx = int(round(max(nw, nh) * a.margin))
    if a.square:
        # 方图：以长边 + 两侧留白为边长，物体在两个方向都居中。
        s = max(nw, nh) + mx * 2
        out = Image.new("RGB", (s, s), (255, 255, 255))
        out.paste(obj, ((s - nw) // 2, (s - nh) // 2), obj)
    else:
        out = Image.new("RGB", (nw + mx * 2, nh + mx * 2), (255, 255, 255))
        out.paste(obj, (mx, mx), obj)

    out.save(a.dst)
    print("wrote %s  %dx%d  (物体 %dx%d @ 原图，抠底=%s)"
          % (a.dst, out.width, out.height, ow, oh,
             "是(阈值%d)" % a.thresh if a.thresh > 0 else "否"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
