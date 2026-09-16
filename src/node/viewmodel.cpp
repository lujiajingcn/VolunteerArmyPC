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

#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/cylinder_mesh.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "sim/va_math.h"
#include "sim/va_world.h"

using namespace godot;

namespace volunteer_army {

namespace {

// 枪模专用渲染层（layer 2 → bit 1）。世界物体不在这一层，所以那盏关键灯
// 不会跑去照亮旁边的石头和队友。
constexpr uint32_t VM_LAYER = 1u << 1;

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

void vm_layer(MeshInstance3D *mi) {
    mi->set_layer_mask(VM_LAYER);
    mi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
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

void ViewModel::build(Camera3D *p_cam) {
    if (p_cam == nullptr) return;
    cam = p_cam;

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
    const Ref<StandardMaterial3D> metal = vm_mat(dbg_mat ? Color(1, 1, 1) : Color(0.170f, 0.174f, 0.186f), 0.12f, 0.58f);
    const Ref<StandardMaterial3D> poly = vm_mat(dbg_mat ? Color(1, 0, 0) : Color(0.165f, 0.161f, 0.152f), 0.0f, 0.70f);
    const Ref<StandardMaterial3D> dark = vm_mat(dbg_mat ? Color(0.35f, 0.35f, 0.35f) : Color(0.092f, 0.090f, 0.086f), 0.0f, 0.62f);
    const Ref<StandardMaterial3D> glove = vm_mat(dbg_mat ? Color(0, 1, 0) : Color(0.160f, 0.152f, 0.140f), 0.0f, 0.92f);
    // 袖子：早期用 0.30 的橄榄绿会变成一根横穿画面的亮条；压到 0.110 又变成
    // 死黑竖杠（实测 64% 像素亮度 <20）。0.170 是这两者之间唯一可用的一档。
    const Ref<StandardMaterial3D> sleeve = vm_mat(dbg_mat ? Color(0, 0, 1) : Color(0.170f, 0.178f, 0.138f), 0.0f, 0.95f);

    // ------------------------------------------------------------------
    // 枪身。gun 局部原点 = 机匣后端（枪托接口）；-Z 为枪口方向；y=0 是枪管轴线。
    // 枪托向 +Z 伸到 +0.173，枪管向 -Z 伸到 -0.575，总长 0.748 m。
    // ------------------------------------------------------------------
    // 枪托往 +Z 只伸到 +0.134。别小看这 4 cm：开镜时枪托是整把枪离眼睛最近的部分，
    // 它每远 1 cm，屏幕上的面积就小一截。原来伸到 +0.173 时，开镜画面正中下方被
    // 托底板糊掉一大块（实测占屏宽 27%）。
    part(gun, box(0.046f, 0.084f, 0.018f), Vector3(0, -0.020f, 0.125f), Vector3(), poly);          // 托底板
    part(gun, box(0.042f, 0.064f, 0.108f), Vector3(0, -0.016f, 0.062f), Vector3(), poly);          // 伸缩托
    part(gun, box(0.034f, 0.016f, 0.096f), Vector3(0, 0.020f, 0.062f), Vector3(), poly);           // 托腮板

    part(gun, box(0.056f, 0.078f, 0.210f), Vector3(0, -0.014f, -0.105f), Vector3(), poly);         // 下机匣
    part(gun, box(0.058f, 0.030f, 0.212f), Vector3(0, 0.028f, -0.105f), Vector3(), metal);         // 上机匣
    part(gun, box(0.038f, 0.012f, 0.298f), Vector3(0, 0.045f, -0.112f), Vector3(), metal);         // 皮卡汀尼导轨
    part(gun, box(0.048f, 0.050f, 0.170f), Vector3(0, -0.006f, -0.310f), Vector3(), poly);         // 护木
    part(gun, box(0.030f, 0.010f, 0.120f), Vector3(0, -0.036f, -0.312f), Vector3(), metal);        // 护木下导轨
    part(gun, tube(0.0100f, 0.150f), Vector3(0, 0.000f, -0.470f), Vector3(90, 0, 0), metal);       // 枪管
    part(gun, tube(0.0175f, 0.045f, 8), Vector3(0, 0.000f, -0.552f), Vector3(90, 0, 0), metal);    // 枪口制退器
    part(gun, box(0.024f, 0.024f, 0.046f), Vector3(0, 0.017f, -0.416f), Vector3(), metal);         // 导气箍
    part(gun, box(0.014f, 0.030f, 0.013f), Vector3(0, 0.029f, -0.532f), Vector3(), metal);         // 准星

    part(gun, box(0.026f, 0.148f, 0.082f), Vector3(0, -0.108f, -0.115f), Vector3(-8, 0, 0), dark); // 弹匣
    part(gun, box(0.032f, 0.100f, 0.048f), Vector3(0, -0.090f, -0.020f), Vector3(18, 0, 0), dark); // 握把
    part(gun, box(0.028f, 0.008f, 0.050f), Vector3(0, -0.052f, -0.062f), Vector3(), metal);        // 扳机护圈
    part(gun, box(0.016f, 0.011f, 0.042f), Vector3(0.028f, 0.040f, -0.020f), Vector3(), metal);    // 拉机柄

    // ------------------------------------------------------------------
    // 分件细节。整把枪如果只有一档灰，在画面上会读成一块平板 —— 加细节比调亮度
    // 有效得多。这三组都是廉价的 BoxMesh，但把"一大块"切成了"有零件的一把枪"。
    // ------------------------------------------------------------------
    // 导轨齿：皮卡汀尼导轨的灵魂，沿 z 排 14 道
    for (int i = 0; i < 14; ++i) {
        const float z = -0.258f + (float)i * 0.0212f;
        part(gun, box(0.036f, 0.006f, 0.011f), Vector3(0, 0.051f, z), Vector3(), metal);
    }
    // 护木散热孔：两侧各 6 个，用深色件陷入
    for (int i = 0; i < 6; ++i) {
        const float z = -0.372f + (float)i * 0.0258f;
        part(gun, box(0.007f, 0.019f, 0.015f), Vector3(0.026f, -0.008f, z), Vector3(), dark);
        part(gun, box(0.007f, 0.019f, 0.015f), Vector3(-0.026f, -0.008f, z), Vector3(), dark);
    }
    // 机匣侧面：抛壳口 / 快慢机 / 弹匣释放钮 / 拉机柄槽
    part(gun, box(0.010f, 0.026f, 0.074f), Vector3(0.030f, 0.006f, -0.148f), Vector3(), dark);   // 抛壳口
    part(gun, tube(0.011f, 0.012f, 8), Vector3(0.030f, -0.030f, -0.086f), Vector3(0, 0, 90), metal); // 快慢机
    part(gun, box(0.010f, 0.016f, 0.018f), Vector3(0.030f, -0.046f, -0.052f), Vector3(), metal); // 弹匣释放钮
    part(gun, box(0.008f, 0.010f, 0.090f), Vector3(0.030f, 0.026f, -0.150f), Vector3(), dark);   // 拉机柄槽
    // 枪托：托底板的调节卡榫 + 背带环
    part(gun, box(0.028f, 0.012f, 0.016f), Vector3(0, -0.052f, 0.076f), Vector3(), metal);       // 卡榫
    part(gun, tube(0.008f, 0.010f, 8), Vector3(-0.022f, -0.028f, 0.104f), Vector3(0, 0, 90), metal); // 背带环

    // 光学瞄具（红点）。开镜时它必须正好压住准心。
    // 外壳做成**框架**而不是实心 box：实心 box 会把镜片完全挡住，从后方看就是一个
    // 不透明的白方块（第一版就是这个效果，完全不像瞄具）。
    part(gun, box(0.040f, 0.008f, 0.104f), Vector3(0, 0.092f, -0.095f), Vector3(), dark);          // 上框
    part(gun, box(0.040f, 0.008f, 0.104f), Vector3(0, 0.058f, -0.095f), Vector3(), dark);          // 下框
    part(gun, box(0.008f, 0.026f, 0.104f), Vector3(-0.016f, 0.075f, -0.095f), Vector3(), dark);    // 左框
    part(gun, box(0.008f, 0.026f, 0.104f), Vector3(0.016f, 0.075f, -0.095f), Vector3(), dark);     // 右框
    part(gun, box(0.036f, 0.014f, 0.098f), Vector3(0, 0.049f, -0.095f), Vector3(), metal);         // 底座
    part(gun, box(0.026f, 0.028f, 0.004f), Vector3(0, 0.075f, -0.132f), Vector3(),
         vm_glow(Color(0.15f, 0.33f, 0.46f), 0.50f));                                              // 镀膜镜片
    Ref<SphereMesh> dot = memnew(SphereMesh);
    dot->set_radius(0.0060f);
    dot->set_height(0.012f);
    dot->set_radial_segments(8);
    dot->set_rings(4);
    part(gun, dot, Vector3(0, 0.075f, -0.135f), Vector3(),
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
    // ------------------------------------------------------------------
    part(gun, box(0.050f, 0.074f, 0.080f), Vector3(0, -0.080f, -0.015f), Vector3(14, 0, 0), glove); // 右手
    part(gun, box(0.012f, 0.020f, 0.055f), Vector3(0, -0.044f, -0.072f), Vector3(), glove);         // 右手食指
    limb(gun, Vector3(0.005f, -0.082f, -0.010f), Vector3(0.300f, -0.440f, -0.130f), 0.032f, sleeve); // 右前臂

    part(gun, box(0.058f, 0.062f, 0.088f), Vector3(-0.010f, -0.032f, -0.260f), Vector3(), glove);   // 左手
    limb(gun, Vector3(-0.006f, -0.034f, -0.260f), Vector3(-0.330f, -0.420f, -0.150f), 0.032f, sleeve); // 左前臂

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
}

void ViewModel::on_shot() {
    if (!valid()) return;
    // 一次冲量：位移 + 角速度。数值靠手感调 —— 太大像在"点头"，太小没反馈。
    recoil_v += 3.6f;
    recoil = std::min(recoil + 0.22f, 1.0f);
    flash_t = 0.045f;
    if (muzzle != nullptr) {
        // 每发随机滚一下枪口焰，避免连发时同一个形状反复闪
        muzzle->set_rotation_degrees(Vector3(0, 0, (float)(rand() % 360)));
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
    if (flash_t > 0.0f) {
        flash_t -= dt;
        const bool on = flash_t > 0.0f;
        flash_group->set_visible(on);
        if (flash_light != nullptr) {
            // 用剩余时间当衰减：枪口焰的持续时间极短，闪一下就灭
            flash_light->set_param(Light3D::PARAM_ENERGY, on ? 7.0f * (flash_t / 0.045f) : 0.0f);
        }
    }

    // ---- FOV：开镜收窄视场，是"进入瞄准"最直接的视觉信号 ----
    if (cam != nullptr) {
        cam->set_fov(fov_base + (fov_ads - fov_base) * ads);
    }
}

} // namespace volunteer_army
