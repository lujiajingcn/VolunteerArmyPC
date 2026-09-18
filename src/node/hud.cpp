// VolunteerArmyPC —— HUD 实现（全部手绘，无 Label）
#include "node/hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/theme_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "sim/va_world.h"
#include "node/scene_builder.h"   // ally_art_key：名册胸像与三维模型共用同一套键

using namespace godot;

namespace volunteer_army {
namespace {

// ------------------------------------------------------------------ 调色板
// 使命召唤的界面配色其实极度克制：九成像素是不同透明度的冷白，
// 只有一档琥珀色做高亮，外加红/绿两个状态色。大面积用色立刻就会"变手游"，
// 所以下面这些函数刻意都是低饱和 / 低明度差的。
inline Color c_text()  { return Color(0.88f, 0.905f, 0.925f, 1.00f); }
inline Color c_dim()   { return Color(0.80f, 0.84f, 0.88f, 0.58f); }
inline Color c_faint() { return Color(0.78f, 0.82f, 0.86f, 0.26f); }

/* 界面外壳专用的两档"弱文字"。
 *
 * 【为什么不能直接复用 c_dim / c_faint】那两档是照 HUD 的暗面板调的
 * （0.58 / 0.26）。HUD 的底是自己画的近黑面板，亮度可控；界面外壳的底是
 * key art —— 亮度不可控，换一张图就变。0.26 的 13px 灰字压到暮色天空上
 * 会直接消失，简报底部那行"Esc 返回主菜单"就是这么丢的，右栏的
 * (0/6) 计数器也一样（0.58 压在士兵剪影和亮天空的交界上）。
 *
 * 所以外壳这一档的下限必须按**最坏情况（亮天空）**来定，而不是按
 * "我们这张 key art 正好偏暗"来定 —— 和 tools/prep_art.py 里水印阈值
 * 那条经验是同一件事：按最好情况定的阈值，换一张图就变成假阴性。 */
inline Color c_shell_dim()   { return Color(0.86f, 0.89f, 0.93f, 0.82f); }
inline Color c_shell_faint() { return Color(0.84f, 0.88f, 0.92f, 0.62f); }
inline Color c_amber() { return Color(0.96f, 0.68f, 0.26f, 1.00f); }
inline Color c_red()   { return Color(0.93f, 0.28f, 0.22f, 1.00f); }
inline Color c_green() { return Color(0.40f, 0.80f, 0.60f, 1.00f); }
inline Color c_cyan()  { return Color(0.46f, 0.78f, 0.92f, 1.00f); }
inline Color c_panel() { return Color(0.028f, 0.038f, 0.048f, 0.56f); }
inline Color c_panel2(){ return Color(0.020f, 0.028f, 0.036f, 0.70f); }
inline Color c_line()  { return Color(0.74f, 0.80f, 0.86f, 0.22f); }
inline Color c_shadow(){ return Color(0.00f, 0.00f, 0.00f, 0.55f); }

constexpr float kPi  = 3.14159265358979323846f;
constexpr float kDeg = 57.29577951308232f;
constexpr float M    = 24.0f;      // 版面外边距（设计单位）
constexpr float RANGE = 620.0f;    // 雷达量程（世界单位；1 世界单位 = 0.05 m → 31 m 半径）

// 罗盘 16 方位的中文缩写。用汉字而不是 N/E/S/W —— 全场界面都是中文，
// 罗盘混英文反而割裂；而且汉字在低分辨率下比两个字母更紧凑。
const char *kCard8[8] = { "北", "东北", "东", "东南", "南", "西南", "西", "西北" };

inline float clampf_(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// 把角度差折到 [-180,180)
inline float wrap180(float d) {
    d = std::fmod(d + 180.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d - 180.0f;
}

inline float world_bearing_deg(float px, float py, float wx, float wy) {
    float b = 90.0f - std::atan2(wy - py, wx - px) * kDeg;
    b = std::fmod(b, 360.0f);
    if (b < 0.0f) b += 360.0f;
    return b;
}

std::string fmt_time(float t) {
    if (t < 0.0f) t = 0.0f;
    const int s = (int)t;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", s / 60, s % 60);
    return std::string(buf);
}

const char *weather_cn(const std::string &w) {
    if (w == "rain")  return "降雨";
    if (w == "night") return "夜战";
    return "晴";
}

// 小队状态 → 颜色。这套配色和雷达、罗盘上的点保持一致，
// 玩家只要认一次色就够，不必再读字。
Color state_color(const va::Unit &u) {
    if (u.dead)   return Color(0.55f, 0.58f, 0.62f, 0.55f);
    if (u.downed) return c_red();
    if (u.hp < 40.0f) return c_amber();
    if (!u.order.active && u.state == "待命") return c_dim();
    return c_green();
}

} // namespace

Hud::Hud() = default;
Hud::~Hud() = default;

void Hud::_notification(int p_what) { (void)p_what; }

Hud::EvShot Hud::take_ev_shot() {
    const EvShot s = pending_shot_;
    pending_shot_ = EVSHOT_NONE;      // 同时只留一张在途，避免事件密集时刷屏
    return s;
}

bool Hud::mission_over() const { return va::W.over; }

// 相机朝向 → 罗盘度数（0=正北，顺时针）。
// 推导：世界 +Y 定为北、+X 为东；逻辑层的 facing 是 atan2(dy,dx)，所以
// facing=0（正东）应对应 90°。相机 yaw = -facing - π/2，与 facing 一一对应，
// 用 facing 换算最直接：heading = 90 - facing_deg。
float Hud::heading_deg() const {
    const va::Unit *p = va::W.player;
    const float a = (p != nullptr) ? p->facing : 0.0f;
    float h = 90.0f - a * kDeg;
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    return h;
}

// ---- SimEvents 转发 ----
// 警报和提示共用一条队列：警报只是"更重的那一档"（ev_alert 里把 alert 置真），
// 这样两者会自然互斥排队，不会同时糊在屏幕中央。
void Hud::ev_toast(const std::string &text) {
    if (text.empty()) return;
    toasts_.push_back({ text, 0.0f, false });
    while (toasts_.size() > 6) toasts_.pop_front();
}

void Hud::ev_alert(const std::string &text, float dur) {
    (void)dur;      // 显示时长由 HUD 统一控制，逻辑层给的 dur 只当参考
    if (text.empty()) return;
    toasts_.push_back({ text, 0.0f, true });
    while (toasts_.size() > 6) toasts_.pop_front();
}

// ===========================================================================
//  布局
// ===========================================================================
// 缩放基准取视口高度 / 1080：界面元素的"视觉重量"应该跟着屏幕高度走，
// 而不是宽度 —— 超宽屏上按宽度缩放会把元素吹得过大。
bool Hud::layout() {
    vp_ = get_viewport_rect().size;
    if (vp_.x < 2.0f || vp_.y < 2.0f) return false;
    s_ = clampf_(vp_.y / 1080.0f, 0.62f, 2.20f);

    if (font_.is_null()) {
        // 事件打点开关只读一次（VA_HUD_EV）。读在这里是因为 layout() 一定早于
        // 任何采样 —— 放在构造函数里则要把它从 `= default` 改回手写。
        static const bool s_ev_log = (std::getenv("VA_HUD_EV") != nullptr);
        static const bool s_ev_cap = (std::getenv("VA_CAPTURE_EV") != nullptr);
        ev_log_ = s_ev_log;
        ev_cap_ = s_ev_cap;
        // 字体来源两条路：
        //  1) ThemeDB 的 fallback font —— 就是默认主题给所有 Label 用的那个，
        //     所以 HUD 的字和原先 Label 的字完全一致（含中文回退链）。
        //  2) 万一它为空（自定义主题时可能），退回拿一个 Label 问它的主题字体。
        if (ThemeDB *tdb = ThemeDB::get_singleton(); tdb != nullptr) {
            font_ = tdb->get_fallback_font();
        }
        if (font_.is_null()) {
            Label *probe = memnew(Label);
            probe->set_visible(false);
            probe->set_name("_font_probe");
            add_child(probe);
            font_ = probe->get_theme_font("font");
        }
    }
    return font_.is_valid();
}

// ===========================================================================
//  绘制原语
// ===========================================================================
PackedVector2Array Hud::chamfer(const Rect2 &r, float cut) const {
    const float x0 = r.position.x, y0 = r.position.y;
    const float x1 = x0 + r.size.x, y1 = y0 + r.size.y;
    const float c = clampf_(cut, 0.0f, std::min(r.size.x, r.size.y) * 0.45f);
    // 顺时针八点；cut=0 时四个角点重合，退化成普通矩形。
    const Vector2 pts[8] = {
        Vector2(x0 + c, y0), Vector2(x1 - c, y0), Vector2(x1, y0 + c),
        Vector2(x1, y1 - c), Vector2(x1 - c, y1), Vector2(x0 + c, y1),
        Vector2(x0, y1 - c), Vector2(x0, y0 + c),
    };
    PackedVector2Array a;
    for (int i = 0; i < 8; ++i) a.push_back(pts[i]);
    return a;
}

void Hud::poly_panel(const Rect2 &r, float cut, const Color &fill, const Color &line, float lw) {
    const PackedVector2Array p = chamfer(r, cut);
    // 先描一圈黑边：面板常常压在亮背景（天空、公路）上，
    // 不描黑边会糊在一起，这是全场可读性的底线。
    if (fill.a > 0.001f) draw_colored_polygon(chamfer(r.grow(1.5f), cut + 1.5f), c_shadow());
    if (fill.a > 0.001f) draw_colored_polygon(p, fill);
    if (line.a > 0.001f && lw > 0.0f) {
        for (int i = 0; i < 8; ++i) {
            draw_line(p[i], p[(i + 1) % 8], line, lw, true);
        }
    }
}

/* 横向渐变（左 / 中 / 右三个停靠点），用两个四边形 + 逐顶点配色一次画完。
 *
 * 【为什么不继续用"叠 N 条 draw_rect"】
 * draw_rect 没有渐变，只能叠 N 条不同 alpha 的矩形。那会同时引入两种可见缺陷：
 *   1) N 段离散 alpha 在段边界上是**硬跳变**；
 *   2) 相邻矩形为了不留缝常写成 width = 段宽 + 1，重叠的 1px 被合成两次，
 *      边界更明显。
 * 这不是理论担忧，是量出来的：40 段压暗层在 2560 宽的菜单背景上留下了
 * 周期 65.6px（= 2560/39）的规则竖纹，自相关在滞后 66/131/197/263 上
 * 给出 0.69/0.42/0.88/0.62 的峰值；而同一张**原始素材**的自相关是平滑衰减、
 * 没有任何周期峰 —— 证明竖纹来自绘制方式，不是素材自带。
 * 肉眼当时只看到"背景有点脏"，是自相关把它定性成"规律人造条纹"的。
 *
 * draw_polygon 支持逐顶点配色，插值在一次调用内完成：没有"段"，
 * 也就不可能有段的边界。左半 + 右半两个四边形拼出「左→中→右」。 */
void Hud::grad_h(const Rect2 &r, const Color &cl, const Color &cm, const Color &cr) {
    const float x0 = r.position.x;
    const float xm = r.position.x + r.size.x * 0.5f;
    const float x1 = r.position.x + r.size.x;
    const float y0 = r.position.y;
    const float y1 = r.position.y + r.size.y;

    PackedVector2Array pl;
    pl.push_back(Vector2(x0, y0));
    pl.push_back(Vector2(xm, y0));
    pl.push_back(Vector2(xm, y1));
    pl.push_back(Vector2(x0, y1));
    PackedColorArray clc;
    clc.push_back(cl);
    clc.push_back(cm);
    clc.push_back(cm);
    clc.push_back(cl);
    draw_polygon(pl, clc);

    PackedVector2Array pr;
    pr.push_back(Vector2(xm, y0));
    pr.push_back(Vector2(x1, y0));
    pr.push_back(Vector2(x1, y1));
    pr.push_back(Vector2(xm, y1));
    PackedColorArray crc;
    crc.push_back(cm);
    crc.push_back(cr);
    crc.push_back(cr);
    crc.push_back(cm);
    draw_polygon(pr, crc);
}

/* 通用版横向渐变：固定色 rgb，alpha 由 alpha_at(t) 给出（t ∈ [0,1]）。
 *
 * 为什么还要一个"任意曲线"的版本：三停靠点的线性近似只够画对称的简单压暗。
 * 罗盘底衬的剖面是 sin^1.35（上一轮为可读性实测调出来的），线性近似会把两翼
 * 压得明显更透 —— 边缘那些刻度本来就只有 35% 不透明度，再失去底衬就读不出来了。
 *
 * 实现上用 96 列顶点 + 逐顶点配色：值在每一个像素上连续，不会像"叠 N 条矩形"
 * 那样在段边界硬跳变；同时又能表达任意形状的 alpha 剖面。
 * 顶点之间是线性插值，会有斜率不连续 —— 但斜率不连续不会被看成条纹，
 * 只有**取值跳变**才会。 */
void Hud::grad_h(const Rect2 &r, const Color &rgb, float (*alpha_at)(float t)) {
    constexpr int N = 96;
    const float y0 = r.position.y;
    const float y1 = r.position.y + r.size.y;

    PackedVector2Array pts;
    PackedColorArray cols;
    // 上沿自左向右
    for (int i = 0; i < N; ++i) {
        const float t = (float)i / (float)(N - 1);
        pts.push_back(Vector2(r.position.x + r.size.x * t, y0));
        cols.push_back(Color(rgb.r, rgb.g, rgb.b, alpha_at(t)));
    }
    // 下沿自右向左，闭合多边形
    for (int i = N - 1; i >= 0; --i) {
        const float t = (float)i / (float)(N - 1);
        pts.push_back(Vector2(r.position.x + r.size.x * t, y1));
        cols.push_back(Color(rgb.r, rgb.g, rgb.b, alpha_at(t)));
    }
    draw_polygon(pts, cols);
}

/* 竖向渐变（t 从上沿 0 到下沿 1）。实现与 grad_h 同构：96 行顶点 + 逐顶点配色，
 * 一次 draw_polygon 画完，值在每个像素上连续。
 *
 * 为什么必须有它，而不是"用一条 alpha 固定的矩形当底部衬底"：
 * 固定 alpha 的矩形上沿就是一条**硬边**，在暗色的岩石/天空上会显出一条横线；
 * 而拆成"N 条不同 alpha 的横向矩形"又会回到分段合成那套，在每条边界上取值跳变。
 * 用竖向连续渐变就同时避开了这两个：上沿 alpha=0（看不出起点），
 * 下沿落在屏幕底边（不产生新边界）。 */
void Hud::grad_v(const Rect2 &r, const Color &rgb, float (*alpha_at)(float t)) {
    constexpr int N = 96;
    const float x0 = r.position.x;
    const float x1 = r.position.x + r.size.x;

    PackedVector2Array pts;
    PackedColorArray cols;
    // 左沿自上而下
    for (int i = 0; i < N; ++i) {
        const float t = (float)i / (float)(N - 1);
        pts.push_back(Vector2(x0, r.position.y + r.size.y * t));
        cols.push_back(Color(rgb.r, rgb.g, rgb.b, alpha_at(t)));
    }
    // 右沿自下而上，闭合多边形
    for (int i = N - 1; i >= 0; --i) {
        const float t = (float)i / (float)(N - 1);
        pts.push_back(Vector2(x1, r.position.y + r.size.y * t));
        cols.push_back(Color(rgb.r, rgb.g, rgb.b, alpha_at(t)));
    }
    draw_polygon(pts, cols);
}

void Hud::bar(const Rect2 &r, float t, const Color &fg, const Color &bg) {
    t = clampf_(t, 0.0f, 1.0f);
    if (bg.a > 0.001f) draw_rect(r, bg, true);
    if (t > 0.0f) draw_rect(Rect2(r.position, Vector2(r.size.x * t, r.size.y)), fg, true);
}

float Hud::tw(const String &t, int size) const {
    if (font_.is_null()) return 0.0f;
    return font_->get_string_size(t, HORIZONTAL_ALIGNMENT_LEFT, -1.0f, size).x;
}

void Hud::tx(const String &t, float x, float baseline, int size, const Color &c,
             HorizontalAlignment al, float w) {
    if (font_.is_null() || t.is_empty()) return;
    // 描边是必须的：HUD 会压在最亮的天空和最暗的树荫上，不描边总有一半场景读不出来。
    draw_string_outline(font_, Vector2(x, baseline), t, al, w, size, 3, c_shadow());
    draw_string(font_, Vector2(x, baseline), t, al, w, size, c);
}

void Hud::tx_top(const String &t, float x, float top, int size, const Color &c,
                 HorizontalAlignment al, float w) {
    const float asc = font_.is_valid() ? font_->get_ascent(size) : (float)size * 0.8f;
    tx(t, x, top + asc, size, c, al, w);
}

void Hud::tx_mid(const String &t, float x, float cy, int size, const Color &c,
                 HorizontalAlignment al, float w) {
    if (font_.is_null()) return;
    const float asc = font_->get_ascent(size);
    const float dsc = font_->get_descent(size);
    tx(t, x, cy + (asc - dsc) * 0.5f, size, c, al, w);
}

void Hud::tx_c(const String &t, float cx, float baseline, int size, const Color &c) {
    tx(t, cx - tw(t, size) * 0.5f, baseline, size, c);
}

void Hud::tx_r(const String &t, float rx, float baseline, int size, const Color &c) {
    tx(t, rx - tw(t, size), baseline, size, c);
}

// ===========================================================================
//  每帧更新：采样 + 计时
// ===========================================================================
void Hud::reset_transient() {
    feed_.clear();
    dmg_.clear();
    popups_.clear();
    toasts_.clear();
    reported_.clear();
    obj_done_.clear();
    hit_t_ = hit_kill_t_ = bloom_ = flash_ = banner_t_ = end_t_ = 0.0f;
    prev_hp_ = -1.0f;
    prev_down_ = prev_over_ = false;
    seen_hits_ = seen_kills_ = 0;
    prev_phase_.clear();
}

void Hud::update(double p_delta) {
    if (!layout()) { queue_redraw(); return; }
    const float d = clampf_((float)p_delta, 0.0f, 0.1f);   // 掉帧时不要一次跳掉整段动画
    clock_ += d;

    /* 界面外壳期间只推进菜单自己的动画，完全不采样战局。
       两个具体理由，都不是洁癖：
         1) 采样会白吃 help_t_ —— 操作提示只在开局显示 18 秒（draw_help 里
            a = (18 - help_t_)/3），玩家在菜单里坐一分钟，进战斗时提示已经过期，
            等于"没教过操作"。
         2) 采样会把瞬时元素提前消费掉 —— 例如"阶段变更"横幅是按
            prev_phase_ != W.phase 触发的，在菜单里就会先弹一次，
            等真正开打时反而没有那条横幅了。
       返回前仍要 queue_redraw，菜单的悬停高亮和呼吸闪烁靠它才有动画。 */
    if (shell_active()) {
        if (screen_ == SCREEN_MENU)  menu_t_ += d;
        if (screen_ == SCREEN_BRIEF) brief_t_ += d;
        queue_redraw();
        return;
    }

    // 新一局：W.t 回退就是最可靠的信号（比监听 start_mission 更省事，
    // 而且不依赖调用顺序 —— reset_mission / start_mission 都会走这里）
    if (last_t_ >= 0.0f && va::W.t < last_t_ - 0.5f) reset_transient();
    last_t_ = va::W.t;

    // ---- 计时衰减 ----
    hit_t_     = std::max(0.0f, hit_t_ - d);
    hit_kill_t_= std::max(0.0f, hit_kill_t_ - d);
    bloom_     = std::max(0.0f, bloom_ - d * 1.9f);
    flash_     = std::max(0.0f, flash_ - d * 2.6f);
    banner_t_  = std::max(0.0f, banner_t_ - d);
    help_t_    += d;
    if (va::W.over) end_t_ = std::min(1.0f, end_t_ + d * 2.4f); else end_t_ = 0.0f;

    for (auto &k : feed_)   k.t += d;
    while (!feed_.empty() && feed_.back().t > 7.0f) feed_.pop_back();
    for (auto &a : dmg_)    a.t += d;
    while (!dmg_.empty() && dmg_.front().t > 1.6f) dmg_.pop_front();
    for (auto &p : popups_) p.t += d;
    while (!popups_.empty() && popups_.front().t > 1.5f) popups_.pop_front();
    for (auto &t : toasts_) t.t += d;
    while (!toasts_.empty() && toasts_.front().t > 5.0f) toasts_.pop_front();

    const va::Unit *pl = va::W.player;
    if (pl == nullptr) { queue_redraw(); return; }

    // ---- 命中：hits 计数跳变（只读，不侵入逻辑层）----
    if (pl->hits > seen_hits_) {
        hit_t_ = 0.24f;
        if (ev_cap_) pending_shot_ = EVSHOT_HIT;
        if (ev_log_) {
            UtilityFunctions::print("[hud-ev] t=", String::num((double)va::W.t, 2),
                                    " 命中 +", pl->hits - seen_hits_, " (hits=", pl->hits,
                                    ") → 命中标记 hit_t_=0.24");
        }
        if (pl->kills > seen_kills_) {
            hit_kill_t_ = 0.40f;
            if (ev_cap_) pending_shot_ = EVSHOT_KILL;   // 覆盖同帧的 HIT：击杀更难得
            if (ev_log_) {
                UtilityFunctions::print("[hud-ev] t=", String::num((double)va::W.t, 2),
                                        " 击杀 (kills=", pl->kills,
                                        ") → 击杀标记 hit_kill_t_=0.40 + 飘字「+1 击杀」");
            }
            Popup p;
            p.text = "+1 击杀";
            p.col = c_amber();
            p.t = 0.0f;
            popups_.push_back(p);
        }
    }
    // 开火 → 准星张开。
    // 判据用"弹匣数下降"而不是 fireCd：fireCd 每发都被重置成 rof，看不出方向；
    // 而换弹会让 magAmmo 反向跳变（变多），正好天然地不会误判成开火。
    if (pl->magAmmo < seen_mag_) bloom_ = std::min(1.2f, bloom_ + 0.5f);
    seen_mag_ = pl->magAmmo;
    seen_hits_ = std::max(seen_hits_, pl->hits);
    seen_kills_ = std::max(seen_kills_, pl->kills);

    // ---- 受击：HP 掉 → 全屏红闪 + 方位弧 ----
    if (prev_hp_ >= 0.0f && pl->hp < prev_hp_ - 0.01f && !pl->downed) {
        flash_ = std::min(1.0f, flash_ + 0.55f);
        const va::Unit *src = pl->lastHurtBy;
        const float wx = src ? src->x : (pl->x - std::cos(pl->facing) * 100.0f);
        const float wy = src ? src->y : (pl->y - std::sin(pl->facing) * 100.0f);
        DmgArc a;
        a.wang = std::atan2(wy - pl->y, wx - pl->x);
        a.t = 0.0f;
        dmg_.push_back(a);
        while (dmg_.size() > 6) dmg_.pop_front();
    }
    prev_hp_ = pl->hp;

    // ---- 倒地 ----
    if (pl->downed && !prev_down_) {
        toasts_.push_back({ "你已倒地 · 等待队友救援", 0.0f, true });
        if (ev_cap_) pending_shot_ = EVSHOT_DOWN;
        if (ev_log_) {
            UtilityFunctions::print("[hud-ev] t=", String::num((double)va::W.t, 2),
                                    " 玩家倒地 (downTimer=", String::num(pl->downTimer, 1),
                                    ") → draw_downed 接管中央（倒计时环 + 大字）");
        }
    }
    prev_down_ = pl->downed;

    // ---- 阶段切换 → 中央横幅 ----
    if (prev_phase_ != va::W.phase || banner_title_.empty()) {
        prev_phase_ = va::W.phase;
        banner_title_ = "阶段变更";
        banner_sub_ = va::W.phaseName;
        banner_t_ = 4.0f;
    }

    // ---- 阵亡播报（敌方 + 我方），带击杀归属 ----
    // 第一次采样只登记不播报：否则开局会把"上一局遗留的阵亡"全弹一遍。
    const bool seed_only = reported_.empty() && feed_.empty();
    for (const auto &u : va::W.units) {
        if (!u.dead && !u.downed) continue;
        if (reported_.count(u.id) != 0) continue;
        reported_.insert(u.id);
        if (seed_only) continue;

        FeedItem it;
        it.victim = u.name;
        it.enemy  = (u.team == va::Team::Enemy);
        const va::Unit *k = u.lastHurtBy;
        if (k != nullptr) {
            it.killer = k->isPlayer ? "你" : k->name;
            it.byPlayer = k->isPlayer;
        } else {
            it.killer = "—";
        }
        it.t = 0.0f;
        if (it.enemy) {
            feed_.push_front(it);
        } else {
            feed_.push_front(it);
            Popup p;
            p.text = u.downed ? (u.name + " 倒地") : (u.name + " 阵亡");
            p.col = c_red();
            p.t = 0.0f;
            popups_.push_back(p);
            // 不覆盖同帧更罕有的事件（玩家倒地 / 击杀）；队友阵亡局局都有，不抢槽位
            if (ev_cap_ && pending_shot_ == EVSHOT_NONE) pending_shot_ = EVSHOT_ALLY;
            if (ev_log_) {
                UtilityFunctions::print("[hud-ev] t=", String::num((double)va::W.t, 2),
                                        " 阵亡播报 ", String::utf8(p.text.c_str()),
                                        " (凶手=", String::utf8(it.killer.c_str()),
                                        ") → 飘字 1.5s");
            }
        }
        while (feed_.size() > 5) feed_.pop_back();
    }

    // ---- 目标完成态变化 → 小幅提示 ----
    if (obj_done_.size() != va::W.objState.size()) {
        obj_done_.assign(va::W.objState.size(), 0);
    }
    for (size_t i = 0; i < va::W.objState.size(); ++i) {
        const unsigned char now = va::W.objState[i].done ? 1 : 0;
        if (obj_done_[i] == 0 && now == 1) {
            toasts_.push_back({ std::string("目标达成 · ") + va::W.objState[i].text, 0.0f, false });
        }
        obj_done_[i] = now;
    }

    queue_redraw();
}

// ===========================================================================
//  _draw：一次画完
// ===========================================================================
void Hud::_draw() {
    if (!layout()) return;

    // 界面外壳的前两屏是"独占屏"：它们自带全屏 key art 背景，
    // 所以直接 return，底下的战术 HUD 一律不画 ——
    // 否则罗盘、雷达会浮在菜单画面上，看起来像 bug。
    if (screen_ == SCREEN_MENU)  { draw_menu();  return; }
    if (screen_ == SCREEN_BRIEF) { draw_brief(); return; }

    const va::Unit *pl = va::W.player;
    if (pl == nullptr) return;

    draw_vignette();      // 最后画的东西要在最上层，所以顺序是"先背景后前景"
    draw_minimap();
    draw_info_strip();
    draw_compass();
    draw_objectives();
    draw_alert();
    draw_killfeed();
    draw_squad();
    draw_command_panel();
    draw_subtitles();
    draw_ammo();
    draw_score_popups();
    if (pl->downed) {
        draw_downed();
    } else {
        draw_damage_arcs();
        draw_crosshair();
        draw_hitmarker();
    }
    draw_help();
    draw_end_panel();
}

// ===========================================================================
//  顶部罗盘
// ===========================================================================
void Hud::draw_compass() {
    const float cx = vp_.x * 0.5f;
    const float w  = 640.0f * s_;
    const float h  = 42.0f * s_;
    const float top = 8.0f * s_;
    const Rect2 r(cx - w * 0.5f, top, w, h);

    // 底衬：中间厚两端透，用逐顶点渐变画同一套 sin^1.35 剖面。
    //
    // 【alpha 为什么定这么高】第一版用的是 0.62 × sin^1.7 的深色底，
    // 实测 u=0.35 处整列亮度是 121~124 —— 几乎完全平坦，也就是刻度根本读不出来。
    // 原因是底衬在天空（亮度 200+）上只压到"中灰"，而刻度本身是接近白的暖白，
    // 白压在中灰上对比度不到 2:1。所以底衬必须压到接近黑（0.86），
    // 让刻度拿到足够的动态范围。这不是审美问题，是可读性问题。
    //
    // 【为什么不再是 44 段 draw_rect】原注释写着"44 段在 1080p 看不出台阶"，
    // 这句是错的：段边界是**取值跳变**，相邻段 alpha 差够大就看得见。
    // 罗盘这一处因为刻度线本身的高对比把它盖住，自相关测不够显著；
    // 但同一类写法在压暗层上已经被量出周期 65.6px 的规则竖纹（见 grad_h 上方），
    // 所以这里一并换掉，不留同类隐患。
    grad_h(Rect2(r.position.x, r.position.y - 2.0f * s_, r.size.x, r.size.y + 4.0f * s_),
           Color(0.015f, 0.020f, 0.028f),
           [](float t) { return 0.86f * std::pow(std::sin(t * kPi), 1.35f); });

    const float hd = heading_deg();
    const float half = 60.0f;                       // 半视角（度）
    const float ppd = r.size.x / (half * 2.0f);     // 像素/度

    // ---- 刻度 ----
    // 刻度从**上沿往下垂**、方位字压在下沿 —— 使命召唤的罗盘就是这个朝向。
    // 第一版是反过来的（刻度从下沿往上长、字在最上面），结果字挤到了条带外，
    // 而且中央指针按 tip.y-22 算直接跑到屏幕上方去了，整条罗盘少了指北针。
    for (int deg = -180; deg < 180; deg += 5) {
        const float diff = wrap180((float)deg - hd);
        if (std::fabs(diff) > half) continue;
        const float x = cx + diff * ppd;
        const float a = std::pow(std::cos(diff / half * (kPi * 0.5f)), 1.10f);
        const int   m = ((deg % 360) + 360) % 360;
        const bool major = (m % 45 == 0);
        const bool mid   = (m % 15 == 0);
        const float len = major ? 11.0f * s_ : (mid ? 7.0f * s_ : 4.0f * s_);
        const float y0 = r.position.y + 2.0f * s_;
        draw_line(Vector2(x, y0), Vector2(x, y0 + len),
                  Color(0.96f, 0.98f, 1.00f, (major ? 0.98f : (mid ? 0.78f : 0.38f)) * a),
                  major ? 2.0f * s_ : (mid ? 1.4f * s_ : 1.0f * s_), true);
        if (major && a > 0.28f) {
            tx_top(String::utf8(kCard8[(m / 45) % 8]), x - 30.0f * s_,
                   r.position.y + r.size.y - 17.0f * s_,
                   13, Color(0.98f, 0.99f, 1.00f, 0.98f * a), HORIZONTAL_ALIGNMENT_CENTER, 60.0f * s_);
        }
    }

    // ---- 上下两条细线，同样两端淡出 ----
    // 也改用逐顶点渐变，不再分段叠矩形（理由同底衬）。
    // 直接传无捕获 lambda：它可以隐式转成 float(*)(float)。
    grad_h(Rect2(r.position.x, r.position.y - 2.0f * s_, r.size.x, 1.4f * s_),
           Color(0.92f, 0.95f, 0.98f),
           [](float t) { return 0.34f * std::pow(std::sin(t * kPi), 1.35f); });
    grad_h(Rect2(r.position.x, r.position.y + r.size.y + 0.6f * s_, r.size.x, 1.4f * s_),
           Color(0.92f, 0.95f, 0.98f),
           [](float t) { return 0.34f * std::pow(std::sin(t * kPi), 1.35f); });

    // ---- 目标方位标记：本作的核心是"把车队堵在哪"，所以公路上的
    //      战术点、撤离点、玩家标记全都挂到罗盘上，抬头就知道往哪转 ----
    const va::Unit *pl = va::W.player;
    struct Mark { float rx, ry; Color col; const char *glyph; };
    std::vector<Mark> marks;
    marks.push_back({ va::W.evac.x, va::W.evac.y, c_green(), "撤" });
    for (int i = 0; i < 7; ++i) {
        marks.push_back({ va::POINTS[i].x, va::POINTS[i].y, c_amber(), va::POINTS[i].shortName });
    }
    if (va::W.hasMarker) {
        marks.push_back({ va::W.marker.x, va::W.marker.y, Color(0.98f, 0.82f, 0.34f, 1.0f), "标" });
    }
    // 敌方载具：它们在公路上开，是全场最该被盯住的东西。
    // 但**必须去重**：车队在出发点附近时六辆车挤在同一个方位，
    // 罗盘上会糊成一坨红菱形（实测 t=5s 就是这样），既看不清也丢了信息。
    // 做法是按方位分箱，4° 内只保留最近的一辆。
    {
        std::vector<float> taken;
        const va::Unit *pl = va::W.player;
        for (const auto &v : va::W.vehicles) {
            if (v.destroyed) continue;
            const float dd = va::distf(pl->x, pl->y, v.x, v.y);
            if (dd > RANGE * 2.0f) continue;          // 太远：没有战术意义，只会塞满条带
            const float b = world_bearing_deg(pl->x, pl->y, v.x, v.y);
            bool dup = false;
            for (float t : taken) if (std::fabs(wrap180(b - t)) < 4.0f) { dup = true; break; }
            if (dup) continue;
            taken.push_back(b);
            marks.push_back({ v.x, v.y, c_red(), "车" });
        }
    }

    for (const Mark &mk : marks) {
        const float b = world_bearing_deg(pl->x, pl->y, mk.rx, mk.ry);
        const float diff = wrap180(b - hd);
        const float a = 1.0f - clampf_(std::fabs(diff) / half, 0.0f, 1.0f);
        if (a < 0.10f) continue;
        const float x = cx + diff * ppd;
        Color col = mk.col;
        col.a *= a;
        // 菱形标记（使命召唤的通用"目标"符号）
        const float dy = 5.2f * s_;
        const Vector2 c(x, r.position.y + 11.0f * s_);
        PackedVector2Array dia;
        dia.push_back(Vector2(c.x, c.y - dy));
        dia.push_back(Vector2(c.x + dy, c.y));
        dia.push_back(Vector2(c.x, c.y + dy));
        dia.push_back(Vector2(c.x - dy, c.y));
        draw_colored_polygon(dia, col);
        // 只给"当前视野中"的标记配字，否则罗盘会糊满字
        if (a > 0.72f) {
            tx_top(String::utf8(mk.glyph), x - 20.0f * s_, c.y + dy + 1.0f * s_, 11,
                   Color(0.94f, 0.96f, 0.98f, 0.92f * a), HORIZONTAL_ALIGNMENT_CENTER, 40.0f * s_);
        }
    }

    // ---- 中央指北针：一个朝下的琥珀色三角缺在条带上沿 ----
    // 它是"我现在朝着多少度"的唯一视觉锚点，必须画在条带内部。
    {
        const float ty = r.position.y - 1.0f * s_;
        PackedVector2Array arrow;
        arrow.push_back(Vector2(cx, ty + 8.5f * s_));
        arrow.push_back(Vector2(cx - 6.0f * s_, ty));
        arrow.push_back(Vector2(cx + 6.0f * s_, ty));
        draw_colored_polygon(arrow, Color(0.98f, 0.72f, 0.28f, 1.0f));
    }
}

// ===========================================================================
//  左上雷达
// ===========================================================================
void Hud::draw_minimap() {
    const float side = 252.0f * s_;
    const Rect2 box(M * s_, (8.0f + 42.0f + 8.0f) * s_, side, side);
    poly_panel(box, 15.0f * s_, c_panel2(), c_line(), 1.2f * s_);

    const float pad = 6.0f * s_;
    const Rect2 inner(box.position + Vector2(pad, pad), box.size - Vector2(pad * 2.0f, pad * 2.0f));
    const Vector2 c = inner.position + inner.size * 0.5f;
    const float k = (inner.size.x * 0.5f) / RANGE;      // 世界单位 → 雷达像素

    const va::Unit *pl = va::W.player;
    const float H = kPi * 0.5f - pl->facing;            // 屏幕坐标下的航向角
    const float ch = std::cos(H), sh = std::sin(H);

    // 世界 (x,y) → 雷达像素。玩家恒在中心、朝向恒朝上（使命召唤的雷达是转图不转人）。
    auto to_radar = [&](float wx, float wy) -> Vector2 {
        const float dx = wx - pl->x, dy = wy - pl->y;
        const float sx =  dx * ch - dy * sh;
        const float sy = -dx * sh - dy * ch;
        return c + Vector2(sx, sy) * k;
    };
    // 方框裁剪：旋转之后内容一定会溢出，逐点判断比开 clip 便宜且够用
    auto inb = [&](const Vector2 &p) {
        return p.x >= inner.position.x - 1.0f && p.x <= inner.position.x + inner.size.x + 1.0f
            && p.y >= inner.position.y - 1.0f && p.y <= inner.position.y + inner.size.y + 1.0f;
    };
    auto seg = [&](const Vector2 &a, const Vector2 &b, const Color &col, float wpx) {
        if (inb(a) && inb(b)) { draw_line(a, b, col, wpx, true); return; }
        // 粗粒度裁剪：把线段按参数切成 12 段，只画落在框内的那些。
        // 雷达上的线都是短折线，这个精度已经看不出接缝。
        const int N = 12;
        Vector2 prev = a;
        for (int i = 1; i <= N; ++i) {
            const Vector2 cur = a.lerp(b, (float)i / N);
            if (inb(prev) && inb(cur)) draw_line(prev, cur, col, wpx, true);
            prev = cur;
        }
    };

    // ---- 底纹网格 ----
    for (int i = 1; i <= 3; ++i) {
        const float t = (float)i / 4.0f;
        draw_line(Vector2(inner.position.x + inner.size.x * t, inner.position.y),
                  Vector2(inner.position.x + inner.size.x * t, inner.position.y + inner.size.y),
                  Color(0.70f, 0.78f, 0.86f, 0.055f), 1.0f, true);
        draw_line(Vector2(inner.position.x, inner.position.y + inner.size.y * t),
                  Vector2(inner.position.x + inner.size.x, inner.position.y + inner.size.y * t),
                  Color(0.70f, 0.78f, 0.86f, 0.055f), 1.0f, true);
    }

    // ---- 公路（横带）+ 河流（纵带）+ 桥 ----
    // 用四边形而不是"加粗的线"：河流带宽 108 世界单位、跨度 1300，用线段表示
    // 会得到一个宽得离谱的带子，而且两端无法裁剪。四边形才是它的本来形状。
    //
    // 【为什么四边形必须自己裁剪】
    // 公路在世界坐标里是 x∈[0,W] 的横贯长带、河流是 y∈[0,H] 的纵贯长带，
    // 经 to_radar 旋转后**一定**伸到方框外面，而 draw_colored_polygon 不做任何裁剪。
    // 实测症状：一条深色斜带从雷达面板一路拖到画面中间，糊在 3D 场景上。
    // 折线那条路（seg）早有粗粒度裁剪，唯独四边形这条路漏了 —— 因为它"看起来"
    // 是画在雷达里的，很容易默认它自动被面板裁掉，而画布绘制根本没有这回事。
    // 这里用 Sutherland–Hodgman 把凸多边形按 inner 矩形裁一遍。
    auto clip_poly = [&](const PackedVector2Array &poly) {
        const float lo[4] = { inner.position.x, inner.position.y,
                              inner.position.x, inner.position.y };
        const float hi[4] = { inner.position.x + inner.size.x, inner.position.y + inner.size.y,
                              inner.position.x + inner.size.x, inner.position.y + inner.size.y };
        // 4 条边：0=左边(x>=x0) 1=上边(y>=y0) 2=右边(x<=x1) 3=下边(y<=y1)
        // 注意 axis 判据是 e%2==0（偶数是 x 轴），不是 e<2。
        // 写成 e<2 的后果很隐蔽：边 1 会拿顶点的 x 去和 y 的边界比，
        // 而边 2 则完全约束不到 x —— 裁剪看似执行了，多边形照样往右溢出。
        PackedVector2Array cur = poly;
        for (int e = 0; e < 4; ++e) {
            const bool vertical = (e % 2 == 0);
            const bool low_side = (e < 2);       // 左/上取 >=lo，右/下取 <=hi
            const float lim = low_side ? lo[e] : hi[e];
            PackedVector2Array next;
            const int n = (int)cur.size();
            for (int i = 0; i < n; ++i) {
                const Vector2 a = cur[i];
                const Vector2 b = cur[(i + 1) % n];
                const float va = vertical ? a.x : a.y;
                const float vb = vertical ? b.x : b.y;
                const bool ia = low_side ? (va >= lim) : (va <= lim);
                const bool ib = low_side ? (vb >= lim) : (vb <= lim);
                if (ia && ib) {
                    next.push_back(b);
                } else if (ia != ib) {
                    const float t = (lim - va) / (vb - va);
                    next.push_back(a.lerp(b, t));
                    if (ib) next.push_back(b);
                }
            }
            cur = next;
            if (cur.size() < 3) return PackedVector2Array();
        }
        return cur;
    };
    auto quad = [&](float x0, float y0, float x1, float y1, const Color &col) {
        PackedVector2Array p;
        p.push_back(to_radar(x0, y0));
        p.push_back(to_radar(x1, y0));
        p.push_back(to_radar(x1, y1));
        p.push_back(to_radar(x0, y1));
        p = clip_poly(p);
        if (p.size() >= 3) draw_colored_polygon(p, col);
    };
    // 公路
    quad(0.0f, va::CFG.roadTop, va::CFG.W, va::CFG.roadBot, Color(0.30f, 0.31f, 0.33f, 0.72f));
    // 河流（被桥分成两段）
    quad(va::CFG.riverX1, 0.0f, va::CFG.riverX2, va::CFG.bridgeY1,
         Color(0.13f, 0.26f, 0.38f, 0.80f));
    quad(va::CFG.riverX1, va::CFG.bridgeY2, va::CFG.riverX2, va::CFG.H,
         Color(0.13f, 0.26f, 0.38f, 0.80f));
    // 桥：压过河面的一小段路面。炸桥是任务里的一条支线，画出来玩家才知道它存在。
    if (va::W.bridgeAlive) {
        quad(va::CFG.riverX1 - 6.0f, va::CFG.bridgeY1, va::CFG.riverX2 + 6.0f, va::CFG.bridgeY2,
             Color(0.46f, 0.47f, 0.48f, 0.92f));
    } else {
        quad(va::CFG.riverX1 - 6.0f, va::CFG.bridgeY1, va::CFG.riverX2 + 6.0f, va::CFG.bridgeY2,
             Color(0.34f, 0.24f, 0.18f, 0.85f));
    }
    // 公路中心虚线。
    // 这里原来是"整条都在框内就直接画，否则切成 24 段只画落在框内的"——
    // 端点会带上最多 1/24 线长的锯齿，而这条虚线是横向贯穿整块雷达的，
    // 端点正好在框边，缺一段非常显眼。改成标准 Liang–Barsky 求出可见参数区间，
    // 再在这个区间里铺虚线段，端点精确落在框上。
    {
        Vector2 a = to_radar(0.0f, va::CFG.roadCY);
        Vector2 b = to_radar(va::CFG.W, va::CFG.roadCY);
        const float bx0 = inner.position.x, bx1 = inner.position.x + inner.size.x;
        const float by0 = inner.position.y, by1 = inner.position.y + inner.size.y;
        float t0 = 0.0f, t1 = 1.0f;
        bool vis = true;
        {
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float pp[4] = { -dx, dx, -dy, dy };
            const float qq[4] = { a.x - bx0, bx1 - a.x, a.y - by0, by1 - a.y };
            for (int i = 0; i < 4 && vis; ++i) {
                if (std::fabs(pp[i]) < 1e-6f) {
                    if (qq[i] < 0.0f) vis = false;
                    continue;
                }
                const float r = qq[i] / pp[i];
                if (pp[i] < 0.0f) { if (r > t1) vis = false; else if (r > t0) t0 = r; }
                else { if (r < t0) vis = false; else if (r < t1) t1 = r; }
            }
        }
        if (vis && t1 > t0) {
            const Vector2 pa = a.lerp(b, t0);
            const Vector2 pb = a.lerp(b, t1);
            const float len = pa.distance_to(pb);
            if (len > 0.5f) {
                const float dash = 5.0f * s_, gap = 6.0f * s_;
                for (float d0 = 0.0f; d0 < len; d0 += dash + gap) {
                    const float d1 = std::min(d0 + dash, len);
                    draw_line(pa + (pb - pa) * (d0 / len), pa + (pb - pa) * (d1 / len),
                              Color(0.86f, 0.87f, 0.82f, 0.30f), 1.0f, true);
                }
            }
        }
    }
    // 车队行进路线（沿公路的那条折线，用极淡的暖色勾出来）
    if (va::CONVOY_WAY.size() >= 2) {
        for (size_t i = 0; i + 1 < va::CONVOY_WAY.size(); ++i) {
            seg(to_radar(va::CONVOY_WAY[i].x, va::CONVOY_WAY[i].y),
                to_radar(va::CONVOY_WAY[i + 1].x, va::CONVOY_WAY[i + 1].y),
                Color(0.92f, 0.60f, 0.30f, 0.22f), 1.4f * s_);
        }
    }

    // ---- 视野锥（使命召唤雷达上那个浅色扇形）----
    {
        const float hf = 52.0f;                 // 水平半视角
        const float rr = inner.size.x * 0.52f;
        PackedVector2Array cone;
        cone.push_back(c);
        for (int i = 0; i <= 12; ++i) {
            const float a = (-hf + 2.0f * hf * i / 12.0f) / kDeg - kPi * 0.5f;
            cone.push_back(c + Vector2(std::cos(a), std::sin(a)) * rr);
        }
        draw_colored_polygon(cone, Color(0.85f, 0.90f, 0.95f, 0.055f));
    }

    // ---- 战术点 / 撤离点 ----
    for (int i = 0; i < 7; ++i) {
        const Vector2 p = to_radar(va::POINTS[i].x, va::POINTS[i].y);
        if (!inb(p)) continue;
        const float zp = va::POINTS[i].r * k;
        if (zp > 2.0f) {
            draw_arc(p, zp, 0.0f, kPi * 2.0f, 28, Color(0.96f, 0.72f, 0.30f, 0.16f), 1.0f, true);
        }
        const float dy = 4.4f * s_;
        PackedVector2Array dia;
        dia.push_back(Vector2(p.x, p.y - dy)); dia.push_back(Vector2(p.x + dy, p.y));
        dia.push_back(Vector2(p.x, p.y + dy)); dia.push_back(Vector2(p.x - dy, p.y));
        draw_colored_polygon(dia, Color(0.96f, 0.72f, 0.30f, 0.92f));
        if (inner.size.x > 0.0f) {
            tx_top(String::utf8(va::POINTS[i].shortName), p.x - 14.0f * s_, p.y + dy + 1.0f * s_,
                   10, Color(0.98f, 0.82f, 0.42f, 0.90f), HORIZONTAL_ALIGNMENT_CENTER, 28.0f * s_);
        }
    }
    {   // 撤离点
        const Vector2 p = to_radar(va::W.evac.x, va::W.evac.y);
        if (inb(p)) {
            const bool armed = va::W.evacArmed;
            const Color col = armed ? c_green() : Color(0.55f, 0.68f, 0.60f, 0.85f);
            draw_arc(p, 6.0f * s_, 0.0f, kPi * 2.0f, 20, col, 1.6f * s_, true);
            if (armed) draw_arc(p, 9.5f * s_ + std::sin(clock_ * 3.0f) * 1.2f * s_, 0.0f, kPi * 2.0f, 22,
                                Color(0.40f, 0.80f, 0.60f, 0.45f), 1.2f * s_, true);
            tx_top(String::utf8("撤"), p.x - 14.0f * s_, p.y - 8.0f * s_, 10, col,
                   HORIZONTAL_ALIGNMENT_CENTER, 28.0f * s_);
        }
    }

    // ---- 玩家标记点 ----
    if (va::W.hasMarker) {
        const Vector2 p = to_radar(va::W.marker.x, va::W.marker.y);
        if (inb(p)) {
            const Color col(0.98f, 0.82f, 0.34f, 1.0f);
            draw_arc(p, 7.0f * s_ + std::sin(clock_ * 5.0f) * 1.6f * s_, 0.0f, kPi * 2.0f, 22,
                     Color(0.98f, 0.82f, 0.34f, 0.75f), 1.6f * s_, true);
            draw_line(p + Vector2(-4 * s_, 0), p + Vector2(4 * s_, 0), col, 1.4f * s_, true);
            draw_line(p + Vector2(0, -4 * s_), p + Vector2(0, 4 * s_), col, 1.4f * s_, true);
        }
    }

    // ---- 敌方载具（车队是全场焦点，雷达上常亮）----
    for (const auto &v : va::W.vehicles) {
        const Vector2 p = to_radar(v.x, v.y);
        if (!inb(p)) continue;
        const Color col = v.destroyed ? Color(0.38f, 0.36f, 0.34f, 0.70f) : c_red();
        const Vector2 f(std::cos(-v.angle), std::sin(-v.angle));
        const Vector2 g(-f.y, f.x);
        PackedVector2Array tri;
        tri.push_back(p + f * 6.0f * s_);
        tri.push_back(p + g * 4.0f * s_ - f * 3.0f * s_);
        tri.push_back(p - g * 4.0f * s_ - f * 3.0f * s_);
        draw_colored_polygon(tri, col);
        if (v.hasBox) {
            // 密码箱在哪辆车上 —— 这是全任务最要紧的信息
            draw_arc(p, 8.5f * s_ + std::sin(clock_ * 4.0f) * 1.4f * s_, 0.0f, kPi * 2.0f, 22,
                     Color(0.98f, 0.82f, 0.34f, 0.85f), 1.6f * s_, true);
        }
    }

    // ---- 步兵 ----
    for (const auto &u : va::W.units) {
        if (u.isPlayer || u.dead) continue;
        const Vector2 p = to_radar(u.x, u.y);
        if (!inb(p)) continue;
        if (u.team == va::Team::Enemy) {
            // 敌军只在"看得见"或"近到听得见"时才上雷达 ——
            // 否则雷达就变成了透视挂，这套以侦察/指挥为核心的玩法会直接失效。
            const bool seen = va::can_see(*pl, u.x, u.y);
            const float dd = va::distf(pl->x, pl->y, u.x, u.y);
            if (!seen && dd > 150.0f) continue;
            const float a = seen ? 1.0f : 0.42f;
            if (u.downed) {
                draw_arc(p, 3.6f * s_, 0.0f, kPi * 2.0f, 14, Color(0.93f, 0.28f, 0.22f, 0.55f * a), 1.4f * s_, true);
            } else {
                draw_circle(p, 3.6f * s_, Color(0.93f, 0.28f, 0.22f, a));
                draw_circle(p, 3.6f * s_, Color(0.0f, 0.0f, 0.0f, 0.45f), false, 1.0f, true);
            }
        } else {
            const Color col = state_color(u);
            if (u.downed) {
                draw_arc(p, 3.8f * s_, 0.0f, kPi * 2.0f, 14, col, 1.6f * s_, true);
            } else {
                draw_circle(p, 3.4f * s_, col);
            }
            // 朝向短线：判断"谁在看着哪边"用
            const Vector2 f(std::cos(u.facing), std::sin(u.facing));
            seg(p, p + Vector2(f.x * ch - f.y * sh, -f.x * sh - f.y * ch) * 8.0f * s_,
                Color(col.r, col.g, col.b, 0.55f), 1.2f * s_);
        }
    }

    // ---- 玩家箭头（恒在中心朝上）----
    {
        PackedVector2Array tri;
        tri.push_back(c + Vector2(0, -7.0f * s_));
        tri.push_back(c + Vector2(5.6f * s_, 6.0f * s_));
        tri.push_back(c + Vector2(0, 3.2f * s_));
        tri.push_back(c + Vector2(-5.6f * s_, 6.0f * s_));
        draw_colored_polygon(tri, Color(0.94f, 0.96f, 0.98f, 1.0f));
    }

    // ---- 正北指示 ----
    {
        const Vector2 np = c + Vector2(0, -inner.size.y * 0.5f + 7.0f * s_);
        // 北在屏幕上的角度 = -H（航向 0 时北朝上）
        const float na = -H - kPi * 0.5f;
        const Vector2 q = c + Vector2(std::cos(na), std::sin(na)) * (inner.size.x * 0.5f - 8.0f * s_);
        (void)np;
        tx_top(String::utf8("北"), q.x - 12.0f * s_, q.y - 7.0f * s_, 11,
               Color(0.86f, 0.90f, 0.94f, 0.80f), HORIZONTAL_ALIGNMENT_CENTER, 24.0f * s_);
    }

    // ---- 边框重描（内容会盖住内框线）----
    poly_panel(box, 15.0f * s_, Color(0, 0, 0, 0), c_line(), 1.2f * s_);

    // ---- 扫描线：一条极淡的旋转亮带，纯装饰，给雷达"活"的感觉 ----
    {
        const float a0 = std::fmod(clock_ * 0.9f, kPi * 2.0f);
        const Vector2 dir(std::cos(a0), std::sin(a0));
        const Vector2 p0 = c;
        const Vector2 p1 = c + dir * (inner.size.x * 0.55f);
        if (inb(p1)) draw_line(p0, p1, Color(0.70f, 0.90f, 0.95f, 0.10f), 12.0f * s_, true);
        if (inb(p1)) draw_line(p0, p1, Color(0.80f, 0.95f, 1.00f, 0.16f), 2.0f * s_, true);
    }
}

// ===========================================================================
//  雷达下方信息条
// ===========================================================================
void Hud::draw_info_strip() {
    const float side = 252.0f * s_;
    const float x = M * s_;
    const float y = (8.0f + 42.0f + 8.0f + 252.0f + 8.0f) * s_;
    const Rect2 box(x, y, side, 104.0f * s_);
    poly_panel(box, 12.0f * s_, c_panel(), c_line(), 1.1f * s_);

    const va::Unit *pl = va::W.player;
    float cy = y + 9.0f * s_;

    // 行 1：任务时间 + 天气
    tx_top(String::utf8(("T+" + fmt_time(va::W.t)).c_str()),
           box.position.x + 11.0f * s_, cy, 17, c_text());
    {
        const std::string w = weather_cn(va::W.weather);
        const float ww = tw(String::utf8(w.c_str()), 13);
        tx_top(String::utf8(w.c_str()), box.position.x + box.size.x - 11.0f * s_ - ww, cy + 2.0f * s_,
               13, va::W.weather == "sunny" ? c_amber() : c_cyan());
    }
    cy += 22.0f * s_;

    // 行 2：阶段 + 方位。
    // 方位读数放在这里而不是罗盘上：罗盘的整个上沿已经被刻度和指北针占满，
    // 再塞数字一定会和方位字打架。信息条是"读数"，罗盘是"取向"，分工更清楚。
    tx_top(String::utf8("阶段"), box.position.x + 11.0f * s_, cy, 11, c_dim());
    tx_top(String::utf8(va::W.phaseName.c_str()), box.position.x + 46.0f * s_, cy, 13, c_text());
    {
        char hb[24];
        std::snprintf(hb, sizeof(hb), "%03d°", (int)(heading_deg() + 0.5f) % 360);
        const String hs = String::utf8(hb);
        tx_top(hs, box.position.x + box.size.x - 11.0f * s_ - tw(hs, 13), cy, 13,
               Color(0.96f, 0.76f, 0.36f, 0.95f));
    }
    cy += 20.0f * s_;

    // 行 3：暴露度（噪声）—— 这条直接决定敌人能不能发现你，必须常显
    {
        tx_top(String::utf8("暴露"), box.position.x + 11.0f * s_, cy + 1.0f * s_, 11, c_dim());
        const Rect2 br(box.position.x + 46.0f * s_, cy + 3.0f * s_, box.size.x - 90.0f * s_, 6.0f * s_);
        const float nz = clampf_(va::W.noise, 0.0f, 1.0f);
        Color nc = c_green();
        if (nz > 0.66f) nc = c_red();
        else if (nz > 0.33f) nc = c_amber();
        bar(br, nz, nc, Color(0.65f, 0.70f, 0.76f, 0.14f));
        char nb[16];
        std::snprintf(nb, sizeof(nb), "%d%%", (int)(nz * 100.0f + 0.5f));
        tx_top(String::utf8(nb), box.position.x + box.size.x - 11.0f * s_
               - tw(String::utf8(nb), 11), cy + 1.0f * s_, 11, nc);
    }
    cy += 18.0f * s_;

    // 行 4：存活 / 敌亡
    {
        const int alive = (int)va::alive_allies().size();
        const int total = (int)va::allies().size();
        char b1[48];
        std::snprintf(b1, sizeof(b1), "小队 %d/%d", alive, total);
        tx_top(String::utf8(b1), box.position.x + 11.0f * s_, cy, 12,
               alive <= 2 ? c_red() : (alive < total ? c_amber() : c_green()));
        char b2[48];
        std::snprintf(b2, sizeof(b2), "敌亡 %d", va::W.stats.enemyDead);
        tx_top(String::utf8(b2), box.position.x + box.size.x - 11.0f * s_
               - tw(String::utf8(b2), 12), cy, 12, c_dim());
    }

    // 密码箱状态：主目标的中途反馈，比"目标清单"更直观
    cy += 17.0f * s_;
    if (va::W.hasBox || va::W.stats.boxTaken) {
        const char *s = va::W.stats.boxEvacuated ? "密码箱已撤离"
                      : (va::W.hasBox ? "密码箱在手 · 前往撤离点" : "密码箱已夺取");
        tx_top(String::utf8(s), box.position.x + 11.0f * s_, cy, 12,
               va::W.stats.boxEvacuated ? c_green() : c_amber());
    }
    (void)pl;
}

// ===========================================================================
//  中央目标横幅
// ===========================================================================
void Hud::draw_objectives() {
    const float w = 520.0f * s_;
    const float x = vp_.x * 0.5f - w * 0.5f;
    const float y = 62.0f * s_;

    // 目标条数决定高度
    int subs = 0;
    for (const auto &o : va::W.objState) if (!o.main) ++subs;
    subs = std::min(subs, 3);
    const float h = (34.0f + subs * 19.0f) * s_;

    poly_panel(Rect2(x, y, w, h), 10.0f * s_, Color(0.020f, 0.028f, 0.036f, 0.48f), c_line(), 1.0f * s_);

    // 阶段标签（小切角块）
    const std::string ph = va::W.phaseName;
    const float pw = tw(String::utf8(ph.c_str()), 12) + 16.0f * s_;
    poly_panel(Rect2(x + 9.0f * s_, y + 8.0f * s_, pw, 18.0f * s_), 4.0f * s_,
               Color(0.96f, 0.68f, 0.26f, 0.18f), Color(0.96f, 0.68f, 0.26f, 0.65f), 1.0f * s_);
    tx_mid(String::utf8(ph.c_str()), x + 9.0f * s_, y + 17.0f * s_, 12, c_amber());

    // 主目标
    const va::WorldState::Objective *main_o = nullptr;
    for (const auto &o : va::W.objState) if (o.main) { main_o = &o; break; }
    if (main_o != nullptr) {
        const Color col = main_o->done ? c_green() : c_text();
        tx_mid(String::utf8(main_o->text.c_str()), x + 9.0f * s_ + pw + 10.0f * s_, y + 17.0f * s_,
               16, col);
        // 完成态：一条删除线，比打勾更"任务简报"
        if (main_o->done) {
            const float tw_ = tw(String::utf8(main_o->text.c_str()), 16);
            const float ly = y + 17.0f * s_;
            draw_line(Vector2(x + 9.0f * s_ + pw + 10.0f * s_, ly - 2.0f * s_),
                      Vector2(x + 9.0f * s_ + pw + 10.0f * s_ + tw_, ly - 2.0f * s_),
                      c_green(), 1.4f * s_, true);
        }
    }

    // 子目标
    float sy = y + 32.0f * s_;
    int shown = 0;
    for (const auto &o : va::W.objState) {
        if (o.main || shown >= 3) continue;
        ++shown;
        const Color col = o.done ? Color(0.55f, 0.80f, 0.65f, 0.75f) : c_dim();
        // 复选框
        const float bs = 8.0f * s_;
        const Vector2 bp(x + 12.0f * s_, sy + 5.0f * s_);
        draw_rect(Rect2(bp, Vector2(bs, bs)), Color(col.r, col.g, col.b, 0.55f), false, 1.2f * s_, true);
        if (o.done) {
            draw_line(bp + Vector2(bs * 0.18f, bs * 0.52f), bp + Vector2(bs * 0.42f, bs * 0.80f),
                      c_green(), 1.6f * s_, true);
            draw_line(bp + Vector2(bs * 0.42f, bs * 0.80f), bp + Vector2(bs * 0.86f, bs * 0.18f),
                      c_green(), 1.6f * s_, true);
        }
        tx_mid(String::utf8(o.text.c_str()), x + 12.0f * s_ + bs + 8.0f * s_, sy + 5.0f * s_,
               13, col);
        if (!o.extra.empty()) {
            tx_mid(String::utf8(o.extra.c_str()),
                   x + w - 11.0f * s_ - tw(String::utf8(o.extra.c_str()), 12), sy + 5.0f * s_,
                   12, col);
        }
        sy += 19.0f * s_;
    }

    // 车队行进进度：伏击阶段最关键的一条读数
    if (va::W.convoyStarted && !va::W.convoyEscaped) {
        const Rect2 br(x, y + h + 4.0f * s_, w, 4.0f * s_);
        bar(br, clampf_(va::W.convoyProgress, 0.0f, 1.0f),
            Color(0.93f, 0.45f, 0.25f, 0.90f), Color(0.70f, 0.74f, 0.78f, 0.12f));
    }

    // 阶段切换横幅：从上方滑入。
    // 位置固定在画面纵向 24% 处（而不是"紧贴目标横幅下沿"）：目标条数会变，
    // 紧贴式布局会随条数上下浮动，正好会撞上下面的警报条。
    if (banner_t_ > 0.0f) {
        const float k = 1.0f - banner_t_ / 4.0f;
        const float ease = 1.0f - std::pow(1.0f - clampf_(k * 4.0f, 0.0f, 1.0f), 3.0f);
        const float alpha = clampf_(banner_t_ / 0.7f, 0.0f, 1.0f) * ease;
        const float by = vp_.y * 0.24f - (1.0f - ease) * 16.0f * s_;
        const float bw = 320.0f * s_;
        const Rect2 bb(vp_.x * 0.5f - bw * 0.5f, by, bw, 38.0f * s_);
        poly_panel(bb, 8.0f * s_, Color(0.03f, 0.04f, 0.05f, 0.78f * alpha),
                   Color(0.96f, 0.68f, 0.26f, 0.80f * alpha), 1.4f * s_);
        tx_c(String::utf8("阶段变更"), bb.position.x + bw * 0.5f, by + 15.0f * s_, 11,
             Color(0.85f, 0.89f, 0.93f, 0.80f * alpha));
        tx_c(String::utf8(banner_sub_.c_str()), bb.position.x + bw * 0.5f, by + 31.0f * s_, 17,
             Color(0.98f, 0.80f, 0.40f, alpha));
    }
}

// ===========================================================================
//  警报
// ===========================================================================
void Hud::draw_alert() {
    // 警报优先于普通提示
    const Toast *pick = nullptr;
    for (const auto &t : toasts_) {
        if (t.alert && (pick == nullptr || t.t < pick->t)) pick = &t;
    }
    const bool is_alert = (pick != nullptr);
    if (pick == nullptr) {
        for (const auto &t : toasts_) {
            if (pick == nullptr || t.t < pick->t) pick = &t;
        }
    }
    if (pick == nullptr) return;

    const float age = pick->t;
    const float alpha = clampf_(std::min(age / 0.22f, (5.0f - age) / 0.8f), 0.0f, 1.0f);
    if (alpha <= 0.01f) return;

    const int fsz = is_alert ? 22 : 15;
    const float w = tw(String::utf8(pick->text.c_str()), fsz) + (is_alert ? 76.0f : 44.0f) * s_;
    const float h = (is_alert ? 42.0f : 30.0f) * s_;
    const float y = 166.0f * s_;
    const Rect2 box(vp_.x * 0.5f - w * 0.5f, y, w, h);

    const Color base = is_alert ? c_red() : c_amber();
    const float pulse = is_alert ? (0.72f + 0.28f * std::sin(clock_ * 9.0f)) : 1.0f;
    poly_panel(box, 9.0f * s_, Color(base.r * 0.22f, base.g * 0.16f, base.b * 0.16f, 0.72f * alpha),
               Color(base.r, base.g, base.b, 0.85f * alpha * pulse), 1.6f * s_);
    if (is_alert) {
        // 左侧一根实心警示条 —— 比整块染色更克制，也更醒目
        draw_rect(Rect2(box.position.x, box.position.y, 4.0f * s_, box.size.y),
                  Color(base.r, base.g, base.b, alpha * pulse), true);
    }
    tx_c(String::utf8(pick->text.c_str()), vp_.x * 0.5f, box.position.y + h * 0.5f + fsz * 0.36f,
         fsz, Color(base.r * 1.0f, base.g * 1.05f, base.b * 1.05f, alpha));
}

// ===========================================================================
//  右上击杀回执
// ===========================================================================
void Hud::draw_killfeed() {
    const float w = 330.0f * s_;
    const float x = vp_.x - M * s_ - w;
    float y = (8.0f + 42.0f + 12.0f) * s_;
    const int n = std::min((int)feed_.size(), 5);

    for (int i = 0; i < n; ++i) {
        const FeedItem &f = feed_[i];
        const float age = f.t;
        const float alpha = clampf_(std::min(age / 0.20f, (7.0f - age) / 0.9f), 0.0f, 1.0f);
        if (alpha <= 0.01f) { y += 26.0f * s_; continue; }
        // 从右侧滑入
        const float slide = (1.0f - clampf_(age / 0.24f, 0.0f, 1.0f)) * 22.0f * s_;
        const float row_h = 24.0f * s_;
        const Rect2 box(x + slide, y, w, row_h);

        const Color vcol = f.enemy ? c_amber() : c_red();
        const Color fill = f.byPlayer ? Color(0.10f, 0.075f, 0.03f, 0.55f)
                                      : Color(0.020f, 0.028f, 0.036f, 0.48f);
        poly_panel(box, 6.0f * s_, Color(fill.r, fill.g, fill.b, fill.a * alpha),
                   Color(0.74f, 0.80f, 0.86f, 0.18f * alpha), 1.0f * s_);
        // 左侧归属色条
        draw_rect(Rect2(box.position.x, box.position.y, 3.0f * s_, row_h),
                  Color(vcol.r, vcol.g, vcol.b, 0.85f * alpha), true);

        const float cx0 = box.position.x + 9.0f * s_;
        const float cy0 = box.position.y + row_h * 0.5f;
        const float kw = tw(String::utf8(f.killer.c_str()), 13);
        tx_mid(String::utf8(f.killer.c_str()), cx0, cy0, 13,
               Color(0.90f, 0.93f, 0.95f, alpha));
        // 分隔符：一条短横 + 小菱形，比文字箭头利落
        const float sx0 = cx0 + kw + 7.0f * s_;
        draw_line(Vector2(sx0, cy0), Vector2(sx0 + 9.0f * s_, cy0),
                  Color(0.80f, 0.84f, 0.88f, 0.55f * alpha), 1.3f * s_, true);
        {
            PackedVector2Array dia;
            const float dy = 3.2f * s_;
            const Vector2 c(sx0 + 14.5f * s_, cy0);
            dia.push_back(Vector2(c.x, c.y - dy)); dia.push_back(Vector2(c.x + dy, c.y));
            dia.push_back(Vector2(c.x, c.y + dy)); dia.push_back(Vector2(c.x - dy, c.y));
            draw_colored_polygon(dia, Color(vcol.r, vcol.g, vcol.b, alpha));
        }
        tx_mid(String::utf8(f.victim.c_str()), sx0 + 21.0f * s_, cy0, 13,
               Color(vcol.r, vcol.g, vcol.b, alpha));

        y += 26.0f * s_;
    }
}

// ===========================================================================
//  右侧小队状态板
// ===========================================================================
void Hud::draw_squad() {
    const float w = 300.0f * s_;
    const float x = vp_.x - M * s_ - w;
    float y = 250.0f * s_;

    tx_top(String::utf8("小队"), x + 2.0f * s_, y, 12, c_dim());
    {
        char b[32];
        std::snprintf(b, sizeof(b), "%d 人", (int)va::alive_allies().size());
        tx_top(String::utf8(b), x, y, 12, c_faint(), HORIZONTAL_ALIGNMENT_RIGHT, w);
    }
    y += 19.0f * s_;

    for (const auto &u : va::W.units) {
        if (u.team != va::Team::Ally) continue;
        const float row_h = 26.0f * s_;
        const Rect2 box(x, y, w, row_h);
        const Color sc = state_color(u);
        const bool dead = u.dead;

        poly_panel(box, 5.0f * s_,
                   dead ? Color(0.02f, 0.025f, 0.03f, 0.36f) : Color(0.020f, 0.028f, 0.036f, 0.50f),
                   Color(0.74f, 0.80f, 0.86f, dead ? 0.09f : 0.16f), 1.0f * s_);
        // 左色条：状态一眼可辨
        draw_rect(Rect2(box.position.x, box.position.y, 3.5f * s_, row_h),
                  Color(sc.r, sc.g, sc.b, dead ? 0.35f : 0.92f), true);

        const float tx0 = box.position.x + 10.0f * s_;
        const float cy0 = box.position.y + row_h * 0.5f;

        // 名字（玩家自己标"你"）。呼号里本来就带"你"的不再追加，
        // 否则会出现"你（队长）·你"这种重复。
        String nm = String::utf8(u.name.c_str());
        if (u.isPlayer && nm.find(String::utf8("你")) < 0) nm += String::utf8(" ·你");
        tx_mid(nm, tx0, cy0, 13, dead ? Color(0.60f, 0.63f, 0.66f, 0.6f) : c_text());

        // 当前命令（有指令时才占位）
        if (u.order.active && !u.order.actLabel.empty() && !dead) {
            const String ol = String::utf8(u.order.actLabel.c_str());
            const float ow = tw(ol, 11);
            poly_panel(Rect2(box.position.x + box.size.x - 10.0f * s_ - ow - 10.0f * s_,
                             cy0 - 8.0f * s_, ow + 10.0f * s_, 16.0f * s_), 3.0f * s_,
                       Color(0.96f, 0.68f, 0.26f, 0.14f), Color(0.96f, 0.68f, 0.26f, 0.45f), 1.0f * s_);
            tx_mid(ol, box.position.x + box.size.x - 10.0f * s_ - ow - 5.0f * s_, cy0, 11, c_amber());
        }

        // HP 条 + 状态字（叠在名字右侧）
        if (!dead) {
            const Rect2 br(box.position.x + 96.0f * s_, cy0 - 2.5f * s_, 62.0f * s_, 5.0f * s_);
            const float hp01 = clampf_(u.hp / std::max(1.0f, u.maxHp), 0.0f, 1.0f);
            bar(br, hp01, sc, Color(0.70f, 0.74f, 0.78f, 0.16f));
            if (u.downed) {
                char b[24];
                std::snprintf(b, sizeof(b), "%.0fs", u.downTimer);
                tx_mid(String::utf8(b), br.position.x + br.size.x + 5.0f * s_, cy0, 11, c_red());
            } else {
                tx_mid(String::utf8(u.state.c_str()), br.position.x + br.size.x + 5.0f * s_, cy0,
                       11, c_dim());
            }
        } else {
            tx_mid(String::utf8("阵亡"), box.position.x + 96.0f * s_, cy0, 11,
                   Color(0.60f, 0.63f, 0.66f, 0.65f));
        }
        y += row_h + 4.0f * s_;
    }
}

// ===========================================================================
//  右下弹药 / 装备 / 生命
// ===========================================================================
void Hud::draw_ammo() {
    const va::Unit *pl = va::W.player;
    const float w = 322.0f * s_;
    const float h = 152.0f * s_;
    const float x = vp_.x - M * s_ - w;
    const float y = vp_.y - M * s_ - h;

    // ---- 生命条（坐在弹药块上方）----
    {
        const float hp01 = clampf_(pl->hp / std::max(1.0f, pl->maxHp), 0.0f, 1.0f);
        const Rect2 br(x, y - 20.0f * s_, w, 8.0f * s_);
        Color hc = c_green();
        if (hp01 < 0.30f) {
            // 濒死：整条心跳式脉动
            const float pulse = 0.55f + 0.45f * std::fabs(std::sin(clock_ * 4.2f));
            hc = Color(c_red().r, c_red().g, c_red().b, pulse);
        } else if (hp01 < 0.60f) {
            hc = c_amber();
        }
        // 分段：每 10 点生命一格，读起来比连续条精确得多
        const int seg = 10;
        const float gap = 1.5f * s_;
        const float sw = (w - gap * (seg - 1)) / seg;
        for (int i = 0; i < seg; ++i) {
            const float t0 = (float)i / seg;
            const bool on = hp01 > t0 + 0.5f / seg;
            const Rect2 sr(x + i * (sw + gap), br.position.y, sw, br.size.y);
            draw_colored_polygon(chamfer(sr, 1.5f * s_),
                                 on ? hc : Color(0.70f, 0.74f, 0.78f, 0.13f));
        }
        char hb[32];
        std::snprintf(hb, sizeof(hb), "%d", (int)(pl->hp + 0.5f));
        tx_mid(String::utf8(hb), x - 6.0f * s_ - tw(String::utf8(hb), 14), y - 16.0f * s_, 14, hc);
    }

    poly_panel(Rect2(x, y, w, h), 14.0f * s_, c_panel(), c_line(), 1.2f * s_);

    // ---- 武器名 + 开火模式 ----
    const std::string wn = (pl->wpn != nullptr) ? pl->wpn->name : "—";
    tx_top(String::utf8(wn.c_str()), x + 13.0f * s_, y + 9.0f * s_, 14, c_dim());
    if (pl->wpn != nullptr) {
        const char *fm = (pl->wpn->burst > 1) ? "点射" : "连发";
        tx_top(String::utf8(fm), x + 13.0f * s_, y + 9.0f * s_, 13, c_faint(),
               HORIZONTAL_ALIGNMENT_RIGHT, w - 26.0f * s_);
    }

    // ---- 弹药大数 ----
    {
        const std::string res = "/ " + std::to_string(pl->ammo);
        const float rw = tw(String::utf8(res.c_str()), 19);
        const float rx = x + w - 14.0f * s_ - rw;
        const float ry = y + h - 26.0f * s_;
        tx(String::utf8(res.c_str()), rx, ry, 19, c_dim());

        const std::string mag = std::to_string(pl->magAmmo);
        const float mw = tw(String::utf8(mag.c_str()), 54);
        const float mx = rx - 9.0f * s_ - mw;
        // 弹药告警：分档变色 + 濒空脉动
        Color mc = c_text();
        const int cap = (pl->wpn != nullptr && pl->wpn->mag > 0) ? pl->wpn->mag : 30;
        const float frac = (float)pl->magAmmo / (float)cap;
        if (pl->magAmmo == 0) {
            mc = Color(c_red().r, c_red().g, c_red().b, 0.55f + 0.45f * std::fabs(std::sin(clock_ * 6.0f)));
        } else if (frac <= 0.15f) {
            mc = c_red();
        } else if (frac <= 0.35f) {
            mc = c_amber();
        }
        tx(String::utf8(mag.c_str()), mx, ry, 54, mc);
    }

    // ---- 换弹进度 ----
    if (pl->reloadT > 0.0f) {
        const float dur = (pl->wpn != nullptr && pl->wpn->reload > 0.01f) ? pl->wpn->reload : 2.2f;
        const float p = clampf_(1.0f - pl->reloadT / dur, 0.0f, 1.0f);
        const Rect2 br(x + 13.0f * s_, y + h - 15.0f * s_, w - 26.0f * s_, 3.5f * s_);
        bar(br, p, c_amber(), Color(0.70f, 0.74f, 0.78f, 0.14f));
        tx_top(String::utf8("换弹"), br.position.x, br.position.y - 15.0f * s_, 11, c_amber());
    }

    // ---- 装备：手雷 / 烟雾 / 火箭 ----
    // 竖排三行靠左，而不是横排一行：横排时它会一路向右撞上弹药大号字
    // （"火箭"两个字正好压在"30"上，实测截图里就是这样）。
    // 竖排还顺带贴近使命召唤的习惯 —— 右下角是一列装备图标。
    {
        struct Eq { const char *key; int n; };
        const Eq eq[3] = { { "G 手雷", pl->grenades },
                           { "F 烟雾", pl->smokes },
                           { "1 火箭", pl->rockets } };
        const float rh = 21.0f * s_;
        float ey = y + h - 16.0f * s_ - rh * 3.0f;
        for (const Eq &e : eq) {
            const bool has = e.n > 0;
            const Color col = has ? c_text() : Color(0.55f, 0.58f, 0.62f, 0.32f);
            const float cy_e = ey + rh * 0.5f;
            // 小方块图标
            draw_rect(Rect2(x + 14.0f * s_, cy_e - 4.5f * s_, 9.0f * s_, 9.0f * s_),
                      Color(col.r, col.g, col.b, has ? 0.85f : 0.28f), false, 1.4f * s_, true);
            tx_mid(String::utf8(e.key), x + 28.0f * s_, cy_e, 12, c_dim());
            char nb[8];
            std::snprintf(nb, sizeof(nb), "×%d", e.n);
            tx_mid(String::utf8(nb), x + 90.0f * s_, cy_e, 12, col);
            ey += rh;
        }
    }
}

// ===========================================================================
//  左下指挥链路
// ===========================================================================
void Hud::draw_command_panel() {
    const float w = 366.0f * s_;
    const float h = 116.0f * s_;
    const float x = M * s_;
    const float y = vp_.y - M * s_ - h;

    poly_panel(Rect2(x, y, w, h), 12.0f * s_, c_panel(), c_line(), 1.1f * s_);

    // 标题 + 实时指示
    tx_top(String::utf8("指挥链路"), x + 12.0f * s_, y + 8.0f * s_, 12, c_dim());
    {
        const bool live = va::W.hasLastCmd;
        const Color c = live ? c_green() : c_faint();
        draw_circle(Vector2(x + w - 16.0f * s_, y + 15.0f * s_), 3.2f * s_,
                    Color(c.r, c.g, c.b, live ? (0.6f + 0.4f * std::sin(clock_ * 4.0f)) : 0.5f));
    }

    float cy = y + 32.0f * s_;
    if (va::W.hasLastCmd && !va::W.lastCmd.actLabel.empty()) {
        const va::ParsedCmd &c = va::W.lastCmd;
        // 三个切角标签：呼号 / 动作 / 地点，用色区分层次
        float cx = x + 12.0f * s_;
        auto chip = [&](const std::string &text, const Color &col, bool strong) {
            if (text.empty()) return;
            const String s = String::utf8(text.c_str());
            const float cw = tw(s, 12) + 14.0f * s_;
            poly_panel(Rect2(cx, cy - 10.0f * s_, cw, 21.0f * s_), 4.0f * s_,
                       Color(col.r, col.g, col.b, strong ? 0.20f : 0.10f),
                       Color(col.r, col.g, col.b, strong ? 0.70f : 0.35f), 1.0f * s_);
            tx_mid(s, cx + 7.0f * s_, cy, 12, col);
            cx += cw + 6.0f * s_;
        };
        // 呼号：如果指定了具体人，显示人名的前两字足够
        chip(c.csLabel.empty() ? std::string("默认") : c.csLabel, c_dim(), false);
        chip(c.actLabel, c_amber(), true);
        if (!c.locName.empty()) chip(c.locName, c_cyan(), false);
        else if (c.hasDir) {
            char b[32];
            std::snprintf(b, sizeof(b), "%.0f°", c.bearing);
            chip(std::string(b), c_cyan(), false);
        }
        cy += 26.0f * s_;
    } else {
        tx_top(String::utf8("尚未下达指令"), x + 12.0f * s_, cy - 8.0f * s_, 13, c_faint());
        cy += 26.0f * s_;
    }

    // 执行统计：下发 / 执行 / 拒绝
    {
        const int a = va::W.stats.cmdIssued;
        const int b = va::W.stats.cmdExec;
        const int c = va::W.stats.cmdRefused;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "下发 %d", a);
        tx_top(String::utf8(buf), x + 12.0f * s_, cy - 8.0f * s_, 12, c_dim());
        std::snprintf(buf, sizeof(buf), "执行 %d", b);
        tx_top(String::utf8(buf), x + 92.0f * s_, cy - 8.0f * s_, 12, c_green());
        std::snprintf(buf, sizeof(buf), "拒绝 %d", c);
        tx_top(String::utf8(buf), x + 172.0f * s_, cy - 8.0f * s_, 12, c_red());

        // 执行率条
        const Rect2 br(x + 12.0f * s_, cy + 10.0f * s_, w - 24.0f * s_, 4.0f * s_);
        const float t = a > 0 ? (float)b / (float)a : 0.0f;
        bar(br, t, c_green(), Color(0.70f, 0.74f, 0.78f, 0.13f));
    }

    // 标记点
    cy += 26.0f * s_;
    if (va::W.hasMarker) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "标记已设 · %d,%d", (int)va::W.marker.x, (int)va::W.marker.y);
        tx_top(String::utf8(buf), x + 12.0f * s_, cy - 6.0f * s_, 12, c_amber());
    } else {
        tx_top(String::utf8("Z 键设置标记点"), x + 12.0f * s_, cy - 6.0f * s_, 12, c_faint());
    }
}

