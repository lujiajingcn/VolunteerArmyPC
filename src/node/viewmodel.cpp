// VolunteerArmyPC —— 第一人称武器视图模型实现
//
// 坐标系：相机局部空间。-Z 前方、+X 右、+Y 上。
//
// ---------------------------------------------------------------------------
// 【姿态是怎么定下来的：投影反推，不是试数】
//
// 第一版把枪托摆到局部 z=+0.31、root 又在 z=-0.262 —— 枪托尾端落在相机**后方**
// 0.05 m，被近裁剪面切开、透视放大成占屏近一半的黑板。第二版把整把枪压到相机
// 前 0.175 m 以内，枪是出来了，但**离眼睛太近**：0.032 m 半径的前臂在 0.19 m 处
// 的角直径高达 20°，在屏幕上是一根 380 px 粗的亮柱子，比枪还抢眼 —— 真实持枪
// 时手腕离眼睛约 0.33 m，不是 0.19 m。
//
// 所以这里反过来：先把"枪要在屏幕上的哪几个点"算出来，再反推几何。
// 取世界（相机空间）垂直半张角 tan(65°/2)=0.6371、水平 tan = 0.6371×2560/1369 = 1.1914。
// 屏幕归一化坐标 u,v 与世界的换算（z 取正值＝前方距离）：
//     u = 0.5 + x / (2·z·1.1914)      v = 0.5 - y / (2·z·0.6371)
//
// gun 局部原点 = **机匣后端（枪托接口）**，枪管沿 -Z 伸出，y=0 是枪管轴线。
// 枪托因此落在局部 +Z（但它很短，尾部只到 +0.173），枪口在 -0.575。
//
// 腰射（hip_pos = 0.150, -0.130, -0.330；姿态角 俯仰2.5° 偏航7° 侧倾-5°）：
//     枪托底板 → u 0.91, v 1.25   （屏幕外右下，枪从画面外伸进来）
//     右手     → u 0.67, v 0.97   （压着画面下沿）
//     左手     → u 0.58, v 0.70
//     瞄具     → u 0.65, v 0.59
//     枪口     → u 0.54, v 0.59
//   枪身横跨大半个右下象限、指向画面中心偏下 —— 这就是使命召唤的持枪构图。
//   枪轴完全平行视轴时，透视缩短会把枪压成一根短棒，"看起来不像枪"。
//
// 开镜（aim_pos = 0.018, -0.075, -0.290；姿态角全部归零）：
//     瞄具 → u 0.52, v 0.50     （正好压住准心）
//     枪口 → v 0.57             （枪身沿视轴缩成中心下方一条）
//   开镜时姿态角必须归零，否则瞄具会偏离准心，玩家会以为"打不准是枪的问题"。
//
// 另一条硬约束：**枪上任何顶点都不能落到相机后方**。最靠后的托底板在腰射时
// z=-0.169、开镜时 z=-0.127，加上后坐最大 +0.045 的退让，最坏 -0.124，
// 距近裁剪面 0.06 还剩 6 cm 余量。
// ---------------------------------------------------------------------------
//
// 【光照隔离】
// 枪模单独占一个渲染层（layer 2），再给它配一盏只照这一层的关键灯。
// 不这么做的话，玩家一转身背对太阳，枪就整个塌成黑色剪影 —— 因为它是被世界光
// 照亮的，而世界光的方向与玩家朝向无关。
#include "node/viewmodel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/capsule_mesh.hpp>
#include <godot_cpp/classes/cylinder_mesh.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "node/scene_builder.h"
#include "sim/va_math.h"
#include "sim/va_world.h"

using namespace godot;

