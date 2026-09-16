// VolunteerArmyPC —— WorldSim 实现：逻辑层 ↔ Godot 的桥
#include "node/world_sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace volunteer_army {

// 站在地面上的眼高：1 世界单位 = 0.05 米，网页版 EYE = 33 → 1.65 米
static constexpr float EYE_H = 1.65f;

// 崩溃诊断：设了 VA_TRACE=<文件路径> 时把执行轨迹写进去（每步 fclose，崩了也留得下）
static void va_trace(const char *msg) {
    const char *p = std::getenv("VA_TRACE");
    if (p == nullptr || *p == '\0') return;
    FILE *f = std::fopen(p, "a");
    if (f == nullptr) return;
    std::fprintf(f, "[T] %s\n", msg);
    std::fclose(f);
}

// 战局随机种子。
// 默认取毫秒时钟（每局都不一样，这是网页版"天气变体带来重玩性"的设计）；
// 但取证/回归时必须能复现，所以留 VA_SEED 覆盖：
//     VA_SEED=12345   → 固定同一张地图布局、同一场天气
// 想扫遍三种天气又不想靠运气，就试 VA_SEED=1/2/3/4/5…（天气由首个 RNG 决定）
static uint32_t new_seed() {
    const char *v = std::getenv("VA_SEED");
    if (v != nullptr && *v != '\0') return (uint32_t)std::strtoul(v, nullptr, 10);
    return (uint32_t)Time::get_singleton()->get_ticks_msec();
}

WorldSim::WorldSim() = default;
WorldSim::~WorldSim() = default;

void WorldSim::_bind_methods() {
    ClassDB::bind_method(D_METHOD("start_mission", "skip_deploy"), &WorldSim::start_mission);
    ClassDB::bind_method(D_METHOD("reset_mission"), &WorldSim::reset_mission);
    ClassDB::bind_method(D_METHOD("get_status"), &WorldSim::get_status);
    ClassDB::bind_method(D_METHOD("get_diag"), &WorldSim::get_diag);
}

// 开局把视线转向公路来向（东北），让玩家一睁眼就看见要伏击的那条路
void WorldSim::aim_at_road() {
    const va::Unit *p = va::W.player;
    if (p == nullptr) return;
    const float a = std::atan2(va::CFG.roadCY - p->y, 2020.0f - p->x);
    va::W.player->facing = a;
    yaw_ = -a - 1.5707963267948966f;
    pitch_ = -0.04f;
}

void WorldSim::_ready() {
    va_trace("_ready:enter");
    va::build_convoy_way();
    va::set_events(this);

    /* 顺序是关键：世界必须先于静态场景建立。
       build_scene() 会读 va::W.props 来生成全部掩体，而 W.props 是在
       init_world() 里由 BASE_PROPS 填充的 —— 反过来写的话，
       掩体节点层会建成一个空容器：逻辑层有 69 处掩体、画面上一个都看不见，
       「掩体评分 / 掩体减伤」这套核心玩法在视觉上完全不可读。
       （实测第一版就是这么错的，截图里光秃秃一片。） */
    va::init_world(new_seed());
    va::W.started = true;
    va::W.deployDone = true;
    mission_started_ = true;
    force_ads_ = (std::getenv("VA_ADS") != nullptr);
    va_trace("_ready:init_world ok");

    build_scene(this, refs_);
    va_trace("_ready:build_scene ok");
    // 取证用消融开关：把某一层藏起来，用来判定"画面上这块到底是什么"。
    // 本机已经在"肉眼认几何体"上错过好几次（把树当成枪、把枪当成草地……），
    // 分层消融是唯一可靠的归属判断手段。
    if (std::getenv("VA_HIDE_PROPS") != nullptr && refs_.props != nullptr) refs_.props->set_visible(false);
    if (std::getenv("VA_HIDE_UNITS") != nullptr && refs_.units != nullptr) refs_.units->set_visible(false);
    if (std::getenv("VA_HIDE_VEH") != nullptr && refs_.vehicles != nullptr) refs_.vehicles->set_visible(false);
    spawn_entity_nodes();
    va_trace("_ready:spawn ok");
    setup_runtime_ui();
    va_trace("_ready:ui ok");

    // 第一人称相机
    cam_ = memnew(Camera3D);
    // FOV 65（垂直）→ 水平约 100°。
    // 之前用 78 是照搬网页版，但那是 2D 俯视时代的遗留：垂直 78 在 2560×1369 下
    // 水平高达 113°，属于广角畸变区，靠近相机的东西会被拉得又大又歪，枪模怎么摆
    // 都不像枪。这是第一人称射击的常规区间，也是使命召唤的手感来源。
    // VA_FOV 可覆盖（想看广角的战场感就 VA_FOV=78）。
    cam_->set_fov(65.0f);
    {
        const char *fv = std::getenv("VA_FOV");
        if (fv != nullptr && *fv != '\0') {
            const float f = (float)std::strtod(fv, nullptr);
            if (f > 20.0f && f < 140.0f) cam_->set_fov(f);
        }
    }
    // near 收到 0.06：视图模型最靠后的枪托底板在开镜时为 z=-0.135、腰射时 z=-0.175，
    // 再加后坐最大 +0.045 的退让，最坏也还有 4 cm 余量。
    cam_->set_near(0.06f);
    cam_->set_far(600.0f);
    add_child(cam_);
    vm_.build(cam_);
    va_trace("_ready:cam ok");

    aim_at_road();
    sync_entity_nodes();
    va_trace("_ready:sync ok");

    capture_setup();

    UtilityFunctions::print("[VolunteerArmyPC] ", get_diag());
}

