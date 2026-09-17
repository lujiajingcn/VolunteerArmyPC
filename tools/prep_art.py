# -*- coding: utf-8 -*-
"""VolunteerArmyPC —— 任务素材预处理：裁到 16:9 + 切掉生成水印 + 规范命名

为什么必须裁：
  生成器在每张图右下角压了一行 "AI生成 WORKBUDDY.CN" 水印。它直接进不了游戏 ——
  菜单/简报是全屏铺图，水印会明晃晃挂在角上。
  而游戏的视口本来就是 1920x1080（16:9），源图是 1536x1024（3:2），
  横竖比本来就要裁。于是两件事合成一件：从顶部裁 1536x864，
  水印（实测落在 y>900）连同多余的底部一起切掉。

为什么从顶部而不是居中裁：
  这五张的构图都是「天空 + 地平线 + 山脊剪影」，重心在上三分之二；
  居中和从底部裁都会把地平线切掉。顶部裁保留全部天空与山脊。

用法：
    python tools/prep_art.py            # 处理 assets/art/raw/*.png
"""

import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from probe_png import read_png  # noqa: E402

RAW_DIR = os.path.join(ROOT, "assets", "art", "raw")
OUT_DIR = os.path.join(ROOT, "assets", "art")

# 源文件名前缀 -> 目标名。用前缀匹配是因为生成器会在文件名后面接时间戳。
RENAME = [
    ("Ultra_wide_cinematic_video_gam", "art_menu_bg"),    # 主菜单背景
    ("Cinematic_video_game_MISSION_B", "art_briefing"),   # 任务简报主视觉
    ("Cinematic_video_game_VICTORY_r", "art_end_win"),    # 结算 · 成功
    ("Cinematic_video_game_DEFEAT_re", "art_end_lose"),   # 结算 · 失败
    ("Cinematic_video_game_LEVEL_EST", "art_chapter"),    # 区域态势（俯瞰公路）
]


def write_png(path, w, h, rows):
    def chunk(typ, data):
        c = struct.pack(">I", len(data)) + typ + data
        return c + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(raw, 6))
    out += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(out)


# 水印亮度阈值。
# 【为什么是 110 而不是 155】水印是**半透明**的，它的实际亮度 = 文字不透明度 × 底色。
# 压在暗岩石上能到 160+，压在雪/天空上就只有 110~120。第一版取 155，
# 结果只有压在最暗背景上的那一张报了"检出"，另外四张全部静默判为"未检出" ——
# 一个会放水印进游戏的假阴性。阈值必须按"最坏情况（亮底）"来定，不能按"最好情况"。
WM_THR = 110.0


def watermark_bbox(rows, ch, w, h):
    """在右下角找那行水印的包围盒。

    判据用「亮度」而不是「某个颜色」：水印是半透明的，没有固定色值。

    【扫描区为什么这么小】第一版取 x∈[0.75w,w) y∈[0.80h,h)，结果在 art_chapter 上
    把右下角那片**被照亮的山脊**整块扫了进来，报出 y[819,1023] 的假阳性 ——
    水印实际只在 x[1415,1525] y[963,1013]，也就是 x>0.92w、y>0.94h。
    区域开大一点就会把"亮的地形"误认成"水印，于是这个检查既会假阴性（阈值太高）
    又会假阳性（区域太大）。现在按水印的真实落点收紧，只留必要的余量。
    """
    x0, y0 = int(w * 0.88), int(h * 0.90)
    bx0, by0, bx1, by1 = w, h, -1, -1
    for y in range(y0, h):
        r = rows[y]
        for x in range(x0, w):
            i = x * ch
            lum = 0.2126 * r[i] + 0.7152 * r[i + 1] + 0.0722 * r[i + 2]
            if lum > WM_THR:
                if x < bx0:
                    bx0 = x
                if x > bx1:
                    bx1 = x
                if y < by0:
                    by0 = y
                if y > by1:
                    by1 = y
    if bx1 < 0:
        return None
    return (bx0, by0, bx1, by1)


def find_raw(prefix):
    for f in sorted(os.listdir(RAW_DIR)):
        if f.startswith(prefix) and f.lower().endswith(".png"):
            return f
    return None


def main():
    if not os.path.isdir(RAW_DIR):
        print("找不到源目录 %s" % RAW_DIR)
        return 1

    print("%-14s %-9s %-22s %s" % ("目标", "裁切", "水印包围盒", "结论"))
    ok = 0
    for prefix, name in RENAME:
        src = find_raw(prefix)
        if src is None:
            print("%-14s %s" % (name, "!! 缺源文件（前缀 " + prefix + "）"))
            continue
        w, h, ch, rows = read_png(os.path.join(RAW_DIR, src))
        cw = w
        chh = (w * 9) // 16                      # 16:9
        if chh > h:
            chh = h

        wb = watermark_bbox(rows, ch, w, h)
        if wb is None:
            note = "未检出"
        elif wb[1] >= chh:
            note = "已裁掉（水印顶 y=%d >= 裁切线 %d）" % (wb[1], chh)
        else:
            note = "!! 仍在画面内（水印 y[%d,%d]）" % (wb[1], wb[3])

        out_rows = rows[:chh]
        dst = os.path.join(OUT_DIR, name + ".png")
        write_png(dst, cw, chh, out_rows)

        box = "无" if wb is None else "%d,%d-%d,%d" % wb
        print("%-14s %-9s %-22s %s" % (name, "%dx%d" % (cw, chh), box, note))
        ok += 1

    print("\n完成 %d 张 -> %s" % (ok, OUT_DIR))
    return 0


if __name__ == "__main__":
    sys.exit(main())
