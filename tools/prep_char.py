# -*- coding: utf-8 -*-
"""VolunteerArmyPC —— 人物立绘预处理：去水印 + 规范命名 + 自动裁头像

这一步做三件事：
  1. 切掉生成器右下角的 "AI生成 WORKBUDDY" 水印（半透明，亮度阈值按最坏情况定）；
  2. 把 11 个角色按职务重命名成 char_*.png，供三维生成与归档使用；
  3. 自动定位每个人的头部，裁出「头 + 肩」胸像，供任务简报的小队名册使用。

为什么头像必须**自动定位**而不是写死一个框：
  这 11 张里人物的水平位置各不相同 —— 有的正面站定、有的半侧身让出爆破筒、
  步枪手还把枪举到了画面右上角。写死居中框的结果是把半边脸切掉。

【两版检测器的教训 —— 这一节比代码重要】
第一版按「该像素与背景估计的差」判前景，背景用**该行左右两条边缘**的亮度
线性插值得到。结果 11 张全部报 `头2`，也就是头顶被定位到画面最上面一行 ——
成因是生成图在四角有暗角（vignette）：左右边缘比画面正中暗 20~40，
于是"中间那片更亮的背景"整段被判成了前景，"最长连续前景段"退化成整行。
同一版里脚底检测也被骗：它永远返回扫描区的最后一行。

所以第二版换了一个**不依赖背景建模**的信号：行内最暗像素。
人物（棉衣的褶皱阴影、靴子、装备）远比背景暗，背景的暗角再暗也到不了那个程度。
取画面顶部若干行（那里必然是纯背景）的行内最暗值当中位数当基准，
往下第一行明显低于基准的位置就是头顶。这个判据对水平渐变、暗角、
中心光池统统免疫，因为它根本不去猜背景长什么样。
"""

import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from probe_png import read_png  # noqa: E402

RAW_DIR = os.path.join(ROOT, "assets", "art", "char", "raw")
FULL_DIR = os.path.join(ROOT, "assets", "art", "char")
PORT_DIR = os.path.join(ROOT, "assets", "art", "char", "portrait")

# 源文件名前缀 -> 目标名 + 中文职务（中文职务只用于打印核对）
RENAME = [
    ("1951年朝鲜战场中国人民志愿军指挥员",   "char_leader",         "队长"),
    ("1951年朝鲜战场中国人民志愿军步枪手",   "char_rifleman",       "步枪手"),
    ("1951年朝鲜战场中国人民志愿军轻机枪手", "char_mg",             "机枪手"),
    ("1951年朝鲜战场中国人民志愿军狙击手",   "char_sniper",         "狙击手"),
    ("1951年朝鲜战场中国人民志愿军反坦克手", "char_at",             "反坦克手"),
    ("1951年朝鲜战场中国人民志愿军工兵爆破手", "char_demo",         "爆破手"),
    ("1951年朝鲜战场中国人民志愿军医疗兵",   "char_medic",          "医疗兵"),
    ("1951年朝鲜战场中国人民志愿军弹药兵",   "char_ammo",           "弹药支援"),
    ("1951年朝鲜战场联合国军_美军_步兵",     "char_enemy_rifle",    "敌步兵"),
    ("1951年朝鲜战场联合国军_美军_轻机枪手", "char_enemy_mg",       "敌机枪手"),
    ("1951年朝鲜战场联合国军_美军_前线军官", "char_enemy_officer",  "敌军官"),
]

# 水印亮度阈值。沿用 prep_art.py 的结论：水印是**半透明**的，
# 实际亮度 = 文字不透明度 × 底色，压在亮底上会掉到 110 附近。
WM_THR = 110.0

# 头顶判据：行内最暗像素比背景基准低多少才算"人物开始了"。
# 取 18 是因为实测人物褶皱阴影比背景暗 30~60，而背景自身的行间起伏 < 8，
# 留出两倍余量。
TOP_DROP = 18.0

