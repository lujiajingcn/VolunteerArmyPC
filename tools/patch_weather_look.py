# -*- coding: utf-8 -*-
"""
VolunteerArmyPC —— 环境/光照改为「天气驱动」

背景（这是个真问题，不是风格偏好）：
  logic 层的 W.weather（sunny / rain / night）本来就参与玩法 ——
    va_world.cpp:259  能见度系数  rain ×0.82 / night ×0.62
    va_combat.cpp:321 伤害系数    rain ×1.12 / night ×1.20
  也就是说「夜战」在逻辑上是一种更难打的条件。
  如果画面永远是大白天，玩家根本读不出自己正处在什么天气条件下，
  玩法信息就丢了。所以布光必须跟着 W.weather 走。

同时把 build_environment 拆成：
    build_environment()  —— 只建节点（WorldEnvironment / Sky / 3 盏灯）
    apply_weather()      —— 把所有随天气变的参数一次性打完
  这样重开一局（天气可能重新抽）时，调 apply_weather() 即可，不必重建节点。
"""
import io
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "node", "scene_builder.cpp")

lines = io.open(SRC, encoding="utf-8").read().split("\n")

# --- 定位并删除旧的「环境 / 光照」整段（从段头注释到 build_environment 的收尾大括号） ---
i0 = next(i for i, l in enumerate(lines) if l.startswith("// ------") and "环境 / 光照" in l)
i1 = next(i for i in range(i0, len(lines)) if lines[i].startswith("static void build_environment"))
i2 = next(i for i in range(i1, len(lines)) if lines[i] == "}")
print("替换 第 %d ~ %d 行（%d 行）" % (i0 + 1, i2 + 1, i2 - i0 + 1))