// ===========================================================================
//  底部无线电字幕
// ===========================================================================
void Hud::draw_subtitles() {
    if (va::W.subs.empty()) return;
    const int total = (int)va::W.subs.size();
    const int n = std::min(total, 3);
    const float y0 = vp_.y - 108.0f * s_;
    const float lh = 21.0f * s_;

    for (int i = 0; i < n; ++i) {
        const va::Subtitle &s = va::W.subs[(size_t)(total - n + i)];
        // 越新的越亮
        const float a = 0.45f + 0.55f * ((float)(i + 1) / (float)n);
        const bool newest = (i == n - 1);
        const float y = y0 + i * lh;

        String who = String::utf8(s.who.c_str());
        String body = String::utf8(s.text.c_str());
        const String line = who + String::utf8("：") + body;
        const float lw = tw(line, 15);
        const float cx = vp_.x * 0.5f - lw * 0.5f;

        // 说话人颜色按语义分：答应/确认=绿，拒绝/求救=红，其余=琥珀
        Color wc = c_amber();
        if (s.cls == "ok") wc = c_green();
        else if (s.cls == "refuse" || s.cls == "panic" || s.cls == "down" || s.cls == "no") wc = c_red();
        else if (s.who == "你") wc = c_cyan();

        tx(line, cx, y, 15, Color(0.90f, 0.93f, 0.95f, a), HORIZONTAL_ALIGNMENT_LEFT, lw);
        // 说话人单独重涂一次颜色（覆盖在整行之上，代价极低）
        tx(who + String::utf8("："), cx, y, 15, Color(wc.r, wc.g, wc.b, a));
        if (newest) {
            // 最新一条前面加个闪烁小条，视线容易被抓住
            draw_rect(Rect2(cx - 9.0f * s_, y - 11.0f * s_, 2.0f * s_, 14.0f * s_),
                      Color(wc.r, wc.g, wc.b, 0.85f), true);
        }
    }
}

