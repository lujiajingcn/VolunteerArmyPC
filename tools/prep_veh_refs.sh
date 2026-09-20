#!/usr/bin/env bash
# VolunteerArmyPC —— 取四辆车的参考图，并规范化为图生3D 的输入。
#
# 产物：assets/art/veh/veh_jeep.png / veh_apc.png / veh_tank.png / veh_truck.png
#       （长边 1600、白底、单物体居中、**正方形画布**、**车头朝右**）
#
# ---------------------------------------------------------------------------
# 【为什么这一轮换了取图通道：维基整站不通了】
# 上一轮三把武器走的是维基共享资源 + wsrv.nl 图片代理（见 tools/fetch_wpn_refs.sh）。
# 2026-09-20 实测维基那条路断了，而且是"断了半截"——最容易误判成可用的那种：
#
#   | 通道 | 实测 |
#   |---|---|
#   | commons.wikimedia.org / upload.wikimedia.org 直连 | HTTP 000（15s 超时） |
#   | 维基各语言站点、WebFetch 抓 Category 页 | 000 / fetch failed |
#   | wsrv.nl 回源 | **仍然可通**（拿已知文件名试是 200 / 3.2s） |
#   | 用 Special:FilePath/<名字> 猜文件名 | 猜错就 404 或 60s 挂住 |
#
# 也就是说 wsrv 只是"能转发"，并没有给我**发现文件名的能力**。武器那批有现成的
# 名字可抄（`ref/wpn/SOURCES.md` 记着），载具没有。于是改用
# **Bing 图片搜索**（cn.bing.com 实测 200 / 0.13s）来"发现图片"：
#     python tools/fetch_refs_bing.py --jobs        # 出候选 + 带编号的对照表
# 挑选口径（见下"选图口径"），挑完把中选的候选另存到 sweep/veh_ref/pick/。
#
# 【为什么不入库这四张参考图】
# 本仓库是 Public。`/ref/` 那条 gitignore 已经写明了既有处置原则：
# 「第三方图版权不在本项目，不能随本仓库分发，需要时按脚本自行重建」。
# 上一轮三把武器是**例外**：它们是维基共享资源的 CC 授权图，附了作者与许可。
# 本轮四张里有两张是**商业素材站的 CG 商品图**（renderhub / TurboSquid 的产品缩略图）、
# 一张是商品摄影（模型套件的商品页图），只有吉普那张是博物馆实拍。
# 授权状况与武器那批不同，所以按 /ref/ 的处置：**不入库**，
# 只把"取法 + 选了哪一张 + 为什么这么裁"写在本脚本里（本脚本入库）。
# 成品 GLB（assets/art/veh/model/*.glb）是我们自己生成的，照常入库。
#
# 【来源】Bing 图片搜索，查询词与中选候选如下
#   | 产物 | 查询词 | 中选 | 来源站点 | 说明 |
#   |---|---|---|---|---|
#   | veh_jeep  | Willys MB jeep side view                          | #00 | motorcarclassics.com | **左侧视**白棚实拍，1918×1280，带地面反光 |
#   | veh_apc   | M3 half-track armored personnel carrier white background | #07 | oldboyhobby.com | 白底商品照，1000×1000，3/4 前视 |
#   | veh_tank  | M4A3E8 Sherman tank 3d model white background     | #09 | cdn.renderhub.com    | 白底 CG 渲染，3840×2160，3/4 前视 |
#   | veh_truck | GMC CCKW truck side view                          | #01 | p.turbosquid.com     | 白底 CG 渲染，**纯侧视** |
#   （#NN 是 fetch_refs_bing.py 对照表里的编号，换一次搜索顺序会变 ——
#     所以脚本优先吃本地已挑好的 raw_*.jpg，没有才去下 URL。）
#
# 【选图口径】四条，按优先级：
#   ① 白底或纯色底 —— cut_bg.py 只有亮度阈值，草地/树林背景吃不掉；
#   ② 避开图库水印（alamy / dreamstime / shutterstock 的预览图都带水印，
#      而水印会被图生3D 当成车身上的涂装烘进贴图，**建好之后不再有机会修**）；
#   ③ 侧视或 3/4 视，能同时看清车头、负重轮、车厢；
#   ④ 分辨率 ≥ 1000 px（生成侧还会缩到 1024 级，低于这个数就是白给）。
#
# 【车头一律朝右：为什么必须统一】
# 引擎里载具节点的局部 **+X 就是车头** —— 依据是两处：
#   · `make_vehicle_node` 用 `Vector3(L, hull_h, W)` 建车体，长度沿 X；
#     坦克炮管也摆在 +X（x=0.95）。
#   · `world_sim` 用 `set_rotation(Vector3(0, -v.angle, 0))` 摆向；而 sim 里
#     `vehicle_blocked` 按 `lx = dx·cos(-angle) - dy·sin(-angle)` 把世界坐标
#     转进车体，angle=π（车队自东向西）时 +lx 指向西 —— 与局部 +X 同向。
# 而图生3D 把"图像的哪个方向"映到"模型的哪个轴"是服务端定的，**只能实测**。
# 武器那轮的实测结论是"图像右 = +X"（三张参考图枪口全朝右，成品枪口也全在 +X）。
# 所以四张参考图统一成车头朝右，**四辆车就能共用一个朝向校正角** ——
# 这比"逐辆试角度"省掉三轮提交（每一轮都是钱）。
# 原始候选里吉普本来就朝右、另外三张朝左，所以脚本对那三张做一次水平镜像。
# 镜像是对合变换，不引入重采样误差（PIL 的 FLIP_LEFT_RIGHT 只是改行序）。
#
# 【为什么必须 --square】
# 图生3D 那侧会把输入**按中心裁成正方形**。车辆侧视图长宽比约 1.5:1 ~ 2:1，
# 不补方就会被切掉车头或车尾 —— 而且**模型照样生成成功、文件非空**，
# 只是成品缺头少尾。"文件非空 ≠ 任务成功"这一条在武器那轮已经吃过一次。
#
# 【每个 --thresh 的来历】cut_bg.py 的语义是"亮度 < thresh 的算物体"：
#   jeep  170 —— 白墙 ~240、水泥地 ~200、地面反光 ~170~220，车身 60~110。
#                取 170 刚好把反光切掉又保住白色星标与车门上的白字
#                （那两块被深色车体包着，灌水到不了，不会被吃掉）。
#   apc/  200 —— 三张都是纯白底（商品图 / CG 渲染），主体 40~120、
#   tank        软化阴影 ~210~240。200 排掉阴影、保住主体。
#   truck
#
# 用法：
#     tools/prep_veh_refs.sh
# ---------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 【必须转成 Windows 路径】本脚本是 bash，拿到的是 MSYS 形式（/e/study/...），
# 而 cut_bg.py 由**原生 Windows Python** 执行 —— 它会把 "/e/study/..." 当成
# 当前盘符下的相对路径，报 "can't open file 'E:\e\study\...'"。
WIN_ROOT="$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")"
PICK="$ROOT/sweep/veh_ref/pick"
OUT="$ROOT/assets/art/veh"
WIN_PICK="$WIN_ROOT\\sweep\\veh_ref\\pick"
WIN_OUT="$WIN_ROOT\\assets\\art\\veh"

