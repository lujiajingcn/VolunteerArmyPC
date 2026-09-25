// VolunteerArmyPC —— 射击光效层实现（见 fx_layer.h 的完整设计说明）
#include "node/fx_layer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/light3d.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "node/scene_builder.h"
#include "sim/va_world.h"

using namespace godot;

namespace volunteer_army {
namespace {

// ---------------------------------------------------------------- 旋钮
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

// ---------------------------------------------------------------- 对象池容量
constexpr int kSlots = 24;      // 发光广告牌（枪口焰 + 火花共用）
constexpr int kLights = 8;      // 点光源（只给枪口焰；火花太小不值一盏灯）

// ---------------------------------------------------------------- 尺寸 / 高度（米）
/* 高度照搬网页版（index.html:4324：flash 用 34、其他用 22 逻辑单位，×0.05 = 1.70 / 1.10 m）。
   尺寸**不能照搬** —— 网页版给的是 13 / 30 逻辑单位（0.65 / 1.50 m），那是俯视视角下的口径；
   第一人称下 3 米处的队友开火，一个 0.65 m 的光斑要占掉 12° 视角，读起来像爆炸而不是枪口火。
   实测（sweep/v_fx_look/cap_220s.png）确认了这个症状，所以按第一人称重新标：
   0.32 m 在 3 m 处约 6°、在 40 m 处约 8 像素（1084 px / 70° FOV）——
   近处是"枪口一亮"，远处是一个看得见的亮点。 */
constexpr float kMuzSize = 0.32f;
constexpr float kMuzSizeBig = 0.80f;
constexpr float kMuzH = 1.70f;
constexpr float kMuzHBig = 1.75f;
constexpr float kSparkSize = 0.22f;
constexpr float kSparkH = 1.10f;

// 曳光弹：长度 46 逻辑单位 = 2.30 m（网页版 index.html:4575 的 len）。
// 子弹 950 逻辑单位/秒 ⇒ 60fps 下每帧走 0.79 m，2.3 m 覆盖约 3 帧行程 —— 正好是拖尾该有的样子。
constexpr float kTracerLen = 2.30f;
constexpr float kTracerWidth = 0.045f;
constexpr float kTracerH = 1.25f;

// ---------------------------------------------------------------- 阵营配色
// 色相照搬网页版曳光弹（index.html:4583：友军冷青白 / 敌方暖橙），但**饱和度加重**。
//
// ⚠️ 为什么不能照抄数值 —— 这一条直接决定"分清敌我"这个需求成不成立：
//   网页版是 2D 画布直接描线，没有 HDR 这一层；这里的光效走
//   「加色混合 → HDR → ACES 色调映射」。原色乘上增益再进 ACES，
//   三通道一起被压向 1.0，于是 (0.745, 0.922, 1.000) 与 (1.0, 0.745, 0.588)
//   **映射出来都接近白色** —— 实测就是这样（近景那张曳光弹是纯白的一条，
//   而不是"冷青白"），敌我再接近也分不出来。
//   对策：**亮度由 gain 给，色相由这里给**。把色相推到更极端，
//   即使 R 通道被压到 1.0，G/B 的相对关系仍然拉得开。
const Color kAllyCol(0.30f, 0.78f, 1.000f);   // 冷青蓝（比网页版更蓝）
const Color kEnemyCol(1.000f, 0.38f, 0.10f);  // 暖橙红（比网页版更橙）
const Color kSparkCol(1.000f, 0.930f, 0.780f);  // 中性暖白（火花拿不到阵营，见 .h 说明）
const Color kNoneCol(0.900f, 0.900f, 0.900f);

Color team_color(va::Team p_t, int p_probe) {
    if (p_probe == 1) {
        // 染色探针：友军绿 / 敌军红 —— 一眼看清"颜色确实按 team 走"
        return (p_t == va::Team::Ally) ? Color(0.10f, 1.00f, 0.10f) : Color(1.00f, 0.10f, 0.10f);
    }
    switch (p_t) {
        case va::Team::Ally:  return kAllyCol;
        case va::Team::Enemy: return kEnemyCol;
        default:              return kNoneCol;
    }
}

/* 枪口焰的可见窗口（秒）—— **与逻辑层 push 那条 flash 的 life 一一对应**：
       spawn_bullet   va_combat.cpp:51   0.055
       spawn_rocket          :80         0.12
       throw_grenade         :97         0.08
       spawn_shell           :113        0.16
       spawn_tank_mg         :135        0.05
   子弹出生位置 = 枪口前方 8 逻辑单位，与 flash 的 x/y 逐位相同；
   而 p.t 就是"出膛后过了多久"。所以 p.t 落在窗口内 ⇔ 逻辑层此刻正有一条 flash 活着。
   这样既"忠于时长"，又**自带阵营**（fx 的 flash 没有 team，见 .h 的说明）。 */
float muzzle_window(va::ProjKind p_kind) {
    switch (p_kind) {
        case va::ProjKind::Bullet:  return 0.055f;
        case va::ProjKind::TankMG:  return 0.050f;
        case va::ProjKind::Rocket:  return 0.120f;
        case va::ProjKind::Shell:   return 0.160f;
        case va::ProjKind::Grenade: return 0.080f;
        case va::ProjKind::SmokeG:  return 0.080f;
        default: return 0.0f;
    }
}

bool is_big_flash(va::ProjKind p_kind) {
    return p_kind == va::ProjKind::Rocket || p_kind == va::ProjKind::Shell ||
           p_kind == va::ProjKind::TankMG;
}

// 曳光弹只画"子弹类"。炮弹/火箭/手雷在网页版里是另一套（发光弹体），这里不做。
bool is_tracer_kind(va::ProjKind p_kind) {
    return p_kind == va::ProjKind::Bullet || p_kind == va::ProjKind::TankMG;
}

/* 程序化径向渐变贴图（64²）。
   为什么要贴图而不是纯色四边形：加色混合的纯色方块会有硬边，看起来像一张纸片。
   径向衰减让它读成"一团光"。生成一次全局共享，零外部资源。 */
Ref<Texture2D> make_glow_tex() {
    const int N = 64;
    Ref<Image> img = Image::create(N, N, false, Image::FORMAT_RGBA8);
    const float c = (float)(N - 1) * 0.5f;
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const float dx = ((float)x - c) / c;
            const float dy = ((float)y - c) / c;
            const float r = std::sqrt(dx * dx + dy * dy);
            float a = 1.0f - r;
            a = (a <= 0.0f) ? 0.0f : std::pow(a, 1.5f);
            img->set_pixel(x, y, Color(1.0f, 1.0f, 1.0f, a));
        }
    }
    // Ref 的赋值有 operator=(const Ref<T>&)/(const Variant&) 两个候选（C2593），
    // 显式构造目标类型即可 —— 这个坑 unit_leg.cpp 踩过。
    return Ref<Texture2D>(ImageTexture::create_from_image(img).ptr());
}

} // namespace

