# -*- coding: utf-8 -*-
"""VolunteerArmyPC —— GLB 瘦身：把内嵌的 4K 贴图降采样后重新封装 GLB。

【为什么必须做这一步】
图生三维产出的 GLB 单个体积是 42.78MB，解剖结果是：
    · 网格 5.0 万三角面 —— 对游戏角色完全合理，不是问题
    · 内嵌贴图 41.3MB    —— 全部体积都在这里，三张 4K 图
三个角色 11 个就是约 470MB —— 既进不了 GitHub（单文件 100MB 上限），
也没法在游戏里按需加载。而这个游戏里士兵在画面上通常只有几十到两百像素高
（1.7 米的人站在 110 米见方的战场上），4K 贴图纯属浪费。

【依赖 Pillow —— 这是本项目唯一一个非纯 Python 的工具】
生成器号称贴图是 image/png，实际写的是 JPEG（mimeType 撒谎，靠魔数才认出来）。
纯 Python 解 PNG 那套（probe_png.py）在这里用不上，而手写 JPEG 解码不现实。
所以本工具用隔离 venv 里的 Pillow：
    C:/Users/<user>/.workbuddy/binaries/python/envs/default/Scripts/python.exe
安装： <venv>/Scripts/python.exe -m pip install pillow
其余工具仍然保持零依赖。

【做法】不动网格、不动 accessor、不动材质，只替换图像 bufferView：
解析 GLB → 取每个图像 bufferView 的字节 → 解码 → 等比缩到 max 边长
→ 重编码 → 按原顺序重排 BIN（4 字节对齐）→ 回写 bufferView 的 offset/length
与 buffer 总长。accessor 的 byteOffset 是相对 bufferView 的，bufferView 内容不变，
所以整条网格数据不受影响。

用法：
    <venv-python> tools/slim_glb.py probe <a.glb> [b.glb ...]
    <venv-python> tools/slim_glb.py slim  <in.glb> <out.glb> [--max 512] [--q 90]
"""

import io
import json
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    print("需要 Pillow。请用隔离 venv 的 python 运行本工具：")
    print("  C:/Users/<user>/.workbuddy/binaries/python/envs/default/Scripts/python.exe "
          "-m pip install pillow")
    sys.exit(2)


