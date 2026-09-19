# -*- coding: utf-8 -*-
"""VolunteerArmyPC —— GLB 三视图预览（不需要引擎就能判朝向与形状）

【为什么需要它】
图生3D 出来的模型朝向是**实测项**：参考图是侧视图，而服务端把"图像的哪个方向"
映到"模型的哪个轴"只有量出来才知道。原先的做法是"改代码 → 编译 → 起引擎 →
截图 → 看图"，一轮好几分钟，而且中间还夹着相机的姿态、光照、遮挡 ——
一屏的变量里只有一个是"模型朝向"。
本工具把这一步摘出来：直接读 GLB 的顶点，正交投影成三视图，
一眼就能看出"枪口在哪一端 / 枪身躺在哪个轴上 / 有没有生成成一坨"。

【怎么读】顶点散布图（splat），不是三角面光栅化。
5 万顶点的密度足够画出枪的剪影，而且不用处理 z-buffer 与背面剔除 ——
判断"这是不是一把枪、哪头是枪口"完全够用。

【坐标系】glTF 空间（右手系，Y 上，Z 朝观察者）。三张视图分别是：
    XY  ：沿 -Z 看（正视图，水平 = X，竖直 = Y）
    ZY  ：沿 -X 看（侧视图，水平 = Z，竖直 = Y）  ← 武器的"侧视图"就是这张
    XZ  ：沿 -Y 看（俯视图，水平 = X，竖直 = Z）
注意 Godot 会在导入时给根节点套一个 +90°绕X（Z-up → Y-up）——
那是**导入之后**的事，与本工具报告的原始朝向是两回事，别混。

用法：
    <venv-python> tools/glb_preview.py <a.glb> [--out png] [--size 520] [--views XY,ZY,XZ]

依赖 Pillow（与 slim_glb.py 共用隔离 venv）：
    C:/Users/<user>/.workbuddy/binaries/python/envs/default/Scripts/python.exe
"""

import io
import json
import math
import os
import struct
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:  # pragma: no cover
    print("需要 Pillow。请用隔离 venv 的 python 运行本工具：")
    print("  C:/Users/<user>/.workbuddy/binaries/python/envs/default/Scripts/python.exe")
    sys.exit(2)


# ------------------------------------------------------------------ GLB 读取
def glb_read(path):
    """与 slim_glb.glb_read 同一套（BIN 块按前 3 字节比，见那边的踩坑注释）。"""
    data = open(path, "rb").read()
    if data[:4] != b"glTF":
        raise ValueError("不是 GLB：%s" % path)
    pos = 12
    js = None
    bin_ = b""
    while pos + 8 <= len(data):
        clen, ctype = struct.unpack("<I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + clen]
        if ctype[:4] == b"JSON":
            js = json.loads(body.decode("utf-8"))
        elif ctype[:3] == b"BIN":
            bin_ = body
        pos += 8 + clen
    if js is None:
        raise ValueError("GLB 里没有 JSON 块")
    return js, bin_


# ------------------------------------------------------------------ 4x4 矩阵
def m_ident():
    return [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]   # 行主序


def m_mul(a, b):
    o = [0.0] * 16
    for r in range(4):
        for c in range(4):
            s = 0.0
            for k in range(4):
                s += a[r * 4 + k] * b[k * 4 + c]
            o[r * 4 + c] = s
    return o


def m_from_trs(t, r, s):
    """glTF 的 rotation 是四元数 (x, y, z, w)。"""
    x, y, z, w = r
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    m = [
        1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy), 0.0,
        2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx), 0.0,
        2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy), 0.0,
        0.0, 0.0, 0.0, 1.0,
    ]
    for c in range(3):
        for rr in range(3):
            m[rr * 4 + c] *= s[c]
    m[3], m[7], m[11] = t[0], t[1], t[2]
    return m


def node_matrix(n):
    if "matrix" in n:
        # glTF 的 matrix 是**列主序**，转成行主序
        cm = n["matrix"]
        return [cm[c * 4 + r] for r in range(4) for c in range(4)]
    return m_from_trs(n.get("translation", [0.0, 0.0, 0.0]),
                      n.get("rotation", [0.0, 0.0, 0.0, 1.0]),
                      n.get("scale", [1.0, 1.0, 1.0]))


def xform(m, p):
    x, y, z = p
    return (m[0] * x + m[1] * y + m[2] * z + m[3],
            m[4] * x + m[5] * y + m[6] * z + m[7],
            m[8] * x + m[9] * y + m[10] * z + m[11])


