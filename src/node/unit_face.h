#pragma once
// VolunteerArmyPC —— 单位"朝向"的表现层平滑
//
// ============================================================ 这一层在修什么
// 逻辑层的 Unit::facing **没有角速度限制**。驱动它的两句是：
//     va_ai_ally.cpp:35    if (!u->aiming) u->facing = a;          ← 直接赋值
//     va_ai_ally.cpp:308   u->facing = normAng(u->facing + angDiff(a, u->facing) * min(1, dt*7));
// 而 35 行的 a 来自 steer_angle()：在"正前方 ±2.2 弧度共 9 个方向里挑第一个可通行的"。
//
// 于是只要"26 逻辑单位处通不通"在相邻两帧之间翻转，选中的偏角就会换档。
// 队员贴着掩体绕行时每帧只前进约 2.6 逻辑单位，采样点跟着挪，这种翻转很常见：
//     换档 +0.45 ↔ -0.45   →  facing 单帧跳 ±51°
//     换档 +0.95 ↔ -0.95   →  facing 单帧跳 ±109°
// 再叠一层：perceive() 是**周期性**刷新的（va_ai_ally.cpp:95，0.18~0.38 s 一次），
// 所以只要敌人和掩体的关系在临界上，u->aiming 就会随之翻转 ——
// facing 的驱动源就在"308 行的平滑追敌"与"35 行的直接赋值"之间来回切。
//
// 这一层之前，world_sim 是**逐帧把 u.facing 原样喂进 unit_transform**
// （world_sim.cpp:1300），也就是上面每一个跳变都 1:1 变成画面上的急转。
// 观察到的现象就是**队友在原地快速转圈** —— 位置几乎不动（移动被
// move_step 的 free_xy 否掉），只有朝向在左右横跳。
//
// ============================================================ 为什么不改逻辑层
// facing 确实参与逻辑：va_commands.cpp:233/246 用它反算 moveGoal
// （"向前推进 70 / 140 逻辑单位"），改它的时序就会改战局、进而动到
// sweep/sim 的离线平衡基线。而"朝向抖"对**弹道没有影响**：
// fire_at 用的是目标点坐标（va_combat.cpp:378 `spawn_bullet(u, tgt.tx(), tgt.ty(), …)`），
// 全程不读 u->facing。
// ⇒ 这纯粹是一件**表现**的事，就放在表现层做。**src/sim/ 一行不改。**
//
// ============================================================ 算法：每帧角速度限速 + 死区
//     d = angDiff(目标, 当前)                        // 归一到 (-π, π]，就近转
//     if (|d| < 死区) 保持                            // 死区：微抖完全静止
//     步长 = clamp(d, ±每帧上限)                      // ← 本层的核心
//     当前 = normAng(当前 + 步长)
//
// ⚠️ 为什么是"限速"而不是"低通比例"（本轮实测，从 low-pass 改过来的）：
// 掉头明细显示抖动的真身是 `Δ≈170~180°`、`moving=1 aiming=0 seen=0` ——
// 也就是 steer_angle 的兜底 `return base + π`（九向皆堵时返回**背对目标**的方向，
// va_ai_ally.cpp:47）与"某个偏角可行"之间的切换。
//
// 试过两条，都不行：
//  ① 一阶低通（当前 += d*k）。比例 k 必须压到 0.02 才能把 170° 压成看不出，
//     而那等于"42 帧才转到位" —— 真转身会慢成幻灯片。k=0.12（8 帧到位）时
//     实测平滑后单帧仍跳 20°（= 0.12 × 170，一眼看得出抖）。
//  ② 振荡抑制（连续两帧反向且都很大 ⇒ 冻结）。实测命中率极低（每区间 1~8 次
//     而抖动有 12~17 次）—— 因为形态**不是隔帧交替**，是"偶发整 180° 掉头"，
//     掉头的前后帧都是正常的，判不出"反向"。
//
// 限速为什么能同时解决：它不需要识别"这是抖还是转身"，只需要承认一条物理事实
// **人不可能在一帧里转 170°**。于是
//   交替翻转 → 有效目标被夹在两个极端之间，稳态摆幅 ≈ 每帧上限（4°）
//   单向转身 → 以每帧上限匀速转到位（视觉上就是正常的"转身"）
// 两种形态一条规则，参数只有一个，而且它是**度/帧**这个能直接读的量。
//
// ⚠️⚠️ 为什么是"每帧"而不是"每秒 × dt"（本轮踩到，代价一整轮取证）：
// 第一版写成 `d * min(1, dt * k)`，dt 取 va::W.t 的差分。结果实测**平滑完全没生效**
// （平滑后单帧 Δ 峰值 170°，与逻辑层的 170° 一模一样）。原因：
//   逻辑步长固定 1/60，而主循环是 `acc += delta * VA_FF; while (acc >= H) step_once(H)`
//   （world_sim.cpp:1393）⇒ **低帧率 + 快进时一帧能跑十几个逻辑步**。
//   实测 12 fps + VA_FF=3 ⇒ dt = 0.25 s ⇒ dt*k = 1.5 ⇒ min(1, …) 饱和 ⇒ 直通。
// 更本质的是"乘 dt"本身就不对：那样平滑强度会随**帧率**漂移
// （60 fps 追 10%、144 fps 追 4%、12 fps 追 100%），同一个旋钮在别人机器上是另一种手感。
// 按帧给没有这个毛病 —— 稳态摆幅只由每帧上限决定，与帧率无关。
// 代价：VA_FF 快进时朝向跟随**不会**跟着加速（取证时看着偏慢），实机 60 fps 正常。
//
// 只有**两个**旋钮：
//     VA_FACE_RATE  每帧最大转角（度），默认 4
//                   60 fps 下 = 240°/s ⇒ 转 180° 约 0.75 秒；同时把抖动摆幅夹在 4° 内
//     VA_FACE_DEAD  死区（度），默认 2
//
// ============================================================ 玩家自己不平滑
// 第一人称的朝向就是鼠标。任何滞后都是手感事故，所以 isPlayer 的单位原样直通。
// （自己那个节点默认根本不画，见 world_sim 的"第一人称不画自己的身体"；
// 但 VA_SHOW_SELF=1 会放回来，那时也不该让它比鼠标慢半拍。）
//
// ============================================================ 本层的两条纪律（与 anim_/leg_/fx_ 一致）
// 1. 只**读**逻辑层状态，一行逻辑都不改；删掉不影响任何逻辑。
// 2. **VA_FACE=0 时逐像素等同未加本层** —— display() 原样返回 u.facing。
//
// ============================================================ 开关
//     VA_FACE=0       整层关闭（消融用，也是"零代价"的证明）
//     VA_DBG_FACE=1   每 0.5 秒墙钟一行 + 收尾一行（见 dump()）

