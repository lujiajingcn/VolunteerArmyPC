# -*- coding: utf-8 -*-
"""
VolunteerArmyPC —— 掩体建模精致化 + 地面材质 + 雾/曝光收口

硬约束：**掩体的位置与 p.r 绝不能改**。
  p.r 参与视线阻挡（segCircle）、掩体减伤、AI 找掩体评分，
  动一下就是改玩法。所以这里只改"怎么画"，不改"在哪里、多大"。

树是最关键的收益点：
  第一版树干 2.4 m + 树冠 2 m 宽，在 1.65 m 眼高下就是一根棒棒糖糊在脸上
  （玩家长在 2.9 m 外的一棵树后面，整条公路都被挡住）。
  逻辑上 p.r 只约束"俯视 2D 的遮挡半径"，高度完全不受约束 ——
  把树抬到 9~13 m，树冠自然跑到视线上方，视野打开，同时终于像一棵真树。
"""
import io
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "node", "scene_builder.cpp")

with io.open(SRC, encoding="utf-8") as f:
    s = f.read()
n0 = len(s)

# ---------------------------------------------------------------- 1. includes
OLD_INC = "#include <godot_cpp/classes/image.hpp>\n"
NEW_INC = ("#include <godot_cpp/classes/array_mesh.hpp>\n"
           "#include <godot_cpp/classes/image.hpp>\n"
           "#include <godot_cpp/classes/surface_tool.hpp>\n")
assert OLD_INC in s
s = s.replace(OLD_INC, NEW_INC, 1)
s = s.replace("#include <cmath>\n#include <cstdlib>\n",
              "#include <algorithm>\n#include <cmath>\n#include <cstdlib>\n#include <vector>\n", 1)

# ---------------------------------------------------------------- 2. 岩石生成器
ROCK_FN = r'''// ------------------------------------------------------- 岩石网格
// 第一版岩石是压扁的 SphereMesh —— 光滑得像鹅卵石，摆在战场上非常塑料。
// 岩石的读感来自**棱角**，所以这里用"极坐标球 + 逐顶点半径扰动"生成低模多面体：
// 环数/段数刻意压低（7×11），让三角面保持大块面，看起来才像被切开的石头。
static Ref<ArrayMesh> make_rock_mesh(uint32_t seed, float radius, float flat) {
    va::Rng rng(seed);
    const int RINGS = 7;
    const int SEGS = 11;
    const float PI = 3.141592653589793f;

    std::vector<Vector3> pts((size_t)(RINGS + 1) * SEGS);
    for (int i = 0; i <= RINGS; ++i) {
        const float phi = (float)i / (float)RINGS * PI;
        const float y = std::cos(phi);
        const float rr = std::sin(phi);
        for (int j = 0; j < SEGS; ++j) {
            const float th = (float)j / (float)SEGS * 2.0f * PI;
            // 低频扰动：同一块石头各顶点半径不同 → 出现不规则棱面而不是光滑球面
            const float k = 0.60f + rng.next() * 0.78f;
            Vector3 v(std::cos(th) * rr * k, y * k * flat, std::sin(th) * rr * k);
            pts[(size_t)i * SEGS + j] = v * radius;
        }
    }

    Ref<SurfaceTool> st;
    st.instantiate();
    st->begin(Mesh::PRIMITIVE_TRIANGLES);
    for (int i = 0; i < RINGS; ++i) {
        for (int j = 0; j < SEGS; ++j) {
            const int j1 = (j + 1) % SEGS;
            const Vector3 a = pts[(size_t)i * SEGS + j];
            const Vector3 b = pts[(size_t)(i + 1) * SEGS + j];
            const Vector3 c = pts[(size_t)(i + 1) * SEGS + j1];
            const Vector3 d = pts[(size_t)i * SEGS + j1];
            if (i != 0) { st->add_vertex(a); st->add_vertex(b); st->add_vertex(c); }
            if (i != RINGS - 1) { st->add_vertex(a); st->add_vertex(c); st->add_vertex(d); }
        }
    }
    // 不平滑 = 保留大块棱面（岩石要的就是这个）
    st->generate_normals();
    return st->commit();
}

// ------------------------------------------------------- 程序化纹理
'''
ANCHOR = "// ------------------------------------------------------- 程序化纹理\n"
assert ANCHOR in s
s = s.replace(ANCHOR, ROCK_FN, 1)

