# -*- coding: utf-8 -*-
"""
VolunteerArmyPC —— 截图像素探针

"画面看着对不对"不能只靠肉眼：AgX 在高光区会强烈去饱和，
一旦总光能过高，草地就会从绿变成灰，肉眼只看到"发灰"却说不清原因。
这个脚本直接读 PNG，报出几个关键区域的平均亮度与饱和度，把"发灰"变成数字。

用法：
    python tools/probe_png.py captures/cap_5s.png
"""
import struct
import sys
import zlib


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
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        prev = line
        rows.append(bytes(line))
    return w, h, ch, rows


def region_stats(rows, ch, x0, y0, x1, y1):
    n = 0
    sr = sg = sb = 0
    mx = 0
    for y in range(y0, y1):
        r = rows[y]
        for x in range(x0, x1):
            o = x * ch
            R, G, B = r[o], r[o + 1], r[o + 2]
            sr += R
            sg += G
            sb += B
            v = max(R, G, B)
            if v > mx:
                mx = v
            n += 1
    r_, g_, b_ = sr / n, sg / n, sb / n
    lum = 0.2126 * r_ + 0.7152 * g_ + 0.0722 * b_
    mxv, mnv = max(r_, g_, b_), min(r_, g_, b_)
    sat = 0.0 if mxv <= 0 else (mxv - mnv) / mxv
    # 过曝比例：任一通道 >= 250 的像素占比
    blown = 0
    for y in range(y0, y1):
        r = rows[y]
        for x in range(x0, x1):
            o = x * ch
            if r[o] >= 250 or r[o + 1] >= 250 or r[o + 2] >= 250:
                blown += 1
    return dict(rgb=(r_, g_, b_), lum=lum, sat=sat, peak=mx, blown=blown / n)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "captures/cap_5s.png"
    w, h, ch, rows = read_png(path)
    print("%s  %dx%d  channels=%d" % (path, w, h, ch))
    # 区域按 1920x1080 布局等比例取
    sx, sy = w / 1920.0, h / 1080.0
    regions = [
        ("天空(上)",      int(700 * sx), int(40 * sy),  int(1100 * sx), int(120 * sy)),
        ("天空(近地平线)", int(700 * sx), int(200 * sy), int(1100 * sx), int(250 * sy)),
        ("地面近处",      int(120 * sx), int(900 * sy), int(420 * sx),  int(1020 * sy)),
        ("地面中景",      int(620 * sx), int(560 * sy), int(860 * sx),  int(620 * sy)),
        ("左树林",        int(180 * sx), int(430 * sy), int(360 * sx),  int(500 * sy)),
    ]
    print("%-14s %-20s %7s %7s %8s" % ("区域", "平均 RGB", "亮度", "饱和", "过曝%"))
    for name, x0, y0, x1, y1 in regions:
        s = region_stats(rows, ch, x0, y0, x1, y1)
        print("%-14s (%3d,%3d,%3d)      %7.1f %7.3f %7.2f%%" % (
            name, s["rgb"][0], s["rgb"][1], s["rgb"][2], s["lum"], s["sat"], s["blown"] * 100))
    # 全图
    s = region_stats(rows, ch, 0, 0, w, h)
    print("%-14s (%3d,%3d,%3d)      %7.1f %7.3f %7.2f%%" % (
        "全图", s["rgb"][0], s["rgb"][1], s["rgb"][2], s["lum"], s["sat"], s["blown"] * 100))


if __name__ == "__main__":
    main()
