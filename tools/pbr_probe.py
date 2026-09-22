# -*- coding: utf-8 -*-
"""量一份 GLB 里的 PBR 贴图实际内容 —— 用来判断"生成器给的材质到底算不算合格"。

【为什么要单独一个工具】图生3D 产出的 GLB 常常**不写** `metallicFactor` /
`roughnessFactor`，而 glTF 规范规定这两项缺省即 **1.0**。于是"材质是不是金属"
这件事既不在文件里写着，也不能从屏幕上直接读出来（一台机器上看着像石头，
换一套灯就变成镜子）。而 glTF 把 metallic / roughness 打包在**同一张贴图**里：

    metallicRoughnessTexture:  G 通道 = roughness，B 通道 = metallic

所以"它到底给了多少 metallic"是一个**可以从字节里量出来的数**，不该靠肉眼猜。
本项目实测（2026-09-22，hy-3d-3.1 + enable_pbr，地物四件套）：
两块岩石的 MR 贴图 B 通道中位 **255**（= 满金属）→ 在没有反射探针的场上
只剩镜面路径，渲染成"顶面惨白、缝里死黑"，正是"金属在反射天空"的样子。

用法：
    python tools/pbr_probe.py assets/art/prop/model/prop_rock_a.glb [更多 glb ...]

需要 Pillow；用隔离 venv 跑：
    C:/Users/lujiajing/.workbuddy/binaries/python/envs/default/Scripts/python.exe \
        tools/pbr_probe.py <glb>
"""

import json
import os
import struct
import sys


def read_glb(path):
    """返回 (json_chunk, bin_chunk)。BIN 块的类型按 3 字符比 —— 它带结尾 NUL。"""
    data = open(path, "rb").read()
    if data[:4] != b"glTF":
        raise ValueError("不是 GLB：%s" % path)
    off = 12
    js = None
    binc = b""
    while off < len(data):
        ln, ty = struct.unpack_from("<II", data, off)
        off += 8
        ctype = data[off - 4:off]        # 类型字段的原始 4 字节（BIN 带结尾 NUL）
        chunk = data[off:off + ln]
        off += ln
        if ty == 0x4E4F534A:             # 'JSON'
            js = json.loads(chunk.decode("utf-8"))
        elif ctype[:3] == b"BIN":        # 注意：比 3 字符。写成 ctype[:4] 永远不相等
            binc = chunk
    if js is None:
        raise ValueError("GLB 里没有 JSON 块")
    return js, binc


def image_bytes(js, binc, img_idx):
    img = js["images"][img_idx]
    if "bufferView" not in img:
        return None, img.get("mimeType")
    bv = js["bufferViews"][img["bufferView"]]
    s = bv.get("byteOffset", 0)
    return binc[s:s + bv["byteLength"]], img.get("mimeType")


def stats(im, chan):
    """返回某通道的 (中位, 均值, 占比>0.5)，按 0..1 归一。"""
    px = list(im.getdata())
    vals = [p[chan] for p in px]
    vals.sort()
    n = len(vals)
    med = vals[n // 2] / 255.0
    mean = sum(vals) / n / 255.0
    hi = sum(1 for v in vals if v > 127) / float(n)
    return med, mean, hi


def probe(path):
    import io
    from PIL import Image

    js, binc = read_glb(path)
    print("=" * 68)
    print(os.path.basename(path))

    for mi, mat in enumerate(js.get("materials", [])):
        pbr = mat.get("pbrMetallicRoughness", {})
        print("  材质[%d] %r" % (mi, mat.get("name")))
        print("    metallicFactor   = %s  (缺省 -> 1.0)"
              % pbr.get("metallicFactor", "<缺>"))
        print("    roughnessFactor  = %s  (缺省 -> 1.0)"
              % pbr.get("roughnessFactor", "<缺>"))
        print("    baseColorFactor  = %s  (缺省 -> 白)"
              % pbr.get("baseColorFactor", "<缺>"))

        # --- metallicRoughness：G = roughness，B = metallic ---
        mrt = pbr.get("metallicRoughnessTexture")
        if mrt is None:
            print("    [无] metallicRoughness 贴图 -> 两个因子就是终值")
        else:
            ti = mrt.get("index", 0)
            src = js["textures"][ti].get("source", 0)
            raw, mime = image_bytes(js, binc, src)
            im = Image.open(io.BytesIO(raw))
            print("    MR 贴图 %s %s（声明 mimeType=%s）"
                  % (im.format, im.size, mime))
            im = im.convert("RGB")
            g_med, g_mean, g_hi = stats(im, 1)
            b_med, b_mean, b_hi = stats(im, 2)
            print("      roughness(G) 中位 %.3f 均值 %.3f  >0.5 占比 %.1f%%"
                  % (g_med, g_mean, g_hi * 100))
            print("      metallic (B) 中位 %.3f 均值 %.3f  >0.5 占比 %.1f%%   "
                  "<== 这个数高就是「满金属」"
                  % (b_med, b_mean, b_hi * 100))

        bct = pbr.get("baseColorTexture")
        if bct is not None:
            ti = bct.get("index", 0)
            src = js["textures"][ti].get("source", 0)
            raw, _ = image_bytes(js, binc, src)
            im = Image.open(io.BytesIO(raw)).convert("RGB")
            # 只报亮度中位，够用来判断"要不要加 albedo 乘数"
            px = list(im.getdata())
            lum = sorted(int(0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2])
                         for p in px)
            n = len(lum)
            print("    反照率贴图 %s 亮度中位 %d 均值 %d  >240 占比 %.1f%%"
                  % (im.size, lum[n // 2], sum(lum) // n,
                     sum(1 for v in lum if v > 240) / float(n) * 100))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    for p in sys.argv[1:]:
        probe(p)
