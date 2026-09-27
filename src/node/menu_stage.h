#pragma once
// VolunteerArmyPC —— 界面外壳的「三维主视觉」：主菜单里缓转的纪念章
//
// ============================================================ 为什么需要这一层
// 界面外壳（主菜单 / 任务简报）此前是**纯 2D**：一个 CanvasLayer + 全屏贴图，
// 见 world_sim.cpp 的 setup_runtime_ui。所以"把 3D 生成出来的模型摆进界面"
// 这件事在本工程里**没有任何现成的挂点** —— 不是缺素材，是缺管线。
//
// 本层补的就是这条管线，而且只补**一次**：SubViewportContainer → SubViewport
// （自带 World3D）→ 相机 + 灯光 + 环境 → 一个绕 Y 缓转的 pivot。
// 换素材只需换 assets/art/ui/model/<键>.glb，代码一行不用动；
// 以后要在简报页摆立体沙盘、在结算页摆勋章，复用同一套。
//
// ============================================================ 坐标与放置
//   · 容器挂在**外壳的 CanvasLayer** 上（和 Hud 同级），不是挂在 3D 世界里 ——
//     外壳期间战局是冻结的、相机被摆到地图别处，界面元素不能跟着战场跑。
//   · 位置按**与 HUD 同一套 s_ 缩放**算（s_ = clamp(视口高/1080, 0.62, 2.20)，
//     hud.cpp:162），左栏的 150*s_ 右边距在舞台这侧镜像复用 ⇒ 两侧留白对称。
//   · 容器中心固定，模型只**自转**：相机不动 ⇒ 不会有透视漂移，
//     光影也不会随角度跳变（转相机会）。
//
// ============================================================ 透明背景这件事
// SubViewport 开 transparent_background，背景透出背后的 key art。
// 环境用**天空**只是为了拿到环境光与金属反射（金属没有反射就是一块黑），
// 不是要把天空画出来 —— 若哪天发现背景不透明了，把 env 的 background 从
// BG_SKY 改成 BG_COLOR（bg_color 的 alpha 给 0）即可，其余一行不用改。
//
// ============================================================ 旋钮（都不必重编译）
//   VA_STAGE=0            整体关闭：**一个节点都不建**（画面与未加本层逐像素相同）
//   VA_STAGE_SPIN=<rad/s> 自转速度，默认 0.30（约 18 秒一圈）
//   VA_STAGE_YAW=<度>     模型的固定偏航（把"章面朝向镜头"标定出来用；默认 0）
//   VA_STAGE_PITCH=<度>   模型的固定俯仰（默认 0）
//   VA_STAGE_FRAC=<0..1>  容器边长占**视口高**的比例（默认 0.54）
//   VA_STAGE_MARGIN=<s倍> 右侧留白（默认 150，与 HUD 左栏的 x0 同一口径）
//   VA_DBG_STAGE=1        收尾报一行：模型是否加载、包围盒、自转角

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/sub_viewport_container.hpp>
#include <godot_cpp/variant/string.hpp>

namespace volunteer_army {

class MenuStage {
public:
    /* 建 SubViewportContainer 那一整套。p_parent 是外壳的 CanvasLayer。
       模型缺失或 VA_STAGE=0 时本层**自我关闭**（enabled_=false），什么都不留。 */
    void setup(godot::Node *p_parent);

    /* 每帧调一次（**墙钟**，不是战局秒 —— 外壳期战局 t 是冻结的，
       用 t 驱动的话菜单里的章会一动不动）。
       p_screen 传 Hud::Screen 的整数值（0=菜单 1=简报 2=战斗中）——
       本层只认 0；用 int 而不是枚举，是为了不必为一个值去 include hud.h。 */
    void step(double p_delta, int p_screen);

    bool enabled() const { return enabled_; }

    // 收尾（WorldSim 的 dump 链）报一行。
    godot::String dump() const;

private:
    godot::SubViewportContainer *cont_ = nullptr;
    godot::SubViewport *sub_ = nullptr;
    godot::Node3D *pivot_ = nullptr;   // 挂模型、绕 Y 自转的那一层

    bool  enabled_ = false;
    bool  dbg_ = false;
    int   frames_ = 0;                // 真正被显示过的帧数（诊断：确认它推进过）
    int   size_frac_ok_ = 0;

    float spin_ = 0.30f;              // 弧度/秒
    float yaw_deg_ = 0.0f;            // 模型固定偏航
    float pitch_deg_ = 0.0f;          // 模型固定俯仰
    float frac_ = 0.54f;              // 容器边长 / 视口高
    float margin_ = 150.0f;           // 右侧留白（s 倍）
    float ang_ = 0.0f;                // 累计自转角（弧度）

    bool  model_ok_ = false;
    float model_max_dim_ = 0.0f;
};

} // namespace volunteer_army