namespace volunteer_army {

namespace {

// 枪模专用渲染层（layer 2 → bit 1）。世界物体不在这一层，所以那盏关键灯
// 不会跑去照亮旁边的石头和队友。
constexpr uint32_t VM_LAYER = 1u << 1;

// 程序化枪模那两只手**手掌方块中心**的基准落点（hand_r / hand_l 组的局部系）。
// 这两个数同时是"换真模型时该平移多少"的参考点：hand_r->set_position(目标 − 基准)。
// 它们必须与 scene_builder.h 的 WPN_HAND_R0 / WPN_HAND_L0 一致（z 那一项直接引用），
// 而 WPN_HAND_R0 / L0 是"程序化枪模自己那套"的落点，没有键的真模型回退到它。
const Vector3 HAND_R_BASE{0.0f, -0.080f, WPN_HAND_R0};
const Vector3 HAND_L_BASE{-0.010f, -0.032f, WPN_HAND_L0};

// 前臂的**腕端**相对掌心的偏移：从掌心往 +z 退出半个掌厚，落到掌背/腕上。
// 起点放在掌心会被自己的圆柱把手掌整个包住（实测手套只剩 525 px 的那个坑）。
const Vector3 WRIST_R_OFF{0.004f, -0.008f, 0.036f};
const Vector3 WRIST_L_OFF{0.004f, -0.008f, 0.042f};

// 肘的位置（枪局部系）。**这是一个常量，不随每把枪的握持点平移** ——
// 肘长在人身上，支撑手在护木上往前挪 17 cm，肘不会跟着挪 17 cm。
//
// 【两个约束一起定这组数：方向要出画，深度不能太浅】
//   ① 竖直角：相机空间的 y/z 之比要够大，肘才落在画面下沿之外。
//      要求 atan(|y|/|z|) > 35.8°（垂直半视场 32.5° + 一点余量）。
//   ② **深度**：这里才是上一版栽的地方。原先把肘放在 z=-0.12（换算到相机
//      空间深度只有 0.22 m），而正交投影下"离视轴多远"是按**沿视轴的深度** z
//      缩放的 —— 0.22 m 深处的一根 2.7 cm 粗的胳膊被透视放大成
//      屏幕上一个 150 px 宽的**喇叭口**，比手还抢眼（放大取证图实测）。
//      深度拉到 0.45 m 之后，同样的半径只占 ~50 px，才读得出是一条前臂。
// 两个约束联立得到：肘放到相机空间深度 ~0.4 m、竖直角 ~40° 附近，再换算回枪局部系。
//
// 【为什么两只肘的 x 差这么多 —— 参考图里只有一条手臂】
// 最初两只肘是对称地甩到 ±0.30 的（"两条前臂从画面两下角插进来"）。换到真模型
// 之后这条不成立了：支撑手在护木上、离眼睛 0.42 m，肘再甩到左边，左前臂就成了
// **一条从画面左下角斜穿到中央的长杠**，横在公路上比枪还抢眼。
// 看参考图（CSGO / BFV）会发现持枪视角里通常**只看得见扳机手那一条前臂**，
// 支撑臂是垂在枪下面、几乎整条出画的。所以左肘收到枪的中线底下（x≈-0.15，
// 几乎垂直向下），右肘留在右下角 —— 这样左臂只剩枪腹下的一小段，
// 视觉重心回到枪上。
// 【2026-09-19 二次结论：肘的位置几乎不影响腰射观感，别再调它了】
// 这一版把两只肘从 (0.30, -0.44, -0.12) 一路挪到 (0.56, -0.60, -0.43)，
// 腰射截图逐像素比对（阈值 8）**只有 1% 的像素有差异** —— 等于没动。
// 原因是肘本来就在画面外，露出来的那一截（腕 → 画面下沿）的屏幕位置只由
// 腕端决定：
//     腕 u0.538 v0.691 ／ 肘 u0.409 v1.230
// 腕不动，露出来的那 423 px 就一分不少。而按真人前臂 0.27 m 的长度反推，
// 肘能提供的横向漂移最多只有 130~190 px —— 撑不出"斜穿画面"的构图。
// 所以这一版不再动肘，改从**明度结构 + 筋络**下手（见下面的 strap / 袖褶）。
//
// 留一个记录：真正能减少"露出来多少"的只有**支撑手的屏幕高度**（v 0.69），
// 而它由枪的握持点决定 —— 动它等于动枪，不是调手臂能解决的。
const Vector3 ELBOW_R{0.560f, -0.600f, -0.430f};
const Vector3 ELBOW_L{-0.150f, -0.560f, -0.500f};

// 前臂半径（腕端 / 肘端）。
// 腕端的口径是"屏幕上的绝对宽度"：0.026 半径 → 直径 5.2 cm，在 0.374 m 处
// 折合 149 px（= 屏高的 11%）—— 一条比枪管还宽的可读块。压到 0.024（138 px）。
// 肘端**不能跟着放大**：投影宽度 ∝ r/深度，而肘比腕更深，所以肘一粗，
// 屏幕上的下沿反而比腕部更宽 —— 实测 0.026/0.034 会让画面下沿比腕部宽 9%，
// 读出来是一根**下粗上细的喇叭口 / 树桩**。0.033 刚好把这一项拉平。
constexpr float ARM_R_WRIST = 0.0215f;
constexpr float ARM_R_ELBOW = 0.030f;
// 腕带（手套腕口）半径 = 前臂腕端 ×1.32，并且**换成深色皮革**。
// 上一版的口径是"靠轮廓上鼓一圈来读，不靠颜色"（手套 0.54 / 袖子 0.50
// 几乎同色）。这一版反过来：袖子压暗到 0.33 之后，手腕处再套一圈 0.15 的
// 深色腕带，于是"手 → 腕带 → 袖子"是一条 **L96 → L30 → L44** 的明度阶梯。
//
// ⚠️ **腕带必须套在腕球的位置上（t≈0）**。第一版把腕带放在腕端往肘 2.2 cm
// 处，结果手套色的小腕球（albedo 0.54）孤零零留在手腕上，渲染出来是画面
// 下方**最亮的一块**（实测 RGB 254 那一档的邻居），读成"金属护腕"；
// 而深色腕带紧贴在它下面、压在同样深的袖子上，等于不存在。
// 判据：识别色图里腕球与腕带都是"手套绿"，**分不出来** —— 只能靠
// "正常图手腕处是不是比手还亮"来判断，别指望染色图。
constexpr float CUFF_R_MUL = 1.32f;
// 袖褶：手臂 62% 处套一圈比前臂粗 13% 的短管，用来**打断那根等宽长管**。
// 一根从腕一直延伸到画面外的等宽圆柱，读出来是"木头 / 管子"；加一道褶，
// 它才变成"布"。这是最便宜的一笔 —— 一行几何体换掉整个"原木"读感。
constexpr float FOLD_AT = 0.62f;
constexpr float FOLD_R_MUL = 1.13f;

// 颜色统一走 sRGB→线性。Godot 的 albedo 在线性空间，直接把选色直觉
// （sRGB 的 0.30）填进去会平白亮一大截。
Color SRGB(float r, float g, float b) {
    return Color(r, g, b).srgb_to_linear();
}

bool env_vec3(const char *name, Vector3 &out) {
    const char *v = std::getenv(name);
    if (v == nullptr || *v == '\0') return false;
    float a = 0.0f, b = 0.0f, c = 0.0f;
    if (std::sscanf(v, "%f,%f,%f", &a, &b, &c) != 3) return false;
    out = Vector3(a, b, c);
    return true;
}

float env_f(const char *name, float def) {
    const char *v = std::getenv(name);
    if (v == nullptr || *v == '\0') return def;
    return (float)std::strtod(v, nullptr);
}

// 本工程所有旋钮的统一语义：**不设 = 默认；显式给了合法值才覆盖**。
// 越界（写错了）退回默认，而不是照一个荒唐的值算下去 —— 视图模型这一块
// 一个越界的数就能让枪整个撞进近裁剪面，而症状（画面上一块黑板）跟"枪的
// 位置不对"看不出关系，排查代价很高。
float env_f_clamped(const char *p_name, float p_def, float p_lo, float p_hi) {
    const char *e = std::getenv(p_name);
    if (e == nullptr || *e == '\0') return p_def;
    const float v = (float)std::strtod(e, nullptr);
    return (v >= p_lo && v <= p_hi) ? v : p_def;
}

// 一盏"只管枪模"的平行光。travel 是光的传播方向（即节点的 -Z 轴）。
//
// 为什么不用 OmniLight：全向灯是点光源，枪托离它 0.48 m、护木 0.53 m，
// 单个面上 NdotL 从 0.86 掉到 0.44 —— 实测同一款材质，枪托顶面 RGB 218、
// 护木侧面 RGB 5，差了 40 倍，整把枪糊成一团黑白剪影。
// 平行光没有距离衰减，且多给几个方向之后任何朝向的面都至少被一两盏照到，
// 对比度立刻回到可读范围。这也是使命召唤/UE 里 viewmodel 独立打光的常规做法。
DirectionalLight3D *vm_dir_light(Node3D *parent, const Vector3 &travel, const Color &c, float energy) {
    DirectionalLight3D *dl = memnew(DirectionalLight3D);
    dl->set_transform(Transform3D(Basis::looking_at(travel.normalized(), Vector3(0, 1, 0)), Vector3()));
    dl->set_color(c);
    dl->set_param(Light3D::PARAM_ENERGY, energy);
    dl->set_shadow(false);
    dl->set_cull_mask(VM_LAYER);
    parent->add_child(dl);
    return dl;
}

Ref<StandardMaterial3D> vm_mat(const Color &srgb_albedo, float metallic, float roughness) {
    Ref<StandardMaterial3D> m;
    m.instantiate();
    m->set_albedo(SRGB(srgb_albedo.r, srgb_albedo.g, srgb_albedo.b));
    m->set_metallic(metallic);
    m->set_roughness(roughness);
    // 枪模不接收世界阴影：离相机太近、阴影贴图精度不够会出条纹，
    // 而且手臂与枪身互相自阴影比没有阴影更难看。
    m->set_flag(BaseMaterial3D::FLAG_DONT_RECEIVE_SHADOWS, true);
    return m;
}

// VA_VM_MAT=1 用的**识别色**材质：不受光（unshaded）。
//
// 【为什么不受光才是对的】识别色的全部价值是"像素颜色 → 哪个部件"的一一对应。
// 一旦它被打光，纯绿会被五盏灯的高光洗成惨白偏黄，"手套=绿"这条判据当场作废：
// 实测有一次，手套明明在画面上占了 5000+ px，按 `g > r+40 且 g > b+40` 的数法
// 却只数出 680 px —— 于是得出了"手还是看不见"的错误结论，差点把量好的握持点推翻。
// 颜色必须由材质常量决定，不能由光照决定。
//
// 判断形状与明暗仍然看**正常材质**那张图：识别色只回答"这块是谁"。
Ref<StandardMaterial3D> id_mat(const Color &c) {
    Ref<StandardMaterial3D> m;
    m.instantiate();
    m->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
    m->set_albedo(c);
    m->set_flag(BaseMaterial3D::FLAG_DONT_RECEIVE_SHADOWS, true);
    return m;
}

Ref<StandardMaterial3D> vm_glow(const Color &c, float energy) {
    Ref<StandardMaterial3D> m;
    m.instantiate();
    m->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
    m->set_albedo(c);
    m->set_emission(c);
    m->set_emission_energy_multiplier(energy);
    m->set_flag(BaseMaterial3D::FLAG_DONT_RECEIVE_SHADOWS, true);
    return m;
}

// ---- 真模型材质的标定旋钮（只读一次）----
//
// 为什么需要三个旋钮而不是一个：图生3D 给出的 PBR 材质是"给离线渲染器看的"
// （albedo 纯白、metallic 1.0、roughness 1.0），而这套光照是为程序化枪模
// 手写标定出来的，两边对不上。三个参数各自解决一个症状：
//
// VA_VM_ART_ALB（默认 0.42）：albedo 乘数。
//   症状：过曝成纯白。来历是照枪的五盏灯按程序化枪模那套**很深的**
//   albedo（0.17 sRGB）扫描标定到 3.8 能量，而真模型贴图是正常枪械色。
//   为什么压 albedo 而不是压灯：灯是枪模与**那两块程序化前臂**共用的，
//   压灯会把前臂一起变暗；外观切换只该影响枪本身。
//   允许 >1（上限 2.0）：贴图本身偏暗时还要往上抬。
//
//   【0.42 是量出来的，不是拍的】用 tools/vm_art_probe.py：同一场景同一秒、
//   只改这一个系数拍若干张，用**差分**取掩码（背景一模一样，变的一定是枪），
//   再在同一掩码上统计亮度分布（莫辛，ROI 900×549）：
//
//     系数   中位亮   死黑<20   正常70-110   过曝>200
//     0.42    131      1.1%       27.9%       15.5%   ← 采用
//     0.55    158      0.3%       14.9%       22.5%
//     0.80    200      0.1%        2.3%       50.3%
//     1.00    226      0.1%        0.9%       78.4%
//
//   单调，所以判据是"在哪一档开始崩"：0.80 起高光成片顶到白（一半以上像素
//   过曝），1.00 时枪机与拉机柄已经糊成白块。0.42 / 0.55 都在可用区，
//   取 0.42 是因为中调占比高一倍、死黑仍只有 1.1%（远低于 10% 上限）；
//   三把枪逐一对照确认 DP-27 那种深蓝钢也没塌黑。
//   ⚠️ 差分掩码天然偏向"两档差得多的像素"（偏亮的），所以**绝对占比不要与
//   tools/vm_eval.py 的数字直接比** —— 它只用于相对比较。
//
// VA_VM_ART_METAL（默认 0.12）：metalness 强制值。
//   症状：高金属度 + 场景里没有反射探针 = 镜面路径采不到环境，只剩黑。
//   本文件里程序化枪身那段注释记过同一个实测（metallic 0.78 的导轨
//   显示 RGB(0,1,10)，整条直接消失）。
//
// VA_VM_ART_ROUGH（默认 0.40）：roughness 强制值。
//   为什么不能留 1.0：roughness=1 的高光被完全摊平，金属件退化成一块
//   没有体积感的塑料；压到 0.4 才有窄高光把圆柱面（枪管、弹鼓）的
//   曲面感拉回来。这三档与程序化枪身那几档材质是同一个思路。
float art_albedo_scale() {
    static const float s = [] {
        const char *e = std::getenv("VA_VM_ART_ALB");
        const float v = (e != nullptr && *e != '\0') ? (float)std::strtod(e, nullptr) : 0.42f;
        return (v > 0.01f && v <= 2.0f) ? v : 0.42f;
    }();
    return s;
}

float art_metal() {
    static const float s = [] {
        const char *e = std::getenv("VA_VM_ART_METAL");
        const float v = (e != nullptr && *e != '\0') ? (float)std::strtod(e, nullptr) : 0.12f;
        return (v >= 0.0f && v <= 1.0f) ? v : 0.12f;
    }();
    return s;
}

float art_roughness() {
    static const float s = [] {
        const char *e = std::getenv("VA_VM_ART_ROUGH");
        const float v = (e != nullptr && *e != '\0') ? (float)std::strtod(e, nullptr) : 0.40f;
        return (v > 0.0f && v <= 1.0f) ? v : 0.40f;
    }();
    return s;
}

// VA_VM_ART_QUIET=1 关掉材质参数的一次性打印。
bool art_report_enabled() {
    static const bool s = (std::getenv("VA_VM_ART_QUIET") == nullptr);
    return s;
}

// VA_DBG_VM=1：视图模型的时序取证（每帧 dt = 帧率，以及每次枪口焰亮了几帧）。
bool dbg_vm() {
    static const bool s = (std::getenv("VA_DBG_VM") != nullptr);
    return s;
}

// VA_VM_HANDS=0 藏掉双手（消融对照用）。**默认显示**。
//
// 语义记牢：是"0 才关"，不是"存在就开"。原先写反了（`getenv != nullptr` = 开），
// 于是默认藏、要看手得显式 VA_VM_HANDS=1 —— 结果就是第一人称画面里
// **只有一把枪悬在空中**。第一人称里"没有手"是最容易被一眼读出来的破绽：
// 枪的朝向、后坐、摆动都做对了，玩家还是会立刻觉得假。
bool hands_enabled() {
    static const bool s = [] {
        const char *e = std::getenv("VA_VM_HANDS");
        return !(e != nullptr && e[0] == '0' && e[1] == '\0');
    }();
    return s;
}

// ---- 真手模（图生3D 产物）----
//
// VA_VM_HAND_ART=0 关掉真手模、回到程序化图元（消融对照用）。**默认开**。
// 语义与 VA_VM_HANDS 一致：只有显式 "0" 才算关（本工程统一约定）。
//
// 【为什么要两个开关，而不是一个】它们回答的是**两个不同的问题**：
//   VA_VM_HANDS     → "这双手画不画"（玩家有没有手）
//   VA_VM_HAND_ART  → "手是程序化图元还是真模型"（真模型到底改了什么）
// 合成一个开关之后，"手不见了"这个症状就有两种成因且分不开 ——
// 而本工程对归属类问题的口径是"一次消融只动一个变量"。
//
// 回退链与枪模/载具/角色同构：**真模型 → 程序化图元**。
// 少一个 .glb 不该让玩家手里没手。
bool hand_art_enabled() {
    static const bool s = [] {
        const char *e = std::getenv("VA_VM_HAND_ART");
        return !(e != nullptr && e[0] == '0' && e[1] == '\0');
    }();
    return s;
}

// ---- 真手模的材质旋钮（只读一次）----
//
// ⚠️ **与上面那三个武器旋钮的单位不一样，别混**：
//   VA_VM_ART_ALB   是**线性空间的标量乘数**（默认 0.42）—— 它是标定出来的补偿量；
//   VA_VM_HAND_TINT 是**sRGB 的色相**（默认 0.71,0.61,0.43）—— 它是一项美术选择。
// 不统一的理由：枪那边只需要"整体压暗"，一个标量就够；手这边要的是
// "把皮肤改成皮革色"，必须按通道给、而且应当按人眼习惯的 sRGB 给。
// 混用的后果是标定值不可移植（同一串数字在两个旋钮上差 2 倍多）。
//
// 【为什么手只乘色调、不像枪那样压成一个灰】参考图是**空手握拳**的实拍
// （取图与选图口径见 ref/vm/SOURCES.md），贴图上带着指节褶皱、掌纹、指缝阴影 ——
// 这些细节是"这是一只手而不是一块肉色几何体"的**全部**信息来源。
// 枪那边把 albedo 压成标量是为了消过曝，代价是丢掉贴图细节；
// 手不能付这个代价，所以改成乘一个色相：既把皮肤改成皮革色、又原样留住细节。
//
// 默认值不是拍的，是照"与现有程序化手套同档"反推的起点：
//   参考图皮肤 ≈ sRGB(0.90,0.71,0.63)；程序化手套的 albedo 是 (0.540,0.410,0.173)。
//   按通道相除得 ≈ (0.60,0.58,0.27) —— 蓝要压得比红绿狠得多，因为手套是**皮革**，
//   而皮肤在蓝通道上高出一大截。这里先取往红绿再收一点的起点，
//   实际定档用 tools/vm_hand_probe.py 扫（口径见该工具的文件头）。
//   目标读数：手套那一行的中位亮度落在现有程序化手套附近（L96 RGB(119,90,83)）。
//
// 【2026-09-24 重标：手模换成"土黄棉手套"之后，这个乘数变小了】
// 上面那套推理的前提是"**贴图是皮肤色**，要把它乘成皮革棕"。新模型（文生3D
// 生成的棉手套）**贴图本身就是深赭黄的粗棉布** —— 再乘 0.50,0.42,0.36 是
// **二次着色**，量出来中位亮度只有 L90 RGB(115,85,72)，画面里读成"深棕皮手套"
// 而不是棉手套。所以改成**接近不变、只轻微提亮偏黄**。
//
// ⚠️ **"越亮越像棉布"是错的**：亮到某个点上，布纹会被冲掉，反而读成"光滑乙烯基"。
// 判据因此不是"够不够亮"，而是**亮面仍留得住布纹**。1.55 倍缩放下看腕口那圈
// 罗纹编织：0.80 档已经糊平，0.71 档还看得见。
//
// 四档扫描（同一机位、同一 ROI x1500-1700,y1000-1160 手套受光面，逐通道中位）：
//   0.56,0.49,0.38  →  RGB(129, 97, 83)  L103   灰褐，偏脏
//   0.63,0.56,0.42  →  RGB(157,115, 88)  L122   浅卡其，饱和度不够、发白
//   0.71,0.61,0.43  →  RGB(193,133, 94)  L142   ← 采用：最贴"土黄"，腕口布纹仍在
//   0.80,0.68,0.45  →  RGB(233,162,102)  L173   金黄偏琥珀，亮面纹理被冲掉
// （另试过 0.95,0.82,0.55 → 过亮发白偏淡黄。参考：场上的草地约 RGB(172,170,129)
//   L167 —— 采用档的手**比背景草略暗但色调分离明确**，不会糊进环境。）
// 教训记一笔：**"色调乘数"只在"贴图是另一种材质"时成立**；贴图已经是对的颜色时，
// 同一个数就从"补偿"变成了"污染"。换件之后要重新量，不能沿用。
Vector3 hand_tint() {
    static const Vector3 s = [] {
        const char *e = std::getenv("VA_VM_HAND_TINT");
        float a = 0.71f, b = 0.61f, c = 0.43f;
        if (e != nullptr && *e != '\0') std::sscanf(e, "%f,%f,%f", &a, &b, &c);
        return Vector3(va::clampf(a, 0.02f, 2.0f), va::clampf(b, 0.02f, 2.0f),
                       va::clampf(c, 0.02f, 2.0f));
    }();
    return s;
}

float hand_metal() {
    static const float s = [] {
        const char *e = std::getenv("VA_VM_HAND_METAL");
        const float v = (e != nullptr && *e != '\0') ? (float)std::strtod(e, nullptr) : 0.0f;
        return (v >= 0.0f && v <= 1.0f) ? v : 0.0f;
    }();
    return s;
}

// 默认 0.88：皮革手套是哑光的，和程序化手套那档（0.92）同量级。
// 别往低里调去"救"它 —— 低粗糙度会在指节这种小而圆的面上打出一堆亮点，
// 读出来是"塑料手"。要提亮应该走 VA_VM_HAND_TINT。
float hand_rough() {
    static const float s = [] {
        const char *e = std::getenv("VA_VM_HAND_ROUGH");
        const float v = (e != nullptr && *e != '\0') ? (float)std::strtod(e, nullptr) : 0.88f;
        return (v > 0.0f && v <= 1.0f) ? v : 0.88f;
    }();
    return s;
}

// ---- 枪口焰的时长 ----
//
// 时长默认 0.045 秒（约等于真枪的两次曝光，视觉上就是"一闪"）。
// VA_VM_FLASH=<秒> 覆盖它，**只为取证**：0.045 秒比本工程三种取证手段的
// 最小粒度都短 —— 事件落盘要等 0.05 秒墙钟、定时截图按战局秒数触发，
// 所以"枪口焰长什么样"本来是一张**注定拍不到**的图（这是"判据必须小于
// 被取证据的寿命"那条铁律的又一个实例）。取证时用 VA_VM_FLASH=5 按住它，
// 拍完就扔；游戏里永远用默认值。
float flash_sec() {
    static const float s = [] {
        const char *e = std::getenv("VA_VM_FLASH");
        const float v = (e != nullptr && *e != '\0') ? (float)std::strtod(e, nullptr) : 0.045f;
        return (v >= 0.005f && v <= 60.0f) ? v : 0.045f;
    }();
    return s;
}

// 枪口自发光几何 / 点光的满亮强度。跟着上面的时长一起用，不再散在字面量里。
constexpr float FLASH_ENERGY = 7.0f;

void vm_layer(MeshInstance3D *mi) {
    mi->set_layer_mask(VM_LAYER);
    mi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
}

// ---- 真模型材质的"配方" ----
// 光照隔离 + 材质标定这套道理，枪与手**完全一样**（同一盏灯、同一个层、
// 同样要躲开阴影条纹、同样要覆盖图生3D 给的 PBR 默认值），差别只在三个参数
// 与日志标签。所以走同一个函数、只换配方 —— 分成两份实现的话，
// 下次给第三个部件（比如刺刀、弹匣）接真模型时一定会漏掉其中一处。
struct VmMatCfg {
    const char *tag;       // 日志标签（同时也是报告去重的键）
    Color       tint;      // albedo 乘数（**线性空间**，直接乘在贴图上）
    float       metal;
    float       rough;
    // 关背面剔除。⚠️ 负缩放（左右镜像）的子树**必须**开这个：
    // 负行列式会翻转三角形绕序，Godot 不会自动纠正，不关的话手掌会被
    // 剔成"半透明"、直接看穿到内壁。
    bool        cull_off;
    // 清掉生成器给的 roughness / metallic **贴图**，让上面那两个标量说了算。
    //
    // 【为什么必须清】Godot 的 `set_roughness()` 是**乘数**，不是赋值：
    // 模型自带 roughness 贴图时，最终 roughness = 标量 × 贴图。图生3D 给的是
    // 给离线渲染器看的 PBR，贴图里大量区域接近 0，于是标量写 0.88 也照样出来
    // 一层镜面高光 —— 实测手模渲染成"抛光皮手套 / 乳胶"，症状与"标量太小"
    // 一模一样，但调标量永远调不好（0.88 → 1.0 几乎没变）。
    // 枪那边不开这个：它的三个参数是在**带贴图**的前提下逐档扫出来的，
    // 中途改口径会让那套标定数字全部作废。
    bool        clear_pbr_maps;
};

// 把整棵子树挪到第一人称专用渲染层、关阴影接收、按配方改写材质。
//
// 【为什么必须做第一步】真模型自带 MeshInstance3D，默认落在 layer 1。
// 而本文件照第一人称模型的五盏平行光 cull_mask 只到 VM_LAYER —— 留在默认层
// 的话它们照不到，世界光却照得到：玩家一转身背对太阳，手里的枪与手就塌成
// 黑剪影，正是文件头"光照隔离"那一段要避免的现象。角色模型不需要这一步，
// 因为角色本来就该被世界光照亮。
//
// 【材质是共享的】duplicate 不会深拷贝 Mesh / 材质，所以这里改的是被缓存的
// 那一份 —— 无所谓：第一人称的模型只有视图模型在用。
//
// 【为什么还要改材质本身】真模型带的是图生3D 给的 PBR 材质，直接上会踩三个坑
// （每个坑对应配方里一个参数，实测数据写在各自旋钮的注释里）：
//   1) **过曝**：那五盏灯的色相与能量是按**程序化**模型的 albedo 标定出来的，
//      真模型贴图亮得多，同一套灯照上去就整片顶到纯白。
//   2) metallic 高：缺反射探针时镜面路径采不到环境，只剩黑。
//      本文件里程序化枪身那段注释记过同一个实测（metallic 0.78 的导轨显示
//      RGB(0,1,10)，整条直接消失）。
//   3) roughness 1.0：高光被完全摊平，圆柱面（枪管、弹鼓、指节）
//      退化成没有体积感的塑料片。
// 处理办法是改写这三个参数，而不是去改灯：改灯会连带把**另一类**部件一起变暗
// （灯是枪与手共用的），而外观切换只该影响它自己。
//
// p_reported 由调用方持有（每份配方一个），用来"只打一次原始参数" ——
// 这是标定的依据，看过一眼就够，每次加载都刷会淹掉别的日志。
//
// p_id_override 非空时（VA_VM_MAT=1）整块换成识别色：走 material_override
// 而不是改材质本体，因为本体是共享的、改了等于连原型一起改。
void adopt_vm_subtree(Node *p_node, const VmMatCfg &p_cfg, bool &p_reported,
                      const Ref<Material> &p_id_override) {
    MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_node);
    if (mi != nullptr) {
        vm_layer(mi);
        if (p_id_override.is_valid()) {
            mi->set_material_override(p_id_override);
        } else {
            Ref<Mesh> mesh = mi->get_mesh();
            if (mesh.is_valid()) {
                const int ns = mesh->get_surface_count();
                for (int i = 0; i < ns; ++i) {
                    StandardMaterial3D *sm =
                        Object::cast_to<StandardMaterial3D>(mesh->surface_get_material(i).ptr());
                    if (sm == nullptr) continue;
                    // 模型离相机只有几十厘米，阴影贴图的精度远远不够，
                    // 蹭上一点就是一道跟着视角爬的条纹，比没有阴影难看得多。
                    sm->set_flag(BaseMaterial3D::FLAG_DONT_RECEIVE_SHADOWS, true);
                    if (p_cfg.cull_off) sm->set_cull_mode(BaseMaterial3D::CULL_DISABLED);

                    if (!p_reported && art_report_enabled()) {
                        p_reported = true;
                        UtilityFunctions::print(String::utf8(p_cfg.tag), String::utf8(" #"), i,
                                                String::utf8(" albedo="), sm->get_albedo(),
                                                String::utf8(" metallic="), sm->get_metallic(),
                                                String::utf8(" roughness="), sm->get_roughness(),
                                                String::utf8(" 贴图 反照="),
                                                sm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO).is_valid(),
                                                String::utf8(" 粗糙="),
                                                sm->get_texture(BaseMaterial3D::TEXTURE_ROUGHNESS).is_valid(),
                                                String::utf8(" 金属="),
                                                sm->get_texture(BaseMaterial3D::TEXTURE_METALLIC).is_valid(),
                                                String::utf8(" 法线="),
                                                sm->get_texture(BaseMaterial3D::TEXTURE_NORMAL).is_valid());
                    }

                    if (p_cfg.clear_pbr_maps) {
                        sm->set_texture(BaseMaterial3D::TEXTURE_ROUGHNESS, Ref<Texture2D>());
                        sm->set_texture(BaseMaterial3D::TEXTURE_METALLIC, Ref<Texture2D>());
                    }

                    // 三个参数**恒定覆盖**，不做条件判断：原始参数是图生3D
                    // 给离线渲染器的（1.0 / 1.0），留哪一项都会留一个坑。
                    // alpha 钉 1.0 —— 透明度没开，但带着小于 1 的 alpha
                    // 会让下一次读参数时误判。
                    sm->set_albedo(Color(p_cfg.tint.r, p_cfg.tint.g, p_cfg.tint.b, 1.0f));
                    sm->set_metallic(p_cfg.metal);
                    sm->set_roughness(p_cfg.rough);
                }
            }
        }
    }
    const int nc = p_node->get_child_count();
    for (int i = 0; i < nc; ++i) {
        adopt_vm_subtree(p_node->get_child(i), p_cfg, p_reported, p_id_override);
    }
}

