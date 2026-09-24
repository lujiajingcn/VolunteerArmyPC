# -*- coding: utf-8 -*-
"""VolunteerArmyPC —— 批量图生3D：立绘/参考图 → assets/art/<档位>/model/<键>.glb

【两档：角色（char）与武器（wpn）】
    角色：输入 assets/art/char/<键>.png      → 成品 assets/art/char/model/<键>.glb
    武器：输入 assets/art/wpn/<键>.png       → 成品 assets/art/wpn/model/<键>.glb
             中间产物 sweep/gen3d_wpn/<键>.json
    用 `--kind wpn` 切换（默认 char）。**中间产物目录分家**是刻意的：
    两档的键名不同、任务 id 不同，混在一个目录里之后"这个 json 是哪批的"只能靠脑子记。
    键名一律带前缀（wpn_ / char_），所以就算目录合并也不会撞名 —— 分家是为了可读，不是防重名。

【三档：载具（veh），2026-09-20 加】
    载具：输入 assets/art/veh/<键>.png       → 成品 assets/art/veh/model/<键>.glb
             中间产物 sweep/gen3d_veh/<键>.json
    `--kind veh`。产物**不走 Godot 导入器**（与武器一样由 scene_builder 的
    load_glb_root() 在运行期解析），所以不会像角色那样在 model/ 旁边冒出
    "_texture_pbr_*.jpg"；.gitignore 里武器那条同理先摆着。
    参考图来源见 ref/veh/SOURCES.md（Bing 图片搜索，非维基 —— 维基本轮整站不通）。

【四档：第一人称手模（vm），2026-09-21 加】
    手模：输入 assets/art/vm/<键>.png       → 成品 assets/art/vm/model/<键>.glb
             中间产物 sweep/gen3d_vm/<键>.json
    `--kind vm`。键名 vm_hand_r（扳机手/握把）、vm_hand_l（支撑手/护木），
    与 viewmodel.cpp 里 hand_r / hand_l 两组一一对应。
    **不属于任何"角色/载具"档位**是有意的：它既不站在战场上、也不像武器那样
    被整个场景共用，而是挂在相机上、离眼睛 0.3~0.45 m 的一小块 ——
    面数/贴图的取舍与它们完全不同（近距离看，贴图吃满屏）。

【五档：战场地物（prop），2026-09-22 加】
    地物：输入 assets/art/prop/<键>.png     → 成品 assets/art/prop/model/<键>.glb
             中间产物 sweep/gen3d_prop/<键>.json
    `--kind prop`。岩石 ×2 / 松树 / 灌木丛 —— 换掉的是场上**数量最多**的那三类
    程序化图元（每关约 30 块石头、20 棵树、15 丛灌木）。
    与武器/载具一样由 scene_builder 的 load_glb_root() 在运行期解析，
    所以 model/ 旁边不会冒出 Godot 抽出来的 "_texture_pbr_*.jpg"。
    参考图来源见 ref/prop/SOURCES.md（Bing 图片搜索的**白底商品照**，非维基）。

【为什么要有这一层，而不是在 shell 里 for 循环】
一次图生3D 是「提交 → 轮询 1~5 分钟 → 拿到 URL → 下载 42MB → 瘦身到 1.5MB」，
四个环节各有各的失败方式（提交被限流、轮询超时、COS 链接断流、GLB 里 BIN 块没取到）。
放进 shell 里意味着任何一个环节失败都只有一个 exit code，事后不知道卡在哪。
这里把每一步的结果都落到 `sweep/gen3d/<键>.*`，失败也能从中断处单点重跑。

【并发而不是串行】串行 11 个要 20~50 分钟，而且这期间没有任何产出。
但服务端对 hy-3d 维度**只放 2 个并发槽位**，超了直接 429 拒绝，
所以并发固定按 2 来（--jobs 可调，调大只会让多出来的任务去撞 429 然后退避等待，
并不会更快）。遇到 429 会自动退避重试 —— 槽位是会自己空出来的，
而"面数超限 / 图片过大"那类不可等，所以只对 429 重试，其余立刻报错退出。

【为什么要去水印后再喂】输入用 `assets/art/char/<键>.png`（tools/prep_char.py 的产物），
**不是** `assets/art/char/raw/` 里的原始立绘 —— 原始图上有半透明水印，
图生3D 会把它当成服装上的图案烘进贴图，而建好的模型不会再走一遍去水印，错就错到底了。

【三个后端，靠 VA_GEN3D_BACKEND 切换】
    builtin（默认）: tools/gen3d.py     → 内置多模态通道，**限 5 次提交/天且当天不重置**
    tc            : tools/gen3d_tc.py  → 腾讯云混元生3D 官方 API 直连（CAM 签名，要 SecretId/SecretKey）
    hy            : tools/gen3d_hy.py  → 混元生3D（TokenHub），**只要一把 sk- 开头的 API Key**（Bearer），
                                        默认 3 并发、无每日提交限制 —— 目前门槛最低的一条路
    三者**结果 JSON 格式相同**，所以下面的下载 / 瘦身 / 续跑逻辑完全共用。
    tc / hy 都不需要 stdin 的 token（凭据由各自脚本读环境变量或 ~/.workbuddy/ 下的文件）；
    并发可以按官方的 3 来（builtin 只有 2）。

用法：
    echo -n "<token>" | python tools/gen3d_batch.py [--jobs 4] [键 ...]      # builtin
    python tools/gen3d_batch.py --jobs 3 [键 ...]                            # VA_GEN3D_BACKEND=tc
    python tools/gen3d_batch.py --jobs 3 [键 ...]                            # VA_GEN3D_BACKEND=hy
    python tools/gen3d_batch.py --kind wpn --jobs 3 wpn_mosin                # 武器档位

    不给键就做该档位全部；已存在 <键>.glb 且非空的**默认跳过**（--force 覆盖），
    所以中断之后直接再跑一次就是"接着做没做完的"。
"""