// ===========================================================================
//  准星
// ===========================================================================
void Hud::draw_crosshair() {
    const va::Unit *pl = va::W.player;
    const Vector2 c = vp_ * 0.5f;

    // 开镜时准星让位给枪上的红点 —— 否则屏幕中央会出现"两个准心"
    if (va::IN.ads) return;

    float sp = va::spread_mul(*pl);
    sp = clampf_((sp - 1.0f) / 2.4f, 0.0f, 1.0f);
    const float gap = (5.0f + sp * 26.0f + bloom_ * 16.0f) * s_;
    const float len = 10.0f * s_;
    const float th  = 2.0f * s_;

    const Color col(0.94f, 0.96f, 0.98f, 0.90f);
    const Color edge(0.0f, 0.0f, 0.0f, 0.55f);
    auto tick = [&](const Vector2 &dir) {
        const Vector2 a = c + dir * gap;
        const Vector2 b = c + dir * (gap + len);
        draw_line(a, b, edge, th + 2.2f * s_, true);
        draw_line(a, b, col, th, true);
    };
    tick(Vector2(0, -1)); tick(Vector2(0, 1));
    tick(Vector2(-1, 0)); tick(Vector2(1, 0));

    // 中心点：散布越大点越小，暗示"精度在流失"
    draw_circle(c, (1.6f - sp * 0.9f) * s_ + 0.6f, Color(0.96f, 0.97f, 0.99f, 0.92f));

    // 换弹时准星变成四段圆弧，明确"现在打不了"
    if (pl->reloadT > 0.0f) {
        const float dur = (pl->wpn != nullptr && pl->wpn->reload > 0.01f) ? pl->wpn->reload : 2.2f;
        const float p = clampf_(1.0f - pl->reloadT / dur, 0.0f, 1.0f);
        draw_arc(c, 30.0f * s_, -kPi * 0.5f, -kPi * 0.5f + kPi * 2.0f * p, 40,
                 c_amber(), 2.4f * s_, true);
    }
}

