# -*- coding: utf-8 -*-
"""
VolunteerArmyPC —— 死黑掩码出图

把「亮度 < 阈值」的像素刷成纯白、其余压成纯黑，输出一张只有黑白两色的图。
为什么需要它：死黑占比是个标量，它只告诉你"有多少"，不告诉你"在哪里、什么形状"。
而形状恰恰是归属判断的关键 —— 上一轮就是靠"宽 106px、贯穿全高、宽度不随高度变化"
这三个形状特征，才把画面正中那根黑竖条判成圆柱侧影，而不是某棵树。

用法：
    python tools/dead_mask.py 源图.png 输出.png [阈值] [步长]
    # 再配 tools/downscale_png.py 缩一张小图方便直接看
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_png import read_png  # noqa: E402

# 阈值取 25：与 pixel_stats.py 的"死黑"定义保持一致，两边的数才对得上。
DEFAULT_TH = 25.0


def write_png(path, w, h, rows):
    def chunk(typ, data):
        c = struct.pack(">I", len(data)) + typ + data
        return c + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + r for r in rows)
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(raw, 6))
    out += chunk(b"IEND", b"")
    open(path, "wb").write(out)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    th = float(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_TH
    w, h, ch, rows = read_png(src)
    out = []
    for y in range(h):
        r = rows[y]
        line = bytearray(w * 3)
        for x in range(w):
            i = x * ch
            lum = 0.2126 * r[i] + 0.7152 * r[i + 1] + 0.0722 * r[i + 2]
            v = 255 if lum < th else 0
            j = x * 3
            line[j] = line[j + 1] = line[j + 2] = v
        out.append(bytes(line))
    write_png(dst, w, h, out)
    print("已写出 %s（%dx%d，阈值 %.0f）" % (dst, w, h, th))
    return 0


if __name__ == "__main__":
    sys.exit(main())