// 枪模那份配方（值 = 上面三个武器旋钮；标定过程写在它们的注释里）。
void adopt_gun_subtree(Node *p_node) {
    static bool s_reported = false;
    VmMatCfg cfg;
    cfg.tag = "[vm] 真模型材质";
    const float a = art_albedo_scale();
    cfg.tint = Color(a, a, a, 1.0f);
    cfg.metal = art_metal();
    cfg.rough = art_roughness();
    cfg.cull_off = false;
    cfg.clear_pbr_maps = false;   // 枪的三个参数是"带贴图"扫出来的，别改口径
    adopt_vm_subtree(p_node, cfg, s_reported, Ref<Material>());
}

// 手模那份配方。
void adopt_hand_subtree(Node *p_node, bool p_dbg_mat) {
    static bool s_reported = false;
    VmMatCfg cfg;
    cfg.tag = "[vm] 手模材质";
    const Vector3 t = hand_tint();
    // 手这边按 sRGB 给色相（见 hand_tint 的注释：单位与 VA_VM_ART_ALB 不同）。
    cfg.tint = SRGB(t.x, t.y, t.z);
    cfg.metal = hand_metal();
    cfg.rough = hand_rough();
    cfg.cull_off = true;          // 左手是负缩放镜像来的，必须关剔除
    cfg.clear_pbr_maps = true;    // 见 VmMatCfg::clear_pbr_maps
    // VA_VM_MAT=1 时手也要变成"手套绿"：tools/vm_hand_probe.py 是按
    // "绿=手 / 蓝=袖 / 红=枪"分类像素的，不给真手模一个识别色，
    // 那套口径会把整只手算进"枪身"那一行 —— 于是"手有多大、多亮"
    // 两个数一起失真，而且失真的方向恰好是"让人以为手没问题"。
    Ref<Material> idov;
    if (p_dbg_mat) idov = id_mat(Color(0, 1, 0));
    adopt_vm_subtree(p_node, cfg, s_reported, idov);
}

MeshInstance3D *part(Node3D *parent, const Ref<Mesh> &mesh, const Vector3 &pos,
                     const Vector3 &rot_deg, const Ref<Material> &mat) {
    MeshInstance3D *mi = memnew(MeshInstance3D);
    mi->set_mesh(mesh);
    mi->set_position(pos);
    mi->set_rotation_degrees(rot_deg);
    mi->set_material_override(mat);
    vm_layer(mi);
    parent->add_child(mi);
    return mi;
}

Ref<BoxMesh> box(float x, float y, float z) {
    Ref<BoxMesh> b = memnew(BoxMesh);
    b->set_size(Vector3(x, y, z));
    return b;
}

// ---- 圆润基元：手用它们，枪用 BoxMesh ----
//
// 【为什么手不能再用方块】掌与指原来都是 BoxMesh。换真模型之后手离眼睛只有
// 0.24~0.42 m，一块 5×7 cm 的方板在这个距离上占 ~190 px，**三条棱各自受不同
// 方向的灯**，于是画面上一眼就是"一摞棕色积木"。这不是尺寸调得不对：
// 盒体在这个尺度下的问题是没有连续法线 —— 相邻两个面的亮度是断的，
// 大脑读到的就是"两个物体"。椭球/胶囊的平滑法线让明暗连续过渡，
// 同样的轮廓立刻就变成"一块肉/一只手套"。代价只是分段数。
MeshInstance3D *part_mi(Node3D *parent, const Ref<Mesh> &mesh, const Vector3 &half,
                        const Vector3 &pos, const Vector3 &rot_deg, const Ref<Material> &mat) {
    MeshInstance3D *mi = memnew(MeshInstance3D);
    mi->set_mesh(mesh);
    mi->set_scale(half);          // Godot 的节点变换是 T·R·S，缩放先于旋转生效
    mi->set_position(pos);
    mi->set_rotation_degrees(rot_deg);
    mi->set_material_override(mat);
    vm_layer(mi);
    parent->add_child(mi);
    return mi;
}

// 椭球：单位球按半轴 half 缩放。半径 1 / 高 2 = 标准单位球。
MeshInstance3D *ball(Node3D *parent, const Vector3 &half, const Vector3 &pos,
                     const Vector3 &rot_deg, const Ref<Material> &mat) {
    Ref<SphereMesh> s = memnew(SphereMesh);
    s->set_radius(1.0f);
    s->set_height(2.0f);
    s->set_radial_segments(16);
    s->set_rings(8);
    return part_mi(parent, s, half, pos, rot_deg, mat);
}

// 胶囊：一根手指 / 一段拇指。h 是**含两端半球**的总长（引擎要求 h >= 2r）。
MeshInstance3D *cigar(Node3D *parent, float r, float h, const Vector3 &pos,
                      const Vector3 &rot_deg, const Ref<Material> &mat) {
    Ref<CapsuleMesh> c = memnew(CapsuleMesh);
    c->set_radius(r);
    c->set_height(std::max(h, r * 2.0f));
    c->set_radial_segments(12);
    c->set_rings(4);
    return part_mi(parent, c, Vector3(1, 1, 1), pos, rot_deg, mat);
}

Ref<CylinderMesh> tube(float r, float h, int seg = 10) {
    Ref<CylinderMesh> c = memnew(CylinderMesh);
    c->set_top_radius(r);
    c->set_bottom_radius(r);
    c->set_height(h);
    c->set_radial_segments(seg);
    c->set_rings(1);
    return c;
}

// 在两点之间架一根圆柱（枪管等静态件用）。
// CylinderMesh 的轴是局部 +Y，所以要构造一个把 +Y 旋到 a→b 方向的基。
// 手写旋转角很容易把前臂摆成"立起来的柱子"（第一版就是这么错的）。
Basis basis_along(const Vector3 &a, const Vector3 &b) {
    const Vector3 dir = (b - a).normalized();
    const Vector3 up(0.0f, 1.0f, 0.0f);
    const float dp = va::clampf(up.dot(dir), -1.0f, 1.0f);
    if (dp > 0.9999f) {
        return Basis();
    } else if (dp < -0.9999f) {
        return Basis(Vector3(1, 0, 0), 3.141592653589793f);
    }
    return Basis(up.cross(dir).normalized(), std::acos(dp));
}

// **带锥度**的圆柱：+Y 端半径 r_top、-Y 端半径 r_bot。
// 【为什么前臂一定要有锥度】一根等径圆柱在这个视角下读不出"胳膊"，读得出的是
// "管子/棍"。真人前臂在腕处约 5 cm、往上到肘附近涨到 8~9 cm，这一条收分就是
// 大脑判定"这是手臂"的主要线索之一。程序化枪模最早那版前臂就是等径的，
// 在放大图里和一根撑杆没有区别。
Ref<CylinderMesh> tube_taper(float r_top, float r_bot, float h, int seg = 10) {
    Ref<CylinderMesh> c = memnew(CylinderMesh);
    c->set_top_radius(r_top);
    c->set_bottom_radius(r_bot);
    c->set_height(h);
    c->set_radial_segments(seg);
    c->set_rings(1);
    return c;
}

// 把一根前臂摆到"腕 a → 肘 b"。**每次换枪都要重摆**。
void set_forearm(MeshInstance3D *p_mi, const Vector3 &a, const Vector3 &b,
                 float r_wrist, float r_elbow) {
    if (p_mi == nullptr) return;
    const float len = (b - a).length();
    if (len < 1e-4f) {
        p_mi->set_visible(false);
        return;
    }
    p_mi->set_visible(true);
    // 柱体的 +Y 端指向 b（肘），所以"顶端粗、底端细"= 肘粗腕细。
    p_mi->set_mesh(tube_taper(r_elbow, r_wrist, len, 10));
    p_mi->set_transform(Transform3D(basis_along(a, b), (a + b) * 0.5f));
}