// --------------------------------------------------------- 截图探针
// 用法：环境变量 VA_CAPTURE="2,30,120,240"（战局秒数，逗号分隔）
//       环境变量 VA_CAPTURE_DIR 可覆盖输出目录（默认 res://captures）
// 用途：在无头/无人值守环境下取证「3D 战场确实渲染出来了」。
void WorldSim::capture_setup() {
    const char *env = std::getenv("VA_CAPTURE");
    if (!env || !*env) return;
    cap_enabled_ = true;
    std::string s(env);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        if (comma == std::string::npos) comma = s.size();
        const std::string tok = s.substr(pos, comma - pos);
        if (!tok.empty()) cap_times_.push_back(std::atof(tok.c_str()));
        pos = comma + 1;
    }
    std::sort(cap_times_.begin(), cap_times_.end());
    UtilityFunctions::print("[VA_CAPTURE] 计划在 ", (int)cap_times_.size(), " 个时刻截图");
}

void WorldSim::capture_step() {
    if (!cap_enabled_ || cap_i_ >= (int)cap_times_.size()) return;
    if (cap_sim_t_ < cap_times_[(size_t)cap_i_]) return;
    // 等两帧，确保这一帧的相机变换与场景同步都已生效（否则会拍到上一帧的画面）
    if (cap_frame_skip_ < 2) { cap_frame_skip_++; return; }
    cap_frame_skip_ = 0;

    const char *dir_env = std::getenv("VA_CAPTURE_DIR");
    const std::string dir = dir_env && *dir_env ? std::string(dir_env) : std::string("res://captures");
    const godot::String gdir = godot::String::utf8(dir.c_str());
    DirAccess::make_dir_recursive_absolute(gdir);

    const Viewport *vp = get_viewport();
    if (vp != nullptr) {
        const Ref<ViewportTexture> tex = vp->get_texture();
        if (tex.is_valid()) {
            const Ref<Image> img = tex->get_image();
            if (img.is_valid()) {
                const int t = (int)cap_times_[(size_t)cap_i_];
                const godot::String path = gdir + godot::String("/cap_") + godot::String::num_int64(t)
                                         + godot::String("s.png");
                const Error err = img->save_png(path);
                UtilityFunctions::print("[VA_CAPTURE] ", path, err == OK ? " OK" : " FAILED");
            }
        }
    }
    cap_i_++;
    if (cap_i_ >= (int)cap_times_.size()) {
        UtilityFunctions::print("[VA_CAPTURE] 全部完成，退出");
        get_tree()->quit();
    }
}