# 头部判定：最长连续"暗于阈值"段至少要占画面宽度这个比例，否则认为没找到头。
#
# 【为什么从 0.085 一路降到 0.025】最初取 0.085（87px）是想顺手排除步枪枪管，
# 但这个门槛把真实的人头也一起排掉了：这组图的轮廓光会把人像边缘照亮，
# "连续暗区"只覆盖人物暗核，实测 11 个角色的暗核只有 31~156px 宽 ——
# 狙击手 85、敌机枪手 35、敌军官 31，全部卡在 87 之下被判"未定位到头部"。
# 排除枪管靠 start_run(0.02w) 就够了（枪管约 0.01w），这里只需挡住退化解。
HEAD_RUN_FRAC = 0.025

# 暗核宽度 -> 真实头宽的补偿系数。
# 轮廓光的勾边不参与"暗区"，所以测到的宽度只是**下界**：实测 99~156，
# 而按"身高 82% × 1/7.3"反算的真实头宽约 142，反推补偿约 1.35 倍。
# 不补偿的后果是胸像框太窄 —— 裁出来的不是"头+肩"而是一张紧贴脸的大头照。
DARK_CORE_COMP = 1.35

# 胸像（头 + 肩）尺寸，以**补偿后的头宽**为单位。
PORT_W_K = 2.20
PORT_H_K = 2.90
# 头顶往上留一点空，不然帽子会顶到画框
PORT_TOP_K = 0.18

PORT_PX_W = 256
PORT_PX_H = 332


def write_png(path, w, h, ch, rows):
    """按输入的实际通道数写回，不要固定 RGB。

    生成器输出的是 RGBA（ct=6, ch=4），如果这里按 RGB（ct=2）声明却写 4 通道
    的字节流，PNG 解码器会把 alpha 当成下一个像素的红色 —— 画面整体错位成斜条纹。
    """
    ct = {1: 0, 3: 2, 4: 6}[ch]

    def chunk(typ, data):
        c = struct.pack(">I", len(data)) + typ + data
        return c + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ct, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(raw, 6))
    out += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(out)


def _lum(r, i):
    return 0.2126 * r[i] + 0.7152 * r[i + 1] + 0.0722 * r[i + 2]


def row_min_lum(rows, ch, w, y, x0f=0.15, x1f=0.85):
    """该行中央带内最暗像素的亮度。

    只在中央 70% 找，是为了避开左右两侧可能的暗角 —— 暗角是背景的一部分，
    把它算进来的话基准会被拉低，人物反而"不够暗"了。
    """
    best = 999.0
    r = rows[y]
    for x in range(int(w * x0f), int(w * x1f)):
        v = _lum(r, x * ch)
        if v < best:
            best = v
    return best


def row_stat(rows, ch, w, y, q=0.05, x0f=0.15, x1f=0.85):
    """该行中央带内亮度的 q 分位数（64 桶直方图）。

    【为什么是分位数而不是最小值】这是整套检测器的关键。这 11 张图有很重的
    胶片颗粒，实测队长那张的**行内最小值**被单个噪点拉到 2.6，而人物真实的暗部
    才 19 —— 拿 2.6 去推算"人物核心亮度"，阈值会低到人物自己都跨不过去，
    11 张里 6 张报"未定位到头部"，且失败的正是背景亮、对比低的那几张。
    分位数把噪点挡在外面：同一行 y=74，最小值 19.4、5% 分位 32.0。

    只在中央 70% 取，是为了避开左右两侧的暗角 —— 暗角是背景的一部分，
    算进来会把基准拉低，人物反而"不够暗"。
    """
    hist = [0] * 64
    n = 0
    r = rows[y]
    for x in range(int(w * x0f), int(w * x1f)):
        b = int(_lum(r, x * ch)) >> 2
        if b > 63:
            b = 63
        hist[b] += 1
        n += 1
    need = max(1, int(n * q))
    acc = 0
    for b in range(64):
        acc += hist[b]
        if acc >= need:
            return b * 4.0
    return 252.0