// ============================================================ setup
void FxLayer::setup(Node3D *p_parent, Camera3D *p_cam) {
    enabled_ = env_flag("VA_FX", true);
    if (!enabled_) {
        UtilityFunctions::print(String::utf8(
            "[fx] VA_FX=0 —— 射击光效关闭（枪口焰 / 曳光弹 / 弹着火花全部不画，画面与未加本层时逐像素相同）"));
        return;
    }
    if (p_parent == nullptr) {
        enabled_ = false;
        return;
    }

    cam_ = p_cam;
    muz_on_ = env_flag("VA_FX_MUZ", true);
    tracer_on_ = env_flag("VA_FX_TRACER", true);
    spark_on_ = env_flag("VA_FX_SPARK", true);
    /* ⚠️ 增益**不能给大**。光效走加色混合，颜色本身已被推到 HDR；
       再乘 3.0 会让三个通道全部远超 1.0，经 ACES 色调映射后**统一压成纯白** ——
       实测扫图（sweep/v_fx_look/cap_220s.png）确认：友军冷青白变成了纯白，
       而"分清敌我"恰恰是这个需求的核心，等于被增益自己抹掉了。
       取 1.35：刚好越过场景 glow 的 HDR 门槛（1.05，见 scene_builder.cpp:2292）产生辉光，
       同时**保住色调** —— 敌方只剩 R 通道过阈（暖），友军只剩 B 通道过阈（冷）。 */
    gain_ = env_f("VA_FX_GAIN", 1.35f);
    near_cull_ = env_f("VA_FX_NEAR", 1.5f);
    // 最小角尺寸 —— 「远处看不见」这个缺陷的修复点，见 fx_layer.h 的 VA_FX_ANG 说明。
    // 默认 0.020 ≈ 1.15°：实测 0.010 时远处的光点只有 15 px，在缩略图上约 8 px，
    // 属于"画了但认不出"。需求是"分得清"，可读性优先。
    ang_muz_ = env_f("VA_FX_ANG", 0.020f);
    ang_tracer_w_ = env_f("VA_FX_TW", 0.0030f);
    ang_tracer_l_ = env_f("VA_FX_TL", 0.016f);
    tracer_gain_ = env_f("VA_FX_TGAIN", 1.15f);
    probe_ = (int)env_f("VA_FX_PROBE", 0.0f);
    dbg_ = env_flag("VA_DBG_FX", false);

    root_ = memnew(Node3D);
    root_->set_name("FxLayer");
    p_parent->add_child(root_);

    glow_tex_ = make_glow_tex();

    // ---- 发光广告牌池：每个槽一份独立材质（加色强度要逐光效调，共享材质做不到）----
    Ref<QuadMesh> quad;
    quad.instantiate();
    quad->set_size(Vector2(1.0f, 1.0f));   // 1 m 见方；实际尺寸靠 set_scale

    slots_.reserve(kSlots);
    for (int i = 0; i < kSlots; ++i) {
        Slot s;
        Ref<StandardMaterial3D> m;
        m.instantiate();
        m->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);  // 光效不参与光照
        m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
        m->set_blend_mode(BaseMaterial3D::BLEND_MODE_ADD);           // 加色 ⇒ 读成"光"
        m->set_billboard_mode(BaseMaterial3D::BILLBOARD_ENABLED);    // 任何角度看都是正面
        m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
        m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, glow_tex_);
        /* ⚠️ 必须关雾 —— 这一条和下面的 VA_FX_ANG 一样，是"需求能不能成立"级别的，
           不是画质微调。场景开了体积雾（scene_builder.cpp 的 volumetric fog），
           雾是**后处理**、对 unshaded 材质照样生效 ⇒ 不关的话枪口焰/曳光弹
           一进雾区就被散射吞掉：**近处（几米）清清楚楚、远处（几十米外）什么都不剩**，
           而"远处的敌人开枪"恰恰是本需求唯一要解决的东西。
           实测依据：同一次交火里近景可见清晰曳光弹、30 m 高空俯视图整个战场看不到任何光效。 */
        m->set_flag(BaseMaterial3D::FLAG_DISABLE_FOG, true);
        // ⚠️ godot-cpp 里这个方法叫 set_albedo，**不是** set_albedo_color
        // （Godot 的属性名是 albedo_color，但绑定层导出成 albedo）。
        m->set_albedo(Color(0, 0, 0, 0));
        s.mat = m;

        s.node = memnew(MeshInstance3D);
        s.node->set_mesh(quad);
        s.node->set_material_override(m);
        // 光效不投影：24 个加色面片进阴影贴图纯属浪费，而且会在地上压出黑斑。
        s.node->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
        s.node->set_visible(false);
        root_->add_child(s.node);
        slots_.push_back(s);
    }

    // ---- 点光源池：只给枪口焰。不投影、限距 —— 否则一次交火就会重编场景的光照 ----
    lights_.reserve(kLights);
    light_used_.reserve(kLights);
    for (int i = 0; i < kLights; ++i) {
        OmniLight3D *l = memnew(OmniLight3D);
        l->set_shadow(false);
        l->set_param(Light3D::PARAM_ENERGY, 0.0f);
        l->set_param(Light3D::PARAM_RANGE, 7.0f);
        l->set_visible(false);
        root_->add_child(l);
        lights_.push_back(l);
        light_used_.push_back(false);
    }

    // ---- 曳光弹：一个 ImmediateMesh 画全部弹道（任意多发只有 1 次 draw call）----
    tracer_mesh_.instantiate();
    tracer_mat_.instantiate();
    tracer_mat_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
    tracer_mat_->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
    /* ⚠️ 曳光弹用**普通 alpha 混合（MIX）**，不是加色（ADD）—— 这是"分清敌我"的最后一处硬伤。
       加色混合的物理含义是 dst + src：背景越亮，加上去的颜色就越快被推到 1.0，
       而 ACES 把三通道一起压向白色 ⇒ **在亮草地/亮天空前，冷青蓝与暖橙红都会变成白线**
       （实测就是如此：颜色常量改对了，画面上的曳光弹仍旧是一条白线）。
       把增益调到多低都没用 —— 只要背景是亮的，加色就必然偏白。
       所以曳光弹走 MIX：线是**实色**，背景亮度不参与它的颜色。
       代价是少一点"发光"的观感，但可读性优先于发光感 —— 需求原文是"分清是哪些敌人在射击"。
       （枪口焰保留加色：它本来就是"一团白光"，加色在那里是对的。） */
    tracer_mat_->set_blend_mode(BaseMaterial3D::BLEND_MODE_MIX);
    tracer_mat_->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
    // 顶点色带阵营色，albedo_color 只当"总增益 × 不透明度"用。
    tracer_mat_->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
    // 同上：曳光弹也不能被体积雾吃掉，否则"远处谁在开枪"还是看不见。
    tracer_mat_->set_flag(BaseMaterial3D::FLAG_DISABLE_FOG, true);
    // 注意这里用 tracer_gain_ 而**不是** gain_：见 .h 的说明。
    tracer_mat_->set_albedo(Color(tracer_gain_, tracer_gain_, tracer_gain_, 0.95f));

    tracer_ = memnew(MeshInstance3D);
    tracer_->set_mesh(tracer_mesh_);
    tracer_->set_material_override(tracer_mat_);
    tracer_->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
    root_->add_child(tracer_);

    attached_ = kSlots;
    UtilityFunctions::print(
        String::utf8("[fx] 射击光效就绪：广告牌 "), kSlots,
        String::utf8(" 槽 · 点光 "), kLights,
        String::utf8(" 盏 · HDR 增益 "), String::num((double)gain_, 2),
        String::utf8(" · 近距剔除 "), String::num((double)near_cull_, 2), String::utf8("m"),
        String::utf8("（枪口焰/曳光弹按阵营分色：友军冷青白 · 敌军暖橙）"));
    UtilityFunctions::print(
        String::utf8("[fx] 分项开关 枪口焰="), muz_on_, String::utf8(" 曳光弹="), tracer_on_,
        String::utf8(" 弹着火花="), spark_on_, String::utf8(" 探针="), probe_);
}

