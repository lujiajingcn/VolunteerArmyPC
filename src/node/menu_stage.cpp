// VolunteerArmyPC —— 界面外壳的三维主视觉（见 menu_stage.h 的完整设计说明）
#include "node/menu_stage.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/procedural_sky_material.hpp>
#include <godot_cpp/classes/sky.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "node/scene_builder.h"

namespace volunteer_army {
namespace {

constexpr float kPi = 3.14159265358979323846f;

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
T clampv(T p_v, T p_lo, T p_hi) {
    return p_v < p_lo ? p_lo : (p_v > p_hi ? p_hi : p_v);
}

/* 一盏平行光。三盏的分工与 scene_builder 的战场布光同思路：
   主光给形状与金属高光、补光把暗面从纯黑里拉出来、轮廓光把章体边缘从
   key art 的暗底上"抠"出来 —— 界面主视觉没有影子可依赖，全靠这三盏分层次。 */
godot::DirectionalLight3D *add_light(godot::Node *p_parent, const godot::Vector3 &p_rot_deg,
                                     const godot::Color &p_col, float p_energy,
                                     bool p_shadow) {
    godot::DirectionalLight3D *l = memnew(godot::DirectionalLight3D);
    l->set_rotation_degrees(p_rot_deg);
    l->set_color(p_col);
    // ⚠️ 能量走 set_param(PARAM_ENERGY)，Light3D 上没有 set_light_energy 这个名字
    //    （那是 GDScript 属性名 light_energy 的直译，C++ 绑定里叫 set_param）。
    l->set_param(godot::Light3D::PARAM_ENERGY, p_energy);
    l->set_shadow(p_shadow);
    p_parent->add_child(l);
    return l;
}

} // namespace

// ============================================================ setup
void MenuStage::setup(godot::Node *p_parent) {
    enabled_ = env_flag("VA_STAGE", true);
    dbg_ = env_flag("VA_DBG_STAGE", false);
    spin_ = env_f("VA_STAGE_SPIN", 0.30f);
    yaw_deg_ = env_f("VA_STAGE_YAW", 0.0f);
    pitch_deg_ = env_f("VA_STAGE_PITCH", 0.0f);
    frac_ = clampv(env_f("VA_STAGE_FRAC", 0.54f), 0.10f, 1.60f);
    margin_ = env_f("VA_STAGE_MARGIN", 150.0f);

    if (!enabled_) {
        godot::UtilityFunctions::print(
            godot::String::utf8("[stage] VA_STAGE=0 —— 界面三维主视觉关闭（一个节点都不建）"));
        return;
    }
    if (p_parent == nullptr) {
        enabled_ = false;
        return;
    }

    /* 先要模型再建节点：缺素材时**一个节点都不留**，
       否则会留下一个空 SubViewport 每帧照常渲染（白花 GPU、还看不出问题）。 */
    godot::Node3D *model = make_stage_model("menu_medal");
    if (model == nullptr) {
        enabled_ = false;
        godot::UtilityFunctions::print(
            godot::String::utf8("[stage] 缺 assets/art/ui/model/menu_medal.glb —— "
                                "本层不启用（界面仍是纯 2D，与加本层之前逐像素相同）"));
        return;
    }

    cont_ = memnew(godot::SubViewportContainer);
    cont_->set_stretch(true);                       // SubViewport 尺寸 = 容器尺寸
    cont_->set_mouse_filter(godot::Control::MOUSE_FILTER_IGNORE);   // 不抢菜单的鼠标
    cont_->set_visible(false);
    p_parent->add_child(cont_);

    sub_ = memnew(godot::SubViewport);
    /* 透明背景：这是"三维元素压在一张二维 key art 上"能成立的前提。
       ⚠️ 若这里失效，症状是主菜单右侧出现一块**实心矩形**（天空/纯色），
       而模型本身画得好好的 —— 这类"内容对了、背景错了"的故障最容易
       被当成美术问题，所以先把它写在注释里。 */
    sub_->set_transparent_background(true);
    /* 独立 World3D：不这么做，SubViewport 会借用主视口那个战场世界，
       菜单期间战场上的一切（地形、车队、单位的枪口光）会一起画进来。 */
    sub_->set_use_own_world_3d(true);
    sub_->set_update_mode(godot::SubViewport::UPDATE_ALWAYS);  // 每帧自转，不能只在可见时更新
    sub_->set_msaa_3d(godot::Viewport::MSAA_4X);               // 章体边缘是圆弧，锯齿最显眼
    cont_->add_child(sub_);

    godot::Camera3D *cam = memnew(godot::Camera3D);
    cam->set_fov(30.0);
    // 模型已归一化到"最大维 = 1 米、几何中心在原点"。fov 30 / 距离 3 m ⇒
    // 可视高度 2·3·tan15° ≈ 1.607 m ⇒ 章体最多占 62% 画高，四周留得住白。
    cam->set_position(godot::Vector3(0.0f, 0.0f, 3.0f));
    cam->set_current(true);
    sub_->add_child(cam);

    /* 环境：天空只用来做**环境光与金属反射**（金属没有可反射的东西就是一块黑），
       背景是否可见由 SubViewport 的 transparent_background 决定 —— 见 .h 的说明。 */
    godot::WorldEnvironment *we = memnew(godot::WorldEnvironment);
    godot::Ref<godot::Environment> env;
    env.instantiate();
    godot::Ref<godot::Sky> sky;
    sky.instantiate();
    godot::Ref<godot::ProceduralSkyMaterial> sky_mat;
    sky_mat.instantiate();
    // 冷灰蓝的天顶 + 略暖的地面反射：金属章面才有"上冷下暖"的自然分层，
    // 全灰的话做旧铜色会读成一块塑料板。
    sky_mat->set_sky_top_color(godot::Color(0.18f, 0.21f, 0.27f));
    sky_mat->set_sky_horizon_color(godot::Color(0.34f, 0.35f, 0.38f));
    sky_mat->set_ground_horizon_color(godot::Color(0.26f, 0.24f, 0.22f));
    sky_mat->set_ground_bottom_color(godot::Color(0.07f, 0.07f, 0.08f));
    sky->set_material(sky_mat);
    env->set_sky(sky);
    env->set_background(godot::Environment::BG_SKY);
    env->set_ambient_source(godot::Environment::AMBIENT_SOURCE_SKY);
    env->set_reflection_source(godot::Environment::REFLECTION_SOURCE_SKY);
    env->set_ambient_light_energy(0.85f);
    env->set_tonemapper(godot::Environment::TONE_MAPPER_ACES);
    we->set_environment(env);
    sub_->add_child(we);

    add_light(sub_, godot::Vector3(-46.0f,  38.0f, 0.0f),
              godot::Color(1.00f, 0.88f, 0.70f), 2.30f, true);    // 主光（暖、带影）
    add_light(sub_, godot::Vector3(-24.0f, -66.0f, 0.0f),
              godot::Color(0.55f, 0.68f, 0.92f), 0.70f, false);   // 补光（冷）
    add_light(sub_, godot::Vector3(-12.0f, 186.0f, 0.0f),
              godot::Color(0.86f, 0.92f, 1.00f), 1.40f, false);   // 轮廓光（背后）

    pivot_ = memnew(godot::Node3D);
    sub_->add_child(pivot_);
    pivot_->add_child(model);

    model_ok_ = true;
    godot::UtilityFunctions::print(
        godot::String::utf8("[stage] 界面三维主视觉就绪：模型 menu_medal（最大维归一化到 1 m）"
                            " 自转 "),
        godot::String::num((double)spin_, 2), godot::String::utf8(" rad/s 固定偏航 "),
        godot::String::num((double)yaw_deg_, 1), godot::String::utf8("° 俯仰 "),
        godot::String::num((double)pitch_deg_, 1), godot::String::utf8("°（VA_STAGE=0 关）"));
}

// ============================================================ step
void MenuStage::step(double p_delta, int p_screen) {
    if (!enabled_ || cont_ == nullptr || sub_ == nullptr || pivot_ == nullptr) return;

    // 只在主菜单出现（0 = Hud::SCREEN_MENU）。其余屏一律藏掉 ——
    // "哪一屏该显示"只有这一处判断，免得以后加屏幕时漏掉一条 hide。
    const bool want = (p_screen == 0);
    cont_->set_visible(want);
    if (!want) return;

    /* 每帧重算位置与尺寸（不是只在 setup 里算一次）：
       窗口尺寸可变，而"重算"只有 4 次浮点运算 —— 比接一遍
       viewport size_changed 信号再维护一套缓存便宜，也不会漏。 */
    godot::Viewport *vp = cont_->get_viewport();
    godot::Vector2 sz(1920.0f, 1080.0f);
    if (vp != nullptr) sz = vp->get_visible_rect().size;
    // 与 hud.cpp:162 同一套缩放口径（s_ = clamp(视口高/1080, 0.62, 2.20)），
    // 否则窗口一变，HUD 的字在缩、舞台却不动，两者立刻对不上。
    const float s = clampv(sz.y / 1080.0f, 0.62f, 2.20f);
    const float side = sz.y * frac_;
    const float cx = sz.x - margin_ * s - side * 0.5f;
    const float cy = sz.y * 0.5f;
    size_frac_ok_ = (int)side;

    cont_->set_position(godot::Vector2(cx - side * 0.5f, cy - side * 0.5f));
    cont_->set_size(godot::Vector2(side, side));
    // SubViewport 的像素尺寸跟着容器走，否则渲染分辨率与显示尺寸脱节（会糊）。
    // stretch=true 时 Godot 内部会同步，这里显式给一次是为了让"渲染分辨率"
    // 这个诊断量在日志里可读。
    sub_->set_size(godot::Vector2i(std::max(2, (int)side), std::max(2, (int)side)));

    // 自转走**墙钟**：外壳期间战局 t 冻结，用 t 驱动的话菜单里的章一动不动。
    ang_ += (float)p_delta * spin_;
    if (ang_ > 2.0f * kPi * 1024.0f) ang_ -= 2.0f * kPi * 1024.0f;
    const float d2r = kPi / 180.0f;
    pivot_->set_rotation(godot::Vector3(pitch_deg_ * d2r, yaw_deg_ * d2r + ang_, 0.0f));

    ++frames_;
}

// ============================================================ dump
godot::String MenuStage::dump() const {
    godot::String s;
    s += godot::String::utf8("界面三维主视觉：");
    if (!model_ok_) return s + godot::String::utf8("未启用");
    s += godot::String::utf8("已挂载，显示帧 ");
    s += godot::String::num(frames_);
    s += godot::String::utf8(" 边长 ");
    s += godot::String::num(size_frac_ok_);
    s += godot::String::utf8("px 自转角 ");
    s += godot::String::num((double)(ang_ * 180.0f / kPi), 1);
    s += godot::String::utf8("°");
    return s;
}

} // namespace volunteer_army