import json
import os
import re
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 档位表。**这三条路径是模块级全局**，由 main() 按 --kind 覆写 ——
# run_one() 是这份脚本里唯一带着"提交/下载/瘦身/校验"全部副作用的地方，
# 与其把它改成到处传参，不如让路径在进入批次之前就定下来、之后不再变。
# 覆写点只有一处（见 _select_kind），所以不存在"两个地方各写一份路径"的问题。
PROFILES = {
    "char": {
        "label": "角色",
        "src_dir": os.path.join(ROOT, "assets", "art", "char"),
        "model_dir": os.path.join(ROOT, "assets", "art", "char", "model"),
        "work": os.path.join(ROOT, "sweep", "gen3d"),
        # 键的顺序 = scene_builder.cpp 里 all_art_keys() 的顺序，方便两边对账
        "keys": [
            "char_leader", "char_rifleman", "char_mg", "char_sniper",
            "char_at", "char_demo", "char_medic", "char_ammo",
            "char_enemy_rifle", "char_enemy_mg", "char_enemy_officer",
        ],
    },
    "wpn": {
        "label": "武器",
        "src_dir": os.path.join(ROOT, "assets", "art", "wpn"),
        "model_dir": os.path.join(ROOT, "assets", "art", "wpn", "model"),
        "work": os.path.join(ROOT, "sweep", "gen3d_wpn"),
        # 顺序 = scene_builder.cpp 里 all_wpn_keys() 的顺序，也是游戏里 V 键循环的顺序。
        # 1951 年志愿军制式：步枪 / 冲锋枪 / 轻机枪。
        "keys": ["wpn_mosin", "wpn_ppsh", "wpn_dp27"],
    },
    "veh": {
        "label": "载具",
        "src_dir": os.path.join(ROOT, "assets", "art", "veh"),
        "model_dir": os.path.join(ROOT, "assets", "art", "veh", "model"),
        "work": os.path.join(ROOT, "sweep", "gen3d_veh"),
        # 顺序 = make_vehicle_node 的 type 字符串，也是 va_config.cpp 里
        # vehicle_of() 认的四个键。1951 年朝鲜战场**美军**车队制式
        # （敌方身份由角色立绘 char_enemy_rifle 钉死：M1 钢盔 + M1 加兰德 +
        #  M1943 野战夹克），所以四辆车全部取美军型号：
        #   吉普 → Willys MB / 装甲车 → M3 Half-track / 坦克 → M4A3E8 Sherman
        #   / 卡车 → GMC CCKW
        "keys": ["veh_jeep", "veh_apc", "veh_tank", "veh_truck"],
    },
    "vm": {
        "label": "第一人称手模",
        "src_dir": os.path.join(ROOT, "assets", "art", "vm"),
        "model_dir": os.path.join(ROOT, "assets", "art", "vm", "model"),
        "work": os.path.join(ROOT, "sweep", "gen3d_vm"),
        # 顺序 = viewmodel.cpp 里 hand_r / hand_l 两组的顺序。
        # 先只做 r（扳机手）：握拳的手在引擎里旋转后左右手可以共用，
        # 所以 r 是"先 1 张确认朝向"那一步，l 视效果再决定要不要单独生成。
        "keys": ["vm_hand_r"],
        #
        # 【文生3D（2026-09-24）：手模为什么也必须换输入通道】
        # 09-21 那张立绘（空手握拳实拍）走图生3D 出来的模型，**几何是一整团钝圆体**：
        # 识别色图下手指只在轮廓上有浅浅起伏，指头的"样子"完全靠贴图糊出来。
        # 放大到 3 倍看：四指在手背上连成一坨、中间还有一块贴图白斑。
        # 根因与上一轮的**树冠叶簇完全同构** —— 图生3D 是**单视图重建**，
        # 而"握拳"这个姿势里，**指缝之间的空腔在任何单张照片里都看不见**
        # （看到的永远是"手指叠在一起的那一面"），重建只能把那些区域蒙成一整块。
        # 参考图换成"指头张得更开的手"也没用 —— 那是"换参考图 vs 换通道"那条
        # 已经证伪过一次的路（树/灌丛那次：同一批参考图不换、只换通道，两件一次全成）。
        #
        # prompt 的写法要点（每条对着一个已知风险）：
        #   ① 主语给"**戴厚棉手套的右手**" —— 手是实体、手套是外层。反过来写
        #      （主语给"棉手套"）会让生成器去建一个**空心的壳**，得到一只塌手套。
        #   ② 点明"**五根手指各自分开、每一根都看得出独立形状**" —— 直接对着
        #      "指头糊成一团"这个根因。上一轮的教训是"加'不要 X'基本不起作用，
        #      起作用的是换掉描述里的主语 / 正面描述要什么形状"，所以这里用
        #      正面陈述而不是"不要粘连"。
        #   ③ 点明"**棉絮撑得圆鼓鼓、厚实有体积**" —— 棉布在 0.3 m 近景最容易
        #      读成"一层薄布"。棉手套的卖点恰恰是羽绒般的鼓胀体量，必须写出来。
        #   ④ 颜色写"**深赭黄 / 黄褐色**"而不是"土黄" —— 军装与背景也是土黄，
        #      手要能分开就得比它们更深更暗一档。
        #   ⑤ 点明"**只有手腕处一小段、没有前臂**" —— 09-21 那个模型自带约 40%
        #      前臂，导致归一化按 0.115 m 而不是"腕→中指尖 18.5 cm"（按后者会
        #      整体放大 1.6 倍）。少带一截前臂能省掉这次标定里最容易错的一步。
        # 两个候选只差**主语与体量先验**（上一轮实测：换主语有效、改措辞无效）：
        #   vm_hand_r2 → 主语是"手"，靠"五指分开"这个解剖先验去分指
        #   vm_hand_r3 → 主语是"戴手套的拳"，靠"鼓胀饱满的体量"去撑开指缝
        "prompts": {
            "vm_hand_r2":
                "一只戴着厚棉手套的右手，五根手指各自分开、每一根都看得出独立的手指形状，"
                "手指向内弯曲收拢成握持的姿势，像是正攥着一根看不见的粗木棍，"
                "拇指横压在食指与中指外侧，手套里絮满了棉花所以圆鼓鼓的、饱满有体积，"
                "深赭黄色的粗棉布，布面有被撑起的柔软褶皱与缝线，"
                "只有手腕处一小段收口、没有前臂，写实摄影，纯白背景，只拍这一只戴手套的手",
            "vm_hand_r3":
                "一只攥成拳头的厚棉手套，鼓胀饱满得像充了气，棉絮把布面撑得圆润结实、"
                "体积厚重而不显薄，拳面上能看出五根手指各自弯曲的棱线、指节分明互不相连，"
                "指缝的凹陷清晰可见，拇指压在其他四指外面，手腕处是一圈收口的罗纹口，"
                "深赭黄偏褐的粗棉布编织纹理，写实摄影，纯白背景，只拍这一只手套",
        },
    },
    "prop": {
        "label": "战场地物",
        "src_dir": os.path.join(ROOT, "assets", "art", "prop"),
        "model_dir": os.path.join(ROOT, "assets", "art", "prop", "model"),
        "work": os.path.join(ROOT, "sweep", "gen3d_prop"),
        # 顺序 = scene_builder.cpp 的 add_prop 里认的四个键。
        # 这一档换的是**掩体本身**（Rock / Tree / Bush 三种 PropType 的渲染），
        # 逻辑层一个字段没动：落位、半径、判定顺序全是原来那套。
        # 两个岩石是**两份不同剪影**（棱角裸岩 / 苔覆圆石），按 hash 交替用，
        # 免得三十多块石头是同一块复制出来的 —— 变体是"少钱多变化"的便宜做法，
        # 真正贵的是再多生成几张。
        "keys": ["prop_rock_a", "prop_rock_b", "prop_pine", "prop_bush"],
        # 【文生3D（2026-09-23）】这两个键**不用立绘**，走 `prompt` 位置参数。
        # 为什么单独开一条通道：图生3D 是**单视图重建**，而树与灌丛是"靠半透明
        # 叶簇表达体积"的东西 —— 参考图里枝干之间透出背景，重建拿不到叶子围成的
        # 体积。实测（09-22 那轮，单图 + 半透明叶簇）：
        #   针叶树 → 水平只有高的 6.3%（一根杆）
        #   灌丛   → 高只有最大维的 0.16%（一张水平圆盘）
        # 两个不透明实体（两块岩石）同一批同样参数全部正常，所以根因是**输入形态**，
        # 不是管线。文生3D 从头合成体量，不受"只能看到一面"的约束。
        #
        # prompt 的写法要点（直接针对根因）：
        #   ① 点明**不透光 / 看不到枝干之间的空隙** —— 这是上轮失败的那件事；
        #   ② 点明形体是**完整的圆锥**（给重建一个明确的体量先验）；
        #   ③ 写"摄影 / 写实"，避免生成卡通或带场景的图（我们只要单体）；
        #   ④ 灌丛额外点明"高度约为宽度的一半"，与 kPropArt 里 hw=0.36 的口径一致。
        #
        # 【2026-09-24 补：针叶树重做，追"一摞飞盘"这个观感的根因】
        # 上一轮的树冠**几何是达标的**（冠幅/高 0.589，判据②要求 ≥0.15），
        # 但近景 1~2 m 读作"一摞飞盘" —— 枝条被生成成一片片水平薄盘。
        # 回头看 v1 的措辞，这个结果是**自己招来的**：
        #   "层层叠叠的针叶枝条从下到上收拢成…圆锥形树冠"
        # "层层叠叠" = 一层压一层，"针叶枝条" = 让生成器去建**枝条**这个实体。
        # 两者叠加，最省面的解法当然就是"叠起来的圆盘"。
        # 所以这一轮不动管线、不动参数，只做**措辞手术 + 换体量先验**，两个候选：
        #   prop_pine2（措辞手术）：把"层层叠叠 / 枝条"换成"连续完整 / 细碎针叶"，
        #     并**显式否定**"水平伸出的枝条""分层的圆盘状枝叶"（点出要避开的东西）。
        #   prop_pine3（换先验）：不去描述"枝条怎么长"，改描述**材质与整体轮廓**
        #     （蓬松浓密如绒团、轮廓连续光滑、均匀收尖）—— 让生成器按"一团密实的
        #     锥体"去补面，而不是按"若干枝条"去拼。
        # 两个都用同一根"完全不透光"（上一轮靠它把树从"一根杆"救回来的那条）。
        "prompts": {
            "prop_pine":
                "一棵高大的针叶松树单体，笔直粗壮的红褐色树干，枝叶极其茂密浓绿，"
                "层层叠叠的针叶枝条从下到上收拢成一个完整饱满的圆锥形树冠，"
                "完全看不到枝干之间的空隙、不透光，朝鲜战场山地常见的东北红松，"
                "写实摄影，纯白背景，只拍这一棵树",
            "prop_pine2":
                "一棵高大的针叶松树单体，笔直粗壮的红褐色树干一直通到树顶，"
                "树冠是一个连续完整的圆锥体，由数不清的细碎针叶紧簇而成，"
                "针叶细密如绒、彼此交织连成一片，表面没有任何一片水平伸出的枝条、"
                "没有分层的圆盘状枝叶，也看不到枝干之间的空隙、完全不透光，"
                "朝鲜战场山地常见的东北红松，写实摄影，纯白背景，只拍这一棵树",
            "prop_pine3":
                "一棵高大的针叶松树单体，笔直的红褐色树干，树冠是一个密实饱满的圆锥体，"
                "通体被极细的针叶紧密覆盖，质感蓬松浓密得如同一个绿色的绒团，"
                "枝条完全被针叶包裹住、看不出分层也看不出任何间隙，"
                "整体轮廓连续光滑、从底部到顶端均匀收尖、没有横向伸出的枝盘，"
                "朝鲜战场山地常见的东北红松，写实摄影，纯白背景，只拍这一棵树",
            "prop_bush":
                "一丛低矮密实的灌木单体，由许多紧密挤在一起的深绿色叶团组成，"
                "整体呈半圆的团簇状，高度约为宽度的一半，完全看不到内部的枝干空隙、"
                "不透光，没有明显的主干，写实摄影，纯白背景，只拍这一丛",
        },
    },
}