void WorldSim::setup_runtime_ui() {
    CanvasLayer *layer = memnew(CanvasLayer);
    layer->set_layer(10);
    add_child(layer);

    // 整个 HUD 就是一个 Control，内容全部手绘。
    // 早期版本用 4 个 Label 平铺文字，能读但完全没有"界面"可言 ——
    // 使命召唤那套 UI 的信息有一半是用形状讲的（罗盘刻度、雷达、切角面板、
    // 动态准星），Label 表达不了。详见 node/hud.h 顶部说明。
    hud_ = memnew(Hud);
    hud_->set_name("Hud");
    // 铺满视口。HUD 只画不点，必须忽略鼠标事件，
    // 否则它会吃掉 _input 里那套鼠标视角控制。
    hud_->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    hud_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    layer->add_child(hud_);

    // 取证用消融开关：把 HUD 整层藏掉，剩下的一定是 3D 画面。
    // 上一轮"画面正中央那根黑竖条到底是谁"靠肉眼认几何体认错过一次，
    // 所以凡是"这块黑东西属于哪一层"的问题，一律用消融来答，不靠眼看。
    if (std::getenv("VA_HIDE_HUD") != nullptr) hud_->set_visible(false);
}

void WorldSim::spawn_entity_nodes() {
    for (auto &u : va::W.units) {
        Node3D *n = make_soldier_node(u.team == va::Team::Enemy, u.downed);
        refs_.units->add_child(n);
        unit_nodes_.push_back(n);
    }
    for (auto &v : va::W.vehicles) {
        Node3D *n = make_vehicle_node(v.type);
        refs_.vehicles->add_child(n);
        veh_nodes_.push_back(n);
    }
}

// 单位节点若数量对不上（例如增援/重开一局），整体重建
void WorldSim::sync_entity_nodes() {
    if (unit_nodes_.size() != va::W.units.size()) {
        va_trace("sync:rebuild units");
        for (auto *n : unit_nodes_) n->queue_free();
        unit_nodes_.clear();
        for (auto &u : va::W.units) {
            Node3D *n = make_soldier_node(u.team == va::Team::Enemy, u.downed);
            refs_.units->add_child(n);
            unit_nodes_.push_back(n);
        }
        va_trace("sync:rebuild units done");
    }
    if (veh_nodes_.size() != va::W.vehicles.size()) {
        va_trace("sync:rebuild vehicles");
        for (auto *n : veh_nodes_) n->queue_free();
        veh_nodes_.clear();
        for (auto &v : va::W.vehicles) {
            Node3D *n = make_vehicle_node(v.type);
            refs_.vehicles->add_child(n);
            veh_nodes_.push_back(n);
        }
        va_trace("sync:rebuild vehicles done");
    }

    for (size_t i = 0; i < va::W.units.size(); ++i) {
        const va::Unit &u = va::W.units[i];
        Node3D *n = unit_nodes_[i];
        if (u.dead) { n->set_visible(false); continue; }
        n->set_visible(true);
        n->set_position(to3(u.x, u.y, u.downed ? 0.0f : 0.0f));
        // 模型前方 = +X（与逻辑层一致），绕 Y 旋转角 = -facing
        n->set_rotation(Vector3(0, -u.facing, 0));
    }
    for (size_t i = 0; i < va::W.vehicles.size(); ++i) {
        const va::Vehicle &v = va::W.vehicles[i];
        Node3D *n = veh_nodes_[i];
        if (v.destroyed) {
            n->set_visible(true);
            n->set_position(to3(v.x, v.y, -0.18f));
            n->set_rotation(Vector3(0, -v.angle, 0));
            n->set_scale(Vector3(1, 0.86f, 1));
            continue;
        }
        n->set_position(to3(v.x, v.y));
        n->set_rotation(Vector3(0, -v.angle, 0));
    }
}