# ---------------------------------------------------------------- 3. tex_grass 重写
i0 = s.index("Ref<Texture2D> tex_grass() {")
i1 = s.index("\n}\n", i0) + 3
NEW_GRASS = r'''Ref<Texture2D> tex_grass() {
    const int N = 256;
    Ref<Image> img = Image::create_empty(N, N, false, Image::FORMAT_RGB8);
    va::Rng r(0x9E3779B9u);

    // 三层噪声叠加：细粒（草叶） + 中频（草皮浓淡） + 低频（枯黄/裸土斑块）。
    // 只用单频白噪声的话，贴出来是"砂纸"而不是"地面" —— 低频决定读感。
    const int M = 32;
    std::vector<float> low((size_t)M * M);
    for (int i = 0; i < M * M; ++i) low[(size_t)i] = r.next();

    auto sample_low = [&](float u, float v) -> float {
        u -= std::floor(u);
        v -= std::floor(v);
        const float fx = u * (float)M, fy = v * (float)M;
        const int x0 = ((int)fx) % M, y0 = ((int)fy) % M;
        const int x1 = (x0 + 1) % M, y1 = (y0 + 1) % M;
        const float tx = fx - std::floor(fx), ty = fy - std::floor(fy);
        const float a = va::lerpf(low[(size_t)y0 * M + x0], low[(size_t)y0 * M + x1], tx);
        const float b = va::lerpf(low[(size_t)y1 * M + x0], low[(size_t)y1 * M + x1], tx);
        return va::lerpf(a, b, ty);
    };

    // 配色刻意压低饱和：军事战场是枯草混裸土，不是高尔夫草坪。
    // （第一版草地渲染出来饱和度 0.45，绿得发"塑料"，这是主要修改动机。）
    const Color dirt(0.360f, 0.310f, 0.235f);
    const Color grass(0.275f, 0.310f, 0.185f);
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const float u = (float)x / (float)N, v = (float)y / (float)N;
            const float fine = r.next() * 0.55f + r.next() * 0.30f + r.next() * 0.15f;
            const float mid = sample_low(u * 3.0f, v * 3.0f);
            const float lo = sample_low(u, v);

            const float t = va::clampf((lo - 0.28f) / 0.42f, 0.0f, 1.0f);   // 0=裸土 1=草
            Color c = dirt.lerp(grass, t);
            const float k = (0.74f + fine * 0.46f) * (0.86f + mid * 0.30f);
            c = Color(c.r * k, c.g * k, c.b * k);
            img->set_pixel(x, y, Color(va::clampf(c.r, 0, 1), va::clampf(c.g, 0, 1), va::clampf(c.b, 0, 1)));
        }
    }
    return ImageTexture::create_from_image(img);
}
'''
s = s[:i0] + NEW_GRASS + s[i1:]

# ---------------------------------------------------------------- 4. tex_road 重写
i0 = s.index("Ref<Texture2D> tex_road() {")
i1 = s.index("\n}\n", i0) + 3
NEW_ROAD = r'''Ref<Texture2D> tex_road() {
    const int N = 256;
    Ref<Image> img = Image::create_empty(N, N, false, Image::FORMAT_RGB8);
    va::Rng r(0x85EBCA6Bu);
    // 沥青：细粒噪声 + 稀疏亮骨料 + 低频深浅（补丁/油渍），
    // 再加几条横向裂缝 —— 一条纯噪声的公路看起来像塑料板。
    std::vector<float> low((size_t)16 * 16);
    for (int i = 0; i < 16 * 16; ++i) low[(size_t)i] = r.next();
    auto sl = [&](float u, float v) -> float {
        u -= std::floor(u); v -= std::floor(v);
        const float fx = u * 16.0f, fy = v * 16.0f;
        const int x0 = ((int)fx) % 16, y0 = ((int)fy) % 16;
        const int x1 = (x0 + 1) % 16, y1 = (y0 + 1) % 16;
        const float tx = fx - std::floor(fx), ty = fy - std::floor(fy);
        const float a = va::lerpf(low[(size_t)y0 * 16 + x0], low[(size_t)y0 * 16 + x1], tx);
        const float b = va::lerpf(low[(size_t)y1 * 16 + x0], low[(size_t)y1 * 16 + x1], tx);
        return va::lerpf(a, b, ty);
    };
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const float u = (float)x / (float)N, v = (float)y / (float)N;
            float val = 0.105f + (r.next() * 0.6f + r.next() * 0.4f) * 0.070f;
            val *= (0.86f + sl(u * 2.0f, v * 2.0f) * 0.28f);
            if (r.next() > 0.992f) val += 0.055f;                     // 亮骨料
            if (std::fabs(v - 0.5f) < 0.012f && sl(u * 6.0f, v) > 0.55f) val *= 0.72f;  // 细裂缝
            val = va::clampf(val, 0.0f, 1.0f);
            img->set_pixel(x, y, Color(val, val * 0.985f, val * 0.955f));
        }
    }
    return ImageTexture::create_from_image(img);
}
'''
s = s[:i0] + NEW_ROAD + s[i1:]