SRC_DIR = PROFILES["char"]["src_dir"]
MODEL_DIR = PROFILES["char"]["model_dir"]
WORK = PROFILES["char"]["work"]
KEYS = list(PROFILES["char"]["keys"])
# 走**文生3D**的键 → 中文描述。空表 = 全档位都走图生3D（既有四个档位就是这样，
# 行为一个字节没变）。见 PROFILES["prop"]["prompts"] 的注释。
PROMPTS = {}

PY = sys.executable
# 瘦身要 Pillow，而 Pillow 只装在隔离 venv 里（本项目唯一一个非纯 Python 的工具）。
# 若该 venv 不存在，脚本会明确报出来而不是悄悄产出一个 42MB 的 GLB 进仓库。
VENV_PY = ("C:/Users/lujiajing/.workbuddy/binaries/python/envs/default/Scripts/python.exe")

# 【`--face-count 50000` 必须显式给，这不是可选项】
# buddy-cloud.py 的 `--face-count` 默认值是 **500000**（10 万~150 万区间），
# 不传就等于向服务端要 50 万面。实测后果（2026-09-18 第二批 5 个）：
#   50 万面 → 单模型 15MB、三角面 500,000（网格自己就占掉约 13.5MB，
#             与贴图无关，瘦身工具压不动）
#   5 万面  → 单模型 1.56MB、三角面 50,162（= 已接入的 char_rifleman 规格）
# 10 倍面数对「1.7 米的人站在 110 米战场上只占几十像素」毫无收益，
# 却让 11 个角色的仓库体积从 17MB 涨到 165MB，并让同屏几何量翻十倍。
# 旁证：两者积分档位不同（5 万面 40 分 / 50 万面 30 分），
# 所以「积分对不上」其实是面数档位不一致的先兆，不只是计价差异。
# 教训：这里原本写着"与试接那次完全一致"，但试接显式带了 50000 ——
# **"参数一致"必须以双方的实际请求体为准，不能凭记忆断言**。
# 面数只在这一个地方写死 —— 内置后端的命令行参数与腾讯云后端的 --face-count
# 都引用它（"同一个数字出现两遍就一定会漏改一处"）。
FACE_COUNT = 50000
GEN_ARGS = ["--enable-pbr", "--generate-type", "Normal", "--face-count", str(FACE_COUNT)]