void WorldSim::_process(double p_delta) {
    if (cam_ == nullptr) return;

    // 固定步长推进逻辑（网页版是「按需要拆成 <=0.022s 的小步」，这里等价处理）
    va_trace("_process:enter");
    const double H = 1.0 / 60.0;
    acc_ += p_delta;
    int n = 0;
    while (acc_ >= H && n < 8) {
        va::step_once((float)H);
        acc_ -= H;
        ++n;
    }
    if (n == 8) acc_ = 0.0;   // 掉帧时放弃追帧，避免螺旋
    va_trace("_process:step ok");

    sync_entity_nodes();
    va_trace("_process:sync ok");

    // 相机跟随：位置取玩家单位，朝向由 yaw/pitch 决定
    const va::Unit *p = va::W.player;
    if (p) {
        // 走路摇晃
        const bool moving = (va::IN.w || va::IN.a || va::IN.s || va::IN.d);
        if (moving) bob_t_ += (float)p_delta * (va::IN.shift ? 13.0f : 9.0f);
        const float bob = moving ? std::sin(bob_t_) * 0.035f : 0.0f;
        cam_->set_position(to3(p->x, p->y, (va::IN.ctrl ? 1.05f : EYE_H) + bob));

        // 逻辑层朝向回写：yaw = -facing - π/2  ⇒  facing = -yaw - π/2
        va::W.viewPitch = pitch_;
        cam_->set_rotation(Vector3(pitch_, yaw_, 0));
    }

    va_trace("_process:cam ok");

    // HUD：采样世界状态 + 推进动画。整屏内容一次 _draw() 画完，
    // 所以这里只需每帧调一次 update（它内部会 queue_redraw）。
    if (hud_ != nullptr) hud_->update(p_delta);

    // 视图模型：只读逻辑层的状态，反过来不影响逻辑
    {
        const va::Unit *pl = va::W.player;
        if (pl != nullptr) {
            const float move01 = (va::IN.w || va::IN.a || va::IN.s || va::IN.d) ? 1.0f : 0.0f;
            // 开火判定：fireCd 被重新赋成 rof 的那一刻就是"打了一发"。
            // 比监听 magAmmo 更可靠 —— 换弹会让 magAmmo 反向跳变。
            if (prev_fire_cd_ >= 0.0f && pl->fireCd > prev_fire_cd_ + 1e-4f) vm_.on_shot();
            prev_fire_cd_ = pl->fireCd;
            if (prev_reload_t_ >= 0.0f && pl->reloadT > prev_reload_t_ + 1e-4f) {
                vm_.on_reload(pl->wpn != nullptr ? pl->wpn->reload : 2.2f);
            }
            prev_reload_t_ = pl->reloadT;
            // 取证开关：VA_ADS=1 强制据枪，用来拍开镜姿态（截图时没法按鼠标右键）
            bool ads = va::IN.ads;
            if (force_ads_) ads = true;
            vm_.update(p_delta, move01, va::IN.shift, pitch_, yaw_, ads);
        }
    }

    va_trace("_process:hud ok");
    cap_sim_t_ = (double)va::W.t;
    capture_step();
    va_trace("_process:end");
}

void WorldSim::_input(const Ref<InputEvent> &p_event) {
    Input *in = Input::get_singleton();
    if (in == nullptr) return;

    const bool captured = (in->get_mouse_mode() == Input::MOUSE_MODE_CAPTURED);

    if (Ref<InputEventMouseMotion> mm = p_event; mm.is_valid()) {
        if (!captured) return;
        yaw_ -= mm->get_relative().x * sens_;
        pitch_ -= mm->get_relative().y * sens_;
        pitch_ = va::clampf(pitch_, -1.45f, 1.45f);
        va::W.player->facing = -yaw_ - 1.5707963267948966f;
        return;
    }

    if (Ref<InputEventMouseButton> mb = p_event; mb.is_valid()) {
        if (mb->get_button_index() == MouseButton::MOUSE_BUTTON_LEFT && mb->is_pressed()) {
            if (!captured) { in->set_mouse_mode(Input::MOUSE_MODE_CAPTURED); return; }
            va::IN.fire = true;
            va::IN.firePressed = true;
            return;
        }
        if (mb->get_button_index() == MouseButton::MOUSE_BUTTON_LEFT && !mb->is_pressed()) {
            va::IN.fire = false;
            return;
        }
        if (mb->get_button_index() == MouseButton::MOUSE_BUTTON_RIGHT) {
            va::IN.ads = mb->is_pressed();
            return;
        }
        return;
    }

    if (Ref<InputEventKey> k = p_event; k.is_valid()) {
        const bool down = k->is_pressed() && !k->is_echo();
        const Key code = k->get_keycode();
        switch (code) {
            case Key::KEY_W: case Key::KEY_UP:    va::IN.w = down; break;
            case Key::KEY_S: case Key::KEY_DOWN:  va::IN.s = down; break;
            case Key::KEY_A: case Key::KEY_LEFT:  va::IN.a = down; break;
            case Key::KEY_D: case Key::KEY_RIGHT: va::IN.d = down; break;
            case Key::KEY_SHIFT: va::IN.shift = down; break;
            case Key::KEY_CTRL:  va::IN.ctrl = down; break;
            // R 有双重语义：战斗中换弹，结算界面上重开一局。
            // 用 W.over 分支而不是再占一个键 —— 结算时换弹毫无意义，
            // 键位重叠不会产生歧义。
            case Key::KEY_R:
                if (down) {
                    if (hud_ != nullptr && hud_->mission_over()) reset_mission();
                    else va::IN.reload = true;
                }
                break;
            case Key::KEY_G:     if (down) va::IN.grenade = true; break;
            case Key::KEY_F:     if (down) va::IN.smoke = true; break;
            case Key::KEY_Q:     if (down) { /* 指令面板（待接入） */ } break;
            case Key::KEY_Z:     if (down) va::IN.markerSet = true; break;
            case Key::KEY_ESCAPE:
                if (down) in->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
                break;
            default: break;
        }
    }
}