# ---------------------------------------------------------------- 5. add_prop 的 Rock/Tree/Bush
i0 = s.index("    switch (p.type) {\n        case va::PropType::Rock: {")
i1 = s.index("        case va::PropType::Barrel: {", i0)
NEW_PROP = r'''    switch (p.type) {
        case va::PropType::Rock: {
            // 低模不规则多面体 + 扁平化，贴地而不是"立起来的蛋"
            const uint32_t sd = (uint32_t)(p.x * 31 + p.y * 17);
            Ref<ArrayMesh> m = make_rock_mesh(sd, r, 0.62f);
            const Color c = hash_color(sd, 0.345f, 0.335f, 0.305f, 0.075f);
            add_mesh(parent, m, pos + Vector3(0, r * 0.26f, 0), mat_solid(c, 0.96f),
                     Vector3(0, p.x * 0.7f, 0));
            break;
        }
        case va::PropType::Tree: {
            // 树干高 9~13 m、树冠跑到视线上方 —— 见本文件头部说明：
            // 逻辑层的 p.r 只约束"俯视遮挡半径"（= 树冠的水平尺度），高度不受约束。
            va::Rng rng((uint32_t)(p.x * 73856093u) ^ (uint32_t)(p.y * 19349663u) ^ 0x9E3779B9u);
            const float h = 9.0f + rng.next() * 4.0f;
            const float trunk_r = r * 0.15f + 0.030f;
            const Color bark_c(0.150f, 0.115f, 0.080f);
            Ref<StandardMaterial3D> bark = mat_solid(bark_c, 0.98f);

            // 树干：上细下粗 + 8 边（低模的"硬"比 24 边圆柱更像木头）
            Ref<CylinderMesh> tr = memnew(CylinderMesh);
            tr->set_top_radius(trunk_r * 0.50f);
            tr->set_bottom_radius(trunk_r);
            tr->set_height(h);
            tr->set_radial_segments(8);
            tr->set_rings(3);
            add_mesh(parent, tr, pos + Vector3(0, h * 0.5f, 0), bark);

            // 分枝：从 52%~82% 高度斜插出去，打断"光杆"的廉价感
            const int nbranch = 3 + (int)(rng.next() * 2.99f);
            for (int b = 0; b < nbranch; ++b) {
                const float bf = 0.52f + rng.next() * 0.30f;
                const float ang = rng.next() * 6.2831853f;
                const float tilt = -(0.55f + rng.next() * 0.45f);
                const float len = h * (0.16f + rng.next() * 0.15f);
                Ref<CylinderMesh> br = memnew(CylinderMesh);
                br->set_top_radius(trunk_r * 0.16f);
                br->set_bottom_radius(trunk_r * 0.44f);
                br->set_height(len);
                br->set_radial_segments(6);
                br->set_rings(1);
                // 先绕 Y 转到方位，再朝外倾斜 —— 圆柱局部轴是 +Y，直接用旋转矩阵摆正
                const Basis rot = Basis(Vector3(0, 1, 0), ang) * Basis(Vector3(1, 0, 0), tilt);
                const Vector3 dirv = rot.xform(Vector3(0, 1, 0));
                MeshInstance3D *mi = add_mesh(parent, br,
                                              pos + Vector3(0, h * bf, 0) + dirv * (len * 0.5f), bark);
                mi->set_transform(Transform3D(rot, pos + Vector3(0, h * bf, 0) + dirv * (len * 0.5f)));
            }

            // 树冠：4~6 个球簇错落堆叠，比单个椭球体真实得多
            const float crown_r = std::max(r * 1.05f, 0.85f);
            const Vector3 top = pos + Vector3(0, h, 0);
            const int ncl = 4 + (int)(rng.next() * 2.99f);
            for (int i = 0; i < ncl; ++i) {
                const float cr = crown_r * (0.60f + rng.next() * 0.58f);
                Ref<SphereMesh> cn = memnew(SphereMesh);
                cn->set_radius(cr);
                cn->set_height(cr * 1.8f);
                cn->set_radial_segments(9);
                cn->set_rings(5);
                const float a = rng.next() * 6.2831853f;
                const float rad = crown_r * rng.next() * 0.80f;
                const float dy = (rng.next() - 0.30f) * crown_r * 1.45f;
                // 叶色比第一版亮一档：第一版在背光面直接塌成纯黑，
                // 抬一点 albedo 让补光有东西可以照。
                const Color c = hash_color((uint32_t)(p.x * 13 + p.y * 29 + i * 977), 0.235f, 0.300f, 0.155f, 0.055f);
                add_mesh(parent, cn, top + Vector3(std::cos(a) * rad, dy, std::sin(a) * rad),
                         mat_solid(c, 1.0f));
            }
            break;
        }
        case va::PropType::Bush: {
            // 一丛 3~5 个小球，不是一个孤零零的大球
            va::Rng rng((uint32_t)(p.x * 40503u) ^ (uint32_t)(p.y * 12289u));
            const int n = 3 + (int)(rng.next() * 2.99f);
            for (int i = 0; i < n; ++i) {
                const float br = r * (0.42f + rng.next() * 0.40f);
                Ref<SphereMesh> m = memnew(SphereMesh);
                m->set_radius(br);
                m->set_height(br * 1.35f);
                m->set_radial_segments(8);
                m->set_rings(4);
                const float a = rng.next() * 6.2831853f;
                const float rad = r * rng.next() * 0.62f;
                const Color c = hash_color((uint32_t)(p.x * 7 + p.y * 11 + i * 131), 0.205f, 0.275f, 0.145f, 0.05f);
                add_mesh(parent, m, pos + Vector3(std::cos(a) * rad, br * 0.55f, std::sin(a) * rad),
                         mat_solid(c, 1.0f));
            }
            break;
        }
'''
s = s[:i0] + NEW_PROP + s[i1:]