# 【模型档位】hy 后端用哪一版混元生3D，默认 3.1（与内置通道 / 已接入模型对齐）。
# 可被 VA_GEN3D_MODEL 覆盖 —— 典型场景：免费额度在 3.1 上耗尽后改试同族的 3.0。
# 依据：TokenHub 的免费体验包是**按模型**领取的，即每个模型各有独立额度；
# 而 3.0 / 3.1 同属混元生3D，风格差异远小于换到 tripo-3d-3.1 / hi3d-2.1 这类别的模型族。
# 只对 hy 生效：builtin 的 model 由 buddy-cloud.py 决定，tc 走 CAM 签名那套字段。
G3D_MODEL = os.environ.get("VA_GEN3D_MODEL", "hy-3d-3.1").strip()

# ---- 后端选择 ----
# builtin：内置多模态通道（gen3d.py），限 5 次提交/天，且**同一天内不重置**。
# tc     ：腾讯云混元生3D 官方 API 直连（gen3d_tc.py），CAM 签名，要 SecretId/SecretKey。
# hy     ：混元生3D（TokenHub，gen3d_hy.py），Bearer 认证，只要一把 sk- API Key，
#          默认 3 并发、无每日提交限制。凭据由后端脚本自己读，不走 stdin。
BACKEND = os.environ.get("VA_GEN3D_BACKEND", "builtin").strip().lower()
if BACKEND not in ("builtin", "tc", "hy"):
    raise SystemExit("VA_GEN3D_BACKEND 只能是 builtin / tc / hy，当前为 %r" % BACKEND)


LOG_LOCK = threading.Lock()