void WorldSim::_notification(int p_what) {
    (void)p_what;
}

// ------------------------------------------------------------- SimEvents
void WorldSim::on_say(const std::string &who, const std::string &text, const std::string &cls) {
    (void)cls;
    push_subtitle(who, text, cls);
}
void WorldSim::on_log(const std::string &text, const std::string &cls) { (void)text; (void)cls; }

void WorldSim::on_sfx(const std::string &id, float x, float y, float gain, bool local) {
    // 音频模块（XAudio2/Godot 混音）尚未接入；此处只做去重节流，避免刷屏
    (void)id; (void)x; (void)y; (void)gain; (void)local;
    const double now = (double)Time::get_singleton()->get_ticks_msec() / 1000.0;
    if (now - last_sfx_t_ < 0.0) return;
    last_sfx_t_ = now;
}
void WorldSim::on_toast(const std::string &text) {
    if (hud_ != nullptr) hud_->ev_toast(text);
}
void WorldSim::on_alert(const std::string &text, float dur) {
    if (hud_ != nullptr) hud_->ev_alert(text, dur);
}
void WorldSim::on_end(const std::string &kind, const std::string &text) {
    // 结算本身由 HUD 的结算面板呈现（成败、评级、统计）；
    // 这里只把逻辑层给的这段文案留个记录，方便对照逻辑输出。
    UtilityFunctions::print("[结算] ", String::utf8(kind.c_str()), "：", String::utf8(text.c_str()));
}
void WorldSim::on_subs_dirty() {}
void WorldSim::on_objectives_dirty() {}

void WorldSim::push_subtitle(const std::string &who, const std::string &text, const std::string &cls) {
    (void)cls;
    UtilityFunctions::print("[无线电] ", String::utf8(who.c_str()), ": ", String::utf8(text.c_str()));
}

// ------------------------------------------------------------- 对外接口
void WorldSim::start_mission(bool p_skip_deploy) {
    va::init_world(new_seed());
    va::W.started = true;
    va::W.deployDone = p_skip_deploy;
    mission_started_ = true;
    apply_weather(refs_);          // 天气在 init_world 里重新抽过，布光必须跟着重打
    aim_at_road();
    sync_entity_nodes();
}

void WorldSim::reset_mission() {
    mission_started_ = false;
    va::init_world(new_seed());
    va::W.started = true;
    va::W.deployDone = true;
    mission_started_ = true;
    // 新一局的掩体表可能不同（油桶已被殉爆打掉的需要复原）→ 视觉层一并重建
    if (refs_.root != nullptr) rebuild_props(refs_.root, refs_);
    apply_weather(refs_);
    spawn_entity_nodes();
    aim_at_road();
    sync_entity_nodes();
}

Dictionary WorldSim::get_status() const {
    Dictionary d;
    d["t"] = va::W.t;
    d["phase"] = String::utf8(va::W.phaseName.c_str());
    d["weather"] = String::utf8(va::W.weather.c_str());
    d["alive_allies"] = (int)va::alive_allies().size();
    d["enemy_dead"] = va::W.stats.enemyDead;
    d["units"] = (int)va::W.units.size();
    d["vehicles"] = (int)va::W.vehicles.size();
    d["props"] = (int)va::W.props.size();
    return d;
}

String WorldSim::get_diag() const {
    String s = String::utf8("VolunteerArmyPC · Godot 4.5 + C++17 GDExtension");
    s += String::utf8(" · 单体 ") + String::num_int64((int64_t)va::W.units.size());
    s += String::utf8(" · 载具 ") + String::num_int64((int64_t)va::W.vehicles.size());
    s += String::utf8(" · 掩体 ") + String::num_int64((int64_t)va::W.props.size());
    s += String::utf8(" · 路线点 ") + String::num_int64((int64_t)va::CONVOY_WAY.size());
    return s;
}

} // namespace volunteer_army