# ------------------------------------------------------------------ accessor
def read_vec3(js, bin_, acc_index):
    acc = js["accessors"][acc_index]
    if acc.get("componentType") != 5126 or acc.get("type") != "VEC3":
        return []
    if "sparse" in acc:
        # 生成器的输出没见过 sparse；真遇到就明确报出来，别静默读少一半顶点。
        print("    !! accessor %d 是 sparse，本工具不处理，可能漏顶点" % acc_index)
    bv = js["bufferViews"][acc["bufferView"]]
    base = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride") or 12
    n = acc["count"]
    out = []
    for i in range(n):
        o = base + i * stride
        out.append(struct.unpack_from("<fff", bin_, o))
    return out


# ------------------------------------------------------------------ 主流程
VIEWS = {
    #          水平轴  竖直轴  翻转？   说明（ASCII，PIL 默认字体只认 ASCII）
    "XY": (0, 1, "view -Z   front   horiz=X vert=Y"),
    "ZY": (2, 1, "view -X   side    horiz=Z vert=Y"),
    "XZ": (0, 2, "view -Y   top     horiz=X vert=Z"),
}


def collect_points(js, bin_):
    nodes = js.get("nodes", [])
    worlds = {}
    if nodes:
        stack = []
        for sc in js.get("scenes", [{}]):
            for root in sc.get("nodes", []):
                stack.append((root, m_ident()))
        while stack:
            idx, parent = stack.pop()
            n = nodes[idx]
            w = m_mul(parent, node_matrix(n))
            worlds[idx] = w
            for c in n.get("children", []):
                stack.append((c, w))
    else:
        worlds[None] = m_ident()

    pts = []
    for ni, n in enumerate(nodes) if nodes else [(None, {"mesh": 0})]:
        if "mesh" not in n:
            continue
        w = worlds.get(ni, m_ident())
        mesh = js["meshes"][n["mesh"]]
        for pr in mesh["primitives"]:
            if "POSITION" not in pr.get("attributes", {}):
                continue
            for p in read_vec3(js, bin_, pr["attributes"]["POSITION"]):
                pts.append(xform(w, p))
    return pts


