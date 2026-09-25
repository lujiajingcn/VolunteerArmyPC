#pragma once
// VolunteerArmyPC —— 单位"跑动动作"：无骨骼模型的程序化跑动节奏
//
// ============================================================ 为什么是"节奏"而不是关节动画
// 场上 11 个角色模型（assets/art/char/model/*.glb）**全部是单块融合网格**，
// 实测每个都是 `nodes=1 meshes=1 prims=1 skins=0 joints=0 animations=0`：
// 没有骨架、没有蒙皮、没有动画轨 —— 所以**用骨骼驱动**腿是做不到的。
//
// 于是**本层**做的是**整体跑动节奏**：上下起伏 + 前倾 + 左右摇 + 肩部摆 + 头部微点。
// 判据不是"好看"，而是"能不能读出人在跑"：一个静止的人不会周期性上下颠、
// 不会前倾、不会左右摇 —— 这三条同时出现，远处几十像素高的士兵也会被读成"在跑"。
//
// 【2026-09-25 更正】**这里原来写的是"腿前后摆做不到"，那是错的** ——
// 它把"不能用骨骼驱动"这条实现路径的限制，说成了能力的边界。确切说法只是
// "没有骨骼可驱动"，而不是"做不到"：
//   实测（工具 `_inspect_glb_legs.py`）腿区间内**跨中线三角形 = 0 个**、
//   中轴 ±2%身高 内几乎无顶点 ⇒ **左右腿本来就是几何上分离的两簇**。
//   所以"按 sign(x) 分左右 + 着色器顶点位移"就能做出真腿摆，
//   既不需要骨架，也不需要重做素材。那条路在 **node/unit_leg.{h,cpp}**。
// 两层的关系：本层管**整体**（作用在单位节点的变换上），UnitLeg 管**腿部局部形变**
// （作用在网格顶点上），互不冲突、可叠加；相位**共用同一个来源** ——
// 本层算出的 PoseVals::leg_phase / leg_deg 直接喂给 UnitLeg，
// 于是"腿在摆而躯干不动"这种脱节不会发生，起步/停下的淡入淡出也自动一致。
//
// ============================================================ 坐标口径（最容易错的一处）
// local_pose() 返回的变换要挂在 unit_transform 的**右侧**（base * pose），
// 因此它工作在**单位模型自己的局部系**里：
//     原点 = 脚底（norm 已经把包围盒底面对到 y=0）
//     前方 = +X（与逻辑层一致，UNIT_MODEL_YAW_DEG 校正完之后）
//     上方 = +Y
//     侧向 = ±Z
// 所以三个旋转轴各管一件事，别用错：
//     绕 Z = 俯仰（前倾 / 点头）
//     绕 X = 侧倾（左右摇）
//     绕 Y = 偏航（肩部摆）
// 单位是**米**（模型归一化后 1 世界单位 = 1 米，士兵高 1.68 m）。
//
// ============================================================ 速度从哪来
// 不用 Unit.moving，也不用 Unit.vx/vy。理由：单位位移不止 move_step 一条路
// （编队跟随是 lerp 直接写 x/y，见 va_ai_ally.cpp:76；出生摆放见 va_world.cpp:290），
// 那两条都不置 moving，用 moving 判会出现"人在跑但不动"。
// 这里改成**按相邻两帧的逻辑坐标差反推速度**，与"谁移动的"完全解耦 ——
// 表现层只读状态，逻辑层改了 AI 也不用同步改这里。
//
// ============================================================ 相位种子复用了一个死字段
// Unit.wobble 只写不读：va_ai_ally.cpp:36 每帧 `+= dt*6`，va_world.cpp:305 出生时
// 用 RNG 随机播种。它本来就是个没被消费的"表现用相位"，正好拿来当**每个单位各自的
// 初始相位** —— 一支小队因此不会整整齐齐同手同脚（这是最刺眼的假）。
// 若哪天有人把 wobble 拿去做别的用途，相位种子会跟着变 —— 也只是起步相位不同，
// 不影响正确性。

#include <cstddef>
#include <deque>
#include <vector>

#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "sim/va_types.h"

namespace volunteer_army {

class UnitAnim {
public:
    // 一帧的姿态分量。抽出来是为了**能被打印**：像素差受"两次运行的帧对齐"干扰，
    // 而"某速度下姿态算出来是多少"是直接可读的判据（见 tick_diag）。
    struct PoseVals {
        float bob = 0.0f;    // 垂直偏移（米）
        float lean = 0.0f;   // 前倾（度，正值表示前倾）
        float roll = 0.0f;   // 左右摇（度）
        float yaw = 0.0f;    // 肩部摆（度）
        float nod = 0.0f;    // 点头（度）
        float amp = 0.0f;    // 幅度包络 0..1
        float speed = 0.0f;  // 地速（米/秒）
        bool  active = false;
        /* ---- 下面两个不进变换，而是喂给 UnitLeg（网格顶点位移）----------------
           为什么放在这里而不是让 UnitLeg 自己算：腿摆与躯干摆必须**同相**，
           而"相位推进 / 幅度包络 / 步频随速度"这一整套只应该有一份实现。
           UnitLeg 自己再算一遍，就等于把同一件事抄成两份，迟早对不上 ——
           症状是"腿和身子各摆各的"，而这类偏差在截图上也很难一句话说清。
           leg_phase 单位弧度，leg_deg 单位度（已乘好包络与摆幅）。 */
        float leg_phase = 0.0f;
        float leg_deg = 0.0f;
    };