// ============================================================ 池分配
FxLayer::Slot *FxLayer::take_slot() {
    for (Slot &s : slots_) {
        if (!s.used) {
            s.used = true;
            return &s;
        }
    }
    return nullptr;   // 池满：宁可少画一个光效，也不动态扩容（扩容会让帧时间抖动）
}

OmniLight3D *FxLayer::take_light() {
    for (size_t i = 0; i < lights_.size(); ++i) {
        if (!light_used_[i]) {
            light_used_[i] = true;
            return lights_[i];
        }
    }
    return nullptr;
}

void FxLayer::emit(float p_x, float p_y, float p_h, float p_size, const Color &p_col,
                   float p_alpha, bool p_with_light) {
    const float gy = ground_h(p_x, p_y);
    const Vector3 wp = to3(p_x, p_y, gy + p_h);

    /* big 用**原始尺寸**判，不能用补偿之后的 —— 补偿会把 30 m 外的一发步枪枪口焰
       放大到 0.3 m 以上，那就误判成"大型火光"了（点光范围/能量各差一档）。 */
    const bool big = (p_size > 1.0f);
    float size = p_size;

    if (cam_ != nullptr) {
        const Vector3 cp = cam_->get_global_position();
        const float d2 = wp.distance_squared_to(cp);

        /* 近距剔除。玩家自己那发枪口焰生成在相机前 0.4 m，一个 0.65 m 的光斑会糊掉
           半个屏幕；而枪模上本来就有独立闪光（viewmodel 的 flash_group + flash_light），
           不缺这一份。半径之外的一概保留。 */
        if (d2 < near_cull_ * near_cull_) {
            ++near_culled_;
            return;
        }

        /* 最小角尺寸（屏幕空间下限）。光效是世界空间固定尺寸 ⇒"近处一大团、
           远处看不见"是必然的；而本需求要的恰恰是**远处的敌人开枪也能认出来**
           （实测：不补偿时 30 m 外的枪口焰只有 1~2 个像素，曳光弹宽 0.045 m
           在 50 m 外是亚像素 —— 画了等于没画）。按距离给角度下限，
           屏幕上占的像素数才不塌；近处 p_size 本身已够大，max 取不到这一支。 */
        size = std::max(p_size, std::sqrt(d2) * ang_muz_);

        // 诊断：这一发光效在相机正面还是背后（累计）。见 .h 的 cum_front_/cum_behind_。
        if (cam_->is_position_behind(wp)) {
            ++cum_behind_;
        } else {
            ++cum_front_;
        }
    }

    Slot *s = take_slot();
    if (s == nullptr) return;

    s->node->set_position(wp);
    s->node->set_scale(Vector3(size, size, size));
    s->mat->set_albedo(Color(p_col.r * gain_, p_col.g * gain_, p_col.b * gain_,
                             std::clamp(p_alpha, 0.0f, 1.0f)));
    s->node->set_visible(true);

    if (!p_with_light) return;
    OmniLight3D *l = take_light();
    if (l == nullptr) return;
    l->set_position(wp);
    l->set_color(p_col);
    l->set_param(Light3D::PARAM_RANGE, big ? 14.0f : 7.0f);
    // 能量随光斑大小走：大型火光（火箭/炮弹）比步枪枪口焰亮得多。
    l->set_param(Light3D::PARAM_ENERGY,
                 (big ? 5.0f : 2.2f) * (0.35f + 0.65f * p_alpha));
    l->set_visible(true);
}