// 手臂上的"一圈"：在腕 a → 肘 b 之间 t 处套一段短管（腕带 / 袖褶都用它）。
// t 是**沿手臂长度的比例**，不是米数 —— 换枪时腕端会挪（莫辛 -0.45、
// DP-27 -0.62），比例口径自动跟着走，不必每把枪重算一圈的位置。
void set_ring(MeshInstance3D *p_mi, const Vector3 &a, const Vector3 &b,
              float r, float t, float len) {
    if (p_mi == nullptr) return;
    const Vector3 d = b - a;
    if (d.length() < 1e-4f) {
        p_mi->set_visible(false);
        return;
    }
    p_mi->set_visible(true);
    p_mi->set_mesh(tube(r, len, 10));
    p_mi->set_transform(Transform3D(basis_along(a, b), a + d * t));
}

// 腕口那一圈（深色腕带）。它比前臂略粗、只有 5 cm 长，摆在腕端往肘一侧一点。
// 参考图里"手套的腕口压在袖子上"是读出手腕最省笔墨的一笔：只有它，
// 前臂与手掌之间才有"关节"而不是两段直接焊在一起。
void set_cuff(MeshInstance3D *p_mi, const Vector3 &a, const Vector3 &b, float r) {
    if (p_mi == nullptr) return;
    const float dl = (b - a).length();
    if (dl < 1e-4f) {
        p_mi->set_visible(false);
        return;
    }
    set_ring(p_mi, a, b, r, 0.008f / dl, 0.056f);
}

} // namespace

// 按当前 cur_hand_r / cur_hand_l 重摆两条前臂与腕口。
// 换枪后必须调一次：每把枪的支撑手握点在护木上的位置差得很远
// （莫辛 z=-0.45、波波沙 -0.48、DP-27 -0.62），腕端要跟着走，肘不动。
void ViewModel::place_arms() {
    if (hands == nullptr) return;
    // 开镜缩到看不清了就把整组藏掉（见 viewmodel.h 里 hand_ads_shrink 的注释）：
    // 门限取 0.03 而不是 0 —— **缩放 0 是退化变换**（基的行列式为 0），
    // Godot 会给出 NaN 法线并把网格渲染成一片乱刺；而在 3% 尺寸上做一次显隐，
    // 玩家看到的只是"手已经没了"，不是"手突然消失了"。
    if (hand_pose_scale < 0.03f) {
        if (hands->is_visible()) hands->set_visible(false);
        return;
    }
    if (!hands->is_visible()) hands->set_visible(hands_enabled());
    const float rw = env_f_clamped("VA_VM_ARM_W", ARM_R_WRIST, 0.008f, 0.080f);
    const float re = env_f_clamped("VA_VM_ARM_E", ARM_R_ELBOW, 0.008f, 0.090f);
    // 双手的"姿态让位"（开镜时按 ads 让开照门，见 update() 里那一段）：
    // **手掌与腕端必须带同一个位移**。只让手不让腕，腕口圆柱与手掌之间会裂开一个断口，
    // 而开镜时那个断口正对着相机（画面中央），比手挡照门还难看。
    const Vector3 off = hand_pose_off;
    if (hand_r != nullptr) {
        hand_r->set_position(cur_hand_r - HAND_R_BASE + off);
        hand_r->set_scale(Vector3(hand_pose_scale, hand_pose_scale, hand_pose_scale));
    }
    if (hand_l != nullptr) {
        hand_l->set_position(cur_hand_l - HAND_L_BASE + off);
        hand_l->set_scale(Vector3(hand_pose_scale, hand_pose_scale, hand_pose_scale));
    }
    const Vector3 wr = cur_hand_r + WRIST_R_OFF + off;
    const Vector3 wl = cur_hand_l + WRIST_L_OFF + off;
    set_forearm(fore_r, wr, ELBOW_R, rw, re);
    set_forearm(fore_l, wl, ELBOW_L, rw, re);
    set_cuff(cuff_r, wr, ELBOW_R, rw * CUFF_R_MUL);
    set_cuff(cuff_l, wl, ELBOW_L, rw * CUFF_R_MUL);
    set_ring(fold_r, wr, ELBOW_R, rw * FOLD_R_MUL, FOLD_AT, 0.034f);
    set_ring(fold_l, wl, ELBOW_L, rw * FOLD_R_MUL, FOLD_AT, 0.034f);
}