# ---------------------------------------------------------------- 6. 曝光 / 雾 / 饱和收口
REPL = [
 # 曝光：AgX 中间调偏暗，实测 ×1.7 才到位（用 VA_EXPOSURE 扫出来的）
 ("      1.15f, 1.14f, 1.08f, 0.45f },", "      1.95f, 1.00f, 1.08f, 0.45f },"),
 ("      1.45f, 0.88f, 0.95f, 0.28f },", "      2.45f, 0.84f, 0.95f, 0.28f },"),
 ("      1.35f, 0.94f, 1.05f, 0.60f },", "      2.25f, 0.92f, 1.05f, 0.60f },"),
 # 雾：要能看出纵深，太淡等于没有空气透视
 ("      SRGB(0.620f, 0.680f, 0.700f), 0.0030f, 0.015f, 0.18f, 0.22f, 0.12f,",
  "      SRGB(0.620f, 0.680f, 0.700f), 0.0060f, 0.030f, 0.20f, 0.40f, 0.14f,"),
 ("      SRGB(0.440f, 0.480f, 0.500f), 0.0080f, 0.030f, 0.05f, 0.30f, 0.18f,",
  "      SRGB(0.440f, 0.480f, 0.500f), 0.0130f, 0.055f, 0.06f, 0.50f, 0.22f,"),
 ("      SRGB(0.055f, 0.075f, 0.115f), 0.0028f, 0.012f, 0.10f, 0.20f, 0.10f,",
  "      SRGB(0.055f, 0.075f, 0.115f), 0.0055f, 0.028f, 0.12f, 0.35f, 0.12f,"),
 # 体积雾密度同步跟上
 ("      0.0030f, 0.32f,", "      0.0050f, 0.32f,"),
 ("      0.0070f, 0.10f,", "      0.0100f, 0.10f,"),
 ("      0.0025f, 0.25f,", "      0.0045f, 0.25f,"),
]
for a, b in REPL:
    assert a in s, "锚点缺失: " + a[:60]
    s = s.replace(a, b, 1)

with io.open(SRC, "w", encoding="utf-8", newline="\n") as f:
    f.write(s)
print("掩体/地面/雾补丁完成：%d -> %d 字节" % (n0, len(s)))