// ============================================================ 曳光弹
void FxLayer::build_tracers(const va::WorldState &p_w) {
    tracer_mesh_->clear_surfaces();
    const Vector3 cam = (cam_ != nullptr) ? cam_->get_global_position() : Vector3();

    bool begun = false;
    for (const va::Projectile &p : p_w.projectiles) {
        if (!is_tracer_kind(p.kind)) continue;

        const float a = std::atan2(p.vy, p.vx);
        const float h = ground_h(p.x, p.y) + kTracerH;
        const Vector3 B = to3(p.x, p.y, h);   // 前端 = 子弹当前位置

        /* ⚠️ 单位口径（踩过，静默错）：`kTracerLen` 是**米**，而 `p.x / p.y` 是
           **逻辑坐标** —— 1 逻辑单位 = S = 0.05 m（scene_builder.h:27）。
           第一版把"2.3 m"直接减在逻辑坐标上，于是曳光弹实际只有 2.3 逻辑单位
           = **0.115 m** 长，短了一个数量级 —— 这正是"画面上看不到曳光弹"的直接原因，
           而它在日志/计数里完全正常（`[fx] 曳光弹 24`），只有看画面才发现。
           网页版是纯 2D 画布、逻辑坐标即像素，没有这一层换算，照抄会踩。
           与 audio 层"SND 的 ref/max 是逻辑单位、必须 ×S"是同一类坑。 */
        const float d_head = (cam_ != nullptr) ? std::sqrt(B.distance_squared_to(cam)) : 0.0f;
        const float len_m = std::max(kTracerLen, d_head * ang_tracer_l_);
        const float len_lu = len_m / S;       // 米 → 逻辑单位

        const float x1 = p.x - std::cos(a) * len_lu;
        const float y1 = p.y - std::sin(a) * len_lu;

        // 两端共用同一个地面高度：数米跨度内地形起伏可忽略，省一次 ground_h。
        const Vector3 A = to3(x1, y1, h);
        Vector3 d = B - A;
        if (d.length_squared() < 1e-8f) continue;
        d = d.normalized();

        /* 手搓面向相机的条带。Godot 的 PRIMITIVE_LINES 线宽固定 1 像素，
           几米外的曳光细到看不见，所以自己算宽度：
           侧向量 = 线段方向 × 指向相机的向量（两者都归一化后叉乘，长度 = sin 夹角）。 */
        const Vector3 mid = (A + B) * 0.5f;
        const Vector3 to_cam = cam - mid;
        if (to_cam.length_squared() < 1e-6f) continue;
        Vector3 side = d.cross(to_cam.normalized());
        if (side.length_squared() < 1e-8f) continue;   // 正对着看：投影不出宽度，跳过
        /* 宽度也给角下限：0.045 m 的条带在 50 m 外是亚像素宽，等于没画。 */
        const float dist_mid = std::sqrt(to_cam.length_squared());
        const float w_m = std::max(kTracerWidth, dist_mid * ang_tracer_w_);
        side = side.normalized() * (w_m * 0.5f);

        if (!begun) {
            const Ref<Material> tmat(tracer_mat_.ptr());
            tracer_mesh_->surface_begin(Mesh::PRIMITIVE_TRIANGLES, tmat);
            begun = true;
        }
        const Color c = team_color(p.team, probe_);
        const Vector3 v0 = A - side, v1 = A + side, v2 = B + side, v3 = B - side;
        tracer_mesh_->surface_set_color(c); tracer_mesh_->surface_add_vertex(v0);
        tracer_mesh_->surface_set_color(c); tracer_mesh_->surface_add_vertex(v1);
        tracer_mesh_->surface_set_color(c); tracer_mesh_->surface_add_vertex(v2);
        tracer_mesh_->surface_set_color(c); tracer_mesh_->surface_add_vertex(v0);
        tracer_mesh_->surface_set_color(c); tracer_mesh_->surface_add_vertex(v2);
        tracer_mesh_->surface_set_color(c); tracer_mesh_->surface_add_vertex(v3);

        ++last_tracer_;
    }
    if (begun) tracer_mesh_->surface_end();
}