void ViewModel::build(Camera3D *p_cam, Node *p_proto_parent) {
    if (p_cam == nullptr) return;
    cam = p_cam;
    proto_parent = p_proto_parent;

    // 开发期旋钮：不动代码就能扫姿态。VA_VM_HIP / VA_VM_AIM / VA_VM_ROT。
    env_vec3("VA_VM_HIP", hip_pos);
    aim_env_over = env_vec3("VA_VM_AIM", aim_pos);
    hip_env_over = env_vec3("VA_VM_ROT", hip_rot);

    // 腰射"枪口指向"的收敛（见 viewmodel.h 里 conv_d 那一节）。
    // 不设 = 25 m；显式给 0 = 关掉收敛、回到 hip_rot 那组手调角度（对照取证用）。
    // 收敛在**每次换枪后**必须重解一次：枪口 z 随枪变，同一个 conv_d 对应的
    // 内收角也就跟着变。
    conv_d = env_f_clamped("VA_VM_CONV", conv_d, 0.0f, 500.0f);
    hip_roll = env_f("VA_VM_ROLL", hip_roll);

    // 视图模型统一缩放。**它和 hip_pos 必须一起调**：
    // 整体乘 s 再把 hip_pos 也乘 s 是一个相似变换，屏幕上分毫不变 ——
    // 真正决定观感的只有"眼睛在枪局部系里的位置" Z0 = -hip_pos.z / vm_scale。
    // s 的作用是让"枪相对人有多大"可变，从而在不撞近裁剪面的前提下把 Z0 压下来。
    vm_scale = env_f_clamped("VA_VM_SCALE", 0.620f, 0.30f, 1.60f);
    // 开镜距离默认 0.270（原 0.200）。**这不是手感问题，是画面占比问题**：
    // 0.200 时托底板落在眼睛前 20 cm 处，它 8.4 cm 高，屏幕高度 1369 px 下占
    // 0.084/(2·0.20·0.6371)·1369 ≈ 820 px —— 开镜画面**下半屏整个被托底糊住**，
    // 连枪自己的机匣都只剩一条。扫过 0.20 / 0.27 / 0.34 三档取证：
    // 0.27 是"托底退到画面下缘之外、枪身仍占满中央"的那一档；0.34 时枪身细成一条。
    // 注意改它**不影响开镜对准** —— 落点是由照门反推的，拉开距离只是等比缩小。
    aim_dist = env_f_clamped("VA_VM_AIMDIST", 0.270f, 0.060f, 0.600f);
    // 开镜时双手的让位量（见 update() 里那一段）：缩小比例 + 位移。
    // 上界放到 1.0（= 缩到无）—— 见 viewmodel.h 里 hand_ads_shrink 的注释：
    // 0.85 这一档在真手模上留下的正是一块"悬空残片"，那是本轮要消掉的东西。
    hand_ads_shrink = env_f_clamped("VA_VM_ADS_HSHRINK", hand_ads_shrink, 0.0f, 1.0f);
    env_vec3("VA_VM_ADS_HOFF", hand_ads_off);

    // 开镜视场由基础视场推导 —— 改世界 FOV 时枪的放大倍率自动跟随。
    fov_base = p_cam->get_fov();
    fov_ads = fov_base * 0.70f;

    root = memnew(Node3D);
    root->set_name("ViewModel");
    // root 只承担"位移 + 动态旋转"，缩放挂在同一层上：
    // root 自己的 set_position 在父空间（相机空间）里，不受自身缩放影响，
    // 所以 pos 那套摆动/后坐的米数不用跟着改。
    root->set_scale(Vector3(vm_scale, vm_scale, vm_scale));
    p_cam->add_child(root);

    gun = memnew(Node3D);
    gun->set_name("Gun");
    root->add_child(gun);

    // VA_VM_MAT=1：给各部件上识别色（枪身红 / 手套绿 / 袖子蓝 / 金属白）。
    // 截图里"哪块几何体是手臂、哪块是枪托"凭肉眼判断已经错过两次，
    // 直接染色是最快的定位手段。
    const bool dbg_mat = std::getenv("VA_VM_MAT") != nullptr;
    // --- 材质标定：这组数值是量化出来的，不是试出来的 ---
    // 方法：拍两张（VA_VM_MAT=1 的识别色图提取掩码 + 正常图取亮度），
    //       用 tools/vm_eval.py 统计各部件亮度分布，再反推照度。
    // 实测：默认灯能量下枪身处照度只有 0.25，枪身中位亮度 19（53% 死黑，糊成一团）；
    //       扫过 1.55 / 3 / 4 / 5 四档能量之后定在 5.0，枪身中位 80、75% 落在正常区间。
    // albedo 按同一照度（≈3.3）反推：
    //     poly  0.185 sRGB → 线性 0.028 → 显示中位 RGB 80   （哑光深灰，COD 的枪就是这个调）
    //     sleeve 0.170     → 袖子和枪身同档，避免前臂在画面里变成一条死黑竖杠
    //     glove 0.160
    //     metal 0.170 + metallic 0.12 → 0.21/0.25 时曾有 8% 过曝，导轨糊成白条
    // metallic 必须压低：金属件在缺少反射探针时镜面路径采不到环境，只剩"黑" ——
    // 实测 0.78 的导轨显示 RGB(0,1,10)，整条直接消失。使命召唤的枪械本身也是哑光，
    // 金属感靠粗糙度与高光，不靠 metallic。
    const Ref<StandardMaterial3D> metal = dbg_mat ? id_mat(Color(1, 1, 1))
        : vm_mat(Color(0.170f, 0.174f, 0.186f), 0.12f, 0.58f);
    const Ref<StandardMaterial3D> poly = dbg_mat ? id_mat(Color(1, 0, 0))
        : vm_mat(Color(0.165f, 0.161f, 0.152f), 0.0f, 0.70f);
    const Ref<StandardMaterial3D> dark = dbg_mat ? id_mat(Color(0.35f, 0.35f, 0.35f))
        : vm_mat(Color(0.092f, 0.090f, 0.086f), 0.0f, 0.62f);
    // 手套与袖子的 albedo 做成旋钮（VA_VM_GLOVE / VA_VM_SLEEVE，sRGB 标量）。
    // 扫它们不会波及枪：程序化枪身有自己那套材质，这两块**只被双手用到**。
    //
    // 【色相为什么从"中性深灰"改成暖褐】这是接真模型之后必须重标的一处。
    // 原先 0.160/0.170 配的是两组几乎中性的灰（手套 1 : 0.95 : 0.875，
    // 袖子 1 : 1.047 : 0.812），在**程序化枪身**（0.165 的深灰）旁边同档，
    // 看起来是"一体的深色装备"。换成真模型之后枪身中位 L143（暖木色 + 冷钢），
    // 而双手实测只有 L52 / L33 —— 手比枪暗 2.7~4.3 倍，再叠上偏蓝的色偏，
    // 于是画面上读出来是"一把枪 + 两根横插进来的树干"，正是玩家报的那个症状。
    //
    // 色偏的来源已经用归因实验排掉了一个想当然的答案：把 VA_AMBIENT 从 1.62 归零，
    // 手套只从 L52 掉到 L49（−5%）、袖子 L33→L30（−9%）—— 所以**不是环境光**，
    // 而是五盏枪模灯里那两盏偏蓝的（冷补光 0.68/0.77/0.95、轮廓光 0.55/0.60/0.68）
    // 正打在"朝相机"的那些面上，而唯一照朝相机面的中性光（第 3 盏轴光）
    // 只有 1.5 能量。修法有两条，选的是第二条：
    //   ① 提轴光（试过，否掉了）：手确实亮了、色偏也修好了，但枪身跟着变
    //      —— 中位 RGB (136,143,168)→(182,141,115)、过曝 25.2%→26.5%。
    //      枪身的亮度是逐档标定出来的，不能让双手把它带跑。详见灯那段的注释。
    //   ② **只动这两个 albedo**（当前方案）：提高标量 + 把色相从近乎中性
    //      改成真正的皮革 / 军装布黄褐，用暖色相抵消剩下的冷偏。
    //      它们只被双手用到，所以改它们对枪的影响恒为 0。
    //
    // 扫的时候仍然别只往亮里调：亮过头袖子会变成横穿画面的一条亮带，
    // 暗过头又变回死黑方板。口径用 tools/vm_hand_probe.py
    // （正常图 + VA_VM_HIDE 消融图 + VA_VM_MAT 识别色图 → 拆出枪身/手套/袖子三行）。
    const float g_alb = va::clampf(env_f("VA_VM_GLOVE", 0.540f), 0.02f, 1.0f);
    // 袖子从 0.500 压到 0.330。**这不是审美问题，是构图问题**：
    // 支撑前臂在腰射画面上是一条 423×150 px 的带子，占屏面积比枪管还大。
    // 上一版它渲染成 L88 的暖卡其，是整个画面下半部**最亮的大块** ——
    // 于是眼睛先看到"一根亮柱子"，再看枪。翻参考图（CSGO / BFV）会发现
    // 那条前臂一律是**暗色剪影**：贴地的暗块，天然往后退。
    // 压暗之后手（L96）与袖子（L~60）拉开一档半，手臂才退回"支撑物"的位置。
    const float s_alb = va::clampf(env_f("VA_VM_SLEEVE", 0.330f), 0.02f, 1.0f);
    // 手套的色相比袖子还要极端 —— 这不是随手加的饱和度，是量出来的：
    // 手套那几个方块**朝相机**的面在蓝通道上的照度远高于红通道，按
    // "正常皮革色"（1 : 0.58 : 0.30）写进去，渲染出来是 RGB(83,60,75) ——
    // 绿分量比蓝还低，成了一种发紫的灰（实测）。
    //
    // ⚠️ 这里**不能按通道线性反推**，会被 ACES 的跨通道混合打脸：实测把蓝 albedo
    // 从 0.187 降到 0.090（只动这一项），渲染出来的红反而从 119 升到 174（+46%）、
    // 绿几乎不动（71→66）。也就是说"降蓝会让红更红"，通道之间是耦合的。
    // 所以色相只能经验扫：先在两个极端之间取中点，再单独扫标量。
    const Ref<StandardMaterial3D> glove = dbg_mat ? id_mat(Color(0, 1, 0))
        : vm_mat(Color(g_alb, g_alb * 0.760f, g_alb * 0.320f), 0.0f, 0.92f);   // 皮革
    // 袖子刻意**不跟枪托同色系**：枪托是暖木色，袖子也做成暖褐的话，画面上就是
    // "两根木条 + 一把木枪"，反而更像板子。所以袖子走军装布卡其（r≈g>b），
    // 与木色拉开色相关系。
    const Ref<StandardMaterial3D> sleeve = dbg_mat ? id_mat(Color(0, 0, 1))
        : vm_mat(Color(s_alb, s_alb * 0.820f, s_alb * 0.400f), 0.0f, 0.95f);   // 军装布
    // 腕带：深色皮革，比袖子还暗两档。dbg 模式下**必须仍旧算"手套绿"** ——
    // tools/vm_hand_probe.py 是按"绿=手 / 蓝=袖 / 红=枪"分类像素的，
    // 给它一个新识别色等于让那套口径失效（腕带会被算成"未知"丢掉）。
    const Ref<StandardMaterial3D> strap = dbg_mat ? id_mat(Color(0, 1, 0))
        : vm_mat(Color(0.150f, 0.138f, 0.120f), 0.04f, 0.88f);

    // 定档实测（莫辛，tools/vm_hand_probe.py；同一口径下改前 → 改后）：
    //     手套  L52 RGB(43,52,74) 偏蓝灰  →  L96 RGB(119,90,83) 皮革棕
    //     袖子  L33 RGB(24,34,45) 近黑    →  L88 RGB(111,84,53) 军装卡其
    //     枪身  L143 过曝 25.2%          →  L143 过曝 25.2%（未动）
    // 扫过的范围：标量 0.30~0.76、色相比 (g : b)/r 从 0.62:0.34 一路试到 0.84:0.44，
    // 两头都不可用 ——
    //   太亮（0.72 配 0.72:0.26）：手套过曝跳到 22.5%、糊成一片亮橙 RGB(243,154,75)；
    //   太灰（0.44 配 0.80:0.38）：渲染成 RGB(79,72,76)，又变回一块灰板。
    // 最终 0.540 配 (1 : 0.760 : 0.320) 是唯一同时满足"读得出是皮革"与
    // "过曝 < 3%"的一档。**中间那些值不必重扫** —— 改色相比就等于同时动了标量
    // （跨通道耦合），所以参数表本身不可插值，只能按"读感 + 过曝"两个判据选点。

    // ------------------------------------------------------------------
    // 分组：枪身 / 双手 / 真模型。
    //
    // 【为什么要分组】武器外观切换要能把"程序化枪身"整块换成"真模型"，
    // 而双手与前臂必须留着 —— 参考图是**光枪**，生成的模型里没有手，
    // 一起藏掉的话画面上就是一把悬空的枪。
    // 分组之后，"换外观"就退化成三个 set_visible，不必记住哪几十个部件属于枪身。
    // ------------------------------------------------------------------
    proc_body = memnew(Node3D);
    proc_body->set_name("ProcBody");
    gun->add_child(proc_body);

    hands = memnew(Node3D);
    hands->set_name("Hands");
    gun->add_child(hands);

    // 再拆左右两组：每把枪的握持点差得很远（见 WpnNodeInfo::hand_r_z），
    // 换外观时要整组沿 z 平移。分组之后"挪手"就是两个 set_position，
    // 不需要逐个记住哪几块几何体属于右手。
    hand_r = memnew(Node3D);
    hand_r->set_name("HandR");
    hands->add_child(hand_r);
    hand_l = memnew(Node3D);
    hand_l->set_name("HandL");
    hands->add_child(hand_l);

    art_holder = memnew(Node3D);
    art_holder->set_name("ArtHolder");
    gun->add_child(art_holder);

    // ------------------------------------------------------------------
    // 枪身。gun 局部原点 = 机匣后端（枪托接口）；-Z 为枪口方向；y=0 是枪管轴线。
    // 枪托向 +Z 伸到 +0.173，枪管向 -Z 伸到 -0.575，总长 0.748 m。
    // ------------------------------------------------------------------
    // 枪托往 +Z 只伸到 +0.134。别小看这 4 cm：开镜时枪托是整把枪离眼睛最近的部分，
    // 它每远 1 cm，屏幕上的面积就小一截。原来伸到 +0.173 时，开镜画面正中下方被
    // 托底板糊掉一大块（实测占屏宽 27%）。
    part(proc_body, box(0.046f, 0.084f, 0.018f), Vector3(0, -0.020f, 0.125f), Vector3(), poly);          // 托底板
    part(proc_body, box(0.042f, 0.064f, 0.108f), Vector3(0, -0.016f, 0.062f), Vector3(), poly);          // 伸缩托
    part(proc_body, box(0.034f, 0.016f, 0.096f), Vector3(0, 0.020f, 0.062f), Vector3(), poly);           // 托腮板

    part(proc_body, box(0.056f, 0.078f, 0.210f), Vector3(0, -0.014f, -0.105f), Vector3(), poly);         // 下机匣
    part(proc_body, box(0.058f, 0.030f, 0.212f), Vector3(0, 0.028f, -0.105f), Vector3(), metal);         // 上机匣
    part(proc_body, box(0.038f, 0.012f, 0.298f), Vector3(0, 0.045f, -0.112f), Vector3(), metal);         // 皮卡汀尼导轨
    part(proc_body, box(0.048f, 0.050f, 0.170f), Vector3(0, -0.006f, -0.310f), Vector3(), poly);         // 护木
    part(proc_body, box(0.030f, 0.010f, 0.120f), Vector3(0, -0.036f, -0.312f), Vector3(), metal);        // 护木下导轨
    part(proc_body, tube(0.0100f, 0.150f), Vector3(0, 0.000f, -0.470f), Vector3(90, 0, 0), metal);       // 枪管
    part(proc_body, tube(0.0175f, 0.045f, 8), Vector3(0, 0.000f, -0.552f), Vector3(90, 0, 0), metal);    // 枪口制退器
    part(proc_body, box(0.024f, 0.024f, 0.046f), Vector3(0, 0.017f, -0.416f), Vector3(), metal);         // 导气箍
    part(proc_body, box(0.014f, 0.030f, 0.013f), Vector3(0, 0.029f, -0.532f), Vector3(), metal);         // 准星

    part(proc_body, box(0.026f, 0.148f, 0.082f), Vector3(0, -0.108f, -0.115f), Vector3(-8, 0, 0), dark); // 弹匣
    part(proc_body, box(0.032f, 0.100f, 0.048f), Vector3(0, -0.090f, -0.020f), Vector3(18, 0, 0), dark); // 握把
    part(proc_body, box(0.028f, 0.008f, 0.050f), Vector3(0, -0.052f, -0.062f), Vector3(), metal);        // 扳机护圈
    part(proc_body, box(0.016f, 0.011f, 0.042f), Vector3(0.028f, 0.040f, -0.020f), Vector3(), metal);    // 拉机柄

    // ------------------------------------------------------------------
    // 分件细节。整把枪如果只有一档灰，在画面上会读成一块平板 —— 加细节比调亮度
    // 有效得多。这三组都是廉价的 BoxMesh，但把"一大块"切成了"有零件的一把枪"。
    // ------------------------------------------------------------------
    // 导轨齿：皮卡汀尼导轨的灵魂，沿 z 排 14 道
    for (int i = 0; i < 14; ++i) {
        const float z = -0.258f + (float)i * 0.0212f;
        part(proc_body, box(0.036f, 0.006f, 0.011f), Vector3(0, 0.051f, z), Vector3(), metal);
    }
    // 护木散热孔：两侧各 6 个，用深色件陷入
    for (int i = 0; i < 6; ++i) {
        const float z = -0.372f + (float)i * 0.0258f;
        part(proc_body, box(0.007f, 0.019f, 0.015f), Vector3(0.026f, -0.008f, z), Vector3(), dark);
        part(proc_body, box(0.007f, 0.019f, 0.015f), Vector3(-0.026f, -0.008f, z), Vector3(), dark);
    }
    // 机匣侧面：抛壳口 / 快慢机 / 弹匣释放钮 / 拉机柄槽
    part(proc_body, box(0.010f, 0.026f, 0.074f), Vector3(0.030f, 0.006f, -0.148f), Vector3(), dark);   // 抛壳口
    part(proc_body, tube(0.011f, 0.012f, 8), Vector3(0.030f, -0.030f, -0.086f), Vector3(0, 0, 90), metal); // 快慢机
    part(proc_body, box(0.010f, 0.016f, 0.018f), Vector3(0.030f, -0.046f, -0.052f), Vector3(), metal); // 弹匣释放钮
    part(proc_body, box(0.008f, 0.010f, 0.090f), Vector3(0.030f, 0.026f, -0.150f), Vector3(), dark);   // 拉机柄槽
    // 枪托：托底板的调节卡榫 + 背带环
    part(proc_body, box(0.028f, 0.012f, 0.016f), Vector3(0, -0.052f, 0.076f), Vector3(), metal);       // 卡榫
    part(proc_body, tube(0.008f, 0.010f, 8), Vector3(-0.022f, -0.028f, 0.104f), Vector3(0, 0, 90), metal); // 背带环

    // 光学瞄具（红点）。开镜时它必须正好压住准心。
    // 外壳做成**框架**而不是实心 box：实心 box 会把镜片完全挡住，从后方看就是一个
    // 不透明的白方块（第一版就是这个效果，完全不像瞄具）。
    part(proc_body, box(0.040f, 0.008f, 0.104f), Vector3(0, 0.092f, -0.095f), Vector3(), dark);          // 上框
    part(proc_body, box(0.040f, 0.008f, 0.104f), Vector3(0, 0.058f, -0.095f), Vector3(), dark);          // 下框
    part(proc_body, box(0.008f, 0.026f, 0.104f), Vector3(-0.016f, 0.075f, -0.095f), Vector3(), dark);    // 左框
    part(proc_body, box(0.008f, 0.026f, 0.104f), Vector3(0.016f, 0.075f, -0.095f), Vector3(), dark);     // 右框
    part(proc_body, box(0.036f, 0.014f, 0.098f), Vector3(0, 0.049f, -0.095f), Vector3(), metal);         // 底座
    part(proc_body, box(0.026f, 0.028f, 0.004f), Vector3(0, 0.075f, -0.132f), Vector3(),
         vm_glow(Color(0.15f, 0.33f, 0.46f), 0.50f));                                              // 镀膜镜片
    Ref<SphereMesh> dot = memnew(SphereMesh);
    dot->set_radius(0.0060f);
    dot->set_height(0.012f);
    dot->set_radial_segments(8);
    dot->set_rings(4);
    part(proc_body, dot, Vector3(0, 0.075f, -0.135f), Vector3(),
         vm_glow(Color(1.00f, 0.14f, 0.10f), 3.0f));                                               // 红点

    // ------------------------------------------------------------------
    // 双手与前臂。
    // 肘部全部留在相机前方（世界 z ≈ -0.45 ~ -0.51），靠"y 很负"把手臂推出画面下沿。
    // 两个坑：
    //   1. 不能靠"z 很正"把手臂藏到相机后面 —— 会撞上近裁剪面，切成巨板。
    //   2. 肘部的 x 必须够大。第一版右肘只放到 x=0.13、左肘 x=-0.145，两条前臂
    //      几乎竖直垂在画面正中，把视觉重心从"右下角持枪"拽到了中间，整把枪都
    //      不像枪了。外移到 ±0.30 之后，右臂斜向右下 (u 0.67→0.82)、左臂斜向左下
    //      (u 0.58→0.29)，画面中下部才让给枪身。
    //
    // 【手心的基准落点】= 手掌方块的中心。它是一把"尺子"：换真模型时整个 hand 组
    // 按 (目标手心 − 基准手心) 平移，见 load_skin。所以这里的位置改了，
    // kWpnArt 里那三组 hand_r / hand_l 也必须跟着改 —— 两边是同一个数。
    //
    // 另外补了一块**拇指**：光一块方板读不出"手"（实测玩家反馈就是"只有一把枪"，
    // 而那时手其实在画面外/陷进枪里）。拇指放在掌心前上方，从背后看是一个越出
    // 掌顶的小鼓包 —— 不管枪的哪一侧面朝相机都看得见，是性价比最高的一笔。
    // ------------------------------------------------------------------
    // 【为什么每只手是"一块掌 + 几根指"而不是一块方板】
    // 上一版每只手只有一块 5×7.4×8 cm 的方板。在**这个视角的灯下**那是读不出"手"的：
    // 五盏枪模灯都相对相机固定，一块正对相机的平面被均匀照亮 —— 出来就是一块**没有任何
    // 明暗变化的浅色平板**，跟枪身那种满是台阶的硬表面完全不是一种东西。
    // 加几根手指之后，指节之间有了朝不同方向的面，才有体积感。
    //
    // ⚠️ 前臂的**起点必须落在手掌后面**（+z 方向），不能放在掌心。
    // 前臂是一根直径 5.4 cm 的圆柱，而手掌只有 5.4×8 cm ——
    // 起点放在掌心 = 手掌整只手被那根管子包在里面。实测症状：
    // 识别色里手套只剩 525 px、袖子 64500 px（"手不见了"的**真正**原因，
    // 比"握持点没量准"更致命：量得再准也看不见）。
    // 所以两根前臂都从掌心往 +z 退到掌背处起笔。
    //
    // 指头的摆法按"从相机（枪的左后方）看过去能看到什么"来定，不是按解剖书：
    //   · 右手攥握把时，近侧看到的是**手背那一坨** + 拇指
    //     → 一块掌加一块拇指就够，再堆指头反而会被握把挡掉；
    //   · 左手托护木时，近侧看到的是**手掌压在管子下面 + 手指从管子顶上扣过来**
    //     → 掌放高一点（够到管顶），再放三条横跨管顶的指头。
    // 拇指一律放在近侧，它是"这里有一只手"最省笔墨的信号。
    // ---- 右手：攥住握把，手背朝相机（近侧看到的是手背那一坨 + 拇指） ----
    //
    // 【回退链】真模型优先，拿不到就退回下面的程序化图元。
    // 程序化那套**必须留着**：少一个 .glb 不该让玩家手里没手，
    // 而且它是"真模型接坏了吗"的对照基准（VA_VM_HAND_ART=0 现场切）。
    // 与枪模那条回退链、载具那条、角色那条是同一个理由。
    VmHandNodeInfo hri;
    Node3D *hr_art = nullptr;
    if (hand_art_enabled()) {
        hr_art = make_vm_hand_node("vm_hand_r", proto_parent, hri);
        if (hr_art != nullptr) {
            adopt_hand_subtree(hr_art, dbg_mat);
            hand_r->add_child(hr_art);
        }
    }
    // ⚠️ 掌那块**本体**不能省。上一版改这一节时把它删掉了（只剩指节+腕+拇指），
    // 结果画面上是一串**互相飘开**的棕方块 —— 指节本来是"贴在掌的棱上"的，
    // 掌没了，它们就失去了参照物。这类"看起来像散件"的症状，先查主体在不在。
    //
    // 【2026-09-19 重做：四指从"竖直排成一列"改成"横着叠成四道指背"】
    // 上一版四指是**沿 Y 竖着**的胶囊、沿 y 排开 1.95 cm。竖胶囊竖着叠 = 从
    // 相机看过去是一根**竖着的柱子**；而且最上面那根（y 比掌顶还高 6 mm）在
    // 画面里是**单独一块**漂在手背上方（放大取证图实测）。
    // 拳头攥住握把时，指背是**横**的（沿 x 绕握把一圈），四道沿握把轴线叠起来。
    // 所以改成：胶囊绕 Z 倒 70°（几乎水平）、沿 y 以 1.7 cm 叠四道，
    // 并且**整列都收进掌的轮廓里**（掌半高抬到 0.042）—— 这样既不会露出
    // 悬空的指节，四个指尖还会从掌的远侧探出去一点，读成"手指绕过去了"。
    if (hr_art == nullptr) {
        ball(hand_r, Vector3(0.026f, 0.042f, 0.036f),                                            // 右手背
             HAND_R_BASE + Vector3(-0.006f, 0.000f, 0.000f), Vector3(10, 0, 6), glove);
        for (int i = 0; i < 4; ++i) {
            cigar(hand_r, 0.0092f, 0.046f,
                  HAND_R_BASE + Vector3(0.002f, 0.016f - (float)i * 0.0170f, -0.026f),
                  Vector3(0, 0, -70.0f + (float)i * 5.0f), glove);
        }
        ball(hand_r, Vector3(0.022f, 0.024f, 0.020f),                                            // 右手腕
             HAND_R_BASE + Vector3(0.002f, -0.010f, 0.038f), Vector3(10, 0, 0), glove);
        // 拇指从手背上沿往前指（真人握枪时拇指是搭在握把上侧的），
        // 镜位在手背近侧，所以它要**贴着手背的左缘**再往外鼓一点才看得见。
        cigar(hand_r, 0.0105f, 0.044f,                                                           // 右手拇指
              HAND_R_BASE + Vector3(-0.030f, 0.023f, -0.025f), Vector3(-55, 0, -20), glove);
    }

    // ---- 左手：从**左侧**托住护木，手背朝相机、四指从木头顶上扣过去 ----
    //
    // 【回退链同右手】真模型优先 → 程序化图元。
    // 左手这份的特殊之处：**自己的 glb 可以不存在**，此时会自动借右手那份
    // 并左右镜像（见 scene_builder.cpp 的 kVmHandArt.borrow）。
    // 镜像用负缩放实现 —— 所以手模材质一律关了背面剔除，理由见 adopt_hand_subtree。
    //
    // 【这一节是 2026-09-19 重做的，症状是"四根竖着的香肠"】
    // 上一版是一块埋在木头里的掌 + 四根**竖直**胶囊。竖直胶囊在画面上就是
    // 四根并排立着的柱子 —— 没有"绕过枪"的方向，也没有和手掌连成一片，
    // 于是读成"几根香肠浮在枪旁边"（放大取证图实测）。
    //
    // 重做按"相机看得见什么"分两层：
    //   ① **手背那一坨必须是画面里能看见的主体。** 上一版掌心 x=-0.026 整个
    //      落在莫辛 z=-0.45 处的木头（x∈[-0.039,+0.006]、y∈[-0.052,+0.016]）
    //      **里面**，只剩木底下一道缝 —— 这才是"看不见手"的真正原因，
    //      比握持点量得准不准更致命（量得再准也看不见）。
    //      现在手背心挪到木头的**左外侧**（x=-0.048，木左缘是 -0.039），
    //      侧面才露出一整片 ~3 cm 宽的手背。
    //   ② 四指**斜搭**在木头左上棱上（绕 Z 倒 34°+）：指根在手背上、
    //      指尖越过顶面 1.7 cm 落在木头另一侧。有了这个方向，它们才不是柱子。
    //      四根沿 z 排开、间距 2.1 cm —— 对应护木上并排的四道指背。
    VmHandNodeInfo hli;
    Node3D *hl_art = nullptr;
    if (hand_art_enabled()) {
        hl_art = make_vm_hand_node("vm_hand_l", proto_parent, hli);
        if (hl_art != nullptr) {
            adopt_hand_subtree(hl_art, dbg_mat);
            hand_l->add_child(hl_art);
        }
    }
    if (hl_art == nullptr) {
        ball(hand_l, Vector3(0.022f, 0.034f, 0.040f),                                            // 左手背
             HAND_L_BASE + Vector3(-0.022f, 0.016f, 0.000f), Vector3(0, 0, 14), glove);
        for (int i = 0; i < 4; ++i) {
            cigar(hand_l, 0.0086f, 0.043f,
                  HAND_L_BASE + Vector3(-0.014f, 0.044f, ((float)i - 1.5f) * 0.0210f),
                  Vector3(0, 0, -34.0f + (float)i * 3.0f), glove);
        }
        // 拇指**顺着护木往前指**（真人托护木就是这样的），而不是从掌顶翘起来。
        // ⚠️ 它必须**压在手背上**（x ≈ -0.058），不能放到手背轮廓之外：
        // 放到 -0.070 时它在染色图里是一个**和手完全分开的椭圆**（中间隔着世界色），
        // 画面上读成"一根漂在枪旁边的香肠"。
        cigar(hand_l, 0.0100f, 0.048f,                                                           // 左手拇指
              HAND_L_BASE + Vector3(-0.032f, 0.002f, -0.026f), Vector3(-72, 0, -12), glove);
        ball(hand_l, Vector3(0.021f, 0.024f, 0.020f),                                            // 左手腕
             HAND_L_BASE + Vector3(0.002f, -0.010f, 0.042f), Vector3(10, 0, 0), glove);
    }

    // 前臂与腕口挂在 hands 上（**不是** hand_r / hand_l）：那两组会随每把枪的
    // 握持点整体平移，而肘长在人身上、不跟着枪走。挂进 hand 组的话，支撑手一
    // 往前（莫辛 z=-0.45、DP-27 z=-0.62），肘跟着往前，前臂就被拉成一根
    // 斜穿画面的细棍 —— 这正是放大取证图里那根"撑杆"的来历。
    fore_r = memnew(MeshInstance3D);
    fore_r->set_material_override(sleeve);
    vm_layer(fore_r);
    hands->add_child(fore_r);
    fore_l = memnew(MeshInstance3D);
    fore_l->set_material_override(sleeve);
    vm_layer(fore_l);
    hands->add_child(fore_l);
    cuff_r = memnew(MeshInstance3D);
    cuff_r->set_material_override(strap);
    vm_layer(cuff_r);
    hands->add_child(cuff_r);
    cuff_l = memnew(MeshInstance3D);
    cuff_l->set_material_override(strap);
    vm_layer(cuff_l);
    hands->add_child(cuff_l);
    // 袖褶用袖子自己的材质 —— 它要读成"同一块布上的一道褶"，不是第二件装备。
    fold_r = memnew(MeshInstance3D);
    fold_r->set_material_override(sleeve);
    vm_layer(fold_r);
    hands->add_child(fold_r);
    fold_l = memnew(MeshInstance3D);
    fold_l->set_material_override(sleeve);
    vm_layer(fold_l);
    hands->add_child(fold_l);

    cur_hand_r = HAND_R_BASE;
    cur_hand_l = HAND_L_BASE;
    place_arms();

    // 报一下两只手各自用的是真模型还是程序化图元。
    // 【为什么值得打一行日志】"手里这只手是真模型吗"这个问题**不能靠看图回答** ——
    // 程序化图元与真模型在缩小之后轮廓相近，而 VA_VM_HAND_ART=0 的消融图
    // 又只能证明"有没有手"。日志里这一行才是那个可分的一手证据。
    if (art_report_enabled()) {
        UtilityFunctions::print(String::utf8("[vm] 双手 "),
                                String::utf8(hr_art != nullptr ? "真模型" : "程序化"),
                                String::utf8(" / "),
                                String::utf8(hl_art != nullptr ? "真模型" : "程序化"),
                                String::utf8(hl_art != nullptr && hli.mirrored ? "（左手为镜像）" : ""));
    }

    // ------------------------------------------------------------------
    // 枪口 + 枪口焰
    // ------------------------------------------------------------------
    muzzle = memnew(Node3D);
    muzzle->set_position(Vector3(0, 0.000f, -0.575f));
    gun->add_child(muzzle);

    // 枪口节点建出来之后就解一次腰射姿态 —— 解它要读枪口 z。
    // （挂真模型时下面 load_skin 会带着新的枪口 z 再解一次，这次是给
    //  VA_VM_NO_ART / 模型缺失时兜底的那条路用的。）
    solve_hip_pose();

    flash_group = memnew(Node3D);
    muzzle->add_child(flash_group);

    const Ref<StandardMaterial3D> flame = vm_glow(Color(1.0f, 0.74f, 0.32f), 7.0f);
    Ref<SphereMesh> fs = memnew(SphereMesh);
    fs->set_radius(0.046f);
    fs->set_height(0.088f);
    fs->set_radial_segments(8);
    fs->set_rings(4);
    part(flash_group, fs, Vector3(0, 0, -0.02f), Vector3(), flame);
    part(flash_group, box(0.160f, 0.019f, 0.019f), Vector3(0, 0, -0.02f), Vector3(0, 0, 36), flame);
    part(flash_group, box(0.132f, 0.017f, 0.017f), Vector3(0, 0, -0.02f), Vector3(0, 0, -52), flame);
    part(flash_group, box(0.019f, 0.132f, 0.019f), Vector3(0, 0, -0.02f), Vector3(0, 0, 18), flame);

    flash_light = memnew(OmniLight3D);
    flash_light->set_position(Vector3(0, 0, -0.06f));
    flash_light->set_color(Color(1.0f, 0.84f, 0.58f));
    flash_light->set_param(Light3D::PARAM_ENERGY, 0.0f);
    flash_light->set_param(Light3D::PARAM_RANGE, 9.0f);
    flash_light->set_shadow(false);
    // 枪口闪光也照到世界（不只是枪），这样开火时周围的草地会亮一下 —— 反馈更有力
    flash_light->set_cull_mask(0xFFFFFFFFu);
    muzzle->add_child(flash_light);

    flash_group->set_visible(false);

    // ------------------------------------------------------------------
    // 枪模专属照明：五盏平行光，方向绕枪一圈分布。
    // 全部 set_cull_mask(VM_LAYER)：所以玩家转任何方向，枪身的明暗都保持稳定可读，
    // 又不会莫名其妙照亮身边的石头。
    // 方向全部相对**相机**固定，因此枪的朝向变化不影响它的读数 —— 这是 viewmodel
    // 独立打光的全部意义。
    //
    // 第 3 盏（轴光）是必需的：前臂是圆柱、从画面下方斜插进来，它的可见面法线
    // 几乎全部朝向相机。只给侧向/后方光的时候，实测袖子有 63% 的像素亮度 <20
    // （albedo 提高 2.1 倍都没用，因为那些像素的照度本来就是 0）。加一盏沿视轴的
    // 光之后，任何"朝相机"的面才拿得到基础照明。
    //
    // 【为什么不能靠"把轴光提上去"来救双手 —— 试过，枪身会跟着变】
    // 直觉是"轴光 travel 近乎纯 -Z，只对法线朝 +Z 的面有效，而枪身上这种面很少"。
    // 实测把这个直觉否掉了（莫辛，tools/vm_hand_probe.py）：
    //     轴光 1.5 → 3.2：手套 L52→65、袖子 L33→66（色偏也修好了），但
    //     枪身中位 RGB (136,143,168)→(182,141,115)、过曝 25.2%→26.5%
    // —— 因为**圆柱体的可见半边本来就朝相机**，枪管、机匣、弹匣都吃这盏灯。
    // 枪身的亮度与过曝是本项目逐档量着标定出来的（见 VA_VM_ART_ALB 那段），
    // 所以这条路放弃：灯一律不动，双手的补偿全部做在它自己的 albedo 上
    // （见上面手套/袖子那两段的注释）。
    // ------------------------------------------------------------------
    const float k_key = env_f("VA_VMK", 3.80f);
    const float k_side = env_f("VA_VMF", 2.00f);
    // 主光的仰角刻意压低（travel 的 |y| 从 0.78 降到 0.58）：仰角太高时，开镜看到的
    // 枪身上表面会被垂直打亮到接近纯白（实测 RGB 240+），整把枪失去体积感。
    // 压低之后上表面与侧面照度接近，无论腰射还是开镜都读得出一把枪的形状。
    static const Vector3 kDir[VM_LIGHT_COUNT] = {
        Vector3(-0.62f, -0.58f, 0.53f),   // 右前上：主光
        Vector3(0.74f, -0.24f, 0.62f),    // 左前侧：冷补光
        Vector3(-0.15f, -0.10f, -0.99f),  // 沿视轴：兜底，治"朝相机的面全黑"
        Vector3(0.16f, 0.44f, -0.88f),    // 后方：轮廓光
        Vector3(-0.10f, 0.94f, 0.32f),    // 下方：弹光，消除底面死黑
    };
    static const Color kCol[VM_LIGHT_COUNT] = {
        Color(1.00f, 0.955f, 0.90f), Color(0.68f, 0.77f, 0.95f), Color(0.94f, 0.92f, 0.90f),
        Color(0.55f, 0.60f, 0.68f), Color(0.62f, 0.55f, 0.44f),
    };
    static const float kMul[VM_LIGHT_COUNT] = {1.0f, 1.0f, 0.75f, 0.80f, 0.68f};
    for (int i = 0; i < VM_LIGHT_COUNT; ++i) {
        vm_energy[i] = (i == 0) ? k_key : k_side * kMul[i];
        vm_lights[i] = vm_dir_light(p_cam, kDir[i], kCol[i], vm_energy[i]);
    }

    // 消融开关：把整把枪藏起来再拍一张，两张的像素差就是"枪真正占了哪些像素"。
    // 靠肉眼在截图里猜哪块几何体是枪，上一轮已经猜错过一次。
    if (std::getenv("VA_VM_HIDE") != nullptr) root->set_visible(false);

    // ------------------------------------------------------------------
    // 武器外观：优先用手里的真模型，没有就保持程序化枪模。
    // 放在最后是因为它要改 proc_body / hands 的可见性，而这两组刚刚才建完。
    // ------------------------------------------------------------------
    art_slots = std::min<int>(MAX_SKINS, (int)all_wpn_keys().size());
    if (std::getenv("VA_VM_NO_ART") != nullptr) {
        // 消融：强制用程序化枪模，用来对比"真模型到底改了什么"。
        UtilityFunctions::print(String::utf8("[vm] VA_VM_NO_ART：强制程序化枪模"));
    } else {
        int start = 0;
        // VA_WM_SKIN=<键 或 序号>：直接指定初始外观。
        // 为什么需要它：按键注入要精确定位窗口、还要碰鼠标捕获，
        // 而"拍一张第 2 把枪的腰射"这种事不该依赖按键 —— 有它就能一条命令拍完。
        if (const char *e = std::getenv("VA_WM_SKIN")) {
            const std::vector<std::string> &keys = all_wpn_keys();
            for (int i = 0; i < (int)keys.size(); ++i) {
                if (keys[i] == e) {
                    start = i;
                    break;
                }
            }
            if (start == 0 && std::strlen(e) == 1 && e[0] >= '0' && e[0] <= '9') {
                const int n = (int)keys.size();
                if (n > 0) start = ((e[0] - '0') % n + n) % n;
            }
        }
        if (!load_skin(start)) {
            UtilityFunctions::print(String::utf8("[vm] 没有可用的武器模型，继续用程序化枪模"));
        }
    }
}

