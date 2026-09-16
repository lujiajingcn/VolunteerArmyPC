# -*- coding: utf-8 -*-
"""VolunteerArmyPC —— PNG 盒式降采样（生成 README 配图用）

用法：
    python tools/downscale_png.py in.png out.png --factor 2

为什么不复用 crop_png.py 的 --scale：那个 --scale 是整数**放大**
（源码里是 `oy // scale` 取源像素），只能放大不能缩小。README 配图要的是
「2560px 截图 → 1280px」，所以另写一个降采样。

用盒式滤波（2x2 求平均）而不是最近邻：降采样后高频噪点被抹平，
zlib 反而压得更省字节，而且不会有最近邻那种锯齿。
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crop_png import read_png  # noqa: E402


def write_png9(path, w, h, rows):
    """crop_png.write_png 用的是 zlib level 6；README 配图改用 level 9 再省一点。"""
    raw = bytearray()
    for r in rows:
        raw.append(0)
        raw += r

    def chunk(typ, data):
        c = struct.pack(">I", len(data)) + typ + data
        return c + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)

    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    out += chunk(b"IEND", b"")
    open(path, "wb").write(out)


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print("usage: downscale_png.py in.png out.png [--factor N]")
        return 1
    src, dst = args[0], args[1]
    factor = 2
    for i, a in enumerate(args):
        if a == "--factor":
            factor = int(args[i + 1])
    if factor < 1:
        print("factor 必须 >= 1")
        return 1

    W, H, ch, rows = read_png(src)
    ow, oh = W // factor, H // factor
    if ow == 0 or oh == 0:
        print("factor 过大，输出尺寸为 0")
        return 1

    n = factor * factor
    out = []
    for oy in range(oh):
        # 逐输出行累加：把 factor 行的 factor*factor 邻域求和后取均值。
        # 用局部 acc 而不是全图 int 缓冲 —— 全图缓冲在 2560x1369 下要几百 MB。
        acc = [0] * (ow * 3)
        for sy in range(factor):
            sr = rows[oy * factor + sy]
            for ox in range(ow):
                base = ox * factor * ch
                ao = ox * 3
                for sx in range(factor):
                    so = base + sx * ch
                    acc[ao] += sr[so]
                    acc[ao + 1] += sr[so + 1]
                    acc[ao + 2] += sr[so + 2]
        row = bytearray(ow * 3)
        for i in range(ow * 3):
            row[i] = acc[i] // n
        out.append(row)

    write_png9(dst, ow, oh, out)
    print("wrote %s  %dx%d (from %dx%d, factor %d)" % (dst, ow, oh, W, H, factor))
    return 0


if __name__ == "__main__":
    sys.exit(main())