# ------------------------------------------------------------------ GLB 读写
def glb_read(path):
    data = open(path, "rb").read()
    if data[:4] != b"glTF":
        raise ValueError("不是 GLB：%s" % path)
    pos = 12
    js = None
    bin_ = b""
    while pos + 8 <= len(data):
        clen, ctype = struct.unpack("<I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + clen]
        # BIN 块的类型是 b"BIN\x00"（带结尾 NUL）。
        # 【踩过的坑】这里原本写 `ctype[:4] == b"BIN"`，而 ctype[:4] 是 b"BIN\x00"，
        # 两个永远不相等 —— BIN 块静默地没被取到，所有贴图读出 0 字节，
        # 现象是"三张贴图都识别成非 PNG"。类型判断必须按 3 个字符比。
        if ctype[:4] == b"JSON":
            js = json.loads(body.decode("utf-8"))
        elif ctype[:3] == b"BIN":
            bin_ = body
        pos += 8 + clen
    if js is None:
        raise ValueError("GLB 里没有 JSON 块")
    return js, bin_


def glb_write(path, js, bin_):
    jb = json.dumps(js, separators=(",", ":")).encode("utf-8")
    jb += b" " * ((4 - len(jb) % 4) % 4)
    bb = bin_ + b"\x00" * ((4 - len(bin_) % 4) % 4)
    total = 12 + 8 + len(jb) + 8 + len(bb)
    out = b"glTF" + struct.pack("<II", 2, total)
    out += struct.pack("<I4s", len(jb), b"JSON") + jb
    out += struct.pack("<I4s", len(bb), b"BIN\x00") + bb
    with open(path, "wb") as f:
        f.write(out)


def image_bytes(js, bin_, bv_index):
    bv = js["bufferViews"][bv_index]
    off = bv.get("byteOffset", 0)
    return bin_[off:off + bv["byteLength"]]


def tri_count(js):
    n = 0
    for m in js.get("meshes", []):
        for pr in m["primitives"]:
            if "indices" in pr:
                n += js["accessors"][pr["indices"]]["count"] // 3
    return n


def cmd_probe(paths):
    for p in paths:
        js, bin_ = glb_read(p)
        print("=== %s  文件 %.2f MB  三角面 %d" % (p, os.path.getsize(p) / 1048576, tri_count(js)))
        for i, im in enumerate(js.get("images", [])):
            raw = image_bytes(js, bin_, im["bufferView"])
            try:
                img = Image.open(io.BytesIO(raw))
                info = "%s %dx%d %s" % (img.format, img.size[0], img.size[1], img.mode)
            except Exception as e:  # noqa: BLE001
                info = "解不开：%s" % e
            print("    [%d] %-24s %8.2f MB  %s  mime=%-10s" % (
                i, (im.get("name") or "")[:24], len(raw) / 1048576, info, im.get("mimeType")))
        for n in js.get("nodes", []):
            if "rotation" in n:
                print("    node rotation=%s scale=%s" % (n["rotation"], n.get("scale")))
        print()


def cmd_slim(src, dst, max_px, quality):
    js, bin_ = glb_read(src)
    old_total = len(bin_)

    pieces = []
    off = 0
    for bi, bv in enumerate(js["bufferViews"]):
        o = bv.get("byteOffset", 0)
        body = bin_[o:o + bv["byteLength"]]
        img = None
        for im in js.get("images", []):
            if im["bufferView"] == bi:
                img = im
                break
        if img is not None:
            try:
                im_obj = Image.open(io.BytesIO(body))
                im_obj.load()
                w, h = im_obj.size
                if max(w, h) > max_px:
                    k = max_px / float(max(w, h))
                    nw, nh = max(1, int(round(w * k))), max(1, int(round(h * k)))
                    # LANCZOS 而不是 NEAREST：法线贴图上的高频噪点用最近邻会整片留下，
                    # 渲染出来人物表面是一层细碎麻点。
                    im_obj = im_obj.resize((nw, nh), Image.LANCZOS)
                    buf = io.BytesIO()
                    if im_obj.mode not in ("RGB", "L"):
                        im_obj = im_obj.convert("RGB")
                    im_obj.save(buf, format="JPEG", quality=quality, optimize=True)
                    nb = buf.getvalue()
                    img["mimeType"] = "image/jpeg"
                    print("    [%-22s] %4dx%-4d %7.2fMB -> %3dx%-3d %7.2fMB" % (
                        (img.get("name") or "")[:22], w, h, len(body) / 1048576,
                        nw, nh, len(nb) / 1048576))
                    body = nb
                else:
                    print("    [%-22s] %4dx%-4d 已小于目标，保留" % (
                        (img.get("name") or "")[:22], w, h))
            except Exception as e:  # noqa: BLE001
                print("    [%-22s] 处理失败，原样保留：%s" % ((img.get("name") or "")[:22], e))
        pad = (4 - off % 4) % 4
        if pad:
            pieces.append(b"\x00" * pad)
            off += pad
        bv["byteOffset"] = off
        bv["byteLength"] = len(body)
        pieces.append(body)
        off += len(body)

    new_bin = b"".join(pieces)
    js["buffers"][0]["byteLength"] = len(new_bin)
    glb_write(dst, js, new_bin)
    print("  %s -> %s : BIN %.2fMB -> %.2fMB，成品 %.2fMB" % (
        os.path.basename(src), os.path.basename(dst),
        old_total / 1048576, len(new_bin) / 1048576, os.path.getsize(dst) / 1048576))


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__)
        return 2

    def opt(name, dflt):
        return type(dflt)(a[a.index(name) + 1]) if name in a else dflt

    max_px = opt("--max", 512)
    quality = opt("--q", 90)
    if a[0] == "probe":
        cmd_probe(a[1:])
        return 0
    if a[0] == "slim":
        cmd_slim(a[1], a[2], max_px, quality)
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