# 服务端对 hy-3d 维度只放 2 个并发槽位，超了直接 429 拒绝（**不扣积分**）。
# 所以并发默认就是 2：开 6 个不是"更快"，只是让 4 个任务在 15 秒内各报一次失败。
#
# 【为什么重试预算要给到 25 次而不是 8 次】实测这 2 个槽位**并不总是空着**：
# 连续跑了 6 分钟、每次都是 429，而我方一个任务都没提交成功 ——
# 说明占着槽位的东西不是我们自己（试接那次早就结束了）。
# 这类共享维度的配额什么时候空出来是外部决定的，只能等。
# 等不到就等不到，脚本会明确报"重试 N 次仍失败"，不会假装成功。
SUBMIT_TRIES = 12
SUBMIT_WAIT_CAP = 30        # 退避上限（秒）：token 只有 ~20 分钟寿命，等太久等于白等

# 产物规格上限（三角面）。已接入的 char_rifleman = 50,162 面，
# 而"没给 --face-count"时服务端会给 500,000 面 —— 两者相差 10 倍，
# 肉眼在战场上分辨不出，但体积差 10 倍、同屏几何量差 10 倍。
# 所以宁可让日志把这件事喊出来，也不要等到 11 个模型都接进场景才发现规格不齐。
MAX_TRIS_OK = 120000

# 429 有**两种含义完全不同**的 429，必须分开对待 —— 这是本轮花掉 10 分钟才看出来的：
#   "concurrent slot limit exceeded (2) for dimension hy-3d"  → 并发槽位被占，**可等**，槽位会空出来
#   "daily submit limit exceeded (5/5) for dimension hy-3d"   → **当日提交配额用尽**，
#                                                              同一个 token / 同一天内重试多少次都是白等
# 第一版把两者都当"可等"，于是日配额用尽之后仍然老老实实重试 12 次 × 最多 30s，
# 而且每次都打印"槽位已满"——把真正的结论（今天做不了了）盖在噪音底下。
# 两个后端的失败语义都在这一处归类（先判 fatal，再判 retry）：
#   内置通道： concurrent slot limit（可等） / daily submit limit（当日终止）
#   腾讯云官方：RequestLimitExceeded、LimitExceeded、InternalError（可等）
#               InsufficientBalance、AuthFailure.*、UnauthorizedOperation（重试无意义）
RETRYABLE_KEYS = (
    "slot limit", "concurrency",
    "requestlimitexceeded", "limitexceeded", "internalerror",
    "ratelimit", "toomanyrequests", "servererror",
)
# 【为什么"超时"故意不列进可重试】提交是**非幂等**的：一个超时的提交可能已经在服务端
# 排上了队。自动重试 = 同一个角色提交两次 = 双倍积分，而且两个模型只有一个能被采用。
# 超时归到 other（不重试、如实报失败），由人决定要不要重跑 —— 这时才该去看
# sweep/gen3d/<键>.json 里有没有已经拿到的任务 id（有就 query 补下载，别重新提交）。
FATAL_KEYS = (
    "daily submit limit", "quota", "daily limit",
    "insufficientbalance", "resourceinsufficient", "arrears",
    "authfailure", "secretidnotfound", "unauthorizedoperation",
    "invalid_api_key", "incorrect api key", "unauthorized", "forbidden",
    # TokenHub 的无效 Key 报的是 401002「The API Key does not exist or signature
    # verification failed…」—— 既不含 invalid_api_key 也不含 unauthorized，
    # 实测漏判过一次（于是坏 Key 会让每个键各自失败一遍而不是立刻停整批）。
    "api key", "401002",
)


def _classify(body):
    """把一次提交失败分成 'retry'(可等) / 'fatal'(今天别再试) / 'other'(不可重试)。"""
    b = body.lower()
    if any(k in b for k in FATAL_KEYS):
        return "fatal"
    if any(k in b for k in RETRYABLE_KEYS):
        return "retry"
    return "other"


def log(*a):
    with LOG_LOCK:
        print(*a)
        sys.stdout.flush()


class QuotaExhausted(Exception):
    """当日提交配额用尽。**整批必须立刻停**，不是这一个键失败。"""


def _select_kind(p_kind):
    """按档位覆写 SRC_DIR / MODEL_DIR / WORK / KEYS。

    【为什么用 global 而不是把路径传进 run_one】run_one 里那四条路径要在
    "续跑判据 / 输入校验 / 结果落盘 / 成品落盘 / 日志"五处以同样的值出现，
    传参等于给每一处都增加一个可以说错话的机会。这里覆写一次、之后再没人改，
    语义上就是"这一批的路径"。覆写点唯一，所以不会出现两份路径不一致。
    """
    global SRC_DIR, MODEL_DIR, WORK, KEYS, PROMPTS
    if p_kind not in PROFILES:
        raise SystemExit("--kind 只能是 %s，当前为 %r"
                         % (" / ".join(sorted(PROFILES)), p_kind))
    p = PROFILES[p_kind]
    SRC_DIR = p["src_dir"]
    MODEL_DIR = p["model_dir"]
    WORK = p["work"]
    KEYS = list(p["keys"])
    # .get()：既有的四个档位没有 prompts 这一项，取空表 → 全部走图生3D，
    # 与引入这条通道之前的行为完全一致（不需要给它们补字段）。
    PROMPTS = dict(p.get("prompts", {}))
    return p