// ===========================================================================
//  命中标记
// ===========================================================================
void Hud::draw_hitmarker() {
    const float t = std::max(hit_t_ / 0.24f, 0.0f);
    const float kt = std::max(hit_kill_t_ / 0.40f, 0.0f);
    if (t <= 0.0f && kt <= 0.0f) return;

    const bool kill = kt > 0.0f;
    const float a = kill ? kt : t;
    // 出现时略微放大再收回 —— 静止的叉号在快节奏里容易被忽略
    const float sc = 1.0f + 0.45f * (1.0f - a);
    const Vector2 c = vp_ * 0.5f;
    const float r0 = 8.0f * s_ * sc;
    const float r1 = 17.0f * s_ * sc;
    const float th = (kill ? 2.8f : 2.2f) * s_;
    const Color col = kill ? Color(0.98f, 0.34f, 0.26f, a) : Color(0.96f, 0.97f, 0.99f, a);

    const float d = 0.70710678f;
    const Vector2 dirs[4] = { Vector2(d, -d), Vector2(d, d), Vector2(-d, d), Vector2(-d, -d) };
    for (const Vector2 &v : dirs) {
        draw_line(c + v * r0, c + v * r1, Color(0, 0, 0, 0.5f * a), th + 2.0f * s_, true);
        draw_line(c + v * r0, c + v * r1, col, th, true);
    }
    if (kill) {
        draw_circle(c, 2.6f * s_, Color(0.98f, 0.34f, 0.26f, a));
    }
}

