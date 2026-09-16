# -*- coding: utf-8 -*-
"""
VolunteerArmyPC —— 截图裁剪 / 缩放 / 叠加 NDC 网格

用法：
    python tools/crop_png.py in.png out.png --box 0.40,0.35,1.0,1.0 --scale 2 --grid

--box 用归一化屏幕坐标 "x0,y0,x1,y1"（0=左/上，1=右/下），便于和透视推导对齐。
--grid 画 NDC 网格：细线每 0.05，粗线每 0.25，并标注 NDC 数值。
        屏幕 NDC 的 x 从 -1(左) 到 +1(右)，y 从 -1(下,正数显示为下) ... 注意
        本工具按"图像坐标"标注：gx = 2*u-1 (u 为横向比例)，gy = 1-2*v。
"""
import struct
import sys
import zlib

# ---------- PNG 读写 ----------


def read_png(path):
    data = open(path, "rb").read()
    pos = 8
    w = h = ct = None
    idat = b""
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, _bd, ct = struct.unpack(">IIBB", chunk[:10])
        elif typ == b"IDAT":
            idat += chunk
        pos += 12 + ln
    raw = zlib.decompress(idat)
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    stride = w * ch
    prev = bytearray(stride)
    rows = []
    p = 0
    for _y in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if f == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i - ch]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                b = prev[i]
                c = prev[i - ch] if i >= ch else 0
                pa = abs(b - c)
                pb = abs(a - c)
                pc = abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        prev = line
        rows.append(line)
    return w, h, ch, rows


def write_png(path, w, h, rows):
    """rows: list of bytearray, RGB (3 ch)"""
    raw = bytearray()
    for r in rows:
        raw.append(0)
        raw += r

    def chunk(typ, data):
        c = struct.pack(">I", len(data)) + typ + data
        return c + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)

    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    out += chunk(b"IEND", b"")
    open(path, "wb").write(out)


# ---------- 5x7 点阵字（只做数字 / 少数符号） ----------
GLYPH = {
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "3": ["11111", "00010", "00100", "00010", "00001", "10001", "01110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
    "-": ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
    ".": ["00000", "00000", "00000", "00000", "00000", "01100", "01100"],
    "+": ["00000", "00100", "00100", "11111", "00100", "00100", "00000"],
    " ": ["00000", "00000", "00000", "00000", "00000", "00000", "00000"],
}


def draw_text(rows, w, h, x, y, text, color):
    cx = x
    for ch in text:
        g = GLYPH.get(ch)
        if g is None:
            cx += 6
            continue
        for ry in range(7):
            for rx in range(5):
                if g[ry][rx] == "1":
                    px, py = cx + rx, y + ry
                    if 0 <= px < w and 0 <= py < h:
                        o = px * 3
                        rows[py][o] = color[0]
                        rows[py][o + 1] = color[1]
                        rows[py][o + 2] = color[2]
        cx += 6


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print("usage: crop_png.py in.png out.png [--box x0,y0,x1,y1] [--scale N] [--grid]")
        return 1
    src, dst = args[0], args[1]
    box = (0.0, 0.0, 1.0, 1.0)
    scale = 1
    grid = "--grid" in args
    for i, a in enumerate(args):
        if a == "--box":
            box = tuple(float(v) for v in args[i + 1].split(","))
        elif a == "--scale":
            scale = int(args[i + 1])

    W, H, ch, rows = read_png(src)
    x0 = int(round(box[0] * W))
    y0 = int(round(box[1] * H))
    x1 = int(round(box[2] * W))
    y1 = int(round(box[3] * H))
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(W, x1), min(H, y1)
    cw, chh = x1 - x0, y1 - y0
    ow, oh = cw * scale, chh * scale

    grid_rgb = bytearray(ow * oh * 3)
    for oy in range(oh):
        sy = y0 + oy // scale
        sr = rows[sy]
        base = oy * ow * 3
        for ox in range(ow):
            sx = x0 + ox // scale
            so = sx * ch
            do = base + ox * 3
            if ch >= 3:
                grid_rgb[do] = sr[so]
                grid_rgb[do + 1] = sr[so + 1]
                grid_rgb[do + 2] = sr[so + 2]
            else:
                grid_rgb[do] = grid_rgb[do + 1] = grid_rgb[do + 2] = sr[so]

    out_rows = [bytearray(grid_rgb[i * ow * 3:(i + 1) * ow * 3]) for i in range(oh)]

    if grid:
        # 每 0.05 归一化画一条细线，每 0.25 画粗线并标注 NDC
        # 注意：out_rows[y] 已是"那一行"，偏移必须用行内偏移 (x*3)，
        # 不能再用全局偏移 (y*ow+x)*3 —— 后者会立刻越界。
        step = 0.05
        n = int(round(1.0 / step))
        for k in range(1, n):
            u = k * step
            px = int(round((u * W - x0) * scale))
            if 0 <= px < ow:
                major = abs(u * 4 - round(u * 4)) < 1e-6
                col = (255, 40, 40) if major else (90, 90, 30)
                for oy in range(oh):
                    o = px * 3
                    out_rows[oy][o] = col[0]
                    out_rows[oy][o + 1] = col[1]
                    out_rows[oy][o + 2] = col[2]
            py = int(round((u * H - y0) * scale))
            if 0 <= py < oh:
                major = abs(u * 4 - round(u * 4)) < 1e-6
                col = (255, 40, 40) if major else (90, 90, 30)
                row = out_rows[py]
                for ox in range(ow):
                    o = ox * 3
                    row[o] = col[0]
                    row[o + 1] = col[1]
                    row[o + 2] = col[2]

        # 标注：沿线写出对应的 NDC x / y
        for k in range(0, n + 1):
            u = k * step
            px = int(round((u * W - x0) * scale))
            if 0 <= px < ow - 24:
                ndc = 2 * u - 1
                txt = ("%+.2f" % ndc).replace("+0.", ".").replace("-0.", "-.")
                draw_text(out_rows, ow, oh, px + 2, 2, txt, (255, 230, 60))
            py = int(round((u * H - y0) * scale))
            if 0 <= py < oh - 8:
                ndc = 1 - 2 * u
                txt = ("%+.2f" % ndc).replace("+0.", ".").replace("-0.", "-.")
                draw_text(out_rows, ow, oh, 2, py + 2, txt, (120, 255, 120))

    write_png(dst, ow, oh, out_rows)
    print("wrote %s  %dx%d (crop %d,%d-%d,%d scale %d%s)" % (dst, ow, oh, x0, y0, x1, y1, scale, " +grid" if grid else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