def _probe_tris(glb):
    """读一个 GLB 的三角面数；读不出来返回 None（视为"规格未知"）。

    【为什么要把它接进续跑判据】2026-09-18 的一次真实事故：那批 50 万面的成品被移走后，
    结果 JSON 与 raw 还在，于是脚本按"json 里 status==DONE"判定 char_leader 已完成，
    **跳过提交、直接拿旧 URL 重新瘦身** —— 又把一个 15MB / 50 万面的模型放回了模型目录。
    只判"任务成功"是不够的：产物规格也是完成度的一部分。
    """
    if not os.path.isfile(VENV_PY) or not os.path.isfile(glb):
        return None
    try:
        p = subprocess.run(
            [VENV_PY, os.path.join(ROOT, "tools", "slim_glb.py"), "probe", glb],
            capture_output=True, text=True, encoding="utf-8", errors="replace", cwd=ROOT,
        )
        m = re.search(r"三角面\s*(\d+)", p.stdout or "")
        return int(m.group(1)) if m else None
    except Exception:
        return None


def run_one(key, token, force):
    tag = key

    def say(msg):
        log("[%s] %s" % (tag, msg))

    out_json = os.path.join(WORK, key + ".json")
    raw_glb = os.path.join(WORK, key + ".raw.glb")
    dst_glb = os.path.join(MODEL_DIR, key + ".glb")

    if not force and os.path.exists(dst_glb) and os.path.getsize(dst_glb) > 0:
        tris = _probe_tris(dst_glb)
        if tris is None or tris <= MAX_TRIS_OK:
            say("已有成品 %.2f MB%s，跳过"
                % (os.path.getsize(dst_glb) / 2 ** 20,
                   "" if tris is None else "（%d 面）" % tris))
            return True
        say("已有成品但规格不对（%d 面 > %d），重做" % (tris, MAX_TRIS_OK))

    # 输入校验：走文生3D 的键不需要立绘（描述在 PROFILES 的表里），走图生3D 的必须有。
    # 缺图是"这个键没准备好"，不是"模型生成失败" —— 早报早好，别等到提交才报。
    prompt = PROMPTS.get(key)
    if prompt:
        if BACKEND != "builtin":
            say("文生3D 只在内置通道（builtin）上实现：tc / hy 两个后端脚本只吃立绘，"
                " 请用 VA_GEN3D_BACKEND=builtin 重跑")
            return False
        src = None
    else:
        src = os.path.join(SRC_DIR, key + ".png")
        if not os.path.isfile(src):
            say("缺立绘 %s" % src)
            return False

    # ---- 0. 上次的 json 是"成功的"还是"失败留下的"？----
    # 【为什么必须看内容而不是只看文件在不在】第一次跑（并发 6）11 个全被 429 拒了，
    # 但每个都留下了一个**非空的** json（里面是错误对象）。只判"文件存在且非空"
    # 就会把这一批全部当成"已完成"跳过 —— 脚本报一切正常，仓库里一个模型都没有。
    # 所以判据是 json 里的状态是不是 DONE。
    done_json = False
    if not force and os.path.exists(out_json) and os.path.getsize(out_json) > 0:
        try:
            j = json.load(open(out_json, encoding="utf-8"))
            done_json = (j.get("status") or j.get("raw_result", {}).get("Status")) == "DONE"
        except Exception:
            done_json = False
        # 光"任务成功"不算完成：成品必须在、且规格正确。
        # 否则一份旧 JSON（比如规格错的那批）会让后续每一次续跑都"跳过提交、
        # 拿旧结果重新瘦身"，把错误规格一次次复活。见 _probe_tris 的注释。
        if done_json:
            tris = _probe_tris(dst_glb) if os.path.exists(dst_glb) else None
            if tris is None or tris > MAX_TRIS_OK:
                done_json = False

    # ---- 1. 提交并轮询（交给各自的后端脚本；两者都改用进程内 argv 绕开命令行长度上限）----
    if not done_json:
        t0 = time.time()
        # 【判据：走文本时没有 src，日志不能去 stat 它】上一步的 src 只在图生3D 分支
        # 才有值，这里必须按 prompt 分流 —— 否则 os.path.getsize(None) 直接抛 TypeError，
        # 而且是在提交之前抛，看起来像"生成失败"其实是"日志打不出来"。
        if prompt:
            say("提交中（文生3D，描述 %d 字，后端 %s）…" % (len(prompt), BACKEND))
        else:
            say("提交中（输入 %.2f MB，后端 %s）…" % (os.path.getsize(src) / 2 ** 20, BACKEND))
        # PYTHONIOENCODING：子进程的 stdout 是管道，Python 会按系统区域（GBK）编码它，
        # 而父进程按 UTF-8 解 —— 不解这一下，日志里所有中文都是乱码，
        # 而"哪一步说了什么"正是这个脚本存在的意义。
        env = dict(os.environ, PYTHONIOENCODING="utf-8")
        if BACKEND == "tc":
            # 腾讯云直连不吃 stdin：凭据由 gen3d_tc.py 自己从环境变量或
            # ~/.workbuddy/tencentcloud.json 读（不落命令行、不进仓库）。
            cmd = [PY, os.path.join(ROOT, "tools", "gen3d_tc.py"), "submit",
                   src, out_json, "--face-count", str(FACE_COUNT)]
            stdin_text = None
        elif BACKEND == "hy":
            # TokenHub 同理：凭据由 gen3d_hy.py 读环境变量 TOKENHUB_API_KEY 或
            # ~/.workbuddy/va_3d_api_key.txt。面数走同一个 FACE_COUNT 真值来源。
            cmd = [PY, os.path.join(ROOT, "tools", "gen3d_hy.py"), "submit",
                   src, out_json, "--face-count", str(FACE_COUNT),
                   "--model", G3D_MODEL]
            stdin_text = None
        else:
            # builtin：`--text` 必须排在两个位置参数之前（gen3d.py 按 argv[0] 认这个开关）。
            # 其余参数（GEN_ARGS）原样透传，两条路的面数 / PBR / 生成方式完全一致 ——
            # 这样"文字版与图片版出来得不一样"就只剩输入这一项变量。
            if prompt:
                cmd = [PY, os.path.join(ROOT, "tools", "gen3d.py"), "--text",
                       prompt, out_json] + GEN_ARGS
            else:
                cmd = [PY, os.path.join(ROOT, "tools", "gen3d.py"), src, out_json] + GEN_ARGS
            stdin_text = token + "\n"
        for attempt in range(1, SUBMIT_TRIES + 1):
            p = subprocess.run(
                cmd, input=stdin_text, capture_output=True, text=True, encoding="utf-8",
                errors="replace", cwd=ROOT, env=env,
            )
            if p.returncode == 0:
                say("生成完成，用时 %.0fs" % (time.time() - t0))
                break
            # 提交阶段失败多半是并发槽位用满，服务端返回
            #   {"error":"HTTP_ERROR","message":"concurrent slot limit exceeded (2) for
            #    dimension hy-3d","http_status":429}
            # 这是**可等**的：槽位会自己空出来。而"面数超限/图片过大"那种不可等，
            # 所以只对 429 / slot limit 这类关键词重试，其余立刻放弃并把原因打出来，
            # 免得把 11 个任务都拖成"重试 6 次 × 每次 1 分钟"却什么都没做。
            # 【为什么把两处输出拼起来而不是二选一】内置后端的错误体写在结果 JSON 里
            # （它无论如何都会落盘）；tc 后端的错误只打在 stdout/stderr（失败时不写 JSON）。
            # 只读一边，另一边后端的失败原因就会"看起来像空字符串"而被归成 other。
            body = (p.stderr or "") + (p.stdout or "")
            try:
                body += open(out_json, encoding="utf-8").read()
            except Exception:
                pass
            kind = _classify(body)
            if kind == "fatal":
                # 当日配额用尽：**继续重试是纯粹浪费时间**，而且会把真正的结论
                # （"今天做不了了"）盖在"槽位已满"的噪音底下。直接终止整个批次 ——
                # 剩下的键今天也不会有结果，让脚本立刻以明确的结论退出，
                # 而不是再花 10 分钟逐个报"失败"。
                say("不可恢复的提交失败（当日配额 / 凭据 / 余额），终止整批：%s"
                    % body.strip()[:200])
                raise QuotaExhausted(body.strip()[:300])
            if kind != "retry":
                say("生成失败（不可重试）rc=%d：%s" % (p.returncode, body.strip()[:300]))
                return False
            if attempt < SUBMIT_TRIES:
                wait = min(SUBMIT_WAIT_CAP, 15 * attempt)
                say("提交被拒（可重试：并发槽位 / 限频），%ds 后重试（第 %d/%d 次）"
                    % (wait, attempt, SUBMIT_TRIES))
                time.sleep(wait)
            else:
                say("提交重试 %d 次仍失败：%s" % (SUBMIT_TRIES, body.strip()[:200]))
                return False
        else:
            return False

    # ---- 2. 从结果里取 GLB 链接 ----
    try:
        d = json.load(open(out_json, encoding="utf-8"))
    except Exception as e:
        say("结果 json 不可读：%r" % e)
        return False

    st = d.get("status") or d.get("raw_result", {}).get("Status")
    if st != "DONE":
        say("任务状态 %s，不是 DONE" % st)
        return False

    url = None
    for f in d.get("raw_result", {}).get("ResultFile3Ds", []) or []:
        if str(f.get("Type", "")).upper() == "GLB":
            url = f.get("Url")
            break
    if not url:
        say("结果里没有 GLB 链接")
        return False
    say("积分 %s" % d.get("raw_result", {}).get("ResultCreditConsumed"))

    # ---- 3. 下载 ----
    # 【这一处曾经"拿旧模型顶替刚生成的新模型"，而且一路报成功（2026-09-23 实测）】
    # 判据原本是"raw 不在才下"，可 raw 是**按 key 命名**的：上一轮遗留的
    # `prop_pine.raw.glb` 会让这一轮的下载被整个跳过 → 刚花掉的 40 积分换来的新模型
    # 被丢掉，改用旧 raw 重新瘦身，成品与上一轮**逐字节相同**（md5 一致）。而日志写的是
    # "生成完成 / 积分 40 / 齐备 2/2 / EXIT=0"，从外面完全看不出异常 ——
    # 唯一能看出的是 raw 的 mtime 还停在上一轮。
    # 所以判据改成"**这一轮有没有真提交过**"：只要提交过（done_json 为假），
    # 同名 raw 必然属于上一轮，先删掉再下。dl.fetch 内部也有"目标在就返回"的短路，
    # 不先删就还是被它吃掉。
    # 【为什么这是一个"上次只补了一半"的坑】dst_glb 那侧的同类问题在续跑判据里
    # 已经处理过（见 _probe_tris 的注释：光"任务成功"不算完成，成品规格也是完成度）。
    # 那次补的是**成品**，漏了**原料** —— 两条判据长得很像，但检查的是两个文件。
    if not done_json and os.path.exists(raw_glb):
        say("清掉上一轮遗留的同名 raw（%.2f MB）—— 这一轮重新下载"
            % (os.path.getsize(raw_glb) / 2 ** 20))
        os.remove(raw_glb)
    if not os.path.exists(raw_glb) or os.path.getsize(raw_glb) < 1024:
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import dl  # 复用它那套 Range 续传 + 退避重试
        try:
            dl.fetch(url, raw_glb)
        except Exception as e:
            # 额度已经花了（任务在服务端是 DONE 的），结果 URL 就在 out_json 里 ——
            # 把这条救回来的路写进日志，而不是只报一个"下载失败"。
            say("下载失败：%r" % e)
            say("⚠ 额度已花掉，结果仍在服务端：URL 在 %s，可手工重下后 %s"
                % (out_json, raw_glb))
            return False

    # ---- 4. 瘦身 ----
    if not os.path.isfile(VENV_PY):
        say("找不到带 Pillow 的隔离 venv：%s" % VENV_PY)
        return False
    p = subprocess.run(
        [VENV_PY, os.path.join(ROOT, "tools", "slim_glb.py"),
         "slim", raw_glb, dst_glb],
        capture_output=True, text=True, encoding="utf-8", errors="replace", cwd=ROOT,
    )
    if p.returncode != 0:
        say("瘦身失败 rc=%d：%s" % (p.returncode, (p.stderr or p.stdout or "")[-400:]))
        return False
    say("成品 %.2f MB（原 %.2f MB）" % (
        os.path.getsize(dst_glb) / 2 ** 20, os.path.getsize(raw_glb) / 2 ** 20))

    # ---- 5. 规格校验：面数是不是我们要的那一档 ----
    # 【为什么值得单独查一次】"参数给了"不等于"服务端照做了"，而面数不对在战场上
    # 完全看不出来（单位只有几十像素高），只有体积和帧率会说话 ——
    # 等到 11 个模型全接进场景才发现规格不齐，代价是重做一整天。
    # 这里只报警不判失败：参数是固定的，报警意味着服务端行为变了，该由人来判断；
    # 而"续跑判据"那边（见 _probe_tris 与 done_json）会直接把不合格的成品重做。
    tris = _probe_tris(dst_glb)
    if tris is None:
        say("规格校验跳过（读不出三角面数）")
    else:
        say("三角面 %d%s" % (tris, "" if tris <= MAX_TRIS_OK else
                            "  ← 警告：超过预期上限 %d，规格与已接入模型不一致" % MAX_TRIS_OK))
    return True