// ===========================================================================
//  受击方位弧
// ===========================================================================
void Hud::draw_damage_arcs() {
    if (dmg_.empty()) return;
    const va::Unit *pl = va::W.player;
    const Vector2 c = vp_ * 0.5f;
    for (const DmgArc &a : dmg_) {
        const float k = clampf_(1.0f - a.t / 1.6f, 0.0f, 1.0f);
        if (k <= 0.01f) continue;
        // 世界方位 → 屏幕角度：正前方映射到屏幕正上方（-π/2）
        const float sa = a.wang - pl->facing - kPi * 0.5f;
        const float alpha = std::pow(k, 1.4f);
        const float rr = (128.0f + (1.0f - k) * 20.0f) * s_;
        const float half = 0.34f;                    // 弧张角（弧度）
        const Color col(0.95f, 0.24f, 0.18f, 0.92f * alpha);
        // 黑底 + 彩色：亮背景（雪地/天空）上也读得出来
        draw_arc(c, rr, sa - half, sa + half, 18, Color(0, 0, 0, 0.45f * alpha), 9.0f * s_, true);
        draw_arc(c, rr, sa - half, sa + half, 18, col, 5.5f * s_, true);
    }
}

// ===========================================================================
//  击杀飘字
// ===========================================================================
void Hud::draw_score_popups() {
    const Vector2 c = vp_ * 0.5f;
    int i = 0;
    for (const Popup &p : popups_) {
        const float k = clampf_(p.t / 1.5f, 0.0f, 1.0f);
        const float a = clampf_(std::min(p.t / 0.12f, (1.5f - p.t) / 0.45f), 0.0f, 1.0f);
        if (a <= 0.01f) { ++i; continue; }
        const float y = c.y + (58.0f + i * 22.0f) * s_ - k * 26.0f * s_;
        tx_c(String::utf8(p.text.c_str()), c.x, y, 16, Color(p.col.r, p.col.g, p.col.b, a));
        ++i;
    }
}