// 把第 p_index 个外观挂上去。已经建过就只切可见性 ——
// 每次按键都重新 duplicate 一棵 5 万面的子树是白花时间，而且旧节点要等
// 帧末才真正释放，来回切会出现"两把枪同时在手里"的一帧。
bool ViewModel::load_skin(int p_index) {
    if (art_holder == nullptr || art_slots <= 0) return false;
    p_index = ((p_index % art_slots) + art_slots) % art_slots;

    if (art_nodes[p_index] == nullptr) {
        const std::string &key = all_wpn_keys()[p_index];
        WpnNodeInfo info;
        Node3D *n = make_wpn_node(key, proto_parent, info);
        if (n == nullptr) return false;
        // 真模型的渲染层必须在这里补上，理由见 adopt_gun_subtree。
        adopt_gun_subtree(n);
        n->set_name(String("Art_") + String::utf8(key.c_str()));
        art_holder->add_child(n);
        art_nodes[p_index] = n;
        art_mz[p_index] = info.muzzle_z;
        art_len[p_index] = info.length;
        art_hand_r[p_index] = info.hand_r_pos;
        art_hand_l[p_index] = info.hand_l_pos;
        art_sight[p_index] = info.sight_pos;
    }
    art_muzzle_z = art_mz[p_index];

    // 双手按这把枪的握持点整组平移（**三个方向都平移**）。
    // 基准 = 程序化那两只手掌方块的中心（HAND_R_BASE / HAND_L_BASE），
    // 偏移 = 本枪的握持点 − 基准；程序化枪模时两者相等、偏移为 0，原样不动。
    //
    // 【为什么不能只挪 z】上一版就是这么写的，理由是"竖直方向留给按截图微调"。
    // 结果是手**整块陷进枪身**：真模型的握把高度和程序化枪模差 2~5 cm，
    // 只挪 z 等于把指头按在枪托内部。症状不是"手飘在枪外"，而是"**手根本看不见**" ——
    // 从背后看只剩两侧各 2 mm 的边，实测绿(手套) 525 px、蓝(袖子) 64500 px，
    // 玩家看到的就是"手里只有一把枪"。所以握持点必须是三维量出来的。
    //
    // 握持点的来历见 scene_builder.cpp 的 kWpnArt（tools/glb_preview.py --probe 量）。
    // 想现场扫偏移而不重编译，用 VA_VM_HANDOFF_R / _L（"x,y,z"，米）。
    Vector3 hr = art_hand_r[p_index];
    Vector3 hl = art_hand_l[p_index];
    Vector3 d;
    if (env_vec3("VA_VM_HANDOFF_R", d)) hr += d;
    if (env_vec3("VA_VM_HANDOFF_L", d)) hl += d;

    // 前臂的腕端跟着新的握持点走，肘不动。
    // 手掌自己的位置与缩放也在 place_arms() 里设 —— 它要带上"开镜让位"，两处分开写
    // 就一定有一处漏掉让位。
    cur_hand_r = hr;
    cur_hand_l = hl;
    place_arms();

    // 开镜对准：把"枪自己的照门"当眼睛要穿过的那一点。
    // 换枪时它必须跟着换 —— 三把枪的照门高度差了一倍多（莫辛 0.075 / DP-27 0.160），
    // 沿用同一组 aim 偏移的话，DP-27 开镜时枪会整个沉到画面下半。
    aim_sight_x = art_sight[p_index].x;
    aim_sight_y = art_sight[p_index].y;

    for (int i = 0; i < art_slots; ++i) {
        if (art_nodes[i] != nullptr) art_nodes[i]->set_visible(i == p_index);
    }
    art_index = p_index;

    // 位置与朝向都由 scene_builder 归一化进 gun 局部系了，这里不再动 art_holder。
    if (muzzle != nullptr) muzzle->set_position(Vector3(0.0f, 0.0f, art_muzzle_z));

    // **枪口 z 变了 → 腰射姿态重解一次。**
    // 老实说在默认 conv_d=25 下三把枪解出来是同一个角（实测 0.202733/0.241899，
    // 三把一位不差）—— 因为枪口的**横向** offset 就是 hip_pos.x/y，与枪口 z 无关，
    // 而 z 只影响"到目标的距离"，25 m 上差 0.3 m 折合 0.7%。
    // 真正会分化的是 conv_d 很小的时候（3 m 处三把枪差 0.27°，1 m 处差 7°）。
    // 保留这次重解是因为它一行、且让"任意 conv_d 都成立"这件事不依赖巧合。
    solve_hip_pose();

    // 真模型接管枪身，程序化枪身整块让位。
    //
    // 双手**要留着**：参考图是光枪，生成的模型里本来就没有手，
    // 连手一起藏掉的话画面上就是一把悬空的枪 —— 这是上一轮踩的坑
    // （当时嫌那两块粗方块"抢眼"，就把默认值改成了藏，结果连"手持"都丢了）。
    // 现在的取舍：手默认显示；每把枪的握持点标定在 kWpnArt 的 hand_r / hand_l 里，
    // 换枪时整组跟着走。实在想对照"有手/没手"，用 VA_VM_HANDS=0。
    if (proc_body != nullptr) proc_body->set_visible(false);
    // 双手的显隐**只在 place_arms() 里决定**（它同时管"开镜缩到看不见就整组藏掉"）。
    // 这里**不要**再补一句 hands->set_visible(hands_enabled())：place_arms() 就在上面
    // 几行，补这一句等于把它的判断推翻 —— 开镜中换枪会让缩到 0.1% 的手闪一帧。

    // 顺手报一下手落在枪的哪个位置 —— 判"手有没有陷进枪身"时，
    // 拿这三个数与 glb_preview --probe 量出来的剖面直接比就行，不必靠看图。
    if (dbg_vm()) {
        UtilityFunctions::print(String::utf8("[vm] 握持点 右手 "), hr,
                                String::utf8("  左手 "), hl);
    }

    UtilityFunctions::print(String::utf8("[vm] 武器外观 "), (int)p_index + 1, "/", art_slots,
                            " ", String::utf8(skin_label()),
                            String::utf8(" 全长 "), art_len[p_index],
                            String::utf8(" 枪口 z "), art_muzzle_z);
    return true;
}

