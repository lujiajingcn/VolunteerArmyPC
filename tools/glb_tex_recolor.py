# -*- coding: utf-8 -*-
"""把 GLB 贴图上「红系」像素的色相重映射成树皮灰褐 —— **几何一个顶点都不动**。

为什么要有这条通道
------------------
2026-09-24：`prop_pine` 的树干是品红色。根因是**生成器给的 baseColorTexture
底色本身就是暗红**（prompt 里当初也明写了"红褐色树干"）。

重新文生3D 能修，但代价是**重掷骰子** —— 上一轮好不容易把树冠的剖面锯齿度
从 0.2120 打到 0.0268（1/7.9），而那个数是形状判据里最好的一条。为了换颜色
把它赔进去不划算。这个工具换的是**贴图像素**，网格/法线/UV 全都不碰，
所以几何指标**逐位不变**。

为什么不能在引擎侧改
--------------------
红与绿在**同一张贴图**里（导出来看过：红底 + 灰绿枝叶团块，像迷彩）。
引擎侧的 tint / albedo 是整张贴图的乘数，改它会把树冠一起染红
—— 这正是"零成本修不了"那句话的来源。要在**像素级**分开，只能在这一层做。

算法（保明暗、换色相）
----------------------
对每个像素：
    ① 转 HSV。**不满足「有色 + 落在红圈」的原样保留**（树冠的绿、灰噪点都不动）。
    ② 红圈定义：色相 ≥ `--h-lo`（默认 330°）或 ≤ `--h-hi`（默认 25°），
       且饱和度 ≥ `--sat`（默认 0.12）。
    ③ 命中后把色相拉向 `--target-h`（默认 30° 灰褐）、饱和度乘 `--sat-mul`、
       明度乘 `--lift`（默认 0.78）。
       ⚠️ **明度不能"原样保留"** —— 实测保留 V 时树干读作**水泥柱**：同一 V 下
       红→黄褐的**感知亮度是上升的**，再叠上降饱和（接近灰），受光面直接发白。
       树皮的木纹/纵裂全在明暗里，所以是**按比例压**而不是抹平。
    ④ 权重 `w` 用来和原像素线性混合：`w = 1` 完全替换。默认在色相的**边缘做软过渡**
       （`--soft`，默认 12°），否则红圈边界会留下一圈硬色带。

⚠️ 归一化基准（`len_m` / `hw`）不受影响：几何没变，包络盒一个数都不变。

用法：
    python tools/glb_tex_recolor.py <in.glb> <out.glb> [--target-h 30] [--sat-mul 0.55]
                                    [--dump <目录>]      # 导出改前/改后贴图便于肉眼比对
                                    [--dry-run]          # 只统计命中比例，不写文件
"""
import argparse
import colorsys
import io
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