#include <cstddef>
#include <deque>
#include <vector>

#include <godot_cpp/variant/string.hpp>

#include "sim/va_types.h"

namespace volunteer_army {

class UnitFace {
public:
    // 读环境旋钮（VA_FACE / VA_FACE_K / VA_FACE_DEAD / VA_DBG_FACE）。幂等。
    void setup();

    /* 每帧调用一次，且要在**单位位置与朝向都已更新之后**。
       调用点与 anim_.step 相同（WorldSim::sync_entity_nodes 的开头），
       时间基也取 va::W.t（战局秒）而不是墙钟 —— VA_FF 快进时平滑跟着加速，
       简报 / 菜单期（t 冻结）自动保持。
       sync_entity_nodes 同一帧可能被调多次：dt=0 时内部一切保持，
       既不会多转一点，也不会把诊断计数重复记一遍。 */
    void step(const std::deque<va::Unit> &p_units);

    /* 第 i 个单位**该用哪个朝向上屏**（弧度）。语义与 va::Unit::facing 完全一样，
       可以直接替进 unit_transform 的第一个朝向参数。
       越界或本层关闭时返回... 见 .cpp（越界返回 0 并不会有单位命中，
       因为调用方恒按 va::W.units 的下标来问）。 */
    float display(std::size_t p_i) const;

    // 每关重来时清状态（重开一局不该带着上一关的显示朝向进来）。
    void reset();

    // VA_DBG_FACE=1：每 0.5 秒墙钟打一行；收尾由 dump() 打一次。
    void tick_diag(double p_wall_delta);
    godot::String dump() const;

    bool enabled() const { return on_; }

private:
    struct St {
        float shown = 0.0f;   // 当前显示朝向（弧度）—— 也就是"有效目标"
        float lf = 0.0f;      // 上一帧逻辑层 facing（算 Δ 用）
        float ldl = 0.0f;     // 上一帧的目标变化量（带符号，仅用于诊断"来回翻"）
        float lx = 0.0f, ly = 0.0f;
        bool  have = false;   // 首帧直接吸附，不从 0 转过去
        int   jump = 0;       // 逻辑层 |Δfacing| > 阈值 的帧数
        int   spin = 0;       // 其中"原地"（位置几乎没动）的帧数
        int   osc = 0;        // 其中被判为"来回翻"的帧数（**只统计，不抑制**）
        float peak = 0.0f;    // 逻辑层 Δfacing 峰值（度）
    };

    std::vector<St>    st_;
    std::vector<float> shown_;      // 对外快照（display 用，避免暴露 St）
    float prev_t_ = -1.0f;

    bool  on_ = true;
    bool  dbg_ = false;
    // 可调参数
    float rate_ = 4.0f;     // 每帧最大转角（度）
    float dead_ = 2.0f;     // 死区（度）
    float osc_deg_ = 60.0f; // "来回翻"的**诊断**阈值（度）—— 不再参与抑制

    // 诊断：区间值（每 0.5 秒打一行后归零）
    int   frames_ = 0;              // 本区间统计了多少帧（dt>0 才算）
    int   jump_ = 0, spin_ = 0;     // 逻辑层抖 / 其中"原地"
    int   osc_ = 0, all_osc_ = 0;   // 其中被判"来回翻"（振荡抑制生效）
    float peak_logic_ = 0.0f;       // 逻辑层 Δfacing 峰值（度）
    float peak_shown_ = 0.0f;       // 平滑后单帧 Δ 峰值（度）—— 复核用
    godot::String peak_logic_name_, peak_shown_name_;
    int   dbg_lines_ = 0;           // 本区间已打了几行"掉头明细"（限流用）
    double dbg_acc_ = 0.0;

    /* 诊断：累计值（**从不归零**，收尾 dump 用）。
       区间峰值每 0.5 秒清一次，收尾时很容易刚好落在没有抖动的窗口里，
       读起来像"这一局根本没抖" —— anim_ 的 peak_speed_ 就是为这个才额外留的。 */
    int   all_jump_ = 0, all_spin_ = 0;
    float all_peak_logic_ = 0.0f, all_peak_shown_ = 0.0f;
    godot::String all_pl_name_, all_ps_name_;
};

} // namespace volunteer_army