// ===========================================================================
//  倒地提示
// ===========================================================================
void Hud::draw_downed() {
    const va::Unit *pl = va::W.player;
    const Vector2 c = vp_ * 0.5f;
    const float t = pl->downTimer;

    // 中央大标
    const float pulse = 0.72f + 0.28f * std::sin(clock_ * 4.0f);
    tx_c(String::utf8("你 已 倒 地"), c.x, c.y - 28.0f * s_, 38,
         Color(0.95f, 0.30f, 0.24f, 0.95f * pulse));
    tx_c(String::utf8("副队长接管指挥 · 等待队友救援"), c.x, c.y + 6.0f * s_, 16,
         Color(0.90f, 0.93f, 0.95f, 0.80f));

    // 倒计时环
    const float p = clampf_(t / 40.0f, 0.0f, 1.0f);
    draw_arc(c, 66.0f * s_, -kPi * 0.5f, -kPi * 0.5f + kPi * 2.0f * p, 56,
             Color(0.95f, 0.30f, 0.24f, 0.85f), 3.6f * s_, true);
    draw_arc(c, 66.0f * s_, 0.0f, kPi * 2.0f, 56, Color(0.95f, 0.30f, 0.24f, 0.14f), 3.6f * s_, true);
    char b[16];
    std::snprintf(b, sizeof(b), "%.0f", t);
    tx_c(String::utf8(b), c.x, c.y + 78.0f * s_, 20, Color(0.95f, 0.42f, 0.34f, 0.9f));
}

// ===========================================================================
//  受伤暗角
// ===========================================================================
void Hud::draw_vignette() {
    const va::Unit *pl = va::W.player;
    const float hp01 = clampf_(pl->hp / std::max(1.0f, pl->maxHp), 0.0f, 1.0f);
    float dmg = clampf_((0.78f - hp01) / 0.78f, 0.0f, 1.0f);
    if (pl->downed) dmg = std::max(dmg, 0.92f);
    dmg = clampf_(dmg + flash_ * 0.35f, 0.0f, 1.0f);
    if (dmg <= 0.004f) return;

    // 脉动：血量越低心跳越快，是"该撤了"的最直接提示
    const float heart = 0.70f + 0.30f * std::fabs(std::sin(clock_ * (2.2f + (1.0f - hp01) * 3.4f)));
    // 用同心的描边矩形叠出径向渐变（draw_rect 没有渐变，30 层足够平滑）
    const int bands = 30;
    const float thick = 3.4f * s_;
    for (int i = 0; i < bands; ++i) {
        const float t = 1.0f - (float)i / bands;
        const float a = dmg * 0.115f * t * t * heart;
        const float o = i * thick;
        draw_rect(Rect2(o, o, vp_.x - 2.0f * o, vp_.y - 2.0f * o),
                  Color(0.60f, 0.045f, 0.035f, a), false, thick + 0.8f);
    }
    // 受击瞬间整屏轻压红
    if (flash_ > 0.01f) {
        draw_rect(Rect2(0, 0, vp_.x, vp_.y), Color(0.55f, 0.03f, 0.02f, flash_ * 0.10f), true);
    }
}

// ===========================================================================
//  操作提示
// ===========================================================================
void Hud::draw_help() {
    // 结算面板自带提示，这里让位（否则底部会被结算的全屏压暗盖成一片灰）
    if (va::W.over) return;
    const float a = clampf_((18.0f - help_t_) / 3.0f, 0.0f, 1.0f);
    if (a <= 0.01f) return;

    const String s = String::utf8(
        "WASD 移动 · Shift 疾跑 · Ctrl 蹲 · 左键 射击 · 右键 瞄准 · R 换弹 · G 手雷 · F 烟雾 · Z 标记 · Esc 释放鼠标");
    tx_c(s, vp_.x * 0.5f, vp_.y - 8.0f * s_, 13, Color(0.86f, 0.90f, 0.94f, 0.55f * a));
}

