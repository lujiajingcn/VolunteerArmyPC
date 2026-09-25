#pragma once
// VolunteerArmyPC —— 单位「双腿交替」：无骨骼模型的程序化分腿摆
//
// ============================================================ 一句话
// 用**着色器顶点位移**把角色网格的腿部按"左/右"与"离髋多远"分别绕髋旋转，
// 左右相位差 π ⇒ 天然交替。不动素材、不动逻辑层、无骨骼。
//
// ============================================================ 为什么上一轮说"做不到"，现在又做了
// unit_anim.h 里写着"腿摆做不到"。那句话的**确切含义**是
// "用骨骼驱动做不到"（11 个 glb 全是单块融合网格：nodes=1 meshes=1
// prims=1 skins=0 joints=0 anims=0，没有可驱动的骨骼）。
// 但它被写成了"做不到"，把一个实现路径的限制说成了能力的边界 —— 这是错的。
//
// 真正把这条路打开的是**实测出来的一个几何事实**（工具 `_inspect_glb_legs.py`）：
//     腿区间（身高下 35~45%）里，**跨中线的三角形 = 0 个（0.0%）**，
//     且中轴 ±2%身高 范围内**几乎没有顶点**（"中轴谷" ≈ 0%）。
// 即：左右腿本来就是**几何上分离的两簇**，中间有一条空谷。
// ⇒ 按 `sign(x)` 判左右既可靠、又不会撕开网格 —— 不需要骨架就能"分腿"。
//
// ============================================================ 为什么是顶点位移，不是蒙皮
// 三条路都评估过（详见 .workbuddy/memory/2026-09-25.md）：
//   ① 着色器顶点位移（本文件所在的路）—— 零 CPU、零内存、~100 行；代价是材质要换。
//   ② 运行时造骨架 + Skin + 权重 —— 材质不用动；代价是 ~150 行 C++、要自己算
//      bind 矩阵、11 个角色各多一份改过的 mesh（约 27 MB），出错面大得多。
//   ③ 重新生成带骨骼的素材 —— 最"正规"，但要重花 11 × 40 积分，且图生3D
//      **不保证**输出骨骼，等于把已跑通的角色管线推翻重来。
// 选 ①。
//
// ============================================================ ⚠️ 坐标口径（错一次，所有符号全反）
// 顶点数据是 Blender 的 Z-up，靠 **root 节点上的 +90°X 旋转** 才变成引擎的 Y-up
// （场景名 "convert"，Blender glTF I/O v4.0.43）。所以**着色器里 `VERTEX` 看到的是**：
//     X = 左右（±）
//     Y = 前后（**+Y 是模型正面**）
//     Z = 高度，但 **−Z 朝上** —— z = 0 是**脚**，z = −H 是**头顶**（H ≈ 1.05~1.09）
// ⇒ "在髋以下"的判据是 `VERTEX.z > hip_z`（**大于**）。这与直觉相反，是量出来的。
//    量错一次的代价：第一版体检脚本没套节点旋转，把"前后轴"当高度、
//    "身高轴"当左右，报出"跨中线 0%、Z 全 ≤0"，看着像模型的性质 —— 其实是量错了轴。
//
// ============================================================ 与 UnitAnim 的分工
//   UnitAnim —— **整体**跑动节奏（起伏 / 前倾 / 侧摇 / 肩摆 / 点头），
//               作用在单位节点的变换上（`unit_transform * local_pose`）。
//   UnitLeg  —— **腿的局部形变**，作用在网格顶点上，两者互不冲突、可叠加。
// 相位共用同一个来源：`UnitAnim::PoseVals::leg_phase / leg_deg`（由 pose_at 算），
// 所以"腿在摆而躯干不动"这种脱节不会发生，静止/起步/停下的淡入淡出也自动一致。
//
// ============================================================ 材质怎么办（唯一被改动的既有行为）
// 顶点位移必须挂在自定义着色器上，而角色原本是 Godot 导入 glTF 生成的
// `StandardMaterial3D`。所以本层会把它换成 `ShaderMaterial`，并把三张贴图搬过去：
//      albedo ← TEXTURE_ALBEDO（: source_color）
//      orm    ← TEXTURE_ROUGHNESS / METALLIC / ORM（glTF 的 MR 布局与 Godot 的 ORM 同布局，
//                G=roughness, B=metallic，所以直接按通道取）
//      normal ← TEXTURE_NORMAL（: hint_normal）
// 着色器只需输出 ALBEDO / ROUGHNESS / METALLIC / NORMAL_MAP，PBR 光照仍由引擎管线负责，
// 不自己写光照。
//
// **VA_LEG=0 时本层完全不动作**（不加载着色器、不换材质、不写实例参数），
// 于是画面与"加这个功能之前"逐像素相同 —— 这正是消融取证的对照组，也是失败时的回退开关。
//
// ============================================================ 旋钮（都不必重编译）
//   VA_LEG=0            整体关闭（= 回退到原始材质）
//   VA_LEG_SWING=<度>   大腿摆幅，默认 26（同时是 UnitAnim 的基准，见 unit_anim.cpp）
//   VA_LEG_HIP=<0..1>   髋高占**包围盒高度**的比例，默认 0.52
//   VA_LEG_BAND=<0..1>  髋下过渡带长度占包围盒高度的比例，默认 0.15
//   VA_LEG_KNEE=<度>    小腿额外弯曲，默认 0（关闭）
//   VA_LEG_KNEE_POS=<0..1> 膝高比例，默认 0.28
//   VA_LEG_PROBE=1      染色探针：左腿绿 / 右腿红、亮度=权重，髋以上中性灰
//   VA_LEG_PROBE=2      把**每实例参数**画成颜色（R=相位 G=摆角 B=髋高）。
//                       跑动排 6 格相位均分 ⇒ 颜色必须逐格不同；
//                       若同色，说明 instance uniform 没逐实例送达。
//   VA_DBG_LEG=1        逐键打印 H / 髋高 / 过渡带（标定髋高时看这个）

