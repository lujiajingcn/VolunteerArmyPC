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
#include <godot_cpp/classes/cylinder_mesh.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
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

// 前臂半径。原先是 0.032，实测在 0.3~0.5 m 处是一条 120~180 px 宽的带子，
// 换真模型之后它比枪本身还抢眼（识别色实测袖子 64500 px、手套只有 525 px）。
// 人前臂直径 6~8 cm，但**这个视角下**看到的有效宽度还含近裁剪放大，
// 所以视觉上要按"看上去像一根手臂"来压，而不是按解剖尺寸。0.024 是压过两档之后的值。
constexpr float FOREARM_R = 0.024f;

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

// 把整棵子树挪到枪模专用渲染层，并关掉阴影接收。
//
// 【为什么必须做这一步】真模型自带 MeshInstance3D，默认落在 layer 1。
// 而本文件照枪的那五盏平行光 cull_mask 只到 VM_LAYER —— 留在默认层的话
// 它们照不到枪，世界光却照得到：玩家一转身背对太阳，手里的枪就塌成黑剪影，
// 正是下面"光照隔离"那一段要避免的现象。角色模型不需要这一步，
// 因为角色本来就该被世界光照亮。
//
// 材质是**共享**的（duplicate 不会深拷贝 Mesh），所以这里改的是被缓存的那一份 ——
// 无所谓：武器模型只有视图模型在用。
//
// 【为什么还要改材质本身】真模型带的是图生3D 给的 PBR 材质，直接上会踩三个坑
// （每个坑对应上面一个旋钮，实测数据写在旋钮的注释里）：
//   1) **过曝**：照枪的那五盏平行光，能量是按程序化枪模那套**很深的** albedo
//      （0.17 sRGB）扫描标定的（VA_VMK 定在 3.8）。真模型的贴图是正常的枪械色、
//      亮得多，同一套灯照上去就整片顶到纯白 —— 实测莫辛的枪管与机匣是纯白，
//      木质枪托反而正常（因为木色偏深）。
//   2) metallic 高：缺反射探针时镜面路径采不到环境，只剩黑。
//      本文件里程序化枪身那段注释记过同一个实测（metallic 0.78 的导轨显示
//      RGB(0,1,10)，整条直接消失）。
//   3) roughness 1.0：高光被完全摊平，圆柱面（枪管、弹鼓、圆盘弹匣）
//      退化成没有体积感的塑料片。
// 处理办法是把这三个参数按枪模那套标定改写，而不是去改灯：
// 改灯会连带把手里那两块程序化前臂（同一套灯）一起变暗。
void adopt_gun_subtree(Node *p_node) {
    MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_node);
    if (mi != nullptr) {
        vm_layer(mi);
        Ref<Mesh> mesh = mi->get_mesh();
        if (mesh.is_valid()) {
            const int ns = mesh->get_surface_count();
            for (int i = 0; i < ns; ++i) {
                StandardMaterial3D *sm =
                    Object::cast_to<StandardMaterial3D>(mesh->surface_get_material(i).ptr());
                if (sm == nullptr) continue;
                // 枪离相机只有几十厘米，阴影贴图的精度远远不够，
                // 蹭上一点就是一道跟着视角爬的条纹，比没有阴影难看得多。
                sm->set_flag(BaseMaterial3D::FLAG_DONT_RECEIVE_SHADOWS, true);

                // 只打一次原始参数：这是标定 albedo 系数的依据，
                // 而"看过一眼就不需要再看"—— 每次加载都刷一遍会淹掉别的日志。
                static bool s_reported = false;
                if (!s_reported && art_report_enabled()) {
                    s_reported = true;
                    UtilityFunctions::print(String::utf8("[vm] 真模型材质 #"), i,
                                            String::utf8(" albedo="), sm->get_albedo(),
                                            String::utf8(" metallic="), sm->get_metallic(),
                                            String::utf8(" roughness="), sm->get_roughness(),
                                            String::utf8(" 有贴图="), sm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO).is_valid());
                }

                // 三个旋钮**恒定覆盖**，不再做条件判断：
                // 原始参数是图生3D 给离线渲染器的（1.0 / 1.0），
                // 留哪一项都会留一个坑。alpha 钉 1.0 —— 透明度没开，
                // 但带着小于 1 的 alpha 会让下一次读参数时误判。
                const float a = art_albedo_scale();
                sm->set_albedo(Color(a, a, a, 1.0f));
                sm->set_metallic(art_metal());
                sm->set_roughness(art_roughness());
            }
        }
    }
    const int nc = p_node->get_child_count();
    for (int i = 0; i < nc; ++i) {
        adopt_gun_subtree(p_node->get_child(i));
    }
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

Ref<CylinderMesh> tube(float r, float h, int seg = 10) {
    Ref<CylinderMesh> c = memnew(CylinderMesh);
    c->set_top_radius(r);
    c->set_bottom_radius(r);
    c->set_height(h);
    c->set_radial_segments(seg);
    c->set_rings(1);
    return c;
}

