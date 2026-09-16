# -*- coding: utf-8 -*-
"""
VolunteerArmyPC —— 光照/大气重做补丁

把 scene_builder.cpp 里原来「单盏平行光 + 纯天空环境光」的简陋布光，
换成使命召唤式的黄昏战场布光：

  主光（暖白，带阴影） + 冷补光（无阴影） + 地面反弹光（暖，无阴影）
  + 程序化 teal-orange 三级色调 LUT + 高度雾 + 体积雾

死黑的根因：只有一盏 DirectionalLight 时，背光面除了 ambient 收不到任何光，
射线一偏就塌成纯黑剪影。补光/反弹光就是专治这个的。
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "node", "scene_builder.cpp")

with io.open(SRC, "r", encoding="utf-8") as f:
    text = f.read()

orig_len = len(text)

# ------------------------------------------------------------------ 1. 补 include
INC_ANCHOR = '#include <godot_cpp/classes/image_texture.hpp>\n'
INC_NEW = (
    INC_ANCHOR
    + '#include <godot_cpp/classes/image_texture3d.hpp>\n'
    + '#include <godot_cpp/classes/texture3d.hpp>\n'
    + '#include <godot_cpp/variant/typed_array.hpp>\n'
)
if '#include <godot_cpp/classes/image_texture3d.hpp>' not in text:
    assert INC_ANCHOR in text, "找不到 image_texture.hpp include 锚点"
    text = text.replace(INC_ANCHOR, INC_NEW, 1)
    print("[1] 已补 include: image_texture3d / texture3d / typed_array")
else:
    print("[1] include 已存在，跳过")

# ------------------------------------------------------------------ 2. 插入 LUT 生成函数
LUT_FN = r'''// ------------------------------------------------------- 色调分级 LUT
// 使命召唤式的 teal-orange 分级：暗部压向青蓝、亮部推向橙黄，中段走一条轻微 S 曲线。
// 用 16³ 的 3D LUT（程序化生成，零外部资源）喂给 Environment 的 color_correction，
// 好处是"冷影暖光"不再依赖一串调参直觉，而是一张可复现、可微调的色彩映射表。
//   - 索引顺序：Image(r,g) 逐层叠成 depth=b，与 Godot 的 3D LUT 约定一致
//   - 用 RGB8 而非 FLOAT：LUT 只做色调搬运，不需要 HDR 精度
static Ref<Texture3D> make_grade_lut() {
    const int N = 16;
    TypedArray<Ref<Image>> layers;
    for (int b = 0; b < N; ++b) {
        Ref<Image> img = Image::create_empty(N, N, false, Image::FORMAT_RGB8);
        for (int g = 0; g < N; ++g) {
            for (int r = 0; r < N; ++r) {
                float rr = (float)r / (float)(N - 1);
                float gg = (float)g / (float)(N - 1);
                float bb = (float)b / (float)(N - 1);
                const float lum = 0.2126f * rr + 0.7152f * gg + 0.0722f * bb;

                // 1) S 曲线：暗部再压、亮部再提，拉出电影感对比（smoothstep 做软拐点，不出死黑硬边）
                const float sc = lum * lum * (3.0f - 2.0f * lum);
                const float d = (sc - lum) * 0.55f;
                rr += d; gg += d; bb += d;

                // 2) 冷影暖光分离：这是"使命召唤味"的主要来源
                const float shadow = 1.0f - va::clampf(lum * 2.4f, 0.0f, 1.0f);
                const float high = va::clampf((lum - 0.58f) * 2.4f, 0.0f, 1.0f);
                rr += -0.022f * shadow + 0.050f * high;
                gg +=  0.006f * shadow + 0.018f * high;
                bb +=  0.042f * shadow - 0.038f * high;

                // 3) 极暗处抽掉一点饱和：避免暗部变成"脏黑"糊成一团
                const float lift = va::clampf(0.15f - lum, 0.0f, 1.0f) * 0.30f;
                rr = va::lerpf(rr, lum, lift);
                gg = va::lerpf(gg, lum, lift);
                bb = va::lerpf(bb, lum, lift);

                img->set_pixel(r, g, Color(va::clampf(rr, 0.0f, 1.0f),
                                           va::clampf(gg, 0.0f, 1.0f),
                                           va::clampf(bb, 0.0f, 1.0f)));
            }
        }
        layers.push_back(img);
    }
    Ref<ImageTexture3D> tex;
    tex.instantiate();
    tex->create(Image::FORMAT_RGB8, N, N, N, false, layers);
    return tex;
}

// ------------------------------------------------------- 环境 / 光照
// 使命召唤式的黄昏战场：冷调阴影 + 暖调阳光 + 强烈空气透视 + 体积光。
//
// 【为什么必须三点布光】
// 只有一盏 DirectionalLight 时，背光面除了 ambient 之外收不到任何方向性光照，
// 只要法线一转过去就塌成纯黑剪影（实测截图里树干、树冠背面全是死黑）。
// 真实世界里背光面是靠天空漫射 + 地面反弹照亮的，所以要显式补两盏无阴影光源：
//   主光 SUN   —— 暖白，投影，定调
//   补光 FILL  —— 冷蓝，无影，从上风向斜射，把暗部结构"读"出来
//   弹光 BOUNCE—— 暖土色，无影，自下而上，模拟大地反弹
static void build_environment(Node3D *root, SceneRefs &out) {
    WorldEnvironment *we = memnew(WorldEnvironment);
    Ref<Environment> env;
    env.instantiate();

    // ---- 天空：黄昏。地平线刻意不用白，改成暖灰，否则远景会糊成一片惨白 ----
    Ref<ProceduralSkyMaterial> sky_mat;
    sky_mat.instantiate();
    sky_mat->set_sky_top_color(Color(0.085f, 0.145f, 0.285f));     // 天顶深蓝
    sky_mat->set_sky_horizon_color(Color(0.545f, 0.520f, 0.470f)); // 地平线暖灰
    sky_mat->set_sky_curve(0.22f);
    sky_mat->set_sky_energy_multiplier(0.85f);
    sky_mat->set_ground_bottom_color(Color(0.075f, 0.070f, 0.058f));
    sky_mat->set_ground_horizon_color(Color(0.255f, 0.235f, 0.200f));
    sky_mat->set_ground_curve(0.06f);
    sky_mat->set_ground_energy_multiplier(0.70f);
    sky_mat->set_sun_angle_max(10.0f);
    sky_mat->set_sun_curve(0.06f);
    sky_mat->set_use_debanding(true);

    Ref<Sky> sky;
    sky.instantiate();
    sky->set_material(sky_mat);
    sky->set_radiance_size(Sky::RADIANCE_SIZE_256);
    sky->set_process_mode(Sky::PROCESS_MODE_REALTIME);

    env->set_background(Environment::BG_SKY);
    env->set_sky(sky);

    // 环境光：天空辐射打 72%，剩下 28% 用手调的冷色补，
    // 纯 SKY(1.0) 会让暗部跟着天空发灰、失去"金属冷"的味道。
    env->set_ambient_source(Environment::AMBIENT_SOURCE_SKY);
    env->set_ambient_light_sky_contribution(0.72f);
    env->set_ambient_light_color(Color(0.40f, 0.48f, 0.62f));
    env->set_ambient_light_energy(1.25f);

    env->set_reflection_source(Environment::REFLECTION_SOURCE_SKY);

    // ---- 雾：指数雾打底做纵深，再叠高度雾做"谷地晨霭" ----
    env->set_fog_enabled(true);
    env->set_fog_mode(Environment::FOG_MODE_EXPONENTIAL);
    env->set_fog_light_color(Color(0.395f, 0.445f, 0.520f));  // 冷灰蓝，不是白
    env->set_fog_light_energy(0.90f);
    env->set_fog_sun_scatter(0.18f);
    env->set_fog_density(0.0055f);
    env->set_fog_aerial_perspective(0.50f);
    env->set_fog_sky_affect(0.30f);
    env->set_fog_height(7.0f);
    env->set_fog_height_density(0.07f);

    // ---- 体积雾：阳光在空气里的丁达尔光柱，是"精致"最便宜的一剂 ----
    // 只铺 64m 深、细节扩散调粗，在集显上也能跑。卡的话把 enabled 关掉即可，
    // 指数雾 + 空气透视已经能撑住画面。
    env->set_volumetric_fog_enabled(true);
    env->set_volumetric_fog_density(0.014f);
    env->set_volumetric_fog_albedo(Color(0.70f, 0.73f, 0.78f));
    env->set_volumetric_fog_emission(Color(0.0f, 0.0f, 0.0f));
    env->set_volumetric_fog_emission_energy(0.0f);
    env->set_volumetric_fog_anisotropy(0.32f);
    env->set_volumetric_fog_length(64.0f);
    env->set_volumetric_fog_detail_spread(2.0f);
    env->set_volumetric_fog_ambient_inject(1.0f);
    env->set_volumetric_fog_sky_affect(0.0f);
    env->set_volumetric_fog_temporal_reprojection_enabled(true);
    env->set_volumetric_fog_temporal_reprojection_amount(0.90f);

    // ---- 色调映射：AgX。相比 ACES，AgX 对高光的滚降更平滑、不偏色， ----
    // 远景天空/雾不会像第一版那样过曝成一片死白。
    env->set_tonemapper(Environment::TONE_MAPPER_AGX);
    env->set_tonemap_exposure(1.0f);
    env->set_tonemap_white(4.0f);

    env->set_glow_enabled(true);
    env->set_glow_normalized(true);
    env->set_glow_intensity(0.42f);
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

    // SSAO 只做缝隙接触阴影，不要拿它当"全局压暗"用。
    // 第一版 intensity=1.6 是在给背光死黑补刀，现在光源补齐了就该收回来。
    env->set_ssao_enabled(true);
    env->set_ssao_radius(1.1f);
    env->set_ssao_intensity(0.85f);
    env->set_ssao_power(1.45f);
    env->set_ssao_detail(0.35f);
    env->set_ssao_horizon(0.06f);
    env->set_ssao_sharpness(0.98f);
    env->set_ssao_direct_light_affect(0.12f);   // 只在背光面生效，受光面几乎不动
    env->set_ssao_ao_channel_affect(0.0f);

    // 屏幕空间间接光：把周围环境的颜色"渗"进暗部，进一步消死黑。
    // 权重压得很低，只是补一口气，不做主光。
    env->set_ssil_enabled(true);
    env->set_ssil_radius(1.6f);
    env->set_ssil_intensity(0.35f);
    env->set_ssil_sharpness(0.94f);
    env->set_ssil_normal_rejection(1.0f);

    // ---- 最终调色：对比 + 饱和 + 三级 LUT ----
    env->set_adjustment_enabled(true);
    env->set_adjustment_brightness(1.0f);
    env->set_adjustment_contrast(1.06f);
    env->set_adjustment_saturation(1.10f);
    env->set_adjustment_color_correction(make_grade_lut());

    we->set_environment(env);
    root->add_child(we);
    out.world_env = we;

    // ---- 主光：黄昏太阳，压得很低但不过分（-48°），让树影拉长又不至于满屏掠射 ----
    DirectionalLight3D *sun = memnew(DirectionalLight3D);
    sun->set_name("Sun");
    sun->set_rotation_degrees(Vector3(-48.0f, 122.0f, 0.0f));
    sun->set_color(Color(1.00f, 0.925f, 0.800f));
    sun->set_param(Light3D::PARAM_ENERGY, 1.15f);
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

    // ---- 补光：从主光反方向斜上方打冷色，专治背光死黑 ----
    // SKY_MODE_LIGHT_ONLY 是关键：否则 Godot 会把这盏灯也当成"天上的第二个太阳"画进天空。
    DirectionalLight3D *fill = memnew(DirectionalLight3D);
    fill->set_name("Fill");
    fill->set_rotation_degrees(Vector3(-26.0f, -58.0f, 0.0f));
    fill->set_color(Color(0.60f, 0.73f, 0.94f));
    fill->set_param(Light3D::PARAM_ENERGY, 0.38f);
    fill->set_param(Light3D::PARAM_INDIRECT_ENERGY, 0.0f);
    fill->set_param(Light3D::PARAM_SPECULAR, 0.35f);
    fill->set_sky_mode(DirectionalLight3D::SKY_MODE_LIGHT_ONLY);
    fill->set_shadow(false);
    root->add_child(fill);
    out.fill = fill;

    // ---- 弹光：自下而上的暖土色，模拟大地反弹 ----
    // 这一盏专门负责把"下巴/下腹部/岩石底边"点亮，是让模型从背景里"立起来"的关键。
    DirectionalLight3D *bounce = memnew(DirectionalLight3D);
    bounce->set_name("Bounce");
    bounce->set_rotation_degrees(Vector3(36.0f, 46.0f, 0.0f));
    bounce->set_color(Color(0.56f, 0.49f, 0.35f));
    bounce->set_param(Light3D::PARAM_ENERGY, 0.20f);
    bounce->set_param(Light3D::PARAM_INDIRECT_ENERGY, 0.0f);
    bounce->set_param(Light3D::PARAM_SPECULAR, 0.15f);
    bounce->set_sky_mode(DirectionalLight3D::SKY_MODE_LIGHT_ONLY);
    bounce->set_shadow(false);
    root->add_child(bounce);
    out.bounce = bounce;
}

'''

MAIN_ANCHOR = '// ------------------------------------------------------- 主入口\nvoid build_scene(Node3D *root, SceneRefs &out) {'
assert MAIN_ANCHOR in text, "找不到 build_scene 锚点"
if 'static void build_environment(Node3D *root, SceneRefs &out)' not in text:
    text = text.replace(MAIN_ANCHOR, LUT_FN + MAIN_ANCHOR, 1)
    print("[2] 已插入 make_grade_lut() + build_environment()")
else:
    print("[2] build_environment 已存在，跳过")

# ------------------------------------------------------------------ 3. 用函数调用替换旧的天空/环境/太阳代码块
OLD_START = '    // ---- 天空 / 环境 / 太阳 ----\n'
OLD_END = '    root->add_child(sun);\n'
i0 = text.index(OLD_START)
i1 = text.index(OLD_END, i0) + len(OLD_END)

NEW_BLOCK = (
    '    // ---- 天空 / 环境 / 光照（三点布光 + 三级调色，见 build_environment）----\n'
    '    build_environment(root, out);\n\n'
)
text = text[:i0] + NEW_BLOCK + text[i1:]
print("[3] 已用 build_environment(root, out) 替换旧光照代码块（%d 字节）" % (i1 - i0))

with io.open(SRC, "w", encoding="utf-8", newline="\n") as f:
    f.write(text)

print("完成：%d -> %d 字节" % (orig_len, len(text)))