// ---------------------------------------------------------------------------
// 腰射姿态：解"枪管穿过准星"
// ---------------------------------------------------------------------------
//
// 约束一句话：**从枪口沿枪管轴射出的那条直线，必须在 conv_d 米处穿过准星。**
// 准星在屏幕中心，而屏幕中心对应的就是相机空间的 -Z 轴（视轴），
// 所以目标点就是 T = (0, 0, -conv_d)（相机局部系，-Z 朝前）。
//
// 枪管 = 枪局部 -Z。枪只承担姿态角（位移在 root 上），所以未知量只有三个欧拉角里的
// 两个（偏航 + 俯仰）—— 第三个是绕枪管轴的侧倾，它**不影响指向**，直接取
// hip_roll 定观感。
//
// 【为什么要迭代而不是一步解出来】枪口在局部 (0,0,mz)，姿态一动它的**位置**也动，
// 于是"枪口 → 目标"的方向跟着变。好在枪口就落在旋转轴上（局部 x=y=0），
// 迭代 6 次已经收敛到 1e-4 量级；不迭代也能用，但既然便宜就让它精确。
//
// 【为什么用 Basis 拼而不是直接写 yaw/pitch 的公式】"让局部 -Z 对准 f"这件事
// 用基向量拼是三行（zc = -f，xc = up×zc，yc = zc×xc），而写公式要同时照顾
// 欧拉顺序与符号 —— 顺序搞错在画面上表现为"枪转了 90°"而不是"差一点"，更难查。
void ViewModel::solve_hip_pose() {
    hip_solved = false;
    if (hip_env_over) return;    // VA_VM_ROT 给过：手调优先（取证 / 找位用）
    if (conv_d <= 0.0f) return;  // VA_VM_CONV=0：关掉收敛，保留 hip_rot 那组手调角

    // 枪口在枪局部系里的 z。**读 muzzle 节点而不是 art_muzzle_z**：
    // 程序化枪模那条路 art_muzzle_z 恒为 0（它没有"归一化"这一步），
    // 而 muzzle 节点在两种外观下都是权威值（build 建它时 -0.575，load_skin 改它）。
    const float mz = (muzzle != nullptr) ? muzzle->get_position().z : -0.575f;
    const Vector3 target(0.0f, 0.0f, -conv_d);

    Vector3 e = Vector3(0.0f, 0.0f, hip_roll);
    for (int it = 0; it < 6; ++it) {
        const Basis b = Basis::from_euler(Vector3(Math::deg_to_rad(e.x),
                                                  Math::deg_to_rad(e.y),
                                                  Math::deg_to_rad(e.z)));
        const Vector3 m = hip_pos + b.xform(Vector3(0.0f, 0.0f, mz)) * vm_scale;
        const Vector3 f = (target - m).normalized();   // 枪管应有的朝向（相机空间）
        // 由 f 造基：局部 -Z → f。侧倾 = 先把"上"绕 f 转再参与构造。
        // 【符号：这里是 -hip_roll】"把 up 绕 f 转 φ"等于"把基绕局部 Z 转 -φ" ——
        // 而 f = 局部 -Z。第一版漏了这个负号，解出来的姿态角是 (…, +6.0)，
        // 而手调那组是 -6.0：指向完全一样（滚转不改变枪管方向），
        // 但枪的倾斜方向**反了**，观感上是另一把枪的握姿。实测抓到的。
        const Vector3 up = Vector3(0.0f, 1.0f, 0.0f).rotated(f, Math::deg_to_rad(-hip_roll));
        const Vector3 zc = -f;
        Vector3 xc = up.cross(zc);
        if (xc.length_squared() < 1e-12f) xc = Vector3(1.0f, 0.0f, 0.0f).cross(zc);
        xc = xc.normalized();
        const Vector3 yc = zc.cross(xc);
        Basis nb;
        nb.set_column(0, xc);
        nb.set_column(1, yc);
        nb.set_column(2, zc);
        // 节点是用**欧拉角**驱动的（Node3D 默认 YXZ，Basis::get_euler 的默认也是 YXZ），
        // 所以换回欧拉角写出去。往返是否一致由 dbg_aim_line 的实测数字兜底 ——
        // 顺序一旦不一致，屏幕上会立刻读出一个几十度的角误差。
        const Vector3 ne = nb.get_euler();
        e = Vector3(Math::rad_to_deg(ne.x), Math::rad_to_deg(ne.y), Math::rad_to_deg(ne.z));
    }
    hip_rot = e;
    hip_solved = true;
    if (dbg_vm()) {
        UtilityFunctions::print(String::utf8("[vm] 腰射姿态解算（枪管穿过准星 "),
                                String::num((double)conv_d, 1), String::utf8(" m 处）→ 姿态角 "),
                                hip_rot);
    }
}

