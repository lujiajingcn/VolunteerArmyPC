# -*- coding: utf-8 -*-
"""GLB 贴图的**颜色验收** —— 把"这东西看着是红的"变成一个数。

为什么必须补这条判据
--------------------
2026-09-24 实测：`prop_pine` 上一轮把形状判据全过了一遍
（剖面锯齿度 0.2120 → 0.0268），**树干是品红色**却没人发现 ——
直到本轮美化手模时，从另一张截图的背景里看到"树是纯品红平板"。

    | 判据 | 拦的是 | 拦不住 |
    |------|--------|--------|
    | ①②（塌了/杆）        | 几何退化  | 形状对、颜色错 |
    | ③（剖面锯齿度）        | 分层像飞盘 | 形状对、颜色错 |
    | 本工具（色相占比）      | 颜色错    | —— |

根因不是引擎：把 `baseColorTexture` 导出来看，**贴图底色本身就是暗红**。
引擎侧零成本修不了 —— 红与绿在**同一张贴图**里，改 tint/材质会把树冠一起染。
所以"颜色对没对"只能在**贴图**这一层判。

判据（口径说明）
----------------
把贴图每个像素转 HSV，只统计**有色的**像素（S ≥ `--sat`，默认 0.20；
黑边/白底/灰噪点不算）：

    红系占比 = (色相落在近红圈 |h-180| 最大的那段) 的像素 / 有色像素
       近红圈 = 色相 ≥ 330° 或 ≤ 20°（覆盖品红→红→橙棕的边界）
       其中**洋红/品红**单列：色相 ≥ 300°（红+蓝同时高）

    正常树皮应该落在 **20°~45°（橙棕/黄褐）**，且红系占比低。

⚠️ 阈值是**相对**的，看两个件的高低而不是绝对数字 —— 与几何判据同一个理由：
生成器的贴图没有统一口径。可用 `--ref <已知件>` 同时量两个并列出差值。

用法：
    python tools/glb_tex_color.py <glb> [<glb> ...] [--dump <目录>]
"""
import argparse
import colorsys
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# 色相分段的边界（度）。给的是**落地语义**，不是通用色彩学 ——
# 这套边界是为了区分"树皮棕"与"生成器理解的暗红/品红"。
BANDS = [
    ("洋红/品红", 300, 330),
    ("红",        330, 360),
    ("红",          0,  20),
    ("橙棕/黄褐",  20,  45),
    ("黄/黄绿",    45,  75),
    ("绿",         75, 170),
    ("青",        170, 200),
    ("蓝",        200, 260),
    ("紫",        260, 300),
]


def read_glb(path):
    """返回 (json, bin)，只为取图片字节 —— 不解析网格。"""
    data = open(path, "rb").read()
    assert data[:4] == b"glTF", "不是 GLB：%s" % path
    pos, js, bin_ = 12, None, None
    while pos < len(data):
        ln, typ = struct.unpack("<II", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + ln]
        if typ == 0x4E4F534A:
            js = json.loads(chunk.decode("utf-8"))
        elif typ == 0x004E4942:
            bin_ = chunk
        pos += 8 + ln
    return js, bin_


def base_tex_bytes(js, bin_):
    """取 material 0 的 baseColorTexture 原始字节与 mime。"""
    bvs = js.get("bufferViews", [])
    for m in js.get("materials", []):
        ti = m.get("pbrMetallicRoughness", {}).get("baseColorTexture", {}).get("index")
        if ti is None:
            continue
        src = js["textures"][ti].get("source")
        if src is None:
            continue
        img = js["images"][src]
        bv = bvs[img["bufferView"]]
        off = bv.get("byteOffset", 0)
        return bin_[off:off + bv["byteLength"]], img.get("mimeType", "image/png")
    return None, None


def classify(path, sat_min=0.20, dump=None):
    from PIL import Image
    js, bin_ = read_glb(path)
    blob, mime = base_tex_bytes(js, bin_)
    if blob is None:
        return {"path": path, "error": "没有 baseColorTexture"}
    if dump:
        os.makedirs(dump, exist_ok=True)
        ext = ".jpg" if "jpeg" in mime else ".png"
        p = os.path.join(dump, os.path.basename(path).replace(".glb", "") + "_base" + ext)
        open(p, "wb").write(blob)
    import io
    im = Image.open(io.BytesIO(blob)).convert("RGB")
    w, h = im.size
    px = im.load()

    bands = {n: 0 for n, _, _ in BANDS}
    colored = 0
    # 同时产一张"被算成红系"的掩码图，用来回答"红在哪里" ——
    # 只报占比不报位置的话，会分不清"整块红底"与"零星被染到的缝"。
    mask_im = Image.new("RGB", (w, h), (0, 0, 0))
    mask = mask_im.load()

    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            hh, ss, vv = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
            deg = hh * 360.0
            if ss < sat_min or vv < 0.10:
                continue
            colored += 1
            for name, lo, hi in BANDS:
                if lo <= deg < hi:
                    bands[name] += 1
                    if name in ("洋红/品红", "红"):
                        mask[x, y] = (r, g, b)
                    break

    res = {"path": os.path.basename(path), "size": (w, h), "colored": colored,
           "total": w * h, "bands": bands}
    if dump:
        mask_im.save(os.path.join(dump, os.path.basename(path).replace(".glb", "") + "_redmask.png"))

    # 红系 = 洋红/品红 + 红
    red = bands["洋红/品红"] + bands["红"]
    res["red_pct"] = 100.0 * red / colored if colored else 0.0
    res["bark_pct"] = 100.0 * bands["橙棕/黄褐"] / colored if colored else 0.0
    res["green_pct"] = 100.0 * bands["绿"] / colored if colored else 0.0
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("glbs", nargs="+")
    ap.add_argument("--sat", type=float, default=0.20, help="算作'有色'的饱和度下限")
    ap.add_argument("--dump", default=None, help="把贴图与红掩码导出到这个目录")
    a = ap.parse_args()

    rows = []
    for p in a.glbs:
        if not os.path.isfile(p):
            print("跳过（不存在）：%s" % p)
            continue
        rows.append(classify(p, a.sat, a.dump))

    print()
    print("%-28s %-13s %8s %9s %9s %9s" %
          ("件", "贴图尺寸", "有色px%", "红系%", "橙棕%", "绿%"))
    print("-" * 82)
    for r in rows:
        if "error" in r:
            print("%-28s  %s" % (r["path"], r["error"]))
            continue
        w, h = r["size"]
        print("%-28s %-13s %7.1f%% %8.1f%% %8.1f%% %8.1f%%" %
              (r["path"], "%dx%d" % (w, h),
               100.0 * r["colored"] / r["total"], r["red_pct"],
               r["bark_pct"], r["green_pct"]))
    print()
    print("判读：红系% 高 = 树皮被染成了红/品红；橙棕% 是'正常树皮'该占的那段。")
    if a.dump:
        print("贴图与 redmask 已导出到 %s（掩码里非黑 = 被算作红系的像素）" % a.dump)

    # 明细
    for r in rows:
        if "error" in r:
            continue
        tot = max(1, r["colored"])
        parts = ["%s %.1f%%" % (n, 100.0 * c / tot)
                 for n, c in sorted(r["bands"].items(), key=lambda kv: -kv[1]) if c]
        print("  %-24s %s" % (r["path"], "  ".join(parts)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