# ----------------------------------------------------------------- GLB 读写
def read_glb(path):
    data = open(path, "rb").read()
    if data[:4] != b"glTF":
        raise SystemExit("不是 GLB：%s" % path)
    pos, js, bin_, bin_off = 12, None, None, None
    while pos < len(data):
        ln, typ = struct.unpack("<II", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        if typ == 0x4E4F534A:
            js = json.loads(body.decode("utf-8"))
        elif typ == 0x004E4942:
            bin_, bin_off = body, pos + 8
        pos += 8 + ln
    return js, bin_, bin_off


def base_tex_slot(js):
    """返回 (image 下标, 对应的 bufferView 下标)。取 material[0] 的 baseColorTexture。"""
    for m in js.get("materials", []):
        ti = m.get("pbrMetallicRoughness", {}).get("baseColorTexture", {}).get("index")
        if ti is None:
            continue
        src = js["textures"][ti].get("source")
        if src is None:
            continue
        return src, js["images"][src]["bufferView"]
    return None, None


def repack_glb(js, bin_, img_bv_index, new_img_bytes, new_mime):
    """把 new_img_bytes 写进 img_bv_index 指向的位置，重排 BIN 并输出完整 GLB 字节。

    【为什么必须重排而不是原地覆盖】新 JPEG 的长度和旧的不一样。图片 bufferView
    后面还有别的 bufferView（本文件：normal / mr 两张图），原地写会把它们顶掉。
    重排 = 按旧 byteOffset 顺序把每段重新摆放（各自 4 字节对齐），再回写每段的
    byteOffset —— accessor 引用的是**索引**不是偏移，所以它们一个都不用改。
    """
    bvs = js["bufferViews"]

    # 收集每段的数据（要替换的那段用新字节）
    segs = []
    for i, bv in enumerate(bvs):
        off = bv.get("byteOffset", 0)
        ln = bv["byteLength"]
        segs.append((off, i, new_img_bytes if i == img_bv_index else bin_[off:off + ln]))

    out = bytearray()
    for _, i, blob in sorted(segs):        # 按旧偏移的顺序摆
        pad = (-len(out)) % 4              # 段起点 4 字节对齐
        out += b"\x00" * pad
        bvs[i]["byteOffset"] = len(out)
        bvs[i]["byteLength"] = len(blob)
        out += blob
    js["buffers"][0]["byteLength"] = len(out)

    for img in js["images"]:
        if img.get("bufferView") == img_bv_index:
            img["mimeType"] = new_mime

    jbytes = json.dumps(js, separators=(",", ":")).encode("utf-8")
    jbytes += b" " * ((-len(jbytes)) % 4)
    bbytes = bytes(out) + b"\x00" * ((-len(out)) % 4)

    total = 12 + 8 + len(jbytes) + 8 + len(bbytes)
    head = struct.pack("<III", 0x46546C67, 2, total)
    return (head
            + struct.pack("<II", len(jbytes), 0x4E4F534A) + jbytes
            + struct.pack("<II", len(bbytes), 0x004E4942) + bbytes)


# ------------------------------------------------------------------- 重着色
def arc_pos(deg, h_lo, h_hi):
    """把色相投到「红圈弧」上的位置；不在圈里返回 None。

    红圈是**跨 0° 的一条弧**：从 h_lo(330°) 顺时针经 360° 到 h_hi(25°)，
    弧长 span = (360 - h_lo) + h_hi（默认 55°）。
        deg >= h_lo  → pos = deg - h_lo
        deg <= h_hi  → pos = (360 - h_lo) + deg
        其余          → None
    """
    span = (360.0 - h_lo) + h_hi
    if deg >= h_lo:
        pos = deg - h_lo
    elif deg <= h_hi:
        pos = (360.0 - h_lo) + deg
    else:
        return None, span
    return pos, span


def in_red(deg, h_lo, h_hi):
    """色相是否落在「红圈」：>= h_lo（如 330）或 <= h_hi（如 25）。"""
    return arc_pos(deg, h_lo, h_hi)[0] is not None


def soft_weight(deg, h_lo, h_hi, soft):
    """软过渡权重 0~1：弧内部 1，距弧两端 soft 度内线性降到 0。

    没有软过渡时，红圈的边界会留下一圈**硬色带**（一侧被改、一侧没改），
    在树干上表现为一道突兀的接缝。
    """
    pos, span = arc_pos(deg, h_lo, h_hi)
    if pos is None:
        return 0.0
    if soft <= 0:
        return 1.0
    d = min(pos, span - pos)          # 到弧两端的角距离
    return max(0.0, min(1.0, d / soft))


def recolor(img, args):
    w, h = img.size
    px = img.load()
    hit = 0
    total = w * h
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            hh, ss, vv = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
            deg = hh * 360.0
            if ss < args.sat or vv < 0.02 or not in_red(deg, args.h_lo, args.h_hi):
                continue
            wt = soft_weight(deg, args.h_lo, args.h_hi, args.soft)
            if wt <= 0.0:
                continue
            # 目标：色相拉到 target_h，饱和度乘 sat_mul，明度乘 lift
            # ⚠️ 明度不能"保留"，实测（2026-09-24）保留 V 会让树干读作**水泥柱**：
            #    同一 V 下，红→黄褐的**感知亮度上升**，再叠上降饱和（接近灰），
            #    受光面直接发白。所以 lift 默认 0.78 —— 让树皮落在"中等偏暗"。
            nh = args.target_h / 360.0
            ns = max(0.0, min(1.0, ss * args.sat_mul))
            nv = max(0.0, min(1.0, vv * args.lift))
            nr, ng, nb = colorsys.hsv_to_rgb(nh, ns, nv)
            nr, ng, nb = nr * 255.0, ng * 255.0, nb * 255.0
            px[x, y] = (int(round(r + (nr - r) * wt)),
                        int(round(g + (ng - g) * wt)),
                        int(round(b + (nb - b) * wt)))
            hit += 1
    return hit, total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--h-lo", type=float, default=330.0, help="红圈下界（度）")
    ap.add_argument("--h-hi", type=float, default=25.0, help="红圈上界（度）")
    ap.add_argument("--sat", type=float, default=0.12, help="算作'有色'的饱和度下限")
    ap.add_argument("--soft", type=float, default=12.0, help="边缘软过渡宽度（度）")
    ap.add_argument("--target-h", type=float, default=30.0, help="目标色相（灰褐）")
    ap.add_argument("--sat-mul", type=float, default=0.55, help="目标饱和度乘数")
    ap.add_argument("--lift", type=float, default=0.78,
                    help="目标明度乘数（<1 压暗；保留 1.0 会读作水泥柱）")
    ap.add_argument("--quality", type=int, default=95, help="重编码 JPEG 质量")
    ap.add_argument("--dump", default=None)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("需要 Pillow：用托管 venv 的解释器跑，"
                         "tools/glb_tex_color.py 的文件头有说明")

    js, bin_, _ = read_glb(a.src)
    src_idx, bv_idx = base_tex_slot(js)
    if src_idx is None:
        raise SystemExit("这份 GLB 的 material[0] 没有 baseColorTexture")
    bv = js["bufferViews"][bv_idx]
    blob = bin_[bv.get("byteOffset", 0):bv.get("byteOffset", 0) + bv["byteLength"]]
    im = Image.open(io.BytesIO(blob)).convert("RGB")
    print("贴图 %dx%d，原 %d 字节（%s）" %
          (im.size[0], im.size[1], len(blob), js["images"][src_idx].get("mimeType")))

    if a.dump:
        os.makedirs(a.dump, exist_ok=True)
        im.save(os.path.join(a.dump, "before.jpg"), quality=95)

    hit, total = recolor(im, a)
    print("命中红系 %d / %d 像素（%.1f%%）；色相 ≥%.0f°或 ≤%.0f°，目标 %.0f°，饱和 ×%.2f，明度 ×%.2f"
          % (hit, total, 100.0 * hit / total, a.h_lo, a.h_hi, a.target_h, a.sat_mul, a.lift))
    if hit == 0:
        print("⚠️ 一个像素都没命中 —— 检查 --h-lo/--h-hi/--sat，别写出一个没变的文件还说成功")

    buf = io.BytesIO()
    im.save(buf, format="JPEG", quality=a.quality, subsampling=0)
    new_blob = buf.getvalue()
    print("重编码后 %d 字节（原 %d，%+.1f%%）"
          % (len(new_blob), len(blob), 100.0 * (len(new_blob) - len(blob)) / max(1, len(blob))))

    if a.dump:
        im.save(os.path.join(a.dump, "after.jpg"), quality=95)

    if a.dry_run:
        print("--dry-run：不写文件")
        return 0

    out = repack_glb(js, bin_, bv_idx, new_blob, "image/jpeg")
    open(a.dst, "wb").write(out)
    print("写出 %s（%d 字节，原 %d）" % (a.dst, len(out), os.path.getsize(a.src)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