// ===========================================================================
//  结算面板
// ===========================================================================
void Hud::draw_end_panel() {
    if (!va::W.over || end_t_ <= 0.01f) return;
    const float e = end_t_;
    const bool win = va::W.score.win;
    const Color accent = win ? Color(0.96f, 0.76f, 0.32f, 1.0f) : c_red();

    /* 背景：胜负各一张 key art。
       为什么成败要分开两张而不是同一张换标题字：结算这一刻玩家最先感知的是
       "发生了什么"，不是文字。晨光下的雪原行军 vs 硝烟里跪着的背影，
       一眼就分得出输赢；只换标题字的话得读一行小字才知道结果。
       两张图都刻意把中央留空，正好给面板让位。 */
    const Ref<Texture2D> bg = win ? art_win_ : art_lose_;
    if (bg.is_valid()) {
        draw_art_bg(bg, 0.86f, 0.42f);
    } else {
        // 素材缺失时退回原来的纯色压暗 —— 结算面板不能因为少一张图就整个消失
        draw_rect(Rect2(0, 0, vp_.x, vp_.y), Color(0.015f, 0.020f, 0.026f, 0.90f * e), true);
    }
    // 再补一层均匀压暗，保证面板上的小字在任何画面上都读得出来
    draw_rect(Rect2(0, 0, vp_.x, vp_.y), Color(0.015f, 0.020f, 0.026f, 0.46f * e), true);

    const float w = 700.0f * s_;
    const float h = 452.0f * s_;
    const float x = vp_.x * 0.5f - w * 0.5f;
    const float y = vp_.y * 0.5f - h * 0.5f + (1.0f - e) * 26.0f * s_;

    poly_panel(Rect2(x, y, w, h), 18.0f * s_, Color(0.030f, 0.040f, 0.050f, 0.94f * e),
               Color(accent.r, accent.g, accent.b, 0.55f * e), 1.6f * s_);
    // 顶部一条实心色带 —— 使命召唤结算面板的标志性装饰
    draw_rect(Rect2(x + 18.0f * s_, y, w - 36.0f * s_, 3.0f * s_),
              Color(accent.r, accent.g, accent.b, 0.95f * e), true);

    // 成败标题
    const String title = String::utf8(win ? "任 务 达 成" : "任 务 失 败");
    tx_c(String::utf8("行动代号 · 志愿军"), x + w * 0.5f, y + 34.0f * s_, 14,
         Color(0.80f, 0.84f, 0.88f, 0.60f * e));
    tx_c(title, x + w * 0.5f, y + 82.0f * s_, 42, Color(accent.r, accent.g, accent.b, e));

    // 评级
    if (!va::W.score.g.empty()) {
        tx_c(String::utf8("评级"), x + w * 0.5f, y + 116.0f * s_, 13,
             Color(0.80f, 0.84f, 0.88f, 0.65f * e));
        tx_c(String::utf8(va::W.score.g.c_str()), x + w * 0.5f, y + 158.0f * s_, 34,
             Color(accent.r, accent.g, accent.b, e));
    }

    // 分隔线
    draw_rect(Rect2(x + 40.0f * s_, y + 182.0f * s_, w - 80.0f * s_, 1.0f * s_),
              Color(0.86f, 0.90f, 0.94f, 0.20f * e), true);

    // 统计两列
    struct Row { const char *k; std::string v; Color c; };
    char b[64];
    std::vector<Row> L, R;
    std::snprintf(b, sizeof(b), "%d", va::W.stats.enemyDead);
    L.push_back({ "敌军阵亡", b, c_text() });
    std::snprintf(b, sizeof(b), "%d", va::W.stats.allyDead);
    L.push_back({ "我方阵亡", b, va::W.stats.allyDead > 0 ? c_red() : c_text() });
    std::snprintf(b, sizeof(b), "%d", va::W.stats.allyDown);
    L.push_back({ "我方重伤", b, c_text() });
    std::snprintf(b, sizeof(b), "%d / %d", va::W.stats.evacCount, (int)va::allies().size());
    L.push_back({ "撤离人数", b, va::W.stats.evacCount > 0 ? c_green() : c_text() });

    std::snprintf(b, sizeof(b), "%d", va::W.stats.apcKilled);
    R.push_back({ "摧毁装甲车", b, c_text() });
    R.push_back({ "击毁坦克", va::W.stats.tankKilled ? "是" : "否",
                  va::W.stats.tankKilled ? c_green() : c_text() });
    R.push_back({ "密码箱", va::W.stats.boxEvacuated ? "已撤离" : (va::W.stats.boxTaken ? "已夺取" : "未取得"),
                  va::W.stats.boxEvacuated ? c_green() : (va::W.stats.boxTaken ? c_amber() : c_red()) });
    std::snprintf(b, sizeof(b), "%d 次 / %d 次", va::W.stats.tankShells, va::W.stats.minesUsed);
    R.push_back({ "炮击 / 地雷", b, c_text() });

    auto draw_col = [&](const std::vector<Row> &rows, float cx, float cy0) {
        float yy = cy0;
        for (const Row &r0 : rows) {
            tx(String::utf8(r0.k), cx, yy, 14, Color(0.80f, 0.84f, 0.88f, 0.72f * e));
            tx_r(String::utf8(r0.v.c_str()), cx + 250.0f * s_, yy, 15,
                 Color(r0.c.r, r0.c.g, r0.c.b, e));
            yy += 27.0f * s_;
        }
    };
    draw_col(L, x + 50.0f * s_, y + 212.0f * s_);
    draw_col(R, x + 380.0f * s_, y + 212.0f * s_);

    // 指挥统计 + 用时
    {
        draw_rect(Rect2(x + 40.0f * s_, y + 330.0f * s_, w - 80.0f * s_, 1.0f * s_),
                  Color(0.86f, 0.90f, 0.94f, 0.20f * e), true);
        char c1[80];
        std::snprintf(c1, sizeof(c1), "指令下发 %d · 执行 %d · 拒绝 %d",
                      va::W.stats.cmdIssued, va::W.stats.cmdExec, va::W.stats.cmdRefused);
        tx(String::utf8(c1), x + 50.0f * s_, y + 358.0f * s_, 14,
           Color(0.80f, 0.84f, 0.88f, 0.80f * e));
        char c2[64];
        std::snprintf(c2, sizeof(c2), "行动时长 %s", fmt_time(va::W.t).c_str());
        tx_r(String::utf8(c2), x + w - 50.0f * s_, y + 358.0f * s_, 14,
             Color(0.80f, 0.84f, 0.88f, 0.80f * e));

        tx_c(String::utf8("按 R 重新部署 · Esc 回主菜单"), x + w * 0.5f, y + h - 26.0f * s_, 15,
             Color(accent.r, accent.g, accent.b, 0.70f + 0.30f * std::sin(clock_ * 4.0f)));
    }
}

// ===========================================================================
//  界面外壳：任务素材 / 主菜单 / 任务简报
// ===========================================================================
//
// 【素材为什么走 Image::load_from_file，而不是 ResourceLoader】
// 这五张 PNG 是运行时才落进工程 assets/art/ 的，从没经过编辑器的导入工序 ——
// res:// 下不存在对应的 .import / .ctex。ResourceLoader 在非编辑器构建里
// 只会去查导入缓存，找不到就报 "No loader found for resource"，纹理拿不到、
// 背景一片黑。Image::load_from_file 读的是磁盘上的原始 PNG
// （"直接跑工程目录"时 res:// 就是盘上的工程目录），绕开整套导入系统。
// 反过来，如果哪天有人用编辑器打开过工程把图导入了，ResourceLoader 那条路
// 更快也更省内存，所以两条都试、谁成用谁。
// 小队名册的显示顺序与对应的 Unit id。
// 玩家排第一（他不在 ROSTER 里 —— 那是 va_config.cpp 的 PLAYER_DEF），
// 其余 10 人按花名册原序。下标与 Hud::art_port_ 一一对应。
constexpr int kRosterN = Hud::kRosterMax;
static const char *const kRosterIds[kRosterN] = {
    "player", "ajie", "laozhou", "xiaoxia", "daliu",
    "alan", "shitou", "houzi", "laobai", "xiaoman", "tietou",
};
// 这张表与 Hud::art_port_ 是按**下标**配对的（负载时同一个下标、绘制时同一个下标），
// 所以两边的长度必须严格相等，而且顺序改动也要同步。
// 长度不等会在越界读胸像之前就先编译失败，比运行期看出"第 9 格是空的"早得多。
static_assert(sizeof(kRosterIds) / sizeof(kRosterIds[0]) == (size_t)kRosterN,
              "kRosterIds 长度必须等于 Hud::kRosterMax");

Ref<Texture2D> Hud::load_tex(const String &p_res_path) {
    const String &path = p_res_path;
    ResourceLoader *rl = ResourceLoader::get_singleton();
    // 先 exists 再 load：直接 load 一个没导入过的路径，引擎会往 stderr
    // 打一行 "No loader found for resource"。五张图就是五行假 ERROR ——
    // 会污染日志，让"日志里有没有 ERROR"这个回归判据当场失效。
    if (rl != nullptr && rl->exists(path)) {
        Ref<Texture2D> t = rl->load(path);
        if (t.is_valid()) return t;
    }
    const Ref<Image> img = Image::load_from_file(path);
    if (img.is_valid() && !img->is_empty()) return ImageTexture::create_from_image(img);
    UtilityFunctions::print("[ui] 素材载入失败：", path);
    return Ref<Texture2D>();
}

void Hud::load_art() {
    if (art_menu_.is_valid()) return;      // 只载一次
    art_menu_    = load_tex("res://assets/art/art_menu_bg.png");
    art_brief_   = load_tex("res://assets/art/art_briefing.png");
    art_chapter_ = load_tex("res://assets/art/art_chapter.png");
    art_win_     = load_tex("res://assets/art/art_end_win.png");
    art_lose_    = load_tex("res://assets/art/art_end_lose.png");

    // 队员胸像。路径由 ally_art_key() 导出 —— 与三维角色模型用的是**同一套键**，
    // 所以以后加一个角色只有一处映射要改，不会出现"模型换了、胸像还是旧的"。
    int n_ok = 0;
    for (int i = 0; i < kRosterN; ++i) {
        art_port_[i] = load_tex(String("res://assets/art/char/portrait/") +
                                String::utf8(ally_art_key(kRosterIds[i]).c_str()) + ".png");
        if (art_port_[i].is_valid()) ++n_ok;
    }
    UtilityFunctions::print("[ui] 任务素材 menu=", art_menu_.is_valid(),
                            " brief=", art_brief_.is_valid(),
                            " chapter=", art_chapter_.is_valid(),
                            " win=", art_win_.is_valid(),
                            " lose=", art_lose_.is_valid(),
                            " 胸像=", n_ok, "/", kRosterN);
}

// 按「cover」铺满 + 横向渐变压暗。
//   dim_edge   画面左右边缘的压暗量（标题 / 菜单项都在左侧，必须压下去）
//   dim_center 画面中央的压暗量
// 【为什么不能直接拉伸】素材是 16:9、设计视口也是 16:9，但窗口尺寸是玩家可改的，
// 一旦比例变了，拉伸会让人脸和山脊一起变形。cover 是等比放大到刚好盖住，
// 多出来的部分居中裁掉 —— 代价是极端比例下会裁掉边缘，「天空 + 山脊」的构图
// 正好是最不怕裁的那一类。
void Hud::draw_art_bg(const Ref<Texture2D> &tex, float dim_edge, float dim_center) {
    if (tex.is_null()) return;
    const Vector2 ts = tex->get_size();
    if (ts.x < 1.0f || ts.y < 1.0f) return;

    const float k = std::max(vp_.x / ts.x, vp_.y / ts.y);
    const Vector2 sz = ts * k;
    draw_texture_rect(tex, Rect2(vp_.x * 0.5f - sz.x * 0.5f, vp_.y * 0.5f - sz.y * 0.5f,
                                 sz.x, sz.y), false);

    // 横向渐变压暗。原来这里是「40 段不同 alpha 的 draw_rect」，
    // 在 2560 宽下留下周期 65.6px 的规则竖纹（见 grad_h 上方的实测数据）。
    // 换成逐顶点配色后是一整块连续渐变，没有段、没有边界。
    const Color edge(0.010f, 0.014f, 0.020f, dim_edge);
    const Color mid(0.010f, 0.014f, 0.020f, dim_center);
    grad_h(Rect2(0.0f, 0.0f, vp_.x, vp_.y), edge, mid, edge);
}

// ---------------------------------------------------------------- 主菜单
Rect2 Hud::menu_item_rect(int i) const {
    return Rect2(150.0f * s_, (430.0f + (float)i * 56.0f) * s_, 420.0f * s_, 46.0f * s_);
}

int Hud::menu_hit(const Vector2 &p) const {
    for (int i = 0; i < 3; ++i) {
        if (menu_item_rect(i).has_point(p)) return i;
    }
    return -1;
}

void Hud::menu_activate(int i) {
    if (i == 0)      start_req_ = true;          // 直接开打
    else if (i == 1) set_screen(SCREEN_BRIEF);   // 先看简报
    else if (i == 2) quit_req_ = true;
    queue_redraw();
}

void Hud::draw_menu() {
    draw_art_bg(art_menu_, 0.94f, 0.28f);

    /* 底部渐浓的衬底。主菜单最下面两行是 13px 的辅助文字，压在这张 key art
       的山体上本来就接近不可读（key art 越亮越糟）。用竖向渐变而不是
       "一条固定 alpha 的矩形"：矩形上沿会留下一条横线，渐变的上沿 alpha=0，
       看不出从哪开始。 */
    grad_v(Rect2(0.0f, vp_.y - 210.0f * s_, vp_.x, 210.0f * s_),
           Color(0.010f, 0.014f, 0.020f),
           [](float t) { return 0.52f * t * t; });

    const float x0 = 150.0f * s_;

    // 顶部：战区标识
    tx(String::utf8("V O L U N T E E R   A R M Y"), x0, 128.0f * s_, 13, c_shell_faint());
    tx(String::utf8("中国人民志愿军 · 1951 · 朝鲜东线"), x0, 154.0f * s_, 15,
       Color(0.88f, 0.91f, 0.94f, 0.70f));
    // 一小段琥珀色的分隔短横：给标题区一个明确的起点
    draw_rect(Rect2(x0, 172.0f * s_, 46.0f * s_, 2.0f * s_),
              Color(0.96f, 0.68f, 0.26f, 0.90f), true);

    // 主标题 + 副标题
    tx(String::utf8("断 头 谷 公 路"), x0, 262.0f * s_, 76, c_text());
    tx(String::utf8("第 一 人 称 伏 击 战"), x0, 302.0f * s_, 21,
       Color(0.82f, 0.86f, 0.90f, 0.76f));

    // 菜单项
    const char *items[3] = { "开 始 行 动", "任 务 简 报", "退 出 游 戏" };
    for (int i = 0; i < 3; ++i) {
        const Rect2 r = menu_item_rect(i);
        const bool sel = (i == menu_sel_);
        if (sel) {
            // 选中底板：从左边实、往右渐隐 —— 比整块实心底板轻，
            // 不会把背后的 key art 压死。同样走逐顶点渐变（理由见 grad_h）：
            // 分层数越少越容易看出条纹，这种"只有几十像素宽"的小底板尤其明显。
            grad_h(r,
                   Color(0.96f, 0.68f, 0.26f, 0.30f),
                   Color(0.96f, 0.68f, 0.26f, 0.13f),
                   Color(0.96f, 0.68f, 0.26f, 0.0f));
            // 左侧实心标记条：使命召唤菜单最典型的选中标记，一眼就知道光标在哪
            draw_rect(Rect2(r.position.x - 20.0f * s_, r.position.y + 7.0f * s_,
                            4.0f * s_, r.size.y - 14.0f * s_),
                      Color(0.96f, 0.68f, 0.26f, 0.95f), true);
        }
        const float pulse = sel ? (0.86f + 0.14f * std::sin(menu_t_ * 4.2f)) : 1.0f;
        const Color col = sel ? Color(1.0f, 1.0f, 1.0f, pulse)
                              : Color(0.84f, 0.88f, 0.92f, 0.74f);
        tx_mid(String::utf8(items[i]), r.position.x, r.position.y + r.size.y * 0.5f,
               sel ? 30 : 27, col);
    }

    // 底部：版本 + 操作提示
    tx(String::utf8("VolunteerArmyPC · Godot 4.5 + C++17 GDExtension"),
       x0, vp_.y - 34.0f * s_, 13, c_shell_faint());
    tx_r(String::utf8("↑ ↓ 选择 · Enter 确认 · 也可直接用鼠标"),
         vp_.x - x0, vp_.y - 34.0f * s_, 13, c_shell_faint());
}

// ---------------------------------------------------------------- 任务简报
/* 小队名册：11 格「胸像 + 姓名 / 职务 + 血条」。
 *
 * 【为什么是一横条而不是列表】简报的上半部分已经被态势图（左栏）与
 * 元信息 + 目标清单（右栏）占满：右栏到 y≈524 就结束了，而底部提示从 990 起 ——
 * 中间是一条约 285px 高的**整宽空带**。横向排开 11 格正好落在这条带里；
 * 而且"一队人一字排开"本来就是军事简报的通用语汇，不需要发明新布局。
 *
 * 【p_cx 是整条的中轴，不是左边缘】第一版传的是左边距 x0(=150)，
 * 结果右边空出 253px、左边只有 150px，一眼就看出偏。改成按视口中轴对齐之后，
 * 和顶栏那条分隔线（同样关于视口中轴对称）落在同一根轴上。
 * 不用左栏的 x0 也不右栏的 rx —— 那两栏是"分栏内容"的边界，不是页面的轴。
 *
 * 【竖直位置是怎么定出来的】把三处边界算出来再居中，不靠贴图试：
 *   · 上方边界 = 区域态势图说明第二行的基线 582.75（第一版摆 590，
 *     和它只差 7px，两行字直接叠在一起 —— 截图上"小队名册"压在
 *     "……河谷上的桥是敌退路，可炸。"上）；
 *   · 下方边界 = 底部渐浓衬底的起点 880（那之上 alpha 还是 0，压不到也不显边）；
 *   · 名册自身高度 = 标题 15 + 28 + 胸像 153 + 姓名 20 + 职务 18 + 血条 3 ≈ 235。
 * 于是居中解出基线 626，实测块占 611~861，上下各留 23 / 19px。
 *
 * 【为什么状态读 va::W.units，而姓名/职务读编制表】编制表（va::ROSTER）是静态的，
 * 而简报可能在打完一局之后再次打开（结算 → Esc 回主菜单 → 任务简报），
 * 那时名单上该显示的是**这一局的实际伤亡**。所以：身份取编制表，状态取战局。
 */
