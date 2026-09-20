# -*- coding: utf-8 -*-
"""VolunteerArmyPC —— 用 Bing 图片搜索取参考图候选，并拼成一张带编号的对照表。

【为什么需要这条通道】
本项目取年代参考图的首选是**维基共享资源**（白底产品图，细节准），
上一轮三把武器就是这么取的（见 `tools/fetch_wpn_refs.sh` 与 `ref/wpn/SOURCES.md`）。
但维基那条路有两个死点，2026-09-20 实测全部复现：

| 通道 | 实测 |
|---|---|
| `commons.wikimedia.org` / `upload.wikimedia.org` 直连 | HTTP 000（15s 超时） |
| 维基各语言站点、`WebFetch` 抓 Category 页 | 同样 000 / fetch failed |
| `wsrv.nl` 图片代理回源 | **仍然可通**，但它是**按文件名**转发的 |
| 用 `Special:FilePath/<名字>` 猜文件名 | 猜错就 404 或 60s 挂住 |

也就是说 wsrv 只是"能转发"，并没有给我**发现文件名的能力** ——
而本轮（载具）不像武器那样有现成的名字可抄。于是要另找一条能"发现图片"的路。
实测 `cn.bing.com/images/search` 可通（200 / 0.13s），返回的 HTML 里带
`murl`（原图地址）与 `turl`（缩略图地址），这就是一条零成本、可发现、可批量
的取图通道。**注意它是"搜索引擎缓存"而非图库**：原图仍指向各自的站点，
可达性要逐个试（本脚本会把失败的记下来，不静默丢弃）。

【为什么先出对照表再选，而不是直接下第一张】
"参考图选错"的代价是把别人的错处烘进网格，而图生3D **不会再回头修**。
所以这里把 N 个候选拼成一张带编号的图，**由人（或近景取证）来挑**，
而不是让脚本按文件大小自动选第一张。挑选口径见 README 的载具章节：
① 侧视或前 3/4，能同时看清车头、负重轮、车厢；
② 主体与背景对比度够（cut_bg.py 的阈值法才吃得掉背景）；
③ 长宽比不要太极端（补方之后主体还能占满画布）。

用法：
    python tools/fetch_refs_bing.py --query "M4A3E8 Sherman tank side view" \
        --out sweep/veh_ref/tank --n 12 --sheet sweep/veh_ref/_sheet_tank.png
    python tools/fetch_refs_bing.py --jobs            # 一次跑本轮的四个目标

依赖 Pillow（拼图），用隔离 venv：
    C:/Users/<user>/.workbuddy/binaries/python/envs/default/Scripts/python.exe
"""

import argparse
import html
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")

# 本轮的四个目标：键名与 assets/art/veh/<键>.png、make_vehicle_node 的 type 一致。
# 括号里是年代与型号依据 —— 敌方是美军（角色立绘 char_enemy_rifle 实测为
# M1 钢盔 + M1 加兰德 + M1943 野战夹克），所以车队车辆取朝鲜战争美军制式。
JOBS = [
    ("veh_jeep",  "Willys MB jeep side view"),
    ("veh_apc",   "M3 half-track side view"),
    ("veh_tank",  "M4A3E8 Sherman tank side view"),
    ("veh_truck", "GMC CCKW truck side view"),
]


def search(q, n, timeout=30):
    """返回 [(原图 URL, 缩略图 URL), ...]。

    【为什么打的是 /images/async 而不是结果页】
    实测（2026-09-20）结果页 `images/search` 是**不可靠的两态**：

    | 查询 | 结果页里的 `murl` 条数 |
    |---|---|
    | `M4A3E8 Sherman tank side view` | **33 条**，全对 |
    | `M4A3E8 Sherman tank side view Korean War` | **1 条** |

    后者的 HTML 里嵌着一个模板
    `images/async?q=%QUERY%&async=content&first=%FIRST%&count=%COUNT%…`
    —— 也就是说这一态的结果是**异步加载**的，服务器只给了壳。
    危险的是它**不报错**：翻"前 10 条 murl"会拿到 1 条，于是脚本拿着
    这个空壳去下载，配上一条被地理/汽车类站点污染的结果，看起来"跑通了"
    却全是无关图（第一版就是这么把黑龙江地图当坦克下回来的）。
    所以判据不能是"HTTP 200"，而是"**murl 条数够不够**"。

    直接打 async 端点，两种查询都稳定返回 35 条：
    `images/async?q=<q>&first=1&count=35&async=content&mmasync=1`
    （`mmasync=1` 实测无关紧要，留着是照抄页面模板的参数集。）

    Bing 把每条结果塞在 `m` 属性的一段 HTML 转义 JSON 里，
    直接按 `murl&quot;:&quot;…` 抓最省事，不用去还原那层 JSON。
    """
    url = ("https://cn.bing.com/images/async?q=" + urllib.parse.quote(q)
           + "&first=1&count=40&async=content&mmasync=1")
    req = urllib.request.Request(url, headers={
        "User-Agent": UA,
        "Referer": "https://cn.bing.com/images/search?q=" + urllib.parse.quote(q),
    })
    with urllib.request.urlopen(req, timeout=timeout) as r:
        page = r.read().decode("utf-8", "replace")
    murls = [html.unescape(u) for u in re.findall(r"murl&quot;:&quot;(.*?)&quot;", page)]
    turls = [html.unescape(u) for u in re.findall(r"turl&quot;:&quot;(.*?)&quot;", page)]
    # 【判据】条数不够就是拿到了异步空壳，当场报出来，别让它伪装成"跑通了"。
    if len(murls) < 3:
        print("  !! 只解析到 %d 条 murl —— 大概率拿到的是异步空壳，结果不可信" % len(murls))
    out, seen = [], set()
    for i, u in enumerate(murls):
        if u in seen:
            continue
        seen.add(u)
        out.append((u, turls[i] if i < len(turls) else ""))
        if len(out) >= n:
            break
    return out