// 一行日志：枪口与"枪管指向"落在屏幕哪儿，和准星并排。
//
// 【为什么必须量、不能靠看截图判读】枪管是细长体，看图容易把**枪身**的方向读成
// 枪管的方向 —— 两者在莫辛上差 100 px 以上（复核时踩过：按枪身读是"指向准星左边
// 一点点"，按枪管读是"偏 186 px"）。这里直接用引擎自己的投影 unproject_position，
// 不给"我觉得"留余地。
void ViewModel::dbg_aim_line() {
    if (cam == nullptr || gun == nullptr || muzzle == nullptr) return;
    Viewport *vp = cam->get_viewport();
    if (vp == nullptr) return;
    const Vector2 size = vp->get_visible_rect().size;
    if (size.x < 2.0f || size.y < 2.0f) return;
    const Vector2 c = size * 0.5f;                                  // 准星（HUD 画在视口中心）
    const Vector2 pm = cam->unproject_position(muzzle->get_global_position());
    // 枪管轴上的远点：从枪口再沿局部 -Z 走 200 m。远超任何交战距离，
    // 所以它落在屏幕哪儿，就是"枪管指向哪儿"的读数。
    const Vector2 pf = cam->unproject_position(
        gun->to_global(Vector3(0.0f, 0.0f, muzzle->get_position().z - 200.0f)));
    // 角误差：把横向像素差换算成视场角。读 px 会被分辨率带跑，读角度才能跨机器比。
    const float half_h = std::tan(Math::deg_to_rad(cam->get_fov()) * 0.5f)
                       * (size.x / (size.y > 1.0f ? size.y : 1.0f));
    const float err = Math::rad_to_deg(std::atan((pf.x - c.x) / (size.x * 0.5f) * half_h));
    UtilityFunctions::print(
        String::utf8("[vm] 指向 枪口 px "), pm,
        String::utf8("  枪管指向 px "), pf,
        String::utf8("  准星 px "), c,
        String::utf8("  角误差 "), String::num((double)err, 2), String::utf8("°"),
        String::utf8("  姿态 "), hip_rot,
        String::utf8(hip_solved ? "（解算）" : "（手调）"));
}

bool ViewModel::next_skin(int p_step) {
    if (art_slots <= 0) return false;
    // 跳过没有模型的槽位：少一个 .glb 不该让"按键切外观"整个失灵。
    for (int i = 1; i <= art_slots; ++i) {
        const int idx = ((art_index + p_step * i) % art_slots + art_slots) % art_slots;
        if (load_skin(idx)) return true;
    }
    return false;
}

const char *ViewModel::skin_key() const {
    if (art_slots <= 0 || art_index < 0 || art_index >= art_slots) return "";
    return all_wpn_keys()[art_index].c_str();
}

const char *ViewModel::skin_label() const {
    return wpn_label(skin_key());
}

void ViewModel::on_shot() {
    if (!valid()) return;
    // 一次冲量：位移 + 角速度。数值靠手感调 —— 太大像在"点头"，太小没反馈。
    recoil_v += 3.6f;
    recoil = std::min(recoil + 0.22f, 1.0f);
    flash_t = flash_sec();
    flash_frames = 0;
    flash_dt_max = 0.0f;
    if (muzzle != nullptr) {
        // 每发随机滚一下枪口焰，避免连发时同一个形状反复闪
        muzzle->set_rotation_degrees(Vector3(0, 0, (float)(rand() % 360)));
    }
    if (dbg_vm()) {
        UtilityFunctions::print(String::utf8("[vm] 开火 → 枪口焰 时长 "),
                                String::num(flash_t, 3), String::utf8("s  枪口 z "),
                                String::num(muzzle != nullptr ? (double)muzzle->get_position().z : 0.0, 3));
    }
}

void ViewModel::on_reload(float dur) {
    reload_dur = std::max(0.3f, dur);
    reload_t = reload_dur;
}

void ViewModel::update(double p_dt, float move01, bool running, float pitch, float yaw, bool ads_want) {
    if (!valid()) return;
    const float dt = (float)p_dt;
    if (dt <= 0.0f) return;

    // ---- 天气亮度联动 ----
    // 枪模用的是专属平行光，对环境亮度完全免疫。好处是任何角度都读得清，
    // 坏处是夜战里它会变成一根**发光塑料棒** —— 实测夜战截图就是如此：场景压到
    // 深蓝，枪还是白天的浅灰，非常出戏。所以按天气给灯乘一个倍率：让它跟着环境
    // 一起暗下去，但保留下限，保证"夜战也能看清手里拿的是什么"。
    const float wmul = (va::W.weather == "night") ? 0.52f : (va::W.weather == "rain" ? 0.82f : 1.00f);
    if (std::fabs(wmul - vm_wmul) > 1e-3f) {
        vm_wmul = wmul;
        for (int i = 0; i < VM_LIGHT_COUNT; ++i) {
            if (vm_lights[i] != nullptr) {
                vm_lights[i]->set_param(Light3D::PARAM_ENERGY, vm_energy[i] * wmul);
            }
        }
    }

    // ---- 开镜：平滑插值 ----
    const float ads_target = ads_want ? 1.0f : 0.0f;
    ads += (ads_target - ads) * std::min(1.0f, dt * 13.0f);

    // ---- 视角惯性滞后 ----
    // 把相机的角增量积分成一个反向偏移，再缓慢回中。这是"枪有重量"的来源；
    // 少了它，不管后坐怎么做，武器都像焊死在屏幕上。
    const float dyaw = va::angDiff(last_yaw, yaw);
    const float dpitch = pitch - last_pitch;
    last_yaw = yaw;
    last_pitch = pitch;
    const float sway_scale = (1.0f - ads * 0.55f);      // 开镜后据枪更稳
    sway_yaw = va::clampf(sway_yaw - dyaw * 1.55f * sway_scale, -0.085f, 0.085f);
    sway_pitch = va::clampf(sway_pitch - dpitch * 1.55f * sway_scale, -0.065f, 0.065f);
    const float recover = std::min(1.0f, dt * 7.5f);
    sway_yaw -= sway_yaw * recover;
    sway_pitch -= sway_pitch * recover;

    // ---- 后坐：弹簧-阻尼回中（线性回弹看起来像机器人） ----
    recoil_v += -recoil * 150.0f * dt;
    recoil_v *= std::max(0.0f, 1.0f - 12.0f * dt);
    recoil += recoil_v * dt;
    recoil = va::clampf(recoil, -0.35f, 1.6f);

    // ---- 行走摆动：与相机 bob 同频 ----
    bob += dt * (running ? 12.4f : 8.6f);
    const float mv = va::clampf(move01, 0.0f, 1.0f) * (1.0f - ads * 0.65f);
    const float bx = std::sin(bob) * 0.0155f * mv;
    const float by = std::fabs(std::cos(bob)) * 0.0125f * mv;

    // ---- 呼吸：静止时也保留极缓的起伏，画面不会"死" ----
    breath += dt;
    const float brx = std::sin(breath * 1.10f) * 0.0034f;
    const float bry = std::sin(breath * 1.73f) * 0.0028f;

    // ---- 换弹：压枪下沉 + 侧转，结束回位 ----
    float rl = 0.0f;
    if (reload_t > 0.0f) {
        reload_t = std::max(0.0f, reload_t - dt);
        const float u = 1.0f - reload_t / reload_dur;
        rl = std::sin(u * 3.14159265f);
    }

    // ---- 姿态：腰射 ↔ 开镜 ----
    // gun 只承担姿态角，位移留给 root。分开之后，开镜过渡不会被后坐的
    // 旋转叠加上去，瞄具才能稳定压住准心。
    gun->set_rotation_degrees(hip_rot.lerp(aim_rot, ads));

    // ---- 开镜时双手让位（照门优先）----
    //
    // 【为什么需要单独一套姿态】真手模是**实尺**的（拳约 10 cm 宽），而开镜时枪轴
    // 与视轴重合 —— 握把上的扳机手、护木上的支撑手**都**落在瞄准轴附近，两只手一起
    // 压住照门。实测（`sweep/vmhand_hip2/` 的取证）：开镜时"最近的手像素到画面中心"
    // = **0 px**（中心即瞄准点），中心 80 px 内 13005 px 是手；程序化手那时是 39 px /
    // 3296 px，所以这不是"接真模型必然的代价"，是新引入的退化。
    //   落位不是解：扫过 5 档（含下移 40 mm、后移 60 mm），最近距中心**始终 0**；
    //   再把任一只手移出画面做消融，**另一只仍然压住中心**（两只手都挡）。
    // 所以照真 FPS 的通行做法，给视图模型**分腰射/开镜两套姿态**：
    // 随 ads 把双手缩一点、往中轴外让一点。缩放只作用在 hand_r / hand_l 自己身上
    // （即绕握持点缩），**不缩 hands** —— 缩 hands 会把前臂一起缩细；位移则同时给
    // 手掌与腕端（见 place_arms），否则腕口会裂开一个正对相机的断口。
    {
        const float hk = 1.0f - hand_ads_shrink * ads;
        const Vector3 hoff = hand_ads_off * ads;
        if (std::fabs(hk - hand_pose_scale) > 1e-6f || hoff.distance_to(hand_pose_off) > 1e-6f) {
            hand_pose_scale = hk;
            hand_pose_off = hoff;
            place_arms();
        }
    }

    // ---- 合成位置 ----
    //
    // 【开镜落点不是常数，是按"枪自己的照门"反推的】
    // 开镜时姿态角归零，枪局部系与相机系同向，于是"照门落在视轴上"这一个条件
    // 直接给出 x/y：aim.x = -sight.x·s，aim.y = -sight.y·s。
    // 上一版把它写死成 (0.018, -0.075)，那是**程序化枪模红点瞄具**的高度；
    // 换成真模型之后每把枪的照门高度都不同，症状是开镜时枪沉在画面下方
    // （实测莫辛机匣顶落在 0.73 屏高，离中心近四分之一屏高，等于瞄了个寂寞）。
    // VA_VM_AIM 显式给过时整套照它的算 —— 找位/取证时要能强行指定。
    Vector3 aim_eff = aim_env_over
        ? aim_pos
        : Vector3(-aim_sight_x * vm_scale, -aim_sight_y * vm_scale, -aim_dist);

    Vector3 pos = hip_pos.lerp(aim_eff, ads);
    pos.x += bx + brx + sway_yaw * 0.35f;
    pos.y += by + bry + sway_pitch * 0.35f - rl * 0.115f;
    // 后坐退让：开镜时行程要收窄，否则枪托会顶到近裁剪面上
    pos.z += recoil * 0.045f * (1.0f - ads * 0.60f);
    pos.y += recoil * 0.017f;
    pos.x += rl * 0.045f;
    // 最后一道保险：枪上任何一点落到相机后方都会被近裁剪面切开，
    // 画面上不是"枪少了半截"而是一块**黑板**（切开的截面正对相机），
    // 症状和"枪的位置不对"完全看不出关系。把枪托尾端到眼睛的距离钉在
    // 0.078 m（近裁剪 0.06 + 1.8 cm 余量）以内不再靠近，无论上面怎么叠加。
    pos.z = std::min(pos.z, -0.078f);
    root->set_position(pos);

    // ---- 合成旋转（root 只承担动态部分）----
    root->set_rotation_degrees(Vector3(
        (recoil * 10.5f - sway_pitch * 150.0f) * (1.0f - ads * 0.5f) - rl * 26.0f,
        sway_yaw * 150.0f * (1.0f - ads * 0.5f) + rl * 15.0f,
        rl * 32.0f));

    // ---- 枪口焰 ----
    //
    // 【判定必须排在扣减之前 —— 这条顺序就是"开火没有光效"的根因】
    // 原先写的是先 `flash_t -= dt` 再判 `flash_t > 0`。只要**一帧的间隔 ≥ 闪光时长**，
    // 整段闪光就被这一帧一次扣光，画面上**一帧都画不出来**，枪口焰彻底消失：
    //     0.045 秒的门槛 = 22 fps。高于它才有闪光，低于它永远没有。
    // 而本机 1080p 实测的帧间隔就在 0.05～0.1 秒（10～20 fps，见 VA_DBG_VM 的日志），
    // 正好整个落在门槛之下 —— 于是这是一个**只在低帧率机器上成立**的 bug，
    // 帧率高的机器上连复现都复现不出来，看代码也很难看出问题。
    // 现在先定"这一帧画不画"，再扣时间：闪光在任意帧率下都至少有一帧的曝光。
    if (flash_t > 0.0f) {
        // 亮度取**扣减之前**的剩余比例 → 第一帧必定满亮
        const float k = va::clampf(flash_t / flash_sec(), 0.0f, 1.0f);
        flash_t -= dt;
        flash_group->set_visible(true);
        if (flash_light != nullptr) {
            flash_light->set_param(Light3D::PARAM_ENERGY, FLASH_ENERGY * k);
        }
        ++flash_frames;
        flash_dt_max = std::max(flash_dt_max, dt);
        if (flash_t <= 0.0f) {
            // 本次闪光结束：几何与点光一起灭掉
            flash_group->set_visible(false);
            if (flash_light != nullptr) flash_light->set_param(Light3D::PARAM_ENERGY, 0.0f);
            if (dbg_vm()) {
                const float fps = (flash_dt_max > 0.0f) ? (1.0f / flash_dt_max) : 0.0f;
                UtilityFunctions::print(String::utf8("[vm] 枪口焰 结束：亮 "), flash_frames,
                                        String::utf8(" 帧  最大帧间隔 "), String::num(flash_dt_max, 3),
                                        String::utf8("s（≈"), (int)fps, String::utf8(" fps）"));
            }
        }
    }

    // VA_DBG_VM=1：每秒一行帧率。枪口焰画不画得出来完全由 dt 决定，
    // 所以报帧率就是报这件事的"因" —— 判读时两行对着看。
    if (dbg_vm()) {
        dbg_dt_acc += dt;
        ++dbg_dt_frames;
        if (dbg_dt_acc >= 1.0f) {
            const float avg = dbg_dt_acc / (float)dbg_dt_frames;
            UtilityFunctions::print(String::utf8("[vm] dt "), String::num(avg, 4),
                                    String::utf8("s ≈ "), (int)(1.0f / avg),
                                    String::utf8(" fps  （枪口焰时长 "), String::num(flash_sec(), 3),
                                    String::utf8("s，要 dt 小于它才画得出来）"));
            dbg_dt_acc = 0.0f;
            dbg_dt_frames = 0;
        }
    }

    // VA_DBG_VMAIM=1：每秒报一次"枪管指向落在屏幕哪儿"，与准星并排（见 dbg_aim_line）。
    // 独立于 VA_DBG_VM：那一行是帧率心跳，这两件事的判读场合不重叠。
    if (std::getenv("VA_DBG_VMAIM") != nullptr) {
        aim_dbg_acc += dt;
        if (aim_dbg_acc >= 1.0f) {
            aim_dbg_acc = 0.0f;
            dbg_aim_line();
        }
    }

    // ---- FOV：开镜收窄视场，是"进入瞄准"最直接的视觉信号 ----
    if (cam != nullptr) {
        cam->set_fov(fov_base + (fov_ads - fov_base) * ads);
    }
}

} // namespace volunteer_army
