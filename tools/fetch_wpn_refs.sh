#!/usr/bin/env bash
# VolunteerArmyPC —— 取三张年代制式武器的参考图，并规范化为图生3D 的输入。
#
# 产物：assets/art/wpn/wpn_mosin.png / wpn_ppsh.png / wpn_dp27.png
#       （长边 1600、白底、单物体居中、**正方形画布**、枪口朝右）
#
# ---------------------------------------------------------------------------
# 【为什么参考图本身不入库，而是用一个脚本重建】
# 这三张是**第三方照片**（维基共享资源），版权不在本项目，而本仓库是 Public。
# 本仓库对第三方图的既有处置是「不入库、但给出可复现的取法」—— 见 .gitignore 里
# /ref/ 那一段的理由。武器参考图同理：入库的只有我们自己生成出来的 GLB，
# 以及这个把原料一条命令取回来的脚本。
# 于是新克隆的仓库里没有 assets/art/wpn/*.png，跑一次本脚本就有了。
#
# 【来源】文件名照抄维基共享资源，许可与作者见对应文件页
#   （本机 commons.wikimedia.org 直连不通，无法在构建期核验许可文本）
#
#   | 产物 | 维基文件名 | 说明 |
#   |---|---|---|
#   | wpn_mosin.png | Mosin nagant m9130 from cia.jpeg | 莫辛-纳甘 M91/30，白底产品图，无瞄镜 |
#   | wpn_ppsh.png  | PPSh-41 from soviet.jpg           | 波波沙-41，白底产品图，71 发弹鼓 |
#   | wpn_dp27.png  | Machine gun DP MON.jpg            | DP-27，博物馆实拍（石地板），需抠底 |
#
# 【为什么走 wsrv.nl 代理】本机 commons.wikimedia.org / upload.wikimedia.org
# 直连是 HTTP 000 超时（10~45 秒零字节），而 wsrv.nl 转发**图片**可通
# （转发返回 JSON 的 API 不行，会报 Invalid or unsupported image format）。
# 维基的 Special:FilePath/<文件名> 可以按文件名直接取图，不必自己算 md5 路径。
#
# 【为什么统一 w=2400】DP-27 那张的残留投影是用 tools/cut_bg.py --erase 定点擦掉的，
# 而 --erase 吃的是**原图像素坐标** —— 换一个下载宽度，那两个矩形就对不上了，
# 会变成"擦掉一块枪身"。所以这个宽度是**契约**，不是随手选的。
#
# 用法：
#     tools/fetch_wpn_refs.sh
# ---------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 【必须转成 Windows 路径】本脚本是 bash，拿到的是 MSYS 形式（/e/study/...），
# 而 cut_bg.py 由**原生 Windows Python** 执行 —— 它会把 "/e/study/..." 当成
# 当前盘符下的相对路径，报 "can't open file 'E:\e\study\...'"。
# capture.sh 里已经踩过同一个坑（Godot 的 --path 同理）。
WIN_ROOT="$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")"
RAW="$ROOT/sweep/wpn_ref"
OUT="$ROOT/assets/art/wpn"
WIN_RAW="$WIN_ROOT\\sweep\\wpn_ref"
WIN_OUT="$WIN_ROOT\\assets\\art\\wpn"

# 瘦身/图像工具（Pillow）装在隔离 venv 里，与 slim_glb.py 共用。
PY="C:/Users/lujiajing/.workbuddy/binaries/python/envs/default/Scripts/python.exe"

[ -x "$PY" ] || { echo "找不到带 Pillow 的隔离 venv：$PY"; exit 1; }
mkdir -p "$RAW" "$OUT"

# 已经下过就不重复下（24 秒超时 × 3 不值得每次都付）
fetch() {
    local wiki="$1" dst="$2"
    if [ -s "$dst" ]; then
        echo "已存在 $(basename "$dst")，跳过下载"
        return 0
    fi
    echo "下载 $(basename "$dst") …"
    curl -fsSL -m 180 -o "$dst" \
        "https://wsrv.nl/?url=commons.wikimedia.org/wiki/Special:FilePath/$wiki&w=2400&output=png"
}

fetch "Mosin%20nagant%20m9130%20from%20cia.jpeg" "$RAW/mosin_b.png"
fetch "PPSh-41%20from%20soviet.jpg"             "$RAW/ppsh_b.png"
fetch "Machine%20gun%20DP%20MON.jpg"            "$RAW/dp_test.png"

# ---- 规范化 ----
# 白底图（莫辛 / 波波沙）：只裁到内容外框 + 补白边，不做抠底。
# --square：把 3.7:1 的侧视图补成方图，防对端把输入中心裁剪成正方形而切掉枪的首尾。
"$PY" "$WIN_ROOT\\tools\\cut_bg.py" "$WIN_RAW\\mosin_b.png" "$WIN_OUT\\wpn_mosin.png" --side 1600 --square
"$PY" "$WIN_ROOT\\tools\\cut_bg.py" "$WIN_RAW\\ppsh_b.png"  "$WIN_OUT\\wpn_ppsh.png"  --side 1600 --square

# DP-27：博物馆石地板实拍，必须抠底。
#   --thresh 110 是实测出来的分界：石地板亮度 150~200、地面投影 130~170、枪身 20~90，
#                110 能同时排掉地板与投影，又保住两脚架那几根细腿（阈值降到 75 会把腿啃碎）。
#   --erase      阈值治不掉的那块投影残留：投影与枪身同为中性灰，靠颜色分不开，
#                只能定点擦。坐标用**原图**（2400×981）像素，且刻意避开 x 856~883
#                那根深色竖件 —— 第一版把矩形按成品坐标给，结果把枪身劈掉了一块。
"$PY" "$WIN_ROOT\\tools\\cut_bg.py" "$WIN_RAW\\dp_test.png" "$WIN_OUT\\wpn_dp27.png" --thresh 110 --side 1600 --square \
    --erase 580,470,850,645 --erase 890,470,1120,645

echo
echo "完成。图生3D 的输入在 $OUT"
ls -la "$OUT"/wpn_*.png