def download(url, dst, referer=None, timeout=25):
    """下到本地。失败不抛，返回 None —— 候选里有一半站点不可达是常态。"""
    hdr = {"User-Agent": UA}
    if referer:
        hdr["Referer"] = referer
    try:
        req = urllib.request.Request(url, headers=hdr)
        with urllib.request.urlopen(req, timeout=timeout) as r:
            data = r.read()
    except Exception as e:                                  # noqa: BLE001
        return None, str(e)[:60]
    if len(data) < 12 * 1024:         # 太小的基本是占位图/红叉
        return None, "too small (%d B)" % len(data)
    with open(dst, "wb") as f:
        f.write(data)
    return dst, "%d KB" % (len(data) // 1024)


def sheet(items, out, cols=4, cell=320):
    """带编号的对照表。编号就是目录里的下标（_c03.jpg 之类），
    方便"我挑了第 5 张"这句话有确切所指。"""
    from PIL import Image, ImageDraw
    rows = (len(items) + cols - 1) // cols
    pad, lab = 4, 20
    im = Image.new("RGB", (cols * cell, rows * (cell + lab)), (22, 22, 26))
    d = ImageDraw.Draw(im)
    for i, it in enumerate(items):
        cx, cy = (i % cols) * cell, (i // cols) * (cell + lab)
        d.text((cx + 4, cy + 4), "#%02d  %s" % (it["i"], it["note"]), fill=(255, 206, 96))
        if not it["path"]:
            continue
        try:
            t = Image.open(it["path"]).convert("RGB")
        except Exception:                                    # noqa: BLE001
            continue
        t.thumbnail((cell - 2 * pad, cell - 2 * pad), Image.LANCZOS)
        im.paste(t, (cx + pad, cy + lab + pad))
    im.save(out)
    return im.size


def run_one(key, query, out_dir, n):
    os.makedirs(out_dir, exist_ok=True)
    print("=== %s：%s ===" % (key, query))
    cands = search(query, n)
    print("  搜索得到 %d 个候选" % len(cands))
    items = []
    for i, (u, _t) in enumerate(cands):
        ext = os.path.splitext(urllib.parse.urlparse(u).path)[1].lower()
        if ext not in (".jpg", ".jpeg", ".png", ".webp"):
            ext = ".jpg"
        dst = os.path.join(out_dir, "_c%02d%s" % (i, ext))
        path, note = download(u, dst, referer=u)
        if path is None:
            note = "X " + note
        items.append({"i": i, "path": path, "note": note, "url": u})
        print("  #%02d %-22s %s" % (i, note, u[:96]))
    ok = [it for it in items if it["path"]]
    print("  下到 %d / %d" % (len(ok), len(items)))
    if ok:
        # 对照表按**目录名**命名，不按固定后缀 —— 否则连跑几条查询时
        # 后一条会把前一条的表覆盖掉，而"我挑了第 5 张"这句话就没有所指了。
        tag = os.path.basename(out_dir.rstrip("/\\"))
        sz = sheet(items, os.path.join(os.path.dirname(out_dir.rstrip("/\\")),
                                       "_sheet_%s.png" % tag))
        print("  对照表 %s" % (sz,))
    return len(ok)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--query")
    ap.add_argument("--out")
    ap.add_argument("--sheet")
    ap.add_argument("--n", type=int, default=12)
    ap.add_argument("--jobs", action="store_true", help="跑本轮的四个目标")
    a = ap.parse_args()
    if a.jobs:
        base = os.path.join(ROOT, "sweep", "veh_ref")
        for k, (key, q) in enumerate(JOBS):
            run_one(key, q, os.path.join(base, key.split("_")[-1]), a.n)
            # 【为什么要有这个间隔】连着打四个查询时实测被降级过：
            # 第一次（jeep）只解析到 1 条 murl、且唯一的下载被服务端
            # `WinError 10054` 掐掉。慢一点换稳定，反正一共就四条查询。
            if k != len(JOBS) - 1:
                time.sleep(3)
        return
    if not (a.query and a.out):
        raise SystemExit("要么给 --jobs，要么同时给 --query 与 --out")
    run_one("ref", a.query, os.path.abspath(a.out), a.n)


if __name__ == "__main__":
    sys.exit(main())