def render_view(pts, ha, va, size, margin):
    xs = [p[ha] for p in pts]
    ys = [p[va] for p in pts]
    x0, x1 = min(xs), max(xs)
    y0, y1 = min(ys), max(ys)
    span = max(x1 - x0, y1 - y0, 1e-6)
    k = (size * (1.0 - 2 * margin)) / span
    cx, cy = (x0 + x1) * 0.5, (y0 + y1) * 0.5

    acc = {}
    for p in pts:
        px = int(round((p[ha] - cx) * k + size * 0.5))
        py = int(round(size * 0.5 - (p[va] - cy) * k))     # 屏幕 y 向下
        if 0 <= px < size and 0 <= py < size:
            key = py * size + px
            acc[key] = acc.get(key, 0) + 1

    img = Image.new("L", (size, size), 255)
    px = img.load()
    peak = max(acc.values()) if acc else 1
    for key, cnt in acc.items():
        # 密度 → 灰度：命中越多越黑，让密集处读成"实体"
        t = min(1.0, cnt / max(4.0, peak * 0.35))
        v = int(255 - t * 240)
        px[key % size, key // size] = min(px[key % size, key // size], v)
    return img, (x1 - x0, y1 - y0)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 2
    src = a[0]

    def opt(name, dflt):
        return type(dflt)(a[a.index(name) + 1]) if name in a else dflt

    size = opt("--size", 520)
    margin = 0.06
    views = (opt("--views", "XY,ZY,XZ")).split(",")
    dst = opt("--out", os.path.splitext(src)[0] + "_views.png")

    js, bin_ = glb_read(src)
    tris = 0
    for m in js.get("meshes", []):
        for pr in m["primitives"]:
            if "indices" in pr:
                tris += js["accessors"][pr["indices"]]["count"] // 3
    pts = collect_points(js, bin_)
    if not pts:
        print("!! 没读到任何 POSITION 顶点")
        return 1

    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    zs = [p[2] for p in pts]
    ext = (max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs))
    axis = max(range(3), key=lambda i: ext[i])
    print("%s  三角面 %d  顶点 %d" % (src, tris, len(pts)))
    print("  包围盒尺寸  X=%.3f  Y=%.3f  Z=%.3f   最长轴 = %s"
          % (ext[0], ext[1], ext[2], "XYZ"[axis]))
    for i, nm in enumerate("XYZ"):
        print("    %s: [%.3f, %.3f]" % (nm, min([p[i] for p in pts]), max([p[i] for p in pts])))

    # ---- 枪管轴线估计：长轴以外的**两个**轴都要量 ----
    # 竖直分量（place.y）决定枪会不会悬空或陷进手里 —— 这是最早只量它的原因。
    # 横向分量（place.x）同样要量，而且更容易漏：
    # make_wpn_node 是按**包围盒**居中的，而包围盒会被非对称突出物带偏
    # （莫辛的拉机柄、波波沙的弹鼓、DP-27 的圆盘弹匣都只长在一边），
    # 于是"枪身居中"实际是"包围盒居中"，枪管整体偏到一边。
    # 实测症状：开镜时枪明显偏在画面左侧（枪口约偏 175 px，见 sweep/_ads.png）。
    #
    # 【为什么切片取 3% 而不是 10%】DP-27 的两脚架装在**枪口附近**，10% 的切片里
    # 混进了两条垂到地面以下的腿，把均值整个拽下去 —— 算出来的轴线比真枪管低
    # 好几厘米，归一化之后枪管会浮在原点上方、枪口焰从枪管下面喷出来。
    # 所以每一片都额外报"跨度"：跨度明显大于一根枪管的直径，就说明混进了附件。
    x_cut = sorted(p[axis] for p in pts)[int(len(pts) * 0.97)]
    front = [p for p in pts if p[axis] >= x_cut]
    if front:
        if axis != 0:
            print("  !! 最长轴不是 X（是 %s）—— 下面的 place 映射是按"
                  "\"长轴=X、绕 Y 转 90°\"推的，换轴向要重推" % "XYZ"[axis])
        # 引擎侧映射：绕 Y 转 90° 后  原始X → -Z(枪口方向)、原始Y → +Y(上)、原始Z → +X(右)
        place_of = {0: "x", 1: "y", 2: "x"}
        for i in (1, 2):
            if i == axis:
                continue
            vals = [p[i] for p in front]
            b = sum(vals) / len(vals)
            c = (min(p[i] for p in pts) + max(p[i] for p in pts)) * 0.5
            span = max(vals) - min(vals)
            print("  最前端 3%% 切片（%d 顶点）在 %s 轴上：均值 %.4f  包围盒中心 %.4f  跨度 %.3f m"
                  % (len(front), "XYZ"[i], b, c, span))
            print("    -> 归一化后轴线落在 %s=%+.4f；要对到 0，place.%s 取 %+.4f"
                  % ("XYZ"[i], b - c, place_of[i], -(b - c)))
        print("    （跨度明显大于一根枪管直径 = 切片混进了附件，改用上边缘那侧推算）")

    # ---- 哪一端是枪口：用"粗细"判，不用"尖端"判 ----
    # 枪托那一端又高又厚（托底 18cm），枪口那一端只有一根枪管（几厘米）。
    # 比最前/最后 5% 薄片的竖直跨度，小的那头就是枪口。
    # 【为什么不用"哪头更细长"或者"哪头有准星"】准星是几毫米的小凸起，
    # 在 splat 图上根本读不出来；而"托底比枪管粗得多"是稳定的结构差异。
    lo, hi = min(xs), max(xs)
    slices = {}
    for tag, a, b in (("最小端", lo, lo + ext[0] * 0.05), ("最大端", hi - ext[0] * 0.05, hi)):
        sel = [p[1] for p in pts if a <= p[0] <= b]
        slices[tag] = (max(sel) - min(sel)) if sel else 0.0
    if slices["最小端"] > 0 and slices["最大端"] > 0:
        thick = "最小端" if slices["最小端"] < slices["最大端"] else "最大端"
        print("  端面厚度：最小端 %.3f m，最大端 %.3f m  ->  枪口在**%s**（X 轴）"
              % (slices["最小端"], slices["最大端"], thick))

    tiles = []
    for v in views:
        if v not in VIEWS:
            print("!! 未知视图 %r" % v)
            continue
        ha, va, label = VIEWS[v]
        img, ext2 = render_view(pts, ha, va, size, margin)
        d = ImageDraw.Draw(img)
        d.rectangle([0, 0, size - 1, size - 1], outline=180)
        d.text((6, 6), "%s  %dx%d" % (v, size, size), fill=90)
        d.text((6, 20), label, fill=90)
        d.text((6, size - 16), "span %.3f x %.3f" % ext2, fill=90)
        tiles.append(img)

    if not tiles:
        return 1
    gap = 8
    out = Image.new("L", (size * len(tiles) + gap * (len(tiles) - 1), size), 235)
    for i, t in enumerate(tiles):
        out.paste(t, (i * (size + gap), 0))
    out.save(dst)
    print("  wrote %s  %dx%d" % (dst, out.width, out.height))
    return 0


if __name__ == "__main__":
    sys.exit(main())