void Hud::draw_roster(float p_cx, float p_baseline) {
    const float tw = 122.0f * s_;                 // 格子宽
    const float gap = 30.0f * s_;
    const float ph = tw * 332.0f / 256.0f;        // 胸像高（源图固定 256x332）
    const float strip_w = kRosterN * tw + (kRosterN - 1) * gap;
    const float p_x = p_cx - strip_w * 0.5f;
    const float p_y = p_baseline;

    tx(String::utf8("小 队 名 册"), p_x, p_y, 17, c_text());
    tx_r(String::utf8("编制 11 人 · 1组 / 2组 / 支援组"), p_x + strip_w, p_y, 12, c_shell_dim());

    const float y = p_y + 28.0f * s_;
    for (int i = 0; i < kRosterN; ++i) {
        const float x = p_x + i * (tw + gap);

        // ---- 身份：编制表 ----
        const bool is_player = (i == 0);
        std::string nm_s = "你（队长）";
        std::string role_s = "队长";
        if (!is_player) {
            const va::RosterDef *d = va::roster_of(kRosterIds[i]);
            if (d != nullptr) { nm_s = d->name; role_s = d->role; }
        }

        // ---- 状态：战局 ----
        const va::Unit *u = nullptr;
        for (const auto &cand : va::W.units) {
            if (cand.team == va::Team::Ally && cand.id == kRosterIds[i]) { u = &cand; break; }
        }
        const bool dead = (u != nullptr && u->dead);
        const bool down = (u != nullptr && u->downed && !u->dead);

        // ---- 胸像 ----
        const Rect2 pr(x, y, tw, ph);
        const Ref<Texture2D> &tex = art_port_[i];
        if (tex.is_valid()) {
            draw_texture_rect(tex, pr, false, Color(1.0f, 1.0f, 1.0f, dead ? 0.32f : 1.0f));
        } else {
            draw_rect(pr, Color(0.06f, 0.08f, 0.10f, 0.90f), true);
        }
        // 压暗一层。胸像上半是亮天空、下半是近黑剪影，不压的话姓名与状态
        // 在深色胸像上会读不出来 —— 和右栏衬底是同一个理由。
        draw_rect(pr, Color(0.010f, 0.014f, 0.020f, dead ? 0.60f : 0.20f), true);

        // ---- 边框：玩家用琥珀、阵亡用暗红、其余冷白细描边 ----
        const Color fc = dead ? Color(0.62f, 0.16f, 0.14f, 0.85f)
                              : (is_player ? c_amber() : Color(0.86f, 0.90f, 0.94f, 0.34f));
        const PackedVector2Array fp = chamfer(pr.grow(1.0f), 9.0f * s_);
        for (int k = 0; k < 8; ++k) {
            draw_line(fp[k], fp[(k + 1) % 8], fc, (is_player ? 1.8f : 1.1f) * s_, true);
        }

        // ---- 姓名 ----
        tx_c(String::utf8(nm_s.c_str()), x + tw * 0.5f, y + ph + 20.0f * s_, 16,
             dead ? Color(0.72f, 0.74f, 0.76f, 0.55f) : c_text());

        // ---- 职务 / 状态。字幕位置固定，避免不同长度文本让整排高低不齐 ----
        String sub = String::utf8(role_s.c_str());
        Color sc = c_shell_dim();
        if (dead)      { sub = String::utf8("阵亡"); sc = Color(0.88f, 0.34f, 0.30f, 0.95f); }
        else if (down) { sub = String::utf8("待救"); sc = c_amber(); }
        tx_c(sub, x + tw * 0.5f, y + ph + 38.0f * s_, 12, sc);

        // ---- 血条：3px。只在"已知且活着"时画，未部署/阵亡不画假数据 ----
        if (u != nullptr && !dead) {
            const float bw = tw * 0.78f;
            const float bx = x + (tw - bw) * 0.5f;
            const float by = y + ph + 46.0f * s_;
            const float t = std::max(0.0f, std::min(1.0f, u->hp / std::max(u->maxHp, 1.0f)));
            const Color hc = (t > 0.60f) ? Color(0.44f, 0.78f, 0.38f, 0.92f)
                                         : (t > 0.30f ? c_amber()
                                                      : Color(0.88f, 0.28f, 0.22f, 0.95f));
            draw_rect(Rect2(bx, by, bw, 3.0f * s_), Color(0.86f, 0.90f, 0.94f, 0.16f), true);
            draw_rect(Rect2(bx, by, bw * t, 3.0f * s_), hc, true);
        }
    }
}

void Hud::draw_brief() {
    draw_art_bg(art_brief_, 0.95f, 0.44f);

    const float x0 = 150.0f * s_;
    const float top = 108.0f * s_;

    // 顶栏
    tx(String::utf8("任 务 简 报"), x0, top, 34, c_text());
    tx(String::utf8("行动代号 · 断头谷"), x0, top + 26.0f * s_, 15, c_shell_faint());
    tx_r(String::utf8("机密 · 仅限参战人员"), vp_.x - x0, top, 13, c_red());
    draw_rect(Rect2(x0, top + 46.0f * s_, vp_.x - 2.0f * x0, 1.0f * s_),
              Color(0.86f, 0.90f, 0.94f, 0.22f), true);

    // ---- 左栏：区域态势图 ----
    const float iw = 620.0f * s_;
    const float ih = iw * 9.0f / 16.0f;
    const Rect2 ir(x0, top + 78.0f * s_, iw, ih);
    if (art_chapter_.is_valid()) {
        draw_texture_rect(art_chapter_, ir, false, Color(1.0f, 1.0f, 1.0f, 0.96f));
    } else {
        draw_rect(ir, Color(0.05f, 0.07f, 0.09f, 0.92f), true);
    }
    // 切角边框
    {
        const PackedVector2Array p = chamfer(ir.grow(1.0f), 14.0f * s_);
        for (int i = 0; i < 8; ++i) draw_line(p[i], p[(i + 1) % 8], c_line(), 1.2f * s_, true);
    }
    // 四角标记：情报图的通用语汇，四笔就能把"这是一张图"说清楚
    {
        const Color cc = Color(0.96f, 0.68f, 0.26f, 0.85f);
        const float L = 24.0f * s_;
        const float w = 1.8f * s_;
        const float l = ir.position.x, t = ir.position.y;
        const float rr = ir.position.x + ir.size.x, bb = ir.position.y + ir.size.y;
        draw_line(Vector2(l, t), Vector2(l + L, t), cc, w, true);
        draw_line(Vector2(l, t), Vector2(l, t + L), cc, w, true);
        draw_line(Vector2(rr, t), Vector2(rr - L, t), cc, w, true);
        draw_line(Vector2(rr, t), Vector2(rr, t + L), cc, w, true);
        draw_line(Vector2(l, bb), Vector2(l + L, bb), cc, w, true);
        draw_line(Vector2(l, bb), Vector2(l, bb - L), cc, w, true);
        draw_line(Vector2(rr, bb), Vector2(rr - L, bb), cc, w, true);
        draw_line(Vector2(rr, bb), Vector2(rr, bb - L), cc, w, true);
    }
    tx(String::utf8("区域态势 · 断头谷公路"), x0, ir.position.y + ih + 26.0f * s_, 14,
       c_shell_dim());
    tx(String::utf8("车队自东沿公路进入伏击圈；河谷上的桥是敌退路，可炸。"),
       x0, ir.position.y + ih + 48.0f * s_, 13, c_shell_faint());

    // ---- 右栏：任务元信息 ----
    const float rx = x0 + iw + 66.0f * s_;

    /* 右栏衬底。这一栏的文字全都压在 key art 的中段上：暮色天空（亮）与
       士兵剪影（近黑）正好在这里交界，任何"固定低不透明度"的小字都会在
       交界处半截读不出来 —— 实测 (0/6) / (0/2) 两个计数器就落在这个交界上，
       支线目标的字尾也被剪影吃掉。所以按"信息栏"的标准做法铺一层暗色衬底：
         · 纵向铺满（0 到 vp_.y）→ 不会多出上/下沿的硬边；
         · 横向是平顶剖面（两端各留 20% 羽化）→ 中间稳定压暗，
           两端 alpha 收到 0，与画面的衔接看不出边界；
         · 单次 draw_polygon 逐顶点配色 → 不引入分段合成的规则竖纹。 */
    grad_h(Rect2(rx - 58.0f * s_, 0.0f, 620.0f * s_ + 116.0f * s_, vp_.y),
           Color(0.010f, 0.014f, 0.020f),
           [](float t) {
               const float a = std::min(t / 0.20f, 1.0f);
               const float b = std::min((1.0f - t) / 0.20f, 1.0f);
               return 0.62f * (a * a * (3.0f - 2.0f * a)) * (b * b * (3.0f - 2.0f * b));
           });

    /* 底部渐浓的衬底。底部两行提示压在近黑岩石上：金色那行够亮，
       13px 的"Esc 返回主菜单"原本用 c_faint()（alpha 0.26）几乎看不见。
       这里补一条竖向渐变，并把两行的颜色换到外壳档。 */
    grad_v(Rect2(0.0f, vp_.y - 200.0f * s_, vp_.x, 200.0f * s_),
           Color(0.010f, 0.014f, 0.020f),
           [](float t) { return 0.58f * t * t; });

    float y = top + 78.0f * s_;

    struct KV { const char *k; std::string v; };
    const KV meta[4] = {
        { "地点",   "朝鲜东线 · 断头谷公路" },
        { "时间",   "1951 年 · 拂晓前" },
        { "天气",   weather_cn(va::W.weather) },
        { "撤离点", va::W.evac.name },
    };
    for (const KV &m : meta) {
        tx(String::utf8(m.k), rx, y, 14, c_shell_dim());
        tx(String::utf8(m.v.c_str()), rx + 92.0f * s_, y, 16, c_text());
        y += 30.0f * s_;
    }

    // ---- 右栏：目标清单 ----
    y += 18.0f * s_;
    draw_rect(Rect2(rx, y - 16.0f * s_, 620.0f * s_, 1.0f * s_),
              Color(0.86f, 0.90f, 0.94f, 0.30f), true);
    y += 10.0f * s_;
    tx(String::utf8("任 务 目 标"), rx, y, 17, c_text());
    y += 28.0f * s_;

    /* 清单直接读 va::W.objState —— 也就是战斗中目标横幅用的同一份数据。
       好处是简报里写的就是实际会被判定的条目，不存在"简报说一套、判定另一套"。
       （set_screen(SCREEN_BRIEF) 里会主动刷一次 check_objectives，
         因为菜单期间逻辑冻结、没人驱动它。） */
    for (size_t i = 0; i < va::W.objState.size(); ++i) {
        const va::WorldState::Objective &o = va::W.objState[i];
        const Color col = o.main ? c_amber() : Color(0.84f, 0.88f, 0.92f, 0.72f);
        const Rect2 box(rx, y - 11.0f * s_, 11.0f * s_, 11.0f * s_);
        // 主目标实心、支线空心：不必读字就能分辨
        if (o.main) draw_rect(box, col, true);
        else        draw_rect(box, col, false, 1.6f * s_);
        tx(String::utf8(o.text.c_str()), rx + 24.0f * s_, y, o.main ? 17 : 16,
           o.main ? c_text() : Color(0.88f, 0.91f, 0.94f, 0.88f));
        if (!o.extra.empty()) {
            tx_r(String::utf8(o.extra.c_str()), rx + 620.0f * s_, y, 14, c_shell_dim());
        }
        y += 27.0f * s_;
    }

    // ---- 下部：小队名册（填满左右两栏之下的整宽空带，关于视口中轴对称）----
    draw_roster(vp_.x * 0.5f, 626.0f * s_);

    // ---- 底部：开始 / 返回 ----
    const float pulse = 0.70f + 0.30f * std::sin(brief_t_ * 3.6f);
    tx_c(String::utf8("按 Enter 或点击任意处开始行动"), vp_.x * 0.5f, vp_.y - 90.0f * s_, 22,
         Color(0.96f, 0.68f, 0.26f, pulse));
    tx_c(String::utf8("Esc 返回主菜单"), vp_.x * 0.5f, vp_.y - 60.0f * s_, 13, c_shell_faint());
}

// ---------------------------------------------------------------- 外壳状态机
void Hud::set_screen(Screen s) {
    if (s == screen_) return;
    screen_ = s;
    if (s == SCREEN_MENU) {
        menu_sel_ = 0;
        menu_t_ = 0.0f;
    } else if (s == SCREEN_BRIEF) {
        brief_t_ = 0.0f;
        /* 简报要列目标，而 objState 是 check_objectives() 填的 ——
           那一趟平时由 step_once 每帧驱动，而菜单/简报期间逻辑是冻结的、没人跑它。
           所以进简报时主动刷一次。它是纯汇总（只读 W.stats、只写 W.objState），
           在任何时刻调用都安全，不会推动战局。 */
        va::check_objectives();
    }
    queue_redraw();
}

bool Hud::shell_key(int64_t p_keycode) {
    if (!shell_active()) return false;
    const Key k = (Key)p_keycode;

    if (screen_ == SCREEN_MENU) {
        if (k == Key::KEY_UP || k == Key::KEY_W) {
            menu_sel_ = (menu_sel_ + 2) % 3;
            queue_redraw();
            return true;
        }
        if (k == Key::KEY_DOWN || k == Key::KEY_S) {
            menu_sel_ = (menu_sel_ + 1) % 3;
            queue_redraw();
            return true;
        }
        if (k == Key::KEY_ENTER || k == Key::KEY_KP_ENTER || k == Key::KEY_SPACE) {
            menu_activate(menu_sel_);
            return true;
        }
        return true;   // 菜单里其它键一律吞掉：绝不让它们漏进逻辑层
    }

    // 简报
    if (k == Key::KEY_ENTER || k == Key::KEY_KP_ENTER || k == Key::KEY_SPACE) {
        start_req_ = true;
        return true;
    }
    if (k == Key::KEY_ESCAPE) {
        set_screen(SCREEN_MENU);
        return true;
    }
    return true;
}

void Hud::shell_hover(const Vector2 &p) {
    if (screen_ != SCREEN_MENU) return;
    const int h = menu_hit(p);
    if (h >= 0 && h != menu_sel_) {
        menu_sel_ = h;
        queue_redraw();
    }
}

bool Hud::shell_click(const Vector2 &p) {
    if (screen_ == SCREEN_MENU) {
        const int h = menu_hit(p);
        if (h >= 0) {
            menu_sel_ = h;
            menu_activate(h);
            return true;
        }
        return false;
    }
    // 简报："点击任意处开始" —— 和屏幕下方那行提示一致。
    // 不设按钮热区是因为简报整屏都是可点的，给个明确的文案比让玩家去找按钮更好。
    if (screen_ == SCREEN_BRIEF) {
        start_req_ = true;
        return true;
    }
    return false;
}

bool Hud::take_start() {
    if (!start_req_) return false;
    start_req_ = false;
    return true;
}

bool Hud::take_quit() {
    if (!quit_req_) return false;
    quit_req_ = false;
    return true;
}

} // namespace volunteer_army