    // 读环境旋钮（VA_RUN / VA_RUN_* / VA_DBG_RUN）。幂等。
    void setup();

    /* 每帧调用一次，且要在单位位置**已经更新之后**（WorldSim::sync_entity_nodes 的开头）。
       时间基取 va::W.t（战局秒），不取墙钟 —— 这样 VA_FF 快进时动作与逻辑同步加速，
       检阅台 / 菜单期（t 冻结）自动不动。
       注意 sync_entity_nodes 有多个调用点，同一帧可能被调多次：dt=0 时内部一切保持，
       不会因为多调一次就把幅度抖掉。 */
    void step(const std::deque<va::Unit> &p_units);

    // 叠加到 unit_transform 右侧的单位局部姿态。未跑动时返回**恒等**变换。
    godot::Transform3D local_pose(std::size_t p_i) const;
    PoseVals pose_vals(std::size_t p_i) const;

    /* 纯函数：给定相位与地速，算一组姿态分量。**没有状态、不依赖战场** ——
       检阅台的"跑动排"用它把一整个步态周期按相位均分摆成一排
       （`VA_UNIT_SHOW=run:<键>`）。一条静态截图就能看到整段循环，
       比"隔 0.15 秒拍三张"可靠得多：不依赖帧对齐，也不需要 VA_FF=1。 */
    PoseVals pose_at(float p_phase, float p_speed) const;
    // 把分量合成成单位局部变换（pose_at 的输出 → Transform3D）。
    static godot::Transform3D pose_transform(const PoseVals &p_v);
    // 满速参考（米/秒）= 46 逻辑单位/秒 × 0.05（定义在 .cpp）。检阅台标定时用得到。
    float full_speed_mps() const;

    bool  enabled() const { return enabled_; }
    int   moving_count() const { return moving_; }
    float max_speed() const { return max_speed_; }

    // VA_DBG_RUN=1：每 1 秒墙钟打一行（跑动人数 / 最快速度 / 幅度），
    // 或经 dump() 由 world_sim 在收尾时打一次。
    void tick_diag(double p_wall_delta);
    godot::String dump() const;

private:
    struct St {
        float x = 0.0f, y = 0.0f;
        bool  have = false;
        float speed = 0.0f;      // 平滑后的地速（米/秒）
        float phase0 = 0.0f;     // 每个单位各自的起步相位（种子 = u.wobble）
        float phase = 0.0f;      // 累计相位（弧度）
        float amp = 0.0f;        // 幅度包络 0..1（静止时归零，避免原地抖）
    };

    std::vector<St> st_;
    float prev_t_ = -1.0f;
    bool  enabled_ = true;
    bool  dbg_ = false;

    // 可调参数（VA_RUN_* 覆盖，用于不重编译标定）
    float kAmp_ = 1.0f;      // 总幅度倍率
    float kStrideHz_ = 1.35f;// 满速时的步频（完整步态周期/秒）
    float kBob_ = 0.055f;    // 垂直起伏幅度（米，一个周期内上下各一次）
    // 前倾 = 基础角 + 随速度增量，**单位是度**（下面的角度都按度给，最后统一转弧度）。
    // 满速时 4+6=10°：跑起来的前倾在 8~12° 之间，低于 6° 读不出"在用力"。
    float kLean_ = 4.0f;
    float kLeanSpd_ = 6.0f;
    float kRoll_ = 3.5f;     // 左右摇（度）
    float kYaw_ = 3.5f;      // 肩部摆（度）
    float kNod_ = 2.0f;      // 头部微点（度）
    /* 大腿摆幅基准（度）。本层只负责"算出该摆多少"，真正让腿动起来的是
       node/unit_leg 那条着色器顶点位移 —— 摆角经 PoseVals::leg_deg 传过去。
       VA_LEG_SWING 覆盖（UnitLeg 不重复读这个旋钮，摆幅只有这一处定义）。
       26° 的量级：跑步时大腿前摆约 25~35°，走路约 15~20°；
       这里偏跑步，因为本层的动作设计就是"跑"（满速 2.3 m/s 是冲刺）。 */
    float kLegSwing_ = 26.0f;

    int    moving_ = 0;
    float  max_speed_ = 0.0f;
    int    max_i_ = -1;            // 本帧最快的是哪个单位（诊断用）
    // 开跑以来的峰值地速。**「这一局到底有没有单位真的跑起来过」只有它能回答** ——
    // 1 秒一行的瞬时值可能正好都落在停顿间隙里（实测踩过：整局 32 行全是 0，
    // 读起来像"没人动过"，其实是采样点恰好在所有人的静止相位上）。
    float  peak_speed_ = 0.0f;
    double dbg_acc_ = 0.0;
};

} // namespace volunteer_army
