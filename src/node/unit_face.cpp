// VolunteerArmyPC —— 单位"朝向"的表现层平滑（见 unit_face.h 的完整说明）
#include "node/unit_face.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include <godot_cpp/variant/utility_functions.hpp>

#include "sim/va_math.h"
#include "sim/va_world.h"

namespace volunteer_army {
namespace {

constexpr float kR2D = 57.29577951308232f;
constexpr float kD2R = 0.017453292519943295f;

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

/* "抖了一下"的阈值（度）：一帧之内朝向变了这么多才算异常。
   标定依据：队员正常移动时 a 是连续的，每帧变化 < 5°；
   steer_angle 换档是 ±51°（±0.45 档）或 ±109°（±0.95 档）。
   25° 落在两者中间 —— 真抖动不会被漏掉，连续转向也不会被误判。 */
constexpr float kJumpDeg = 25.0f;

/* "原地"的阈值（**逻辑单位**）：一帧位移小于它就算没动。
   标定依据：满速 46 逻辑单位/秒 ÷ 60 帧 ≈ 0.77 单位/帧，所以 0.5 不会把
   "真的在走"算成原地；而"原地转圈"那条路径下移动被 free_xy 全否，
   位移是 0.00。 */
constexpr float kStillU = 0.5f;

/* "掉头"的阈值（度）：单帧转了这么多，就不是"蹭"，而是整条朝向翻过去了。
   明细行的限流也用它（见 step 里的"掉头明细"）。 */
constexpr float kTurnDeg = 90.0f;

godot::String deg_str(float p_v) {
    return godot::String::num((double)p_v, 1);
}
godot::String deg_str(float p_v, int p_digits) {
    return godot::String::num((double)p_v, p_digits);
}
godot::String i_str(int p_v) {
    return godot::String::num_int64((int64_t)p_v);
}

} // namespace

void UnitFace::setup() {
    on_  = env_flag("VA_FACE", true);
    dbg_ = env_flag("VA_DBG_FACE", false);

    if (const char *e = std::getenv("VA_FACE_RATE")) rate_ = (float)std::atof(e);
    if (const char *e = std::getenv("VA_FACE_DEAD")) dead_ = (float)std::atof(e);
    if (const char *e = std::getenv("VA_FACE_OSC"))  osc_deg_ = (float)std::atof(e);
    if (rate_ < 0.0f) rate_ = 0.0f;
    if (dead_ < 0.0f) dead_ = 0.0f;
    if (osc_deg_ < 0.0f) osc_deg_ = 0.0f;

    if (!on_) {
        godot::UtilityFunctions::print(godot::String::utf8(
            "[face] VA_FACE=0 —— 朝向平滑层关闭，display() 原样返回逻辑层 facing"));
    } else if (dbg_) {
        godot::UtilityFunctions::print(
            godot::String::utf8("[face] 就绪：每帧最多转 ") + deg_str(rate_) +
            godot::String::utf8("°（60 fps 下约 ") + deg_str(rate_ * 60.0f, 0) +
            godot::String::utf8("°/秒，转 180° 约 ") + deg_str(rate_ > 0.0f ? 180.0f / (rate_ * 60.0f) : 0.0f, 2) +
            godot::String::utf8(" 秒）· 死区 ") + deg_str(dead_) +
            godot::String::utf8("° · 玩家自己不平滑"));
    }
}

void UnitFace::step(const std::deque<va::Unit> &p_units) {
    /* 时间基取战局秒（与 anim_.step 同一口径）：VA_FF 快进时平滑跟着加速，
       简报 / 菜单期 t 冻结则一切保持。
       ⚠️ t 会**回退**（reset_mission → begin_level → W.t 归零），
       差分必须是负数就当 0 —— 负 dt 会让 `min(1, dt*k)` 变成负值，
       于是朝向**朝反方向**转，而且越转越远。 */
    const float t = va::W.t;
    float dt = (prev_t_ < 0.0f) ? 0.0f : (t - prev_t_);
    if (dt < 0.0f) dt = 0.0f;
    prev_t_ = t;

    if (st_.size() != p_units.size()) {
        st_.assign(p_units.size(), St{});
        shown_.assign(p_units.size(), 0.0f);
    }

    // 只有"真的过了一帧"才推进与统计 —— sync_entity_nodes 一帧可能被调多次。
    const bool live = dt > 0.0f;
    if (live) ++frames_;

    for (std::size_t i = 0; i < p_units.size(); ++i) {
        const va::Unit &u = p_units[i];
        St &s = st_[i];
        const float target = u.facing;

        if (!s.have) {
            /* 首帧**直接吸附**。否则每个单位都会从 0 弧度"转"到自己的朝向 ——
               开局那 0.3 秒全队一起甩头，比原来的抖动还扎眼。 */
            s.shown = target;
            s.have = true;
            s.lf = target; s.lx = u.x; s.ly = u.y;
            shown_[i] = s.shown;
            continue;
        }

        /* ---- ① 先量逻辑层抖了多少（量的是**原始** facing，与开关无关）----
           ⚠️ 这一段不看 on_ ：关掉本层时更要能量出"原来抖成什么"，
           否则 VA_FACE=0 的对照就没数了 —— 与 fx_layer 里"测偏角不看开关"同一条纪律。 */
        if (live) {
            const float dTarget = va::angDiff(target, s.lf);
            const float dth = std::fabs(dTarget) * kR2D;
            const float dx = u.x - s.lx, dy = u.y - s.ly;
            const float dp = std::sqrt(dx * dx + dy * dy);
            if (dth > kJumpDeg) {
                ++s.jump; ++jump_;
                ++all_jump_;
                if (dp < kStillU) { ++s.spin; ++spin_; ++all_spin_; }
            }
            if (dth > s.peak) s.peak = dth;

            if (dth > peak_logic_) { peak_logic_ = dth; peak_logic_name_ = godot::String::utf8(u.name.c_str()); }
            if (dth > all_peak_logic_) { all_peak_logic_ = dth; all_pl_name_ = godot::String::utf8(u.name.c_str()); }

            /* "来回翻"的**统计**（不再参与抑制，理由见 .h）：
               连续两帧的目标变化都很大、且方向相反。真转身是连续同向，
               命中率极低，所以它是个干净的观测口径 —— 用来判断
               "这次的抖动到底是隔帧交替，还是偶发掉头"。 */
            if (osc_deg_ > 0.0f && std::fabs(s.ldl) * kR2D > osc_deg_ && dth > osc_deg_ &&
                dTarget * s.ldl < 0.0f) {
                ++s.osc; ++osc_; ++all_osc_;
            }
            s.ldl = dTarget;

            /* 掉头明细（限流 8 行/区间）：**「抖」和「掉头」是两回事** ——
               前者是几度的蹭，后者是整 180° 的翻。只有把"跳了多少、当时在不在动、
               朝的是不是同一个目标"打出来，才分得清是 steer_angle 换档
               （±51°/±109°、位置在动）还是别的机制（≈180°、位置不动）。 */
            if (dbg_ && dth > kTurnDeg && dbg_lines_ < 8) {
                ++dbg_lines_;
                godot::String s = godot::String::utf8("[face] 掉头 ") +
                    godot::String::utf8(u.name.c_str()) +
                    godot::String::utf8(" Δ=") + deg_str(dth) + godot::String::utf8("° 移=") +
                    godot::String::num((double)dp, 2) +
                    godot::String::utf8(" 位=(") + godot::String::num((double)u.x, 0) +
                    godot::String::utf8(",") + godot::String::num((double)u.y, 0) +
                    godot::String::utf8(")");
                if (u.hasMoveGoal) {
                    s += godot::String::utf8(" 目标距=") +
                         godot::String::num((double)va::distf(u.x, u.y, u.moveGoal.x, u.moveGoal.y), 0);
                } else {
                    s += godot::String::utf8(" 无目标");
                }
                s += godot::String::utf8(" moving=") + i_str(u.moving ? 1 : 0) +
                     godot::String::utf8(" aiming=") + i_str(u.aiming ? 1 : 0) +
                     godot::String::utf8(" seen=") + i_str(u.seen.ok() ? 1 : 0) +
                     godot::String::utf8(" state=") + godot::String::utf8(u.state.c_str());
                godot::UtilityFunctions::print(s);
            }

            s.lf = target; s.lx = u.x; s.ly = u.y;
        }

        /* ---- ② 再限速 ----
           玩家自己直通：第一人称的朝向就是鼠标，任何滞后都是手感事故。
           ⚠️ 整段套在 live 里：本函数一帧可能被调多次（sync_entity_nodes 有多个调用点），
           而步长是"每帧上限"（不乘 dt）—— 不挡住的话，多调一次就多转一步。 */
        if (on_ && !u.isPlayer) {
            if (live) {
                const float d = va::angDiff(target, s.shown);
                if (std::fabs(d) * kR2D >= dead_) {
                    const float lim = rate_ * kD2R;
                    const float step = (d > lim) ? lim : ((d < -lim) ? -lim : d);
                    s.shown = va::normAng(s.shown + step);
                }
            }
        } else {
            s.shown = target;
        }

        /* ⚠️ 这里**不看 on_**：关掉本层时"平滑后"那一栏就等于逻辑层的值 ——
           那正是消融的对照数（"本层把 180° 压成了 4°"要靠这两栏一起读出来）。 */
        if (live) {
            const float dsh = std::fabs(va::angDiff(s.shown, shown_[i])) * kR2D;
            if (dsh > peak_shown_) { peak_shown_ = dsh; peak_shown_name_ = godot::String::utf8(u.name.c_str()); }
            if (dsh > all_peak_shown_) { all_peak_shown_ = dsh; all_ps_name_ = godot::String::utf8(u.name.c_str()); }
        }
        shown_[i] = s.shown;
    }
}

float UnitFace::display(std::size_t p_i) const {
    if (p_i < shown_.size()) return shown_[p_i];
    return 0.0f;
}

void UnitFace::reset() {
    prev_t_ = -1.0f;
    st_.clear();
    shown_.clear();
    frames_ = 0; jump_ = 0; spin_ = 0;
    osc_ = 0;
    peak_logic_ = 0.0f; peak_shown_ = 0.0f;
    peak_logic_name_ = godot::String();
    peak_shown_name_ = godot::String();
    dbg_lines_ = 0;
}

void UnitFace::tick_diag(double p_wall_delta) {
    if (!dbg_) return;
    dbg_acc_ += p_wall_delta;
    if (dbg_acc_ < 0.5) return;
    dbg_acc_ = 0.0;

    /* 这一行是**本层存在的理由**：左半边是逻辑层原来抖成什么样（与本层无关），
       右半边是平滑之后画面上真正会转多少。两者一起看才知道层有没有干活。 */
    godot::String s = godot::String::utf8("[face] 帧 ") + i_str(frames_);
    if (!on_) {
        s += godot::String::utf8("  VA_FACE=0（直通）");
    } else {
        s += godot::String::utf8("  平滑后 Δ 峰值 ") + deg_str(peak_shown_) + godot::String::utf8("°");
    }
    s += godot::String::utf8("  ｜ 逻辑层 Δ朝向 峰值 ") + deg_str(peak_logic_) +
         godot::String::utf8("°（>") + deg_str(kJumpDeg) + godot::String::utf8("° 共 ") +
         i_str(jump_) + godot::String::utf8(" 次，其中**原地** ") + i_str(spin_) +
         godot::String::utf8(" 次、**来回翻** ") + i_str(osc_) + godot::String::utf8(" 次）");
    if (!peak_logic_name_.is_empty()) {
        s += godot::String::utf8("  最抖=") + peak_logic_name_;
    }
    godot::UtilityFunctions::print(s);

    frames_ = 0; jump_ = 0; spin_ = 0;
    osc_ = 0;
    peak_logic_ = 0.0f; peak_shown_ = 0.0f;
    peak_logic_name_ = godot::String();
    peak_shown_name_ = godot::String();
    dbg_lines_ = 0;
}

godot::String UnitFace::dump() const {
    godot::String s = godot::String::utf8("朝向层：");
    if (!on_) return s + godot::String::utf8("VA_FACE=0 关闭（display 直通逻辑层）");

    /* 打**累计**而不是区间：区间值每 0.5 秒归零，收尾时很可能刚好落在
       一个没有抖动的窗口里，读起来像"这一局根本没抖" ——
       anim_ 的 peak_speed_ 就是为这个才额外留了一份从不归零的峰值。 */
    s += godot::String::utf8("每帧最多转 ") + deg_str(rate_) + godot::String::utf8("° 死区 ") +
         deg_str(dead_) + godot::String::utf8("°");
    s += godot::String::utf8(" ｜ 全场合计：逻辑层 Δ朝向 >") + deg_str(kJumpDeg) +
         godot::String::utf8("° 共 ") + i_str(all_jump_) +
         godot::String::utf8(" 次（其中原地 ") + i_str(all_spin_) +
         godot::String::utf8(" 次、来回翻 ") + i_str(all_osc_) +
         godot::String::utf8(" 次），峰值 ") + deg_str(all_peak_logic_) + godot::String::utf8("°");
    if (!all_pl_name_.is_empty()) s += godot::String::utf8("（") + all_pl_name_ + godot::String::utf8("）");
    s += godot::String::utf8("；平滑后单帧 Δ 峰值 ") + deg_str(all_peak_shown_) + godot::String::utf8("°");
    if (!all_ps_name_.is_empty()) s += godot::String::utf8("（") + all_ps_name_ + godot::String::utf8("）");
    return s;
}

} // namespace volunteer_army
