# -*- coding: utf-8 -*-
"""
VolunteerArmyPC —— 裁剪 + 整数降采样出图

为什么不用 tools/crop_png.py：那个脚本会一次性分配 ow*oh*3 的网格缓冲，
裁 2560x1369 这种竖屏大图时直接 MemoryError。这里改成按需分配、整数抽取，
只做"裁一块出来看"这一件事。

用法：
    python tools/view_crop.py 源.png 输出.png x0,y0,x1,y1 [因子]
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_png import read_png  # noqa: E402


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
    a = sys.argv[1:]
    if len(a) < 3:
        print(__doc__)
        return 1
    src, dst = a[0], a[1]
    x0, y0, x1, y1 = (int(v) for v in a[2].split(","))
    f = int(a[3]) if len(a) > 3 else 1

    W, H, ch, rows = read_png(src)
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(W, x1), min(H, y1)
    ow, oh = (x1 - x0) // f, (y1 - y0) // f
    out = []
    for oy in range(oh):
        srcy = y0 + oy * f
        r = rows[srcy]
        line = bytearray(ow * 3)
        for ox in range(ow):
            i = (x0 + ox * f) * ch
            j = ox * 3
            line[j] = r[i]
            line[j + 1] = r[i + 1]
            line[j + 2] = r[i + 2]
        out.append(bytes(line))
    write_png(dst, ow, oh, out)
    print("裁剪 %s x[%d,%d) y[%d,%d) 因子 %d -> %dx%d" % (os.path.basename(src), x0, x1, y0, y1, f, ow, oh))
    return 0


if __name__ == "__main__":
    sys.exit(main())