// 在两点之间架一根圆柱（前臂、枪管都用它）。
// CylinderMesh 的轴是局部 +Y，所以要构造一个把 +Y 旋到 a→b 方向的基。
// 手写旋转角很容易把前臂摆成"立起来的柱子"（第一版就是这么错的）。
MeshInstance3D *limb(Node3D *parent, const Vector3 &a, const Vector3 &b, float r,
                     const Ref<Material> &mat) {
    const Vector3 d = b - a;
    const float len = d.length();
    if (len < 1e-4f) return nullptr;
    const Vector3 dir = d / len;
    const Vector3 up(0.0f, 1.0f, 0.0f);
    Basis basis;
    const float dp = va::clampf(up.dot(dir), -1.0f, 1.0f);
    if (dp > 0.9999f) {
        basis = Basis();
    } else if (dp < -0.9999f) {
        basis = Basis(Vector3(1, 0, 0), 3.141592653589793f);
    } else {
        basis = Basis(up.cross(dir).normalized(), std::acos(dp));
    }
    MeshInstance3D *mi = memnew(MeshInstance3D);
    mi->set_mesh(tube(r, len, 8));
    mi->set_transform(Transform3D(basis, (a + b) * 0.5f));
    mi->set_material_override(mat);
    vm_layer(mi);
    parent->add_child(mi);
    return mi;
}

} // namespace

void ViewModel::build(Camera3D *p_cam, Node *p_proto_parent) {
    if (p_cam == nullptr) return;
    cam = p_cam;
    proto_parent = p_proto_parent;

    // 开发期旋钮：不动代码就能扫姿态。VA_VM_HIP / VA_VM_AIM / VA_VM_ROT。
    env_vec3("VA_VM_HIP", hip_pos);
    env_vec3("VA_VM_AIM", aim_pos);
    env_vec3("VA_VM_ROT", hip_rot);

    // 开镜视场由基础视场推导 —— 改世界 FOV 时枪的放大倍率自动跟随。
    fov_base = p_cam->get_fov();
    fov_ads = fov_base * 0.70f;

    root = memnew(Node3D);
    root->set_name("ViewModel");
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
    const float s_alb = va::clampf(env_f("VA_VM_SLEEVE", 0.500f), 0.02f, 1.0f);
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
    part(hand_r, box(0.054f, 0.080f, 0.080f), HAND_R_BASE, Vector3(14, 0, 0), glove);            // 右手掌（手背朝相机）
    part(hand_r, box(0.020f, 0.024f, 0.030f),                                                    // 右手拇指
         HAND_R_BASE + Vector3(-0.034f, 0.030f, -0.014f), Vector3(24, -18, 0), glove);
    limb(hand_r, HAND_R_BASE + Vector3(0.004f, -0.006f, 0.030f),          // 起笔在掌背，不在掌心
         Vector3(0.290f, -0.540f, -0.130f), FOREARM_R, sleeve);                                  // 右前臂

    part(hand_l, box(0.056f, 0.086f, 0.086f), HAND_L_BASE, Vector3(), glove);                    // 左手掌（托在护木下）
    for (int i = 0; i < 3; ++i) {                                                                // 左手四指扣过护木顶面
        part(hand_l, box(0.058f, 0.017f, 0.021f),
             HAND_L_BASE + Vector3(0.0f, 0.046f, (float)(i - 1) * 0.028f), Vector3(), glove);
    }
    part(hand_l, box(0.018f, 0.022f, 0.034f),                                                    // 左手拇指
         HAND_L_BASE + Vector3(-0.036f, 0.022f, -0.030f), Vector3(0, -20, 0), glove);
    limb(hand_l, HAND_L_BASE + Vector3(0.004f, -0.006f, 0.036f),          // 起笔在掌背，不在掌心
         Vector3(-0.320f, -0.540f, -0.150f), FOREARM_R, sleeve);                                 // 左前臂

    // ------------------------------------------------------------------
    // 枪口 + 枪口焰
    // ------------------------------------------------------------------
    muzzle = memnew(Node3D);
    muzzle->set_position(Vector3(0, 0.000f, -0.575f));
    gun->add_child(muzzle);

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
    if (hand_r != nullptr) hand_r->set_position(hr - HAND_R_BASE);
    if (hand_l != nullptr) hand_l->set_position(hl - HAND_L_BASE);

    for (int i = 0; i < art_slots; ++i) {
        if (art_nodes[i] != nullptr) art_nodes[i]->set_visible(i == p_index);
    }
    art_index = p_index;

    // 位置与朝向都由 scene_builder 归一化进 gun 局部系了，这里不再动 art_holder。
    if (muzzle != nullptr) muzzle->set_position(Vector3(0.0f, 0.0f, art_muzzle_z));

    // 真模型接管枪身，程序化枪身整块让位。
    //
    // 双手**要留着**：参考图是光枪，生成的模型里本来就没有手，
    // 连手一起藏掉的话画面上就是一把悬空的枪 —— 这是上一轮踩的坑
    // （当时嫌那两块粗方块"抢眼"，就把默认值改成了藏，结果连"手持"都丢了）。
    // 现在的取舍：手默认显示；每把枪的握持点标定在 kWpnArt 的 hand_r / hand_l 里，
    // 换枪时整组跟着走。实在想对照"有手/没手"，用 VA_VM_HANDS=0。
    if (proc_body != nullptr) proc_body->set_visible(false);
    if (hands != nullptr) hands->set_visible(hands_enabled());

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

    // ---- 合成位置 ----
    Vector3 pos = hip_pos.lerp(aim_pos, ads);
    pos.x += bx + brx + sway_yaw * 0.35f;
    pos.y += by + bry + sway_pitch * 0.35f - rl * 0.115f;
    // 后坐退让：开镜时行程要收窄，否则枪托会顶到近裁剪面上
    pos.z += recoil * 0.045f * (1.0f - ads * 0.60f);
    pos.y += recoil * 0.017f;
    pos.x += rl * 0.045f;
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

    // ---- FOV：开镜收窄视场，是"进入瞄准"最直接的视觉信号 ----
    if (cam != nullptr) {
        cam->set_fov(fov_base + (fov_ads - fov_base) * ads);
    }
}

} // namespace volunteer_army