PY="C:/Users/lujiajing/.workbuddy/binaries/python/envs/default/Scripts/python.exe"
[ -x "$PY" ] || { echo "找不到带 Pillow 的隔离 venv：$PY"; exit 1; }
mkdir -p "$PICK" "$OUT"

# 中选候选的原始地址。**注意 CDN 地址会轮换**（TurboSquid / renderhub 尤其），
# 所以这只是"首选路径"而不是契约；失效时重跑 fetch_refs_bing.py 按上面的表重挑。
# 本地已经有 raw_*.jpg 就完全不碰网络。
declare -A URL=(
  [jeep]="https://www.motorcarclassics.com/galleria_images/22/22_p8_l.jpg"
  [apc]="http://www.oldboyhobby.com/cdn/shop/files/a36b760d6704b42ab94c11406667881f.jpg?v=1768873893"
  [tank]="https://cdn.renderhub.com/3dstudio/m4-sherman-tank/m4-sherman-tank-01.jpg"
  [truck]="https://p.turbosquid.com/ts-thumb/GS/h07cAt/c8NfkFXg/gmc_cckw_angle_04/jpg/1511376331/1920x1080/fit_q87/01.jpg"
)
# 原始候选里本来就朝右的（不需要镜像）
FACES_RIGHT="jeep"

for k in jeep apc tank truck; do
    f="$PICK/raw_$k.jpg"
    if [ ! -s "$f" ]; then
        echo "下载 raw_$k.jpg …"
        curl -fsSL -m 120 -A "Mozilla/5.0" -o "$f" "${URL[$k]}" || {
            echo "  ↓ 下载失败。重跑：python tools/fetch_refs_bing.py --query \"<上表的查询词>\" --out sweep/veh_ref/re_$k"
            exit 1
        }
    else
        echo "已存在 raw_$k.jpg（$(stat -c%s "$f") B），跳过下载"
    fi
done

# ---- 统一车头朝右 + 抠底补方 ----
for k in jeep apc tank truck; do
    if [ "$k" != "$FACES_RIGHT" ]; then
        "$PY" -c "from PIL import Image; p=r'$WIN_PICK\\raw_$k.jpg'; Image.open(p).transpose(Image.FLIP_LEFT_RIGHT).save(p)"
    fi
done

"$PY" "$WIN_ROOT\\tools\\cut_bg.py" "$WIN_PICK\\raw_jeep.jpg"  "$WIN_OUT\\veh_jeep.png"  --thresh 170 --side 1600 --square
"$PY" "$WIN_ROOT\\tools\\cut_bg.py" "$WIN_PICK\\raw_apc.jpg"   "$WIN_OUT\\veh_apc.png"   --thresh 200 --side 1600 --square
"$PY" "$WIN_ROOT\\tools\\cut_bg.py" "$WIN_PICK\\raw_tank.jpg"  "$WIN_OUT\\veh_tank.png"  --thresh 200 --side 1600 --square
"$PY" "$WIN_ROOT\\tools\\cut_bg.py" "$WIN_PICK\\raw_truck.jpg" "$WIN_OUT\\veh_truck.png" --thresh 200 --side 1600 --square

echo
echo "完成。图生3D 的输入在 $OUT"
ls -la "$OUT"/veh_*.png
echo
echo "下一步：python tools/gen3d_batch.py --kind veh --jobs 2 <键...>   （token 走 stdin）"