NEW = r'''// ------------------------------------------------------- 环境 / 光照
// 使命召唤式的战场氛围：冷调阴影 + 暖调阳光 + 强烈空气透视 + 体积光。
//
// 【为什么必须三点布光】
// 只有一盏 DirectionalLight 时，背光面除了 ambient 之外收不到任何方向性光照，
// 只要法线一转过去就塌成纯黑剪影（实测截图里树干、树冠背面全是死黑）。
// 真实世界的背光面靠天空漫射 + 地面反弹照亮，所以要显式补两盏无阴影光源：
//     SUN    主光，暖白，投影，定调
//     FILL   冷蓝，无影，从主光反方向斜射，把暗部结构"读"出来
//     BOUNCE 暖土色，无影，自下而上，模拟大地反弹，让模型从背景里"立起来"
//
// 【为什么分三套配方】
// logic 层的 W.weather 本来就参与玩法（能见度、伤害、雾），见文件头的说明。
// 画面必须跟着变，否则"夜战"在屏幕上是白天，玩法信息就丢了。

struct WeatherLook {
    const char *name;
    Vector3 sun_rot_deg;   // 太阳欧拉角
    Color   sun_color;
    float   sun_energy;
    Color   ambient_color;
    float   ambient_energy;
    Color   fill_color;
    float   fill_energy;
    float   bounce_energy;
    Color   sky_top;
    Color   sky_horizon;
    Color   sky_ground;
    float   sky_energy;
    Color   fog_color;
    float   fog_density;
    float   fog_height_density;
    float   fog_sun_scatter;
    float   vol_density;
    float   vol_anisotropy;
    float   exposure;
    float   saturation;
    float   contrast;
    float   glow_intensity;
};

// 数值来源：网页版 FOG 表（index.html:4066）与天空 zenith 表（index.html:4226），
// 换算到 Godot 的线性 HDR 空间后再按色调映射特性做反向补偿。
static const WeatherLook LOOKS[] = {
    // ---------------- 晴天：黄昏偏暖，能见度高，对比强 ----------------
    { "sunny",
      Vector3(-48.0f, 122.0f, 0.0f), Color(1.000f, 0.935f, 0.840f), 2.05f,
      Color(0.44f, 0.52f, 0.66f), 0.60f,
      Color(0.60f, 0.73f, 0.94f), 0.20f, 0.11f,
      Color(0.120f, 0.190f, 0.365f), Color(0.440f, 0.420f, 0.380f), Color(0.300f, 0.280f, 0.235f), 1.00f,
      Color(0.620f, 0.680f, 0.700f), 0.0032f, 0.055f, 0.18f,
      0.010f, 0.32f,
      1.05f, 1.12f, 1.07f, 0.45f },

    // ---------------- 雨天：低压冷灰，对比低、空气浑 ----------------
    { "rain",
      Vector3(-64.0f, 140.0f, 0.0f), Color(0.855f, 0.890f, 0.945f), 0.80f,
      Color(0.50f, 0.55f, 0.60f), 0.46f,
      Color(0.72f, 0.78f, 0.86f), 0.16f, 0.08f,
      Color(0.110f, 0.130f, 0.150f), Color(0.355f, 0.380f, 0.400f), Color(0.230f, 0.240f, 0.240f), 0.92f,
      Color(0.440f, 0.480f, 0.500f), 0.0092f, 0.120f, 0.05f,
      0.022f, 0.10f,
      1.40f, 0.86f, 0.94f, 0.28f },

    // ---------------- 夜战：月光 + 深蓝，能见度最差 ----------------
    { "night",
      Vector3(-30.0f, 108.0f, 0.0f), Color(0.560f, 0.680f, 0.960f), 0.30f,
      Color(0.150f, 0.215f, 0.340f), 0.22f,
      Color(0.42f, 0.55f, 0.85f), 0.09f, 0.05f,
      Color(0.018f, 0.032f, 0.072f), Color(0.048f, 0.062f, 0.100f), Color(0.030f, 0.038f, 0.055f), 0.80f,
      Color(0.055f, 0.075f, 0.115f), 0.0030f, 0.040f, 0.10f,
      0.008f, 0.25f,
      1.35f, 0.92f, 1.05f, 0.60f },
};

static const int LOOK_COUNT = (int)(sizeof(LOOKS) / sizeof(LOOKS[0]));

static const WeatherLook *look_of(const std::string &w) {
    for (int i = 0; i < LOOK_COUNT; ++i) {
        if (w == LOOKS[i].name) return &LOOKS[i];
    }
    return &LOOKS[0];   // 兜底：晴天
}

// 建节点（只建一次）。参数由 apply_weather() 打，见下面。
static void build_environment(Node3D *root, SceneRefs &out) {
    WorldEnvironment *we = memnew(WorldEnvironment);
    Ref<Environment> env;
    env.instantiate();

    Ref<ProceduralSkyMaterial> sky_mat;
    sky_mat.instantiate();
    sky_mat->set_sky_curve(0.22f);
    sky_mat->set_ground_curve(0.06f);
    sky_mat->set_sun_angle_max(10.0f);
    sky_mat->set_sun_curve(0.06f);
    sky_mat->set_use_debanding(true);
    out.sky_mat = sky_mat;

    Ref<Sky> sky;
    sky.instantiate();
    sky->set_material(sky_mat);
    sky->set_radiance_size(Sky::RADIANCE_SIZE_256);
    sky->set_process_mode(Sky::PROCESS_MODE_REALTIME);

    env->set_background(Environment::BG_SKY);
    env->set_sky(sky);
    env->set_reflection_source(Environment::REFLECTION_SOURCE_SKY);

    // 环境光：混合「天空辐射」与「手调冷色」。
    // 纯 SKY(1.0) 会让暗部跟着天空发灰、丢掉金属冷感；所以留一部分手动色。
    env->set_ambient_source(Environment::AMBIENT_SOURCE_SKY);
    env->set_ambient_light_sky_contribution(0.72f);

    // 雾：指数雾打底做纵深，再叠高度雾做"谷地晨霭"
    env->set_fog_enabled(true);
    env->set_fog_mode(Environment::FOG_MODE_EXPONENTIAL);
    env->set_fog_light_energy(0.90f);
    env->set_fog_aerial_perspective(0.50f);
    env->set_fog_sky_affect(0.30f);
    env->set_fog_height(7.0f);

    // 体积雾：空气里的丁达尔光柱，是"精致"最便宜的一剂。
    // 只铺 64m 深、细节扩散调粗，集显也能跑；卡的话把 enabled 关掉即可，
    // 指数雾 + 空气透视已经能撑住画面。
    env->set_volumetric_fog_enabled(true);
    env->set_volumetric_fog_albedo(Color(0.70f, 0.73f, 0.78f));
    env->set_volumetric_fog_emission(Color(0.0f, 0.0f, 0.0f));
    env->set_volumetric_fog_emission_energy(0.0f);
    env->set_volumetric_fog_length(64.0f);
    env->set_volumetric_fog_detail_spread(2.0f);
    env->set_volumetric_fog_ambient_inject(1.0f);
    env->set_volumetric_fog_sky_affect(0.0f);
    env->set_volumetric_fog_temporal_reprojection_enabled(true);
    env->set_volumetric_fog_temporal_reprojection_amount(0.90f);

    // 色调映射用 AgX：高光滚降平滑、不偏色。
    // （第一版用 ACES + 亮色雾，远景直接爆成一片死白，这是换 AgX 的直接原因。）
    env->set_tonemapper(Environment::TONE_MAPPER_AGX);
    env->set_tonemap_white(4.0f);

    env->set_glow_enabled(true);
    env->set_glow_normalized(true);
    env->set_glow_strength(0.92f);
    env->set_glow_bloom(0.06f);
    env->set_glow_blend_mode(Environment::GLOW_BLEND_MODE_SOFTLIGHT);
    env->set_glow_hdr_bleed_threshold(1.05f);
    env->set_glow_hdr_bleed_scale(1.8f);
    env->set_glow_hdr_luminance_cap(6.0f);
    env->set_glow_level(0, 0.0f);
    env->set_glow_level(1, 0.35f);
    env->set_glow_level(2, 0.55f);
    env->set_glow_level(3, 0.65f);
    env->set_glow_level(4, 0.45f);
    env->set_glow_level(5, 0.20f);
    env->set_glow_level(6, 0.0f);

    // SSAO 只做缝隙接触阴影，不拿它当"全局压暗"用。
    // （第一版 intensity=1.6，等于在背光死黑上又补一刀；光源补齐后必须收回来。）
    env->set_ssao_enabled(true);
    env->set_ssao_radius(1.1f);
    env->set_ssao_power(1.45f);
    env->set_ssao_detail(0.35f);
    env->set_ssao_horizon(0.06f);
    env->set_ssao_sharpness(0.98f);
    env->set_ssao_direct_light_affect(0.12f);   // 只在背光面生效，受光面几乎不动
    env->set_ssao_ao_channel_affect(0.0f);

    // 屏幕空间间接光：把周围环境的颜色"渗"进暗部，进一步消死黑。
    // 权重压得很低，只补一口气，不做主光。
    env->set_ssil_enabled(true);
    env->set_ssil_radius(1.6f);
    env->set_ssil_intensity(0.35f);
    env->set_ssil_sharpness(0.94f);
    env->set_ssil_normal_rejection(1.0f);

    env->set_adjustment_enabled(true);
    env->set_adjustment_brightness(1.0f);
    env->set_adjustment_color_correction(make_grade_lut());

    we->set_environment(env);
    root->add_child(we);
    out.world_env = we;

    DirectionalLight3D *sun = memnew(DirectionalLight3D);
    sun->set_name("Sun");
    sun->set_param(Light3D::PARAM_INDIRECT_ENERGY, 0.9f);
    sun->set_param(Light3D::PARAM_VOLUMETRIC_FOG_ENERGY, 1.35f);
    sun->set_param(Light3D::PARAM_SPECULAR, 1.0f);
    sun->set_sky_mode(DirectionalLight3D::SKY_MODE_LIGHT_AND_SKY);
    sun->set_shadow(true);
    sun->set_shadow_mode(DirectionalLight3D::SHADOW_PARALLEL_4_SPLITS);
    sun->set_blend_splits(true);
    sun->set_param(Light3D::PARAM_SHADOW_MAX_DISTANCE, 120.0f);
    sun->set_param(Light3D::PARAM_SHADOW_SPLIT_1_OFFSET, 0.06f);
    sun->set_param(Light3D::PARAM_SHADOW_SPLIT_2_OFFSET, 0.18f);
    sun->set_param(Light3D::PARAM_SHADOW_SPLIT_3_OFFSET, 0.46f);
    sun->set_param(Light3D::PARAM_SHADOW_FADE_START, 0.85f);
    sun->set_param(Light3D::PARAM_SHADOW_BIAS, 0.030f);
    sun->set_param(Light3D::PARAM_SHADOW_NORMAL_BIAS, 1.8f);
    sun->set_param(Light3D::PARAM_SHADOW_BLUR, 1.15f);
    sun->set_param(Light3D::PARAM_SHADOW_PANCAKE_SIZE, 32.0f);
    root->add_child(sun);
    out.sun = sun;

    // 补光 / 弹光：SKY_MODE_LIGHT_ONLY 是关键 ——
    // 否则 Godot 会把它们也当成"天上的太阳"画进天空，出现三个太阳。
    DirectionalLight3D *fill = memnew(DirectionalLight3D);
    fill->set_name("Fill");
    fill->set_rotation_degrees(Vector3(-26.0f, -58.0f, 0.0f));
    fill->set_param(Light3D::PARAM_INDIRECT_ENERGY, 0.0f);
    fill->set_param(Light3D::PARAM_SPECULAR, 0.35f);
    fill->set_sky_mode(DirectionalLight3D::SKY_MODE_LIGHT_ONLY);
    fill->set_shadow(false);
    root->add_child(fill);
    out.fill = fill;

    DirectionalLight3D *bounce = memnew(DirectionalLight3D);
    bounce->set_name("Bounce");
    bounce->set_rotation_degrees(Vector3(36.0f, 46.0f, 0.0f));
    bounce->set_param(Light3D::PARAM_INDIRECT_ENERGY, 0.0f);
    bounce->set_param(Light3D::PARAM_SPECULAR, 0.15f);
    bounce->set_sky_mode(DirectionalLight3D::SKY_MODE_LIGHT_ONLY);
    bounce->set_shadow(false);
    root->add_child(bounce);
    out.bounce = bounce;
}

// 把「当前天气」对应的那一整套参数打上去。
// 重开一局天气可能重新抽到别的值，重建后调一次即可。
void apply_weather(SceneRefs &refs) {
    if (refs.world_env == nullptr || refs.sun == nullptr) return;
    const WeatherLook *L = look_of(va::W.weather);

    // 旋钮默认 1.0 = 完全按配方走；非 1.0 才是开发期扫描用（见 knob() 说明）
    const float k_sun = knob("VA_SUN", 1.0f);
    const float k_fill = knob("VA_FILL", 1.0f);
    const float k_bounce = knob("VA_BOUNCE", 1.0f);
    const float k_amb = knob("VA_AMBIENT", 1.0f);
    const float k_exp = knob("VA_EXPOSURE", 1.0f);
    const float k_fog = knob("VA_FOG", 1.0f);

    const Ref<Environment> env = refs.world_env->get_environment();
    if (env.is_valid()) {
        env->set_ambient_light_color(L->ambient_color);
        env->set_ambient_light_energy(L->ambient_energy * k_amb);

        env->set_fog_light_color(L->fog_color);
        env->set_fog_density(L->fog_density * k_fog);
        env->set_fog_height_density(L->fog_height_density);
        env->set_fog_sun_scatter(L->fog_sun_scatter);

        env->set_volumetric_fog_density(L->vol_density * k_fog);
        env->set_volumetric_fog_anisotropy(L->vol_anisotropy);

        env->set_tonemap_exposure(L->exposure * k_exp);

        env->set_glow_intensity(L->glow_intensity);
        env->set_ssao_intensity(0.85f);
        env->set_adjustment_contrast(L->contrast);
        env->set_adjustment_saturation(L->saturation);
    }

    if (refs.sky_mat.is_valid()) {
        ProceduralSkyMaterial *sm = Object::cast_to<ProceduralSkyMaterial>(refs.sky_mat.ptr());
        if (sm != nullptr) {
            sm->set_sky_top_color(L->sky_top);
            sm->set_sky_horizon_color(L->sky_horizon);
            sm->set_ground_horizon_color(L->sky_ground);
            sm->set_ground_bottom_color(L->sky_ground.darkened(0.55f));
            sm->set_sky_energy_multiplier(L->sky_energy);
            sm->set_ground_energy_multiplier(L->sky_energy * 0.85f);
        }
    }

    refs.sun->set_rotation_degrees(L->sun_rot_deg);
    refs.sun->set_color(L->sun_color);
    refs.sun->set_param(Light3D::PARAM_ENERGY, L->sun_energy * k_sun);

    if (refs.fill != nullptr) {
        refs.fill->set_color(L->fill_color);
        refs.fill->set_param(Light3D::PARAM_ENERGY, L->fill_energy * k_fill);
    }
    if (refs.bounce != nullptr) {
        refs.bounce->set_param(Light3D::PARAM_ENERGY, L->bounce_energy * k_bounce);
    }
}
'''

lines[i0:i2 + 1] = NEW.split("\n")
io.open(SRC, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
print("环境模块已替换为天气驱动版")

# ---------------- 头文件：SceneRefs 增两个字段 + 声明 apply_weather ----------------
H = os.path.join(ROOT, "src", "node", "scene_builder.h")
h = io.open(H, encoding="utf-8").read()
h = h.replace(
    "    godot::DirectionalLight3D *bounce = nullptr;\n};",
    "    godot::DirectionalLight3D *bounce = nullptr;\n"
    "    godot::Ref<godot::Material> sky_mat;   // ProceduralSkyMaterial，随天气换色\n};",
    1)
h = h.replace(
    "void rebuild_props(godot::Node3D *root, SceneRefs &refs);",
    "void rebuild_props(godot::Node3D *root, SceneRefs &refs);\n\n"
    "// 按 logic 层当前的 va::W.weather 重新打一整套布光/雾/天空参数。\n"
    "// 天气在 init_world() 里随机抽（重玩性），所以重开一局必须重调一次。\n"
    "void apply_weather(SceneRefs &refs);",
    1)
io.open(H, "w", encoding="utf-8", newline="\n").write(h)
print("scene_builder.h 已更新")