#include <map>
#include <string>

#include <godot_cpp/classes/canvas_item.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/string.hpp>

namespace volunteer_army {

class UnitLeg {
public:
    // 读环境变量 + 载入着色器。幂等。失败时内部置 enabled_=false 并**保持原材质**。
    void setup();

    bool enabled() const { return enabled_; }

    /* 给一个单位节点套上腿摆。
       幂等、且**每帧调用**是设计内的用法：
         - 节点会被整体重建（增援 / 重开一局），缓存节点指针必然悬空，
           所以这里每次现场在子树里找 MeshInstance3D（深度 ≤ 3，代价可忽略）。
         - set_material_override 传同一个材质时 Godot 内部 early-return，
           不会每帧触发一遍通知。
       p_swing_deg = 0 时形变退化为恒等（站桩不抖），但材质仍是着色器材质 ——
       检阅台的"近景单体"靠这一点与跑动排保持同一套外观，材质对照才有意义。 */
    void apply(godot::Node3D *p_unit_node, const std::string &p_key,
               float p_phase, float p_swing_deg);

    // VA_DBG_LEG / 收尾：逐键报 H、髋高、过渡带；报一共挂上了几个网格。
    godot::String dump() const;

private:
    struct KeyInfo {
        godot::Ref<godot::ShaderMaterial> mat;
        float hip_z = 0.0f;        // 髋高（mesh 空间 z）
        float band = 0.16f;        // 过渡带长度（mesh 单位）
        float knee_z = -1e9f;      // 膝高（mesh 空间 z）
        bool  ok = false;          // false = 该键没有可用网格/材质，不再重试
        bool  reported = false;
    };

    // 深度优先找子树里第一个 MeshInstance3D。
    static godot::MeshInstance3D *find_mesh(godot::Node3D *p_root);
    // 首次遇到某键时建立材质与几何参数；之后直接返回缓存。
    KeyInfo &ensure(const std::string &p_key, godot::MeshInstance3D *p_mi);

    std::map<std::string, KeyInfo> keys_;
    godot::Ref<godot::Shader> shader_;
    godot::Ref<godot::Texture2D> fallback_orm_;   // 1×1：G=0.8(rough) B=0(metal)

    bool  enabled_ = false;
    int   probe_ = 0;         // 0=关 1=权重染色（验髋高/左右） 2=每实例参数染色（验传递）
    bool  dbg_ = false;

    // 旋钮
    float hip_frac_ = 0.52f;
    float band_frac_ = 0.15f;
    float knee_deg_ = 0.0f;
    float knee_frac_ = 0.28f;

    int   attached_ = 0;      // 累计成功挂上的网格数（诊断）
    int   no_mesh_ = 0;       // 找不到网格的单位数（诊断）
};

} // namespace volunteer_army