def main():
    argv = sys.argv[1:]
    jobs = 2
    force = False
    kind = "char"
    keys = []
    i = 0
    while i < len(argv):
        if argv[i] == "--jobs":
            jobs = int(argv[i + 1]); i += 2
        elif argv[i] == "--force":
            force = True; i += 1
        elif argv[i] == "--kind":
            kind = argv[i + 1]; i += 2
        else:
            keys.append(argv[i]); i += 1

    # 档位必须在"读 token / 建目录 / 报批次"之前定下来 ——
    # 它决定的是这一批的输入目录与成品目录，晚一步就会把武器写进角色目录里。
    prof = _select_kind(kind)

    # tc / hy 后端的凭据由各自脚本自己读，不吃 stdin（所以这里不检查、也不阻塞等待）。
    token = ""
    if BACKEND == "builtin":
        token = sys.stdin.readline().strip()
        if not token:
            print("stdin 没有拿到 token")
            return 2
    if not keys:
        keys = list(KEYS)

    os.makedirs(WORK, exist_ok=True)
    os.makedirs(MODEL_DIR, exist_ok=True)

    log("批次：%d 个%s，并发 %d，后端 %s%s"
        % (len(keys), prof["label"], jobs, BACKEND, "，强制重做" if force else ""))
    log("  输入 %s" % SRC_DIR)
    log("  成品 %s" % MODEL_DIR)

    lock = threading.Lock()
    todo = list(keys)
    fail = []
    quota = []          # 一旦非空，说明当日配额用尽，整批停

    def worker():
        while True:
            with lock:
                if not todo or quota:
                    return
                k = todo.pop(0)
            try:
                ok = run_one(k, token, force)
            except QuotaExhausted as e:
                # 整批停：把剩下的任务全部记为未完成，各线程看 quota 非空就会退出。
                # 这里不把 quota 当作"异常"往上抛 —— 它是**一个明确的业务结论**，
                # 不是脚本坏了，所以走正常收尾路径把话说清楚。
                with lock:
                    quota.append(str(e))
                    # 触发终止的那个键也要记进去：它在 pop 时就离开了 todo，
                    # 只 extend(todo) 会把它漏掉，结论行就会少报一个。
                    fail.append(k)
                    fail.extend(todo)
                    todo.clear()
                return
            except Exception as e:      # 任何意外都不该带走整个批次
                log("[%s] 异常 %r" % (k, e))
                ok = False
            if not ok:
                with lock:
                    fail.append(k)

    ths = [threading.Thread(target=worker, daemon=True) for _ in range(max(1, jobs))]
    for t in ths:
        t.start()
    for t in ths:
        t.join()

    if quota:
        log("")
        log("=== 结论：提交无法继续（配额 / 凭据 / 余额），剩余 %d 个没做成 ===" % len(fail))
        log("=== 原文：%s" % quota[0])
        if BACKEND in ("tc", "hy"):
            log("=== 提示：%s 后端没有每日提交上限；若是余额或凭据问题，"
                "处理后在控制台确认即可重跑（已完成的会自动跳过）===" % BACKEND)
        else:
            log("=== 提示：内置通道按天限 5 次提交且当天不重置，明天直接重跑本脚本即可 ===")
        log("=== 也可以换后端：VA_GEN3D_BACKEND=hy（TokenHub，一把 sk- Key 即可）"
            " 或 tc（CAM 签名，要 SecretId/SecretKey）===")

    have = [k for k in keys if os.path.exists(os.path.join(MODEL_DIR, k + ".glb"))]
    log("")
    log("=== 齐备 %d / %d：%s" % (len(have), len(keys), ", ".join(have)))
    if fail:
        log("=== 失败：%s（重跑本脚本即可只补这些）" % ", ".join(fail))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