// ============================================================ 每帧
void FxLayer::step(const va::WorldState &p_w, float p_dt, float p_logical_dt) {
    if (!enabled_) return;

    for (Slot &s : slots_) s.used = false;
    for (size_t i = 0; i < light_used_.size(); ++i) light_used_[i] = false;
    last_muz_ = last_spark_ = last_tracer_ = 0;
    last_muz_ally_ = last_muz_enemy_ = 0;
    near_culled_ = 0;

    // 本帧推进的逻辑秒数。所有"瞬时事件"的可见性判据都基于它 —— 见下面两段。
    const float dlog = std::max(p_logical_dt, 0.0f);

    // ---- 曳光弹：全部子弹类，1 次 draw call ----
    if (tracer_on_) {
        build_tracers(p_w);
    } else {
        tracer_mesh_->clear_surfaces();
    }

    /* ---- 枪口焰：从**弹丸**反推，不读 W.fx 的 flash（那个没有 team，见 .h）----
       判据不是「当前时刻落在窗口内」，而是「**本帧的时间区间**与窗口有交集」：
       子弹在这一帧里走过的逻辑时段是 [p.t − dlog, p.t]（p.t 刚在逻辑步进里累加过）。
       步枪枪口焰只有 0.055 s，而掉帧或 VA_FF 快进时一帧能推进 0.1~0.13 s ——
       用"当前时刻"去判会**整帧跳过**闪光，画面上表现成"敌人开枪完全没有光"，
       而这恰恰是本需求要解决的东西。
       改成区间相交后：60fps 下自然覆盖 4 帧（略长于逻辑层的 3.3 帧，换来不漏），
       快进时退化为"每帧画一次"（跳帧但不断）。
       k 用**帧中点**算，跨帧那种情况下才不会一上来就是"最老最淡"。 */
    if (muz_on_) {
        for (const va::Projectile &p : p_w.projectiles) {
            const float win = muzzle_window(p.kind);
            if (win <= 0.0f) continue;

            /* 玩家自己那发不画：枪模上本来就有独立闪光（viewmodel.cpp 的 flash_group
               + flash_light），再叠一层世界光效只会糊在屏幕正中。
               用 owner->isPlayer 判而**不是**只靠距离 —— 距离剔除依赖相机位置，
               也拦不住"身边的队友"；这一条是精确的，与距离剔除互补、不互替。 */
            if (p.owner != nullptr && p.owner->isPlayer) continue;

            const float t0 = std::max(0.0f, p.t - dlog);
            if (t0 > win) continue;   // 本帧区间完全在窗口之后 ⇒ 这一发已经闪过了

            const float t_mid = (t0 + p.t) * 0.5f;
            const float k = std::clamp(1.0f - t_mid / win, 0.0f, 1.0f);   // 1 = 刚出膛
            const bool big = is_big_flash(p.kind);
            const float base = big ? kMuzSizeBig : kMuzSize;
            // 与网页版同式：越老越大越淡（分辨消散）。
            const float size = base * (1.6f - k * 0.6f);
            const float alpha = 0.55f + k * 0.42f;

            emit(p.x, p.y, big ? kMuzHBig : kMuzH, size, team_color(p.team, probe_), alpha, true);
            ++last_muz_;
            if (p.team == va::Team::Ally) ++last_muz_ally_; else ++last_muz_enemy_;
        }
    }

    /* ---- 弹着火花：读 W.fx（**中性色** —— 命中那一帧子弹已被 erase，拿不到阵营）----
       判据同枪口焰：区间相交，不是"当前时刻"。火花寿命 0.16~0.18 s，比枪口焰宽裕得多，
       但快进到 ff=6 时一帧就是 0.1 s，同样会漏。 */
    if (spark_on_) {
        for (const va::FxItem &f : p_w.fx) {
            if (f.type != "spark") continue;
            const float life = std::max(f.life, 1e-4f);
            const float t0 = std::max(0.0f, f.t - dlog);
            if (t0 > life) continue;

            const float t_mid = (t0 + f.t) * 0.5f;
            const float k = std::clamp(1.0f - t_mid / life, 0.0f, 1.0f);
            const float base = (f.b != 0.0f) ? kSparkSize * 1.6f : kSparkSize;
            const float size = base * (1.6f - k * 0.6f);
            const Color sc = (probe_ == 1) ? Color(0.25f, 0.45f, 1.00f) : kSparkCol;
            emit(f.x, f.y, kSparkH, size, sc, 0.55f + k * 0.42f, false);
            ++last_spark_;
        }
    }

    hide_rest();

    peak_muz_ = std::max(peak_muz_, last_muz_);
    peak_tracer_ = std::max(peak_tracer_, last_tracer_);

    if (dbg_) {
        dbg_t_ += p_dt;
        if (dbg_t_ >= 0.5f) {
            dbg_t_ = 0.0f;
            UtilityFunctions::print(dump());
        }
    }
}

