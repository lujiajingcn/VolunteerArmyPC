// VolunteerArmyPC —— 单位跑动节奏实现（见 unit_anim.h 的完整设计说明）
#include "node/unit_anim.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "node/scene_builder.h"   // S（1 世界单位 = 0.05 米）
#include "sim/va_world.h"         // va::W.t（战局秒，本层的时间基）

namespace volunteer_army {
namespace {

constexpr float kPi = 3.14159265358979323846f;

/* 满速参考值：ally 的移动速度 = 46 逻辑单位/秒（va_ai_ally.cpp:17）
   × S(0.05 m/单位) = 2.30 m/s。逻辑层改这个数时，动作幅度只是"跑得更满/更不满"，
   不会错位 —— 因为速度是**量出来的**，不是假定的。 */
constexpr float kFullSpeedMps = 46.0f * S;

// 低于这个速度完全不做动作（站桩 / 微调站位时不该抖）
constexpr float kAmpLoMps = 0.35f;
// 到这个速度幅度到 1
constexpr float kAmpHiMps = 1.15f;

float env_f(const char *p_key, float p_def) {
    const char *v = std::getenv(p_key);
    if (v == nullptr || *v == '\0') return p_def;
    return (float)std::strtod(v, nullptr);
}

bool env_flag(const char *p_key, bool p_def) {
    const char *v = std::getenv(p_key);
    if (v == nullptr || *v == '\0') return p_def;
    return !(std::strcmp(v, "0") == 0 || std::strcmp(v, "false") == 0);
}

template <typename T>
T clampf(T p_v, T p_lo, T p_hi) {
    return p_v < p_lo ? p_lo : (p_v > p_hi ? p_hi : p_v);
}

// 每秒衰减到 0 的比例（用于包络做"指数趋近"）
float approach(float p_cur, float p_want, float p_dt, float p_rate) {
    const float k = clampf(p_dt * p_rate, 0.0f, 1.0f);
    return p_cur + (p_want - p_cur) * k;
}

} // namespace

// ============================================================ setup
void UnitAnim::setup() {
    enabled_ = env_flag("VA_RUN", true);
    dbg_ = env_flag("VA_DBG_RUN", false);

    if (const char *e = std::getenv("VA_RUN_AMP"))    kAmp_ = (float)std::atof(e);
    if (const char *e = std::getenv("VA_RUN_HZ"))     kStrideHz_ = (float)std::atof(e);
    if (const char *e = std::getenv("VA_RUN_BOB"))    kBob_ = (float)std::atof(e);
    if (const char *e = std::getenv("VA_RUN_LEAN"))   kLean_ = (float)std::atof(e);
    if (const char *e = std::getenv("VA_RUN_ROLL"))   kRoll_ = (float)std::atof(e);
    if (const char *e = std::getenv("VA_RUN_YAW"))    kYaw_ = (float)std::atof(e);
    if (const char *e = std::getenv("VA_RUN_NOD"))    kNod_ = (float)std::atof(e);
    /* 大腿摆幅基准。**只有这一处读 VA_LEG_SWING** ——
       摆角由本层算好后经 PoseVals::leg_deg 传给 UnitLeg，
       UnitLeg 那边不再读一遍（两处都读的话，改一个旋钮会得到两种结果）。 */
    if (const char *e = std::getenv("VA_LEG_SWING"))  kLegSwing_ = (float)std::atof(e);

    if (!enabled_) {
        godot::UtilityFunctions::print(
            godot::String::utf8("[run] VA_RUN=0 —— 跑动层关闭（单位恒静立姿态）"));
        return;
    }
    godot::UtilityFunctions::print(
        godot::String::utf8("[run] 跑动层就绪：满速参考 "),
        godot::String::num((double)kFullSpeedMps, 2), godot::String::utf8(" m/s 步频 "),
        godot::String::num((double)kStrideHz_, 2), godot::String::utf8("Hz 起伏 "),
        godot::String::num((double)kBob_, 3), godot::String::utf8("m 前倾 "),
        godot::String::num((double)(kLean_ + kLeanSpd_), 2), godot::String::utf8("° 摇 "),
        godot::String::num((double)kRoll_, 1), godot::String::utf8("° 摆 "),
        godot::String::num((double)kYaw_, 1), godot::String::utf8("° 大腿摆幅 "),
        godot::String::num((double)kLegSwing_, 1),
        godot::String::utf8("°（顶点位移实现在 node/unit_leg，VA_LEG=0 关）"));
}

// ============================================================ step
void UnitAnim::step(const std::deque<va::Unit> &p_units) {
    if (!enabled_) return;

    const float t = va::W.t;
    // 重开一局 / 换关：W.t 归零，相位与速度都该丢掉重来
    if (t + 0.001f < prev_t_) {
        st_.clear();
        prev_t_ = -1.0f;
    }
    float dt = (prev_t_ < 0.0f) ? 0.0f : (t - prev_t_);
    prev_t_ = t;
    /* 只滤掉"真冻结"（负值 / 超长跨度），**不能**滤掉快进。
       ⚠️ 这里踩过一次：原先的上限写 0.25 秒，而 VA_FF=15 时每渲染帧推进的
       战局时间就是 0.25 秒 —— 于是整局动作全被当成"卡帧"丢掉，
       日志里一直是「跑动 0/11」，看着像"没人移动"，其实是守卫把 dt 吃了。
       0.25 秒那条线是照 60fps 的正常帧推的，忘了本工程取证默认带 VA_FF。
       现在只有 >8 秒（菜单/检阅台那种真冻结）才不追。
       同一帧重复调用 sync_entity_nodes 时 dt 恰好为 0，走"保持"分支。 */
    if (dt < 0.0f || dt > 8.0f) dt = 0.0f;
    if (st_.size() != p_units.size()) {
        st_.assign(p_units.size(), St{});
    }

    moving_ = 0;
    max_speed_ = 0.0f;
    max_i_ = -1;

    for (std::size_t i = 0; i < p_units.size(); ++i) {
        St &s = st_[i];
        const va::Unit &u = p_units[i];

        // 首帧：用 u.wobble 当起步相位（逻辑层出生时随机播种，见头文件说明）
        if (!s.have) s.phase0 = u.wobble;

        float sp = 0.0f;
        if (s.have && dt > 0.0f) {
            const float dx = u.x - s.x, dy = u.y - s.y;
            sp = std::sqrt(dx * dx + dy * dy) * S / dt;   // 逻辑单位 → 米/秒
        }
        s.x = u.x; s.y = u.y; s.have = true;

        // 平滑：单帧位置噪声不该变成抖动
        s.speed = approach(s.speed, sp, dt, 12.0f);

        const bool alive_and_up = !u.dead && !u.downed;
        if (!alive_and_up) s.speed = 0.0f;

        // 幅度包络：kAmpLo 以下不动，kAmpHi 以上到满
        float want = clampf((s.speed - kAmpLoMps) / (kAmpHiMps - kAmpLoMps), 0.0f, 1.0f);
        if (!alive_and_up) want = 0.0f;
        s.amp = approach(s.amp, want, dt, 9.0f);

        // 步频随速度提高；静止时相位停住（不累加），回来时从原相位续上
        if (s.amp > 0.002f) {
            const float sn = clampf(s.speed / kFullSpeedMps, 0.0f, 1.6f);
            const float hz = kStrideHz_ * clampf(0.55f + 0.45f * sn, 0.55f, 1.6f);
            s.phase += dt * hz * 2.0f * kPi;
            if (s.phase > 2.0f * kPi * 1024.0f) s.phase -= 2.0f * kPi * 1024.0f;
        }

        if (s.amp > 0.02f) {
            ++moving_;
            if (s.speed > max_speed_) { max_speed_ = s.speed; max_i_ = (int)i; }
            if (s.speed > peak_speed_) peak_speed_ = s.speed;
        }
    }
}

// ============================================================ 姿态
UnitAnim::PoseVals UnitAnim::pose_at(float p_phase, float p_speed) const {
    PoseVals v;
    v.speed = p_speed;
    // 纯函数也要认 VA_RUN=0：否则检阅台那条路会绕开开关照摆姿态，
    // "开/关对照"就失去意义了（踩过：两次渲染只差 15 像素，因为两边都摆了）。
    if (!enabled_) return v;
    const float sn = clampf(p_speed / kFullSpeedMps, 0.0f, 1.6f);
    // 幅度包络：<kAmpLo 不动、>kAmpHi 到满。纯函数版没有历史状态，直接由速度算，
    // 与 step() 里那条指数趋近收敛到同一个值（检阅台要的就是稳态）。
    v.amp = clampf((p_speed - kAmpLoMps) / (kAmpHiMps - kAmpLoMps), 0.0f, 1.0f);
    if (v.amp <= 0.001f) return v;

    const float p = p_phase;
    // 垂直起伏：一个步态周期上下各一次（sin(2p)）
    v.bob = kBob_ * v.amp * std::sin(2.0f * p);
    // 前倾：速度越快压得越狠
    v.lean = v.amp * (kLean_ + kLeanSpd_ * sn);
    // 左右摇（绕前方轴）与肩部摆（绕上轴）：相差 90°，跑起来才是"拧"的
    v.roll = kRoll_ * v.amp * std::sin(p);
    v.yaw  = kYaw_ * v.amp * std::cos(p);
    // 头部微点：2 倍频，与起伏错开一点相位，避免看起来像整体在跳
    v.nod  = kNod_ * v.amp * std::sin(2.0f * p + 1.2f);
    /* 大腿摆：本层**只算幅度与相位**，真正的顶点位移在 node/unit_leg 的着色器里。
       左右腿的"交替"不在这里区分 —— 着色器按 sign(x) 给左腿加 π 相位差，
       所以这里只需要一个基准相位。
       摆幅随速度略增（0.55→1.0 倍），与步频随速度提高的趋势一致：
       跑得越快腿抬得越高。基准相位取和 roll 同一个 p，于是"腿在摆"与
       "躯干在侧摇"是同相的，不会各摆各的（这是"看起来像在跑"的关键之一）。 */
    v.leg_phase = p;
    v.leg_deg = kLegSwing_ * v.amp * (0.55f + 0.45f * sn);
    v.active = true;
    return v;
}

UnitAnim::PoseVals UnitAnim::pose_vals(std::size_t p_i) const {
    if (!enabled_ || p_i >= st_.size()) return PoseVals();
    const St &s = st_[p_i];
    if (s.amp <= 0.001f) {
        PoseVals v;
        v.speed = s.speed;
        return v;
    }
    // 稳态分量照纯函数算，再乘上这一帧的包络 —— 包络负责"起步/停下"的淡入淡出，
    // 分量公式只有一份（否则两处迟早对不上，而"跑动的幅度"正是靠肉眼比对的量）。
    PoseVals v = pose_at(s.phase0 + s.phase, s.speed);
    const float k = (s.amp * kAmp_) / std::max(v.amp, 1e-6f);
    v.bob *= k; v.lean *= k; v.roll *= k; v.yaw *= k; v.nod *= k;
    // 腿摆角按同一个比例缩放：包络的淡入淡出必须在腿上也生效，
    // 否则起步/停下的瞬间腿会"啪"地跳到位（那是这一层最容易露馅的地方）。
    v.leg_deg *= k;
    v.amp = s.amp * kAmp_;
    v.active = v.amp > 0.001f;
    return v;
}

godot::Transform3D UnitAnim::pose_transform(const PoseVals &p_v) {
    if (!p_v.active) return godot::Transform3D();
    const float d2r = kPi / 180.0f;
    const godot::Vector3 up(0.0f, 1.0f, 0.0f);
    const godot::Vector3 fwd(1.0f, 0.0f, 0.0f);
    const godot::Vector3 lat(0.0f, 0.0f, 1.0f);
    // 顺序：先俯仰、再侧倾、最后偏航 —— 都在单位自己的局部系里做。
    // 前倾取负号：绕 +Z 转正角会把 +X（前方）抬起来 = 后仰，所以要压下去就得负。
    godot::Basis b = godot::Basis(up, p_v.yaw * d2r)
                   * godot::Basis(fwd, p_v.roll * d2r)
                   * godot::Basis(lat, -(p_v.lean + p_v.nod) * d2r);
    return godot::Transform3D(b, godot::Vector3(0.0f, p_v.bob, 0.0f));
}

godot::Transform3D UnitAnim::local_pose(std::size_t p_i) const {
    return pose_transform(pose_vals(p_i));
}

float UnitAnim::full_speed_mps() const {
    return kFullSpeedMps;
}

// ============================================================ 诊断
void UnitAnim::tick_diag(double p_wall_delta) {
    if (!dbg_) return;
    dbg_acc_ += p_wall_delta;
    // 0.5 秒墙钟一行：快进（VA_FF）时墙钟与战局秒差 ff 倍，
    // 采样太稀会整段错过"队伍在机动"的那几分钟。
    if (dbg_acc_ < 0.5) return;
    dbg_acc_ = 0.0;
    godot::UtilityFunctions::print(
        godot::String::utf8("[run] t="), godot::String::num((double)va::W.t, 1),
        godot::String::utf8(" 跑动 "), moving_, "/", (int)st_.size(),
        godot::String::utf8(" 最快 "), godot::String::num((double)max_speed_, 2),
        godot::String::utf8(" m/s"));
    /* 把**最快那一个单位**的姿态分量摊开。
       这是本层最直接的一条证据：相位在推进 → 五个分量逐行变化；
       静止的单位（amp=0）则整行归零。像素差会被"两次运行的帧对齐"干扰，
       这几个数是函数输出，不依赖任何截图。 */
    if (max_i_ >= 0) {
        const PoseVals v = pose_vals((std::size_t)max_i_);
        godot::UtilityFunctions::print(
            godot::String::utf8("[run]   #"), max_i_,
            godot::String::utf8(" 包络 "), godot::String::num((double)v.amp, 2),
            godot::String::utf8(" 起伏 "), godot::String::num((double)v.bob, 3),
            godot::String::utf8("m 前倾 "), godot::String::num((double)v.lean, 1),
            godot::String::utf8("° 摇 "), godot::String::num((double)v.roll, 1),
            godot::String::utf8("° 摆 "), godot::String::num((double)v.yaw, 1),
            godot::String::utf8("° 点头 "), godot::String::num((double)v.nod, 1),
            godot::String::utf8("° 腿摆 "), godot::String::num((double)v.leg_deg, 1),
            godot::String::utf8("° 相位 "), godot::String::num((double)v.leg_phase, 2));
    }
}

godot::String UnitAnim::dump() const {
    godot::String s;
    s += godot::String::utf8("跑动层：");
    if (!enabled_) return s + godot::String::utf8("VA_RUN=0 关闭");
    int n_amp = 0, n_spd = 0;
    for (const St &v : st_) {
        if (v.amp > 0.02f) ++n_amp;
        if (v.speed > kAmpLoMps) ++n_spd;
    }
    s += godot::String::utf8("单位 ");
    s += godot::String::num((int)st_.size());
    s += godot::String::utf8(" 有速度 ");
    s += godot::String::num(n_spd);
    s += godot::String::utf8(" 在动 ");
    s += godot::String::num(n_amp);
    s += godot::String::utf8(" 最快 ");
    s += godot::String::num((double)max_speed_, 2);
    s += godot::String::utf8(" m/s（峰值 ");
    s += godot::String::num((double)peak_speed_, 2);
    s += godot::String::utf8("）");
    return s;
}

} // namespace volunteer_army