def bg_floor(rows, ch, w):
    """背景亮度基准：画面顶部若干行分位数的中位数。

    顶部必然是纯背景（人物的头顶不会顶到画框边缘）。
    实测这 11 张的背景基准落在 28~47 —— 背景亮度本身就有 1.7 倍浮动，
    这正是不该用固定落差、而要用"背景与人物之间的相对位置"定阈值的原因。
    """
    vals = sorted(row_stat(rows, ch, w, y) for y in range(6, 34))
    return vals[len(vals) // 2]


def subject_floor(rows, ch, w, h):
    """人物核心亮度：躯干区间各行的分位数，再取其中较低的 5%。

    两级稳健：行内先取分位数（挡颗粒），再对"很多行"取分位数（挡个别异常行，
    比如恰好被轮廓光扫亮的一行）。
    """
    vals = sorted(row_stat(rows, ch, w, y) for y in range(int(h * 0.15), int(h * 0.85), 3))
    return vals[max(0, len(vals) // 20)]


def longest_dark_run(rows, ch, w, y, thr):
    """该行最长的连续"暗于 thr"段（长度, 起点）。"""
    best = 0
    best_x = 0
    cur = 0
    cur_x = 0
    r = rows[y]
    for x in range(w):
        if _lum(r, x * ch) < thr:
            if cur == 0:
                cur_x = x
            cur += 1
            if cur > best:
                best = cur
                best_x = cur_x
        else:
            cur = 0
    return best, best_x


def find_head(rows, ch, w, h):
    """返回 (头顶行, 头部水平中心, 头宽)。找不到返回 None。"""
    ref = bg_floor(rows, ch, w)
    subj = subject_floor(rows, ch, w, h)
    # 阈值取"背景"与"人物核心"的中点，而不是"背景减一个固定值"。
    #
    # 【第一版为什么全军覆没】原式 thr = ref - 18 是绝对落差。队长的背景基准
    # 37.5、人物只压得到 19，落差才 18.5 —— 阈值恰好卡在人物自身的暗度上，
    # "连续暗于阈值的像素"只剩 1~2 个，头宽测出来远低于门槛，直接判失败。
    # 而它失败的正是背景亮、对比低的那几张。取中点则自动适配：
    # 队长 thr≈30.8、狙击手 thr≈14.6，两者都能切出完整的暗区。
    thr = ref - max(6.0, (ref - subj) * 0.5)

    # 找头顶用的连续段门槛放宽到 0.02w：棉帽的顶是尖的，一开始只有二三十像素宽；
    # 用头宽门槛（0.085w）去找头顶，会把顶点一直压到帽子中段，胸像就切掉帽顶。
    # 0.02w 仍能排除步枪枪管（实测约 0.01w）。
    start_run = max(5, int(w * 0.020))
    head_run = max(6, int(w * HEAD_RUN_FRAC))
    y_lim = int(h * 0.30)

    head_top = -1
    for y in range(2, y_lim):
        if row_stat(rows, ch, w, y) >= thr:
            continue
        run, _x0 = longest_dark_run(rows, ch, w, y, thr)
        if run >= start_run:
            head_top = y
            break
    if head_top < 0:
        return None

    head_cx = 0
    head_w = 0
    for y in range(head_top, min(head_top + int(h * 0.09), y_lim)):
        run, x0 = longest_dark_run(rows, ch, w, y, thr)
        if run > head_w:
            head_w = run
            head_cx = x0 + run // 2
    if head_w < head_run:
        return None
    return head_top, head_cx, head_w


def watermark_bbox(rows, ch, w, h):
    """右下角水印的包围盒。扫描区按实测落点收紧（沿用 prep_art.py 的经验：
    区域开大会把被照亮的地面误认成水印）。"""
    x0, y0 = int(w * 0.80), int(h * 0.90)
    bx0, by0, bx1, by1 = w, h, -1, -1
    for y in range(y0, h):
        r = rows[y]
        for x in range(x0, w):
            if _lum(r, x * ch) > WM_THR:
                if x < bx0:
                    bx0 = x
                if x > bx1:
                    bx1 = x
                if y < by0:
                    by0 = y
                if y > by1:
                    by1 = y
    if bx1 < 0:
        return None
    return (bx0, by0, bx1, by1)


def crop_resize(rows, ch, x0, y0, x1, y1, pw, ph):
    """矩形区域盒式平均降采样到 pw x ph。

    盒式平均（而不是最近邻）在这里是必须的：胸像要从约 300x400 降到 256x332，
    最近邻会保留一圈高频噪点，缩略图里人物的轮廓光会碎成断续的白斑。
    """
    sw = max(x1 - x0, 1)
    sh = max(y1 - y0, 1)
    out = []
    for py in range(ph):
        ya = y0 + sh * py // ph
        yb = max(ya + 1, y0 + sh * (py + 1) // ph)
        line = bytearray(pw * ch)
        for px in range(pw):
            xa = x0 + sw * px // pw
            xb = max(xa + 1, x0 + sw * (px + 1) // pw)
            n = 0
            acc = [0] * ch
            for y in range(ya, yb):
                r = rows[y]
                for x in range(xa, xb):
                    i = x * ch
                    for c in range(ch):
                        acc[c] += r[i + c]
                    n += 1
            o = px * ch
            for c in range(ch):
                line[o + c] = acc[c] // max(n, 1)
        out.append(bytes(line))
    return pw, ph, out


def find_raw(prefix):
    for f in sorted(os.listdir(RAW_DIR)):
        if f.startswith(prefix) and f.lower().endswith(".png"):
            return f
    return None


def main():
    for d in (FULL_DIR, PORT_DIR):
        if not os.path.isdir(d):
            os.makedirs(d)

    print("%-20s %-6s %-22s %-24s %s" % ("目标", "职务", "水印包围盒", "头顶/头心/头宽", "结论"))
    ok = 0
    for prefix, name, cn in RENAME:
        src = find_raw(prefix)
        if src is None:
            print("%-20s %-6s !! 缺源文件（前缀 %s）" % (name, cn, prefix))
            continue
        w, h, ch, rows = read_png(os.path.join(RAW_DIR, src))

        wb = watermark_bbox(rows, ch, w, h)
        head = find_head(rows, ch, w, h)

        # 整身图：裁到水印上方。用**水印位置**而不是"脚底"来定裁切线 ——
        # 脚底检测要先建背景模型，而背景模型正是上一版被骗的地方；
        # 水印位置是亮度阈值直接量出来的，确定得多。
        # 人物实测占到 0.945h（约 1452 行），裁到水印顶上方仍有富余。
        cut = h
        if wb is not None:
            cut = wb[1] - 6
        cut = max(cut, int(h * 0.80))
        cut = min(cut, h)

        note = "OK"
        if head is None:
            note = "!! 未定位到头部"
        elif wb is None:
            note = "!! 未检出（可能没水印）"
        elif wb[1] < cut:
            note = "!! 水印仍在画面内"

        write_png(os.path.join(FULL_DIR, name + ".png"), w, cut, ch, rows[:cut])

        # 胸像
        if head is not None:
            htop, hcx, hw = head
            hw = int(hw * DARK_CORE_COMP)          # 暗核 -> 真实头宽，见常量说明
            pw = int(hw * PORT_W_K)
            ph = int(hw * PORT_H_K)
            x0 = int(hcx - pw * 0.5)
            y0 = int(htop - hw * PORT_TOP_K)
            x1 = x0 + pw
            y1 = y0 + ph
            # 夹到画幅内（宁可挪框也不缩框，缩框会把头挤小）
            if x0 < 0:
                x1 -= x0; x0 = 0
            if x1 > w:
                x0 -= (x1 - w); x1 = w
            if y0 < 0:
                y1 -= y0; y0 = 0
            if y1 > cut:
                y0 -= (y1 - cut); y1 = cut
            x0 = max(0, x0); y0 = max(0, y0)
            nw, nh, prows = crop_resize(rows, ch, x0, y0, x1, y1,
                                        PORT_PX_W, PORT_PX_H)
            write_png(os.path.join(PORT_DIR, name + ".png"), nw, nh, ch, prows)

        box = "无" if wb is None else "%d,%d-%d,%d" % wb
        hp = "头%d 心%d 宽%d" % (head[0] if head else -1,
                                 head[1] if head else -1,
                                 head[2] if head else -1)
        print("%-20s %-6s %-22s %-24s %s" % (name, cn, box, hp, note))
        ok += 1

    print("\n完成 %d 张" % ok)
    print("  整身 -> %s" % FULL_DIR)
    print("  胸像 -> %s" % PORT_DIR)
    return 0


if __name__ == "__main__":
    sys.exit(main())