void FxLayer::hide_rest() {
    for (Slot &s : slots_) s.node->set_visible(s.used);
    for (size_t i = 0; i < lights_.size(); ++i) lights_[i]->set_visible(light_used_[i]);
}

void FxLayer::reset() {
    if (!enabled_) return;
    for (Slot &s : slots_) {
        s.used = false;
        s.node->set_visible(false);
    }
    for (OmniLight3D *l : lights_) l->set_visible(false);
    if (tracer_mesh_.is_valid()) tracer_mesh_->clear_surfaces();
    peak_muz_ = peak_tracer_ = 0;
}

// ============================================================ 诊断
String FxLayer::dump() const {
    String s = String::utf8("[fx] ");
    if (!enabled_) return s + String::utf8("VA_FX=0 关闭");
    s += String::utf8("枪口焰 ") + String::num((double)last_muz_) +
         String::utf8("（友 ") + String::num((double)last_muz_ally_) +
         String::utf8(" / 敌 ") + String::num((double)last_muz_enemy_) + String::utf8("）") +
         String::utf8(" 曳光弹 ") + String::num((double)last_tracer_) +
         String::utf8(" 火花 ") + String::num((double)last_spark_) +
         String::utf8(" | 峰值 枪口焰 ") + String::num((double)peak_muz_) +
         String::utf8(" 曳光弹 ") + String::num((double)peak_tracer_) +
         String::utf8(" · 近距剔除 ") + String::num((double)near_culled_) +
         String::utf8(" · 槽位 ") + String::num((double)attached_) +
         String::utf8(" · 增益 ") + String::num((double)gain_, 2) +
         // 诊断：光效在相机正面/背后的**累计**比。正面远多于背后 ⇒
         // "画面上没有光"就不是朝向问题，得往尺寸/雾/颜色上找。
         String::utf8(" · 正面 ") + String::num((double)cum_front_) +
         String::utf8("/背后 ") + String::num((double)cum_behind_) +
         String::utf8(" · 角尺寸 ") + String::num((double)ang_muz_, 4);
    return s;
}

} // namespace volunteer_army
