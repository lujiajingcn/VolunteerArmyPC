// VolunteerArmyPC —— 3D 战场场景构建（程序化生成，零外部资源）
#include "node/scene_builder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/cylinder_mesh.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/gltf_document.hpp>
#include <godot_cpp/classes/gltf_state.hpp>
#include <godot_cpp/classes/light3d.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/surface_tool.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/image_texture3d.hpp>
#include <godot_cpp/classes/texture3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/plane_mesh.hpp>
#include <godot_cpp/classes/procedural_sky_material.hpp>
#include <godot_cpp/classes/sky.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace volunteer_army {

// ------------------------------------------------------------------ 小工具
static Color hash_color(uint32_t seed, float base_r, float base_g, float base_b, float vary) {
    va::Rng r(seed);
    const float k = (r.next() - 0.5f) * 2.0f * vary;
    return Color(va::clampf(base_r + k, 0, 1), va::clampf(base_g + k * 0.9f, 0, 1), va::clampf(base_b + k * 0.7f, 0, 1));
}

static Ref<StandardMaterial3D> mat_solid(const Color &c, float roughness = 0.9f, float metallic = 0.0f) {
    Ref<StandardMaterial3D> m;
    m.instantiate();
    m->set_albedo(c);
    m->set_roughness(roughness);
    m->set_metallic(metallic);
    return m;
}

static MeshInstance3D *add_mesh(Node3D *parent, const Ref<Mesh> &mesh, const Vector3 &pos,
                                const Ref<Material> &mat, const Vector3 &rot_deg = Vector3()) {
    MeshInstance3D *mi = memnew(MeshInstance3D);
    mi->set_mesh(mesh);
    mi->set_position(pos);
    mi->set_rotation_degrees(rot_deg);
    if (mat.is_valid()) mi->set_material_override(mat);
    parent->add_child(mi);
    return mi;
}

// 只重建掩体层（见头文件说明）
// add_prop 定义在文件后半段，这里先声明（掩体建模函数很长，不搬家了）
static void add_prop(Node3D *parent, const va::Prop &p);

void rebuild_props(Node3D *root, SceneRefs &refs) {
    if (refs.props != nullptr) {
        refs.props->queue_free();
        refs.props = nullptr;
    }
    Node3D *props = memnew(Node3D);
    props->set_name("Props2");
    for (const auto &p : va::W.props) add_prop(props, p);
    root->add_child(props);
    refs.props = props;
}

// ------------------------------------------------------- 云层纹理
// ProceduralSkyMaterial 本身不带云，只有一个 sky_cover 贴图槽（按方向映射，用 alpha 决定覆盖度）。
// 生成一张 fBm 噪声当云：低频定云团大小，高频定边缘碎絮。
// 没有云的天空在 3D 里会显得非常"塑料"，这是性价比最高的一处补强。
Ref<Texture2D> tex_clouds() {
    const int N = 512;
    Ref<Image> img = Image::create_empty(N, N, false, Image::FORMAT_RGBA8);
    va::Rng r(0x2545F491u);
    // 三层不同频率的值噪声，双线性插值后叠加
    struct Layer { int m; std::vector<float> d; };
    Layer ls[3];
    const int ms[3] = { 6, 14, 34 };
    for (int li = 0; li < 3; ++li) {
        ls[li].m = ms[li];
        ls[li].d.resize((size_t)ms[li] * ms[li]);
        for (int i = 0; i < ms[li] * ms[li]; ++i) ls[li].d[(size_t)i] = r.next();
    }
    auto samp = [&](const Layer &L, float u, float v) -> float {
        u -= std::floor(u); v -= std::floor(v);
        const float fx = u * (float)L.m, fy = v * (float)L.m;
        const int x0 = ((int)fx) % L.m, y0 = ((int)fy) % L.m;
        const int x1 = (x0 + 1) % L.m, y1 = (y0 + 1) % L.m;
        const float tx = fx - std::floor(fx), ty = fy - std::floor(fy);
        const float a = va::lerpf(L.d[(size_t)y0 * L.m + x0], L.d[(size_t)y0 * L.m + x1], tx);
        const float b = va::lerpf(L.d[(size_t)y1 * L.m + x0], L.d[(size_t)y1 * L.m + x1], tx);
        return va::lerpf(a, b, ty);
    };
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const float u = (float)x / (float)N, v = (float)y / (float)N;
            float n = samp(ls[0], u, v) * 0.58f + samp(ls[1], u, v) * 0.28f + samp(ls[2], u, v) * 0.14f;
            // 阈值抬一点，让云成"团"而不是"雾"
            float a = va::clampf((n - 0.545f) / 0.30f, 0.0f, 1.0f);
            a = a * a * (3.0f - 2.0f * a);          // 软过渡
            a *= 0.62f;                              // 别太厚，留出天光
            img->set_pixel(x, y, Color(1.0f, 1.0f, 1.0f, a));
        }
    }
    return ImageTexture::create_from_image(img);
}

// ------------------------------------------------------- 远景山脊
// 一张 700×700 的地面铺到雾里之后，地平线会是一条死板的直线 —— 这是"廉价"最明显的标志。
// 在 200~340 m 处摆一圈低模丘陵，配合指数雾自然形成层叠的空气透视剪影，
// 纵深立刻就出来了（代价只有几百个三角面）。
Ref<ArrayMesh> make_ridges_mesh(float cx, float cz) {
    va::Rng rng(0xB5297A4Du);
    const int NH = 46;
    const float PI = 3.141592653589793f;
    Ref<SurfaceTool> st;
    st.instantiate();
    st->begin(Mesh::PRIMITIVE_TRIANGLES);
    for (int i = 0; i < NH; ++i) {
        const float ang = (float)i / (float)NH * 2.0f * PI + rng.next() * 0.09f;
        // 一半近一半远：两圈不同尺度叠在一起，才有层叠山脊的感觉，
        // 单圈等距同高的锥体排一排，看起来像几何题不像地貌。
        const bool far_ring = (i % 2) == 0;
        const float r0 = far_ring ? (250.0f + rng.next() * 150.0f) : (140.0f + rng.next() * 95.0f);
        const float dist = r0;
        const float w = (far_ring ? 56.0f : 26.0f) + rng.next() * (far_ring ? 96.0f : 40.0f);
        const float h = (far_ring ? 24.0f : 9.0f) + rng.next() * (far_ring ? 46.0f : 18.0f);
        const float px = cx + std::cos(ang) * dist;
        const float pz = cz + std::sin(ang) * dist;
        const Vector3 o(px, -2.0f, pz);
        const Vector3 side(std::cos(ang + 1.5707963f), 0.0f, std::sin(ang + 1.5707963f));
        // 一个四棱锥山头：底边 ±w，顶点抬高 h，再让山顶左右错开一点做出不对称轮廓
        const Vector3 a = o - side * w;
        const Vector3 b = o + side * w;
        const Vector3 c = o + side * w * 0.22f + side.cross(Vector3(0, 1, 0)) * 0.0f;
        const Vector3 apex = Vector3(px + (rng.next() - 0.5f) * w * 0.7f, h, pz + (rng.next() - 0.5f) * w * 0.5f);
        st->add_vertex(a); st->add_vertex(b); st->add_vertex(apex);
        st->add_vertex(b); st->add_vertex(c); st->add_vertex(apex);
        st->add_vertex(c); st->add_vertex(a); st->add_vertex(apex);
    }
    st->generate_normals();
    return st->commit();
}

// ------------------------------------------------------- 岩石网格
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
Ref<Texture2D> tex_grass() {
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
    const Color dirt(0.330f, 0.298f, 0.238f);
    const Color grass(0.283f, 0.306f, 0.207f);
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const float u = (float)x / (float)N, v = (float)y / (float)N;
            const float fine = r.next() * 0.55f + r.next() * 0.30f + r.next() * 0.15f;
            const float mid = sample_low(u * 3.0f, v * 3.0f);
            const float lo = sample_low(u, v);

            const float t = va::clampf((lo - 0.26f) / 0.50f, 0.0f, 1.0f);   // 0=裸土 1=草
            Color c = dirt.lerp(grass, t);
            const float k = (0.74f + fine * 0.46f) * (0.86f + mid * 0.30f);
            c = Color(c.r * k, c.g * k, c.b * k);
            img->set_pixel(x, y, Color(va::clampf(c.r, 0, 1), va::clampf(c.g, 0, 1), va::clampf(c.b, 0, 1)));
        }
    }
    return ImageTexture::create_from_image(img);
}

Ref<Texture2D> tex_road() {
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

Ref<Texture2D> tex_water() {
    const int N = 128;
    Ref<Image> img = Image::create_empty(N, N, false, Image::FORMAT_RGB8);
    va::Rng r(0xC2B2AE35u);
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const float n = r.next() * 0.5f + r.next() * 0.5f;
            img->set_pixel(x, y, Color(0.10f + n * 0.05f, 0.17f + n * 0.07f, 0.20f + n * 0.06f));
        }
    }
    return ImageTexture::create_from_image(img);
}

// ------------------------------------------------------- 实体模型
Node3D *make_soldier_node(bool enemy, bool downed) {
    Node3D *n = memnew(Node3D);

    const Color uniform = enemy ? Color(0.22f, 0.20f, 0.16f) : Color(0.30f, 0.33f, 0.26f);
    const Color vest = enemy ? Color(0.16f, 0.15f, 0.13f) : Color(0.20f, 0.22f, 0.18f);
    const Color skin = Color(0.52f, 0.40f, 0.31f);

    // 躯干（胶囊近似：圆柱 + 球）
    Ref<CylinderMesh> torso = memnew(CylinderMesh);
    torso->set_top_radius(0.20f);
    torso->set_bottom_radius(0.17f);
    torso->set_height(0.56f);
    add_mesh(n, torso, Vector3(0, 1.06f, 0), mat_solid(uniform, 0.92f));

    Ref<CylinderMesh> vest_mesh = memnew(CylinderMesh);
    vest_mesh->set_top_radius(0.215f);
    vest_mesh->set_bottom_radius(0.195f);
    vest_mesh->set_height(0.34f);
    add_mesh(n, vest_mesh, Vector3(0, 1.14f, 0), mat_solid(vest, 0.85f));

    // 头 + 头盔
    Ref<SphereMesh> head = memnew(SphereMesh);
    head->set_radius(0.105f);
    head->set_height(0.21f);
    add_mesh(n, head, Vector3(0, 1.50f, 0), mat_solid(skin, 0.85f));

    Ref<SphereMesh> helmet = memnew(SphereMesh);
    helmet->set_radius(0.128f);
    helmet->set_height(0.20f);
    add_mesh(n, helmet, Vector3(0, 1.54f, 0), mat_solid(uniform.darkened(0.25f), 0.8f));

    // 腿
    Ref<CylinderMesh> leg = memnew(CylinderMesh);
    leg->set_top_radius(0.085f);
    leg->set_bottom_radius(0.07f);
    leg->set_height(0.78f);
    Ref<StandardMaterial3D> legm = mat_solid(uniform.darkened(0.15f), 0.95f);
    add_mesh(n, leg, Vector3(-0.09f, 0.39f, 0), legm);
    add_mesh(n, leg, Vector3(0.09f, 0.39f, 0), legm);

    // 手臂
    Ref<CylinderMesh> arm = memnew(CylinderMesh);
    arm->set_top_radius(0.055f);
    arm->set_bottom_radius(0.05f);
    arm->set_height(0.50f);
    add_mesh(n, arm, Vector3(-0.24f, 1.16f, 0), mat_solid(uniform, 0.9f), Vector3(0, 0, 12));
    add_mesh(n, arm, Vector3(0.24f, 1.16f, 0), mat_solid(uniform, 0.9f), Vector3(0, 0, -12));

    // 枪（朝 +X，与逻辑层「模型前方 = +X」一致）
    Ref<BoxMesh> gun = memnew(BoxMesh);
    gun->set_size(Vector3(0.62f, 0.06f, 0.055f));
    add_mesh(n, gun, Vector3(0.20f, 1.10f, 0.10f), mat_solid(Color(0.11f, 0.11f, 0.12f), 0.55f, 0.6f));

    // 倒地姿态**不在这里设**。
    // 原来这里有一段 `if (downed) { set_rotation_degrees(84); set_position(0,0.28,0); }`，
    // 但它从来没有生效过：sync_entity_nodes 每帧都会重写这个节点的
    // position/rotation，建节点时写进去的姿态在第一帧就被覆盖掉了 ——
    // 也就是说**倒地的士兵一直站着**。现在倒地姿态改由 sync_entity_nodes
    // 用「偏航 × 侧翻」合成基施加，两条渲染路径（图元 / 三维模型）共用同一套逻辑。
    (void)downed;
    return n;
}

// ---------------------------------------------- 角色三维模型（图生3D 产物）
//
// 数据来源与尺寸：见 tools/gen3d.py（图生三维）与 tools/slim_glb.py（贴图瘦身）。
// 生成器给的 GLB 单张 42.78MB，其中 41.3MB 是三张 4096² 贴图；
// 收到 512² 之后是 1.56MB，网格 5 万面不动。这个比例是必须的 ——
// 士兵在画面上通常只有几十到两百像素高。

// 目标身高（节点单位，与图元士兵一致：头盔顶 y≈1.64，见 make_soldier_node）
constexpr float UNIT_MODEL_H = 1.68f;

// 朝向校正：glTF 模型的正面普遍朝 +Z，而本工程的契约是"模型前方 = +X"
// （逻辑层的 facing 与实体节点的偏航都按这个来）。绕 Y 转 +90° 正好把 +Z 转到 +X：
// 绕 Y 转 θ 把 (0,0,1) 映到 (sinθ, 0, cosθ)，θ=90° 即 (1,0,0)。
// 留 VA_MODEL_YAW 旋钮覆盖，是为了万一某批模型朝向不同，能靠截图量出来改，
// 而不是靠重编译猜。
constexpr float UNIT_MODEL_YAW_DEG = 90.0f;

static std::map<std::string, Node3D *> s_unit_proto;   // 键 -> 外层原型（含完整的归一化子树）
static std::map<std::string, bool> s_unit_failed;      // 失败过就别每个单位再试一次

static void collect_aabb(Node *p_node, const Transform3D &p_xf, AABB &p_box, bool &p_has) {
    Transform3D xf = p_xf;
    Node3D *n3 = Object::cast_to<Node3D>(p_node);
    if (n3 != nullptr) {
        xf = p_xf * n3->get_transform();
    }
    VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(p_node);
    if (vi != nullptr) {
        const AABB b = xf.xform(vi->get_aabb());
        if (!p_has) {
            p_box = b;
            p_has = true;
        } else {
            p_box.merge_with(b);
        }
    }
    const int nc = p_node->get_child_count();
    for (int i = 0; i < nc; ++i) {
        collect_aabb(p_node->get_child(i), xf, p_box, p_has);
    }
}

std::string ally_art_key(const std::string &p_id) {
    // 我方按花名册 id 查表（va_config.cpp 的 ROSTER）。
    // 用 id 而不是 role 字符串：role 有"弹药/支援"这种带斜杠的写法，
    // 而且铁头的 role 是"弹药/支援"但武器是步枪，按武器分档会把他错认成步枪手。
    if (p_id == "player")  return "char_leader";
    if (p_id == "laozhou") return "char_mg";
    if (p_id == "xiaoxia") return "char_sniper";
    if (p_id == "shitou" || p_id == "houzi") return "char_at";
    if (p_id == "laobai")  return "char_demo";
    if (p_id == "xiaoman") return "char_medic";
    if (p_id == "tietou")  return "char_ammo";
    return "char_rifleman";        // ajie / daliu / alan 与一切未登记的
}

std::string enemy_art_key(bool p_officer, bool p_mg) {
    if (p_officer) return "char_enemy_officer";
    if (p_mg)      return "char_enemy_mg";
    return "char_enemy_rifle";
}

const std::vector<std::string> &all_art_keys() {
    // 顺序 = 检阅台（VA_UNIT_SHOW）的陈列顺序，也是 hud.cpp 名册的职务顺序：
    // 我方 8 种职务，再到敌方 3 种。
    // 【为什么要有一份总表】检阅台要按固定顺序摆一遍、"模型齐备"的自检要遍历一遍，
    // 两处各抄一遍键名，加第 12 个角色时必然漏掉其中一处 —— 而漏掉的那一处
    // 只会表现为"检阅台上少一个人"，不会报错。
    static const std::vector<std::string> k = {
        "char_leader", "char_rifleman", "char_mg", "char_sniper",
        "char_at", "char_demo", "char_medic", "char_ammo",
        "char_enemy_rifle", "char_enemy_mg", "char_enemy_officer",
    };
    return k;
}

std::string unit_model_key(const va::Unit &u) {
    if (u.team == va::Team::Enemy) {
        return enemy_art_key(u.officer, u.mg);
    }
    return ally_art_key(u.id);
}

static Node3D *load_unit_proto(const std::string &p_key, Node *p_parent) {
    auto it = s_unit_proto.find(p_key);
    if (it != s_unit_proto.end()) {
        return it->second;
    }
    if (s_unit_failed.count(p_key) != 0) {
        return nullptr;
    }

    const String path = String("res://assets/art/char/model/") +
                        String::utf8(p_key.c_str()) + String(".glb");
    // 先判存在再解析：直接解析一个不存在的路径会在 stderr 打一行 ERROR，
    // 而"日志里有没有 ERROR"是本工程的回归判据之一，不能被这种假错误污染。
    if (!FileAccess::file_exists(path)) {
        s_unit_failed[p_key] = true;
        return nullptr;
    }

    Ref<GLTFDocument> doc;
    doc.instantiate();
    Ref<GLTFState> st;
    st.instantiate();
    if (doc.is_null() || st.is_null()) {
        UtilityFunctions::print("[unit] GLTFDocument 不可用，全部回退图元士兵");
        s_unit_failed[p_key] = true;
        return nullptr;
    }

    const Error err = doc->append_from_file(path, st);
    if (err != OK) {
        UtilityFunctions::print("[unit] 模型解析失败 ", path, " err=", (int)err);
        s_unit_failed[p_key] = true;
        return nullptr;
    }
    Node *scene = doc->generate_scene(st);
    Node3D *raw = Object::cast_to<Node3D>(scene);
    if (raw == nullptr) {
        UtilityFunctions::print("[unit] 模型没有可用的场景根 ", path);
        s_unit_failed[p_key] = true;
        return nullptr;
    }

    // 量包围盒。glTF 的节点自带 +90°绕X（Z-up → Y-up），
    // 所以这里量到的就是引擎空间的尺寸，不必再自己转。
    AABB box;
    bool has = false;
    collect_aabb(raw, Transform3D(), box, has);
    if (!has || box.size.y <= 1e-5f) {
        UtilityFunctions::print("[unit] 模型没有网格 ", path);
        raw->queue_free();
        s_unit_failed[p_key] = true;
        return nullptr;
    }

    const float k = UNIT_MODEL_H / box.size.y;

    float yaw_deg = UNIT_MODEL_YAW_DEG;
    if (const char *e = std::getenv("VA_MODEL_YAW")) {
        yaw_deg = (float)std::atof(e);
    }

    Node3D *outer = memnew(Node3D);
    Node3D *norm = memnew(Node3D);
    // 变换：绕 Y 转 yaw，再等比缩放到目标身高，最后把脚底挪到 y=0、水平居中到原点。
    // 用 set_transform 一次给全，不分成 set_scale + set_rotation 两步 ——
    // 那两步在 Node3D 里靠内部缓存的 euler/scale 重新合成基，顺序与语义都要额外确认，
    // 而这里是要一次性表达一个明确的仿射变换。
    Basis b(Vector3(0.0f, 1.0f, 0.0f), yaw_deg * 3.14159265358979323846f / 180.0f);
    b.scale(Vector3(k, k, k));
    const Vector3 c = box.get_center();
    norm->set_transform(Transform3D(b, Vector3(-c.x * k, -box.position.y * k, -c.z * k)));
    norm->add_child(raw);
    outer->add_child(norm);

    // 原型挂进场景树并隐藏，生命周期交给场景树。
    // 若不挂（只放到静态 map 里），它持有的 mesh / material / 3 张贴图
    // 会在进程退出时被 Godot 报成 8 条 "leaked at exit" 的 ERROR ——
    // 数量精确对得上：1 mesh / 1 material / 1 shader / 3 texture / 1 instance。
    if (p_parent != nullptr) {
        p_parent->add_child(outer);
        outer->set_visible(false);
    } else {
        UtilityFunctions::print("[unit] 警告：没有原型挂载点，模型资源会在退出时报泄漏");
    }

    UtilityFunctions::print("[unit] 模型 ", String::utf8(p_key.c_str()),
                            " 原始包围盒 ", box.size,
                            " 缩放 ", k, " yaw ", yaw_deg);
    s_unit_proto[p_key] = outer;
    return outer;
}

Node3D *make_unit_node_by_key(const std::string &p_key, Node *p_proto_parent) {
    Node3D *proto = load_unit_proto(p_key, p_proto_parent);
    if (proto != nullptr) {
        Node *dup = proto->duplicate();
        Node3D *n = Object::cast_to<Node3D>(dup);
        if (n != nullptr) {
            // duplicate() 会把 visible=false 一起复制过来（原型是隐藏的），
            // 这里显式打开，否则整支小队在画面上集体消失。
            n->set_visible(true);
            return n;
        }
        if (dup != nullptr) {
            dup->queue_free();
        }
    }
    return nullptr;
}

Node3D *make_unit_node(const va::Unit &u, Node *p_proto_parent) {
    Node3D *n = make_unit_node_by_key(unit_model_key(u), p_proto_parent);
    if (n != nullptr) return n;
    // 没有模型就退回图元士兵 —— 少一个模型文件不该让战场上少一个人。
    return make_soldier_node(u.team == va::Team::Enemy, u.downed);
}

Transform3D unit_transform(float p_x, float p_y, float p_facing, bool p_downed) {
    // 模型前方 = +X（与逻辑层一致），绕 Y 旋转角 = -facing；
    // 倒地再叠一个绕 Z 的 84° 侧翻。
    //
    // 【合成顺序：先偏航、再侧翻】b = yaw * roll —— 这样侧翻绕的是**模型自身的
    // 前方轴**，人朝哪边倒都跟自身朝向一致。反过来写（roll * yaw）得到的是
    // 绕**世界 Z** 侧翻，人转向哪边都会往同一个世界方向倒。
    // roll 的符号取 +84°（+Y 绕 +Z 转 84° 后倒向 -X，即向后倒）。
    Basis b(Vector3(0.0f, 1.0f, 0.0f), -p_facing);
    if (p_downed) {
        b = b * Basis(Vector3(0.0f, 0.0f, 1.0f), 84.0f * 3.14159265358979323846f / 180.0f);
    }
    return Transform3D(b, to3(p_x, p_y));
}

Node3D *make_vehicle_node(const std::string &type) {
    Node3D *n = memnew(Node3D);
    const va::VehicleSpec *sp = va::vehicle_of(type);
    const float L = sp->len * S;
    const float W = sp->wid * S;

    const Color body = (type == "tank") ? Color(0.20f, 0.22f, 0.18f)
                     : (type == "truck") ? Color(0.28f, 0.26f, 0.20f)
                                         : Color(0.22f, 0.24f, 0.20f);
    const float hull_h = (type == "tank") ? 0.62f : 0.52f;
    const float hull_y = (type == "tank") ? 0.62f : 0.62f;

    Ref<BoxMesh> hull = memnew(BoxMesh);
    hull->set_size(Vector3(L, hull_h, W));
    add_mesh(n, hull, Vector3(0, hull_y, 0), mat_solid(body, 0.72f, 0.35f));

    if (type == "tank") {
        Ref<CylinderMesh> turret = memnew(CylinderMesh);
        turret->set_top_radius(W * 0.42f);
        turret->set_bottom_radius(W * 0.50f);
        turret->set_height(0.30f);
        add_mesh(n, turret, Vector3(0, 1.08f, 0), mat_solid(body.darkened(0.08f), 0.68f, 0.45f));
        Ref<CylinderMesh> barrel = memnew(CylinderMesh);
        barrel->set_top_radius(0.055f);
        barrel->set_bottom_radius(0.065f);
        barrel->set_height(1.9f);
        add_mesh(n, barrel, Vector3(0.95f, 1.08f, 0), mat_solid(body.darkened(0.2f), 0.5f, 0.7f), Vector3(0, 0, 90));
    } else if (type == "truck") {
        Ref<BoxMesh> cab = memnew(BoxMesh);
        cab->set_size(Vector3(L * 0.30f, 0.52f, W * 0.96f));
        add_mesh(n, cab, Vector3(L * 0.33f, 1.14f, 0), mat_solid(body.darkened(0.12f), 0.7f, 0.3f));
        Ref<BoxMesh> canvas = memnew(BoxMesh);
        canvas->set_size(Vector3(L * 0.58f, 0.78f, W * 1.02f));
        add_mesh(n, canvas, Vector3(-L * 0.18f, 1.26f, 0), mat_solid(Color(0.24f, 0.23f, 0.18f), 0.98f));
    } else if (type == "apc") {
        Ref<BoxMesh> top = memnew(BoxMesh);
        top->set_size(Vector3(L * 0.86f, 0.26f, W * 0.88f));
        add_mesh(n, top, Vector3(-L * 0.03f, 1.02f, 0), mat_solid(body.lightened(0.05f), 0.75f, 0.35f));
        // 车顶机枪
        Ref<CylinderMesh> mgpost = memnew(CylinderMesh);
        mgpost->set_top_radius(0.035f);
        mgpost->set_bottom_radius(0.035f);
        mgpost->set_height(0.22f);
        add_mesh(n, mgpost, Vector3(L * 0.10f, 1.26f, 0), mat_solid(Color(0.12f, 0.12f, 0.13f), 0.5f, 0.6f));
    } else { // jeep
        Ref<BoxMesh> top = memnew(BoxMesh);
        top->set_size(Vector3(L * 0.52f, 0.06f, W * 0.92f));
        add_mesh(n, top, Vector3(-L * 0.06f, 1.10f, 0), mat_solid(body.lightened(0.08f), 0.8f));
    }

    // 轮子
    Ref<CylinderMesh> wheel = memnew(CylinderMesh);
    wheel->set_top_radius(W * 0.26f);
    wheel->set_bottom_radius(W * 0.26f);
    wheel->set_height(W * 0.22f);
    Ref<StandardMaterial3D> wm = mat_solid(Color(0.055f, 0.055f, 0.055f), 1.0f);
    const float wx = L * 0.32f;
    const float wy = W * 0.52f;
    const float wr = 0.30f;
    const float zs[2] = { wy, -wy };
    for (float z : zs) {
        add_mesh(n, wheel, Vector3(wx, wr, z), wm, Vector3(90, 0, 0));
        add_mesh(n, wheel, Vector3(-wx, wr, z), wm, Vector3(90, 0, 0));
    }
    return n;
}

Node3D *make_box_node() {
    Node3D *n = memnew(Node3D);
    Ref<BoxMesh> b = memnew(BoxMesh);
    b->set_size(Vector3(0.55f, 0.34f, 0.36f));
    add_mesh(n, b, Vector3(0, 0.20f, 0), mat_solid(Color(0.42f, 0.34f, 0.16f), 0.55f, 0.4f));
    return n;
}

// ------------------------------------------------------- 掩体物件
static void add_prop(Node3D *parent, const va::Prop &p) {
    const Vector3 pos = to3(p.x, p.y);
    const float r = p.r * S;
    switch (p.type) {
        case va::PropType::Rock: {
            // 低模不规则多面体 + 扁平化，贴地而不是"立起来的蛋"
            const uint32_t sd = (uint32_t)(p.x * 31 + p.y * 17);
            Ref<ArrayMesh> m = make_rock_mesh(sd, r, 0.50f);
            const Color c = hash_color(sd, 0.212f, 0.204f, 0.188f, 0.055f);
            add_mesh(parent, m, pos + Vector3(0, r * 0.26f, 0), mat_solid(c, 0.96f),
                     Vector3(0, p.x * 0.7f, 0));
            break;
        }
        case va::PropType::Tree: {
            // 树干高 9~13 m、树冠跑到视线上方 —— 见本文件头部说明：
            // 逻辑层的 p.r 只约束"俯视遮挡半径"（= 树冠的水平尺度），高度不受约束。
            va::Rng rng((uint32_t)(p.x * 73856093u) ^ (uint32_t)(p.y * 19349663u) ^ 0x9E3779B9u);
            const float h = 7.0f + rng.next() * 3.0f;
            const float trunk_r = r * 0.15f + 0.030f;
            const Color bark_c(0.238f, 0.186f, 0.130f);
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
                const float len = h * (0.085f + rng.next() * 0.075f);
                Ref<CylinderMesh> br = memnew(CylinderMesh);
                br->set_top_radius(trunk_r * 0.16f);
                br->set_bottom_radius(trunk_r * 0.58f);
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
            const float crown_r = std::max(r * 1.15f, 0.95f);
            const Vector3 top = pos + Vector3(0, h * 0.82f, 0);
            const int ncl = 7 + (int)(rng.next() * 3.99f);
            for (int i = 0; i < ncl; ++i) {
                const float cr = crown_r * (0.72f + rng.next() * 0.52f);
                Ref<SphereMesh> cn = memnew(SphereMesh);
                cn->set_radius(cr);
                cn->set_height(cr * 1.8f);
                cn->set_radial_segments(9);
                cn->set_rings(5);
                const float a = rng.next() * 6.2831853f;
                const float rad = crown_r * rng.next() * 0.78f;
                const float dy = (rng.next() - 0.36f) * h * 0.34f;
                // 叶色比第一版亮一档：第一版在背光面直接塌成纯黑，
                // 抬一点 albedo 让补光有东西可以照。
                const Color c = hash_color((uint32_t)(p.x * 13 + p.y * 29 + i * 977), 0.170f, 0.285f, 0.098f, 0.048f);
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
                const Color c = hash_color((uint32_t)(p.x * 7 + p.y * 11 + i * 131), 0.172f, 0.232f, 0.118f, 0.045f);
                add_mesh(parent, m, pos + Vector3(std::cos(a) * rad, br * 0.55f, std::sin(a) * rad),
                         mat_solid(c, 1.0f));
            }
            break;
        }
        case va::PropType::Barrel: {
            Ref<CylinderMesh> m = memnew(CylinderMesh);
            m->set_top_radius(0.30f);
            m->set_bottom_radius(0.30f);
            m->set_height(0.88f);
            add_mesh(parent, m, pos + Vector3(0, 0.44f, 0), mat_solid(Color(0.45f, 0.16f, 0.10f), 0.75f, 0.2f));
            break;
        }
        case va::PropType::Wall: {
            Ref<BoxMesh> m = memnew(BoxMesh);
            m->set_size(Vector3(r * 2.0f, 1.05f, r * 0.85f));
            add_mesh(parent, m, pos + Vector3(0, 0.52f, 0), mat_solid(Color(0.40f, 0.39f, 0.36f), 0.95f), Vector3(0, p.y * 0.3f, 0));
            break;
        }
        case va::PropType::Trench: {
            Ref<BoxMesh> m = memnew(BoxMesh);
            m->set_size(Vector3(p.w * S, 0.14f, p.h * S));
            add_mesh(parent, m, pos + Vector3(0, 0.05f, 0), mat_solid(Color(0.24f, 0.20f, 0.15f), 1.0f));
            break;
        }
    }
}

// ------------------------------------------------------- 开发期视觉调参
// 布光是"看着调"的活：每改一个能量值就重编译一轮太慢，所以留一组环境变量旋钮做快速扫描。
// 全部有合理默认值，不设任何变量时行为完全一致；正式发布可以视作不存在。
//   VA_EXPOSURE=1.30     色调映射曝光
//   VA_SUN=1.80          主光能量（"太阳有多硬"）
//   VA_FILL=0.18         冷补光能量（背光面的可读性）
//   VA_BOUNCE=0.10       地面反弹光能量
//   VA_AMBIENT=0.55      环境光能量（最影响"平不平"的一项）
//   VA_LUT=0.28          LUT 里 S 曲线的混合量（0 = 纯冷影暖光、不加对比）
//   VA_TONEMAP=aces      aces | agx | filmic | reinhardt
// Godot 的 Environment / 材质颜色都在线性空间，而"挑颜色"这件事人是按 sRGB 直觉做的。
// 把网页版的色值（FOG 表 index.html:4066、天空 zenith 表 index.html:4226）原样搬进
// 线性字段，会平白亮一大截 —— 0.62 的 sRGB 灰当线性用，等于屏幕上的 0.81。
// 第一版的雾就是这么栽的：整张画面糊成一片惨白。所有"外观色"统一走这个转换。
static Color SRGB(float r, float g, float b) {
    return Color(r, g, b).srgb_to_linear();
}

static float knob(const char *name, float def) {
    const char *v = std::getenv(name);
    if (v == nullptr || *v == '\0') return def;
    return (float)std::atof(v);
}

// ------------------------------------------------------- 色调分级 LUT
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
                const float d = (sc - lum) * knob("VA_LUT", 0.28f);
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
// 使命召唤式的战场氛围：冷调阴影 + 暖调阳光 + 强烈空气透视 + 体积光。
//
// 【为什么必须四点布光】
// 只有一盏 DirectionalLight 时，背光面除了 ambient 之外收不到任何方向性光照，
// 只要法线一转过去就塌成纯黑剪影（实测截图里树干、树冠背面全是死黑）。
// 真实世界的背光面靠天空漫射 + 地面反弹照亮，所以要显式补无阴影光源：
//     SUN    主光，暖白，投影，定调
//     FILL   冷蓝，无影，**正对主光反面**，负责把"相机看到的那一面"照出来
//     BOUNCE 暖土色，无影，自下而上，模拟大地反弹，让模型从背景里"立起来"
//     RIM    补洞光，无影，压在 SUN 与 FILL 之间那个两边都照不到的扇区上
//
// 【FILL 和 RIM 不是一回事，别合并】
// SUN 与 FILL 对冲，各自覆盖约 180°，但**覆盖边缘的权重趋近于零**：
// 取方位 φ=210°（SUN 118° 与 FILL 302° 的正中），两边的 cos 都 ≈ 0，
// 即存在两个"谁都不管"的扇区（约 210° 与 30°）。
// 竖直面（树干/油桶/载具侧板/电线杆）法线水平，正好会被扫进这两个扇区，
// 于是 RIM 单独补的就是这两处 —— 它救的是少数朝向，FILL 救的是整个逆光半区。
// 两者能量差了 3 倍多，作用面完全不同，不能互相替代。
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
    float   fog_aerial;      // 空气透视强度：远处染上天空色，是纵深感的主要来源
    float   fog_sky_affect;  // 雾对天空本身的污染程度
    float   vol_density;
    float   vol_anisotropy;
    float   exposure;
    float   saturation;
    float   contrast;
    float   glow_intensity;
    // RIM 补洞光。追加在**末尾**而不是插在中间：这张表是 25 个字段的聚合初始化，
    // 中间插一个字段会让后面每一行的值整体错位一格，而且是静默错位 ——
    // 编得过、跑得起来、只是画面微妙地不对，极难发现。加字段一律只往尾部加。
    Color   rim_color;
    float   rim_energy;
};

// 数值来源：网页版 FOG 表（index.html:4066）与天空 zenith 表（index.html:4226），
// 换算到 Godot 的线性 HDR 空间后再按色调映射特性做反向补偿。
//
// 【fill_energy 的标定过程（0.32 → 2.05）】
// 这一格原先只有 0.32，是本文件里最贵的一个错。症状：白天空下画面正中一根
// 贯穿全高的纯黑柱（树干，实测 RGB(9,15,26)，偏蓝），另外所有石块侧面也是死黑。
// 定位过程与结论：
//   1. 把 ambient_light_color 换成刺眼的洋红 (1,0,1) —— 树干变成 RGB(73,0,45)。
//      **它被环境光照到了**，之所以黑是因为**只剩环境光**。这一刀切掉了
//      "环境光路径没接上"的假设。
//   2. VA_NO_AO / VA_NO_SHADOW 逐个摘除 —— 死黑只降 0.1~0.9pp，AO 与阴影都不是主因。
//   3. 逐灯单独加压：SUN 翻 1.74 倍树干只从 L15 到 L19；FILL 翻到 2.56 直接到 L51。
//      树干对补光的响应是主光的 2 倍以上。
//   4. 把补光方位角扫一圈（VA_RIM_AZ 0..315）—— 树干在任何方位都点不亮，
//      说明它正好朝着太阳的反面；而补光方位 302° 与太阳 118° 恰好对冲，
//      也就是**"被逆光的那一面就是相机看到的那一面"**。
// 说白了：太阳负责照亮朝向它的一半世界，补光负责另一半，而补光被压到了 0.32，
// 于是"相机看向哪一面，那一面就是黑的"。这不是审美参数，是结构性欠账。
// 标定结果（VA_FILL 倍率扫描，VA_SEED=1）：
//     倍率 1.0（0.32）死黑 7.21%  树干 L15  草地 L163
//     倍率 5.0（1.60）死黑 2.09%  树干 L36  草地 L174
//     倍率 6.4（2.05）死黑 1.95%  树干 L43  草地 L177   ← 取这一档
//     倍率 8.0（2.56）死黑 1.85%  树干 L51  草地 L181
// 取 6.4：死黑从 7.21% 降到 1.95%（过曝仍 0.30%），而草地只从 L163 到 L177 ——
// 代价小、收益最大的一档。雨天/夜战按同一比例缩放。
static const WeatherLook LOOKS[] = {
    // ---------------- 晴天：能见度高，暖光冷影，对比强 ----------------
    { "sunny",
      Vector3(-58.0f, 118.0f, 0.0f), SRGB(1.000f, 0.960f, 0.900f), 2.30f,
      Color(0.44f, 0.52f, 0.66f), 1.62f,
      Color(0.60f, 0.73f, 0.94f), 2.05f, 0.16f,
      SRGB(0.300f, 0.440f, 0.620f), SRGB(0.680f, 0.700f, 0.680f), SRGB(0.340f, 0.320f, 0.280f), 1.00f,
      SRGB(0.520f, 0.570f, 0.600f), 0.0055f, 0.028f, 0.20f, 0.30f, 0.12f,
      0.0050f, 0.32f,
      1.32f, 1.02f, 1.08f, 0.42f,
      SRGB(0.560f, 0.640f, 0.800f), 0.62f },

    // ---------------- 雨天：低压冷灰，对比低、空气浑 ----------------
    { "rain",
      Vector3(-66.0f, 138.0f, 0.0f), SRGB(0.900f, 0.930f, 0.970f), 0.95f,
      Color(0.50f, 0.55f, 0.60f), 1.22f,
      Color(0.72f, 0.78f, 0.86f), 1.73f, 0.12f,
      SRGB(0.260f, 0.300f, 0.330f), SRGB(0.500f, 0.520f, 0.530f), SRGB(0.260f, 0.270f, 0.260f), 0.92f,
      SRGB(0.400f, 0.440f, 0.470f), 0.0125f, 0.052f, 0.06f, 0.40f, 0.20f,
      0.0100f, 0.10f,
      1.62f, 0.84f, 0.95f, 0.26f,
      SRGB(0.620f, 0.680f, 0.760f), 0.42f },

    // ---------------- 夜战：月光 + 深蓝，能见度最差 ----------------
    { "night",
      Vector3(-30.0f, 108.0f, 0.0f), SRGB(0.620f, 0.720f, 0.950f), 0.55f,
      Color(0.16f, 0.22f, 0.34f), 0.60f,
      Color(0.42f, 0.55f, 0.85f), 0.70f, 0.08f,
      SRGB(0.020f, 0.035f, 0.075f), SRGB(0.075f, 0.090f, 0.130f), SRGB(0.045f, 0.050f, 0.062f), 0.80f,
      SRGB(0.055f, 0.075f, 0.115f), 0.0055f, 0.028f, 0.12f, 0.35f, 0.12f,
      0.0045f, 0.25f,
      1.50f, 0.92f, 1.05f, 0.55f,
      SRGB(0.300f, 0.400f, 0.620f), 0.30f },
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
    sky_mat->set_sky_cover(tex_clouds());
    sky_mat->set_sky_cover_modulate(Color(0.86f, 0.88f, 0.92f, 1.0f));
    out.sky_mat = sky_mat;

    Ref<Sky> sky;
    sky.instantiate();
    sky->set_material(sky_mat);
    sky->set_radiance_size(Sky::RADIANCE_SIZE_256);
    sky->set_process_mode(Sky::PROCESS_MODE_REALTIME);

    env->set_background(Environment::BG_SKY);
    env->set_sky(sky);
    env->set_reflection_source(Environment::REFLECTION_SOURCE_SKY);

    // 环境光：负责把「太阳照不到的那一半世界」从纯黑里捞回来。
    // 来源可切，便于 A/B：VA_AMB_SRC=sky|color（默认 sky，保持原行为）
    //   sky   —— 天空辐照度按 sky_contribution 混入手调色
    //   color —— 完全走 ambient_light_color × ambient_light_energy 这条确定路径
    {
        const char *src = std::getenv("VA_AMB_SRC");
        const std::string s = (src != nullptr && *src != '\0') ? std::string(src) : std::string("sky");
        env->set_ambient_source(s == "color" ? Environment::AMBIENT_SOURCE_COLOR
                                             : Environment::AMBIENT_SOURCE_SKY);
    }
    /* sky_contribution：天空辐照度占环境光的比例。
       为什么默认压到 0：实测 sky_contribution=0.72 时，把 ambient_light_energy
       乘 40 倍，草地只从 RGB(79,96,128) 变到 (82,98,131)，但掩体背光面从
       RGB(4,8,20) 直接恢复正常 —— 说明**被照面靠太阳（能量 2.30）撑着、
       背光面几乎无光**，中间那一档本该由这里补。天空辐照度这条路径在本工程里
       观测不到有效贡献，与其依赖一个测不准的量，不如关掉它、把环境色做成
       一个能直接调的确定值。 */
    env->set_ambient_light_sky_contribution(knob("VA_AMB_SKY", 0.0f));

    // 雾：指数雾打底做纵深，再叠高度雾做"谷地晨霭"
    env->set_fog_enabled(true);
    env->set_fog_mode(Environment::FOG_MODE_EXPONENTIAL);
    env->set_fog_light_energy(0.90f);
    env->set_fog_height(3.0f);

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
    // 色调映射可选：AgX 高光滚降最平滑，但去饱和极重 —— 实测把树冠的绿
    // 洗成了灰蓝、岩石洗成石膏白。ACES 保留色相、对比更硬，更接近使命召唤。
    // 留一个旋钮方便 A/B：VA_TONEMAP=aces|agx|filmic|reinhardt
    {
        const char *tm = std::getenv("VA_TONEMAP");
        const std::string t = (tm && *tm) ? std::string(tm) : std::string("aces");
        if (t == "agx") env->set_tonemapper(Environment::TONE_MAPPER_AGX);
        else if (t == "filmic") env->set_tonemapper(Environment::TONE_MAPPER_FILMIC);
        else if (t == "reinhardt") env->set_tonemapper(Environment::TONE_MAPPER_REINHARDT);
        else env->set_tonemapper(Environment::TONE_MAPPER_ACES);
    }
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
    // VA_NO_AO=1 可整体关掉 AO 链（SSAO + SSIL）：AO 是乘在环境光上的，
    // 它要是过强，表现就是「环境光怎么加都点不亮暗部」——和当前这个 bug 的
    // 症状完全一致，所以必须能单独摘掉它验一验。
    const bool no_ao = (std::getenv("VA_NO_AO") != nullptr);
    env->set_ssao_enabled(!no_ao);
    env->set_ssao_radius(1.1f);
    env->set_ssao_power(1.45f);
    env->set_ssao_detail(0.35f);
    env->set_ssao_horizon(0.06f);
    env->set_ssao_sharpness(0.98f);
    env->set_ssao_direct_light_affect(0.12f);   // 只在背光面生效，受光面几乎不动
    env->set_ssao_ao_channel_affect(0.0f);

    // 屏幕空间间接光：把周围环境的颜色"渗"进暗部，进一步消死黑。
    // 权重压得很低，只补一口气，不做主光。
    env->set_ssil_enabled(!no_ao);
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
    // VA_NO_SHADOW=1：给太阳摘掉阴影贴图，用来判定「死黑的那些像素」
    // 到底是阴影里的表面，还是根本不参与光照的东西。
    sun->set_shadow(std::getenv("VA_NO_SHADOW") == nullptr);
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

    // RIM —— 补洞光。这一盏不是"再来点氛围"，它是**结构性必需**的。
    //
    // 【为什么三盏灯必然漏一个扇区】
    // 对一根竖直圆柱（树干、电线杆、油桶、载具侧面）法线是水平的，N·L 完全由
    // 方位角差决定。SUN 方位 118°、FILL 302°（正好对冲）、BOUNCE 46° 且能量只有
    // 0.16。取方位 φ=210°（SUN 与 FILL 的正中）：
    //     cos(210-118) ≈ -0.03   SUN   → 0
    //     cos(210-302) ≈ -0.03   FILL  → 0
    //     cos(210-46)  <  0      BOUNCE→ 0
    // **三盏灯全部 N·L ≤ 0**，那个朝向的竖直面一点直射光都收不到。
    // 实测症状：树干 RGB(9,15,26)（偏蓝 —— 因为只剩蓝色的环境光），
    // 白天空下是一根贯穿全高的纯黑剪影。玩家鼠标一转就能看进这个洞里。
    //
    // 【为什么方位角是"太阳+88°"而不是写死一个数】
    // SUN 与 FILL 永远对冲，所以洞的中心永远在两者正中 = 太阳方位 +88°。
    // 三套天气配方的太阳方位各不相同（118/138/108），写死数值会在换天气时失配，
    // 于是这里直接从 L->sun_rot_deg.y 推出来 —— 太阳转到哪，补洞光跟到哪。
    //
    // 仰角取 -20°（略俯），这样它同时还能擦亮一点朝上的面，不至于只救竖直面。
    DirectionalLight3D *rim = memnew(DirectionalLight3D);
    rim->set_name("Rim");
    rim->set_param(Light3D::PARAM_INDIRECT_ENERGY, 0.0f);
    rim->set_param(Light3D::PARAM_SPECULAR, 0.25f);
    rim->set_sky_mode(DirectionalLight3D::SKY_MODE_LIGHT_ONLY);
    rim->set_shadow(false);
    root->add_child(rim);
    out.rim = rim;

    // 节点建完必须立刻把当前天气那一套参数打上去（声明在头文件里，定义在本函数之后）。
    // 漏掉这一步的后果非常隐蔽：Godot 的 Environment 默认 fog_density=0.01，
    // 比晴天配方浓 3 倍多，整张画面会被一层均匀奶白盖住 —— 而且因为盖住之后
    // 所有区域亮度趋同，会让人误判成"光照太平"，跑去反复调光源（实测就是这么绕远的）。
    apply_weather(out);
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
        // 环境色可覆盖，专供取证：VA_AMB_COLOR="r,g,b"（线性 0~1）。
        // 用途是把环境光换成刺眼的洋红之类 —— 如果画面完全不变，说明这条路
        // 根本没接上；如果画面整体染上洋红，说明接上了、只是原来的基数偏小。
        // 这是唯一能一刀切开「路径断」与「量太小」两种假设的办法。
        Color amb_c = L->ambient_color;
        const char *ac = std::getenv("VA_AMB_COLOR");
        if (ac != nullptr && *ac != '\0') {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (std::sscanf(ac, "%f,%f,%f", &r, &g, &b) == 3) amb_c = Color(r, g, b);
        }
        env->set_ambient_light_color(amb_c);
        env->set_ambient_light_energy(L->ambient_energy * k_amb);

        env->set_fog_light_color(L->fog_color);
        env->set_fog_density(L->fog_density * k_fog);
        env->set_fog_height_density(L->fog_height_density);
        env->set_fog_sun_scatter(L->fog_sun_scatter);
        env->set_fog_aerial_perspective(L->fog_aerial);
        env->set_fog_sky_affect(L->fog_sky_affect);

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
    if (refs.rim != nullptr) {
        // 方位角默认 = 太阳方位 + 88°（SUN 与 FILL 的正中，也就是三灯理论上漏掉的扇区中心）。
        // VA_RIM_AZ 可以**绝对覆盖**方位角：扫一圈就能实测出这个洞到底在哪一侧，
        // 比拿纸笔推欧拉角可靠 —— 引擎的 rotation_degrees→光方向 映射里
        // 手性/轴向很容易推错，而"扫一圈看树干哪一档变亮"是不会骗人的。
        const float az = knob("VA_RIM_AZ", L->sun_rot_deg.y + 88.0f);
        const float el = knob("VA_RIM_EL", -20.0f);
        refs.rim->set_rotation_degrees(Vector3(el, az, 0.0f));
        refs.rim->set_color(L->rim_color);
        refs.rim->set_param(Light3D::PARAM_ENERGY, L->rim_energy * knob("VA_RIM", 1.0f));
    }

    // 读回一次，把「我以为打进去的」和「引擎真正拿到的」对上账。
    // 这一步必须放在函数**最后** —— 上一版把它插在 ambient 之后，
    // 结果 exposure 打印出 1.0（真实值 1.32，还没轮到那行赋值），
    // 差点被当成「曝光设置没生效」去追一个不存在的 bug。
    if (env.is_valid()) {
        UtilityFunctions::print(String("[env] ambient src="), (int)env->get_ambient_source(),
                                String(" sky_contrib="), env->get_ambient_light_sky_contribution(),
                                String(" color="), env->get_ambient_light_color(),
                                String(" energy="), env->get_ambient_light_energy(),
                                String(" exposure="), env->get_tonemap_exposure(),
                                String(" tonemap="), (int)env->get_tonemapper(),
                                String(" ae="), env->is_adjustment_enabled(),
                                String(" ssao="), env->is_ssao_enabled(),
                                String(" ssil="), env->is_ssil_enabled(),
                                String(" fog="), env->get_fog_density());
        // 四盏灯的能量也报出来。教训：改完配方只截图看画面，是看不出
        // "这次改的值到底有没有编进去"的 —— 实测有两处 Edit 被 NTFS 丢更新静默吞掉，
        // 而画面"看起来差不多"，白跑了一整轮扫描。参数读回是唯一可靠的对照。
        UtilityFunctions::print(String("[light] sun="),
                                (refs.sun != nullptr ? refs.sun->get_param(Light3D::PARAM_ENERGY) : -1.0f),
                                String(" fill="),
                                (refs.fill != nullptr ? refs.fill->get_param(Light3D::PARAM_ENERGY) : -1.0f),
                                String(" bounce="),
                                (refs.bounce != nullptr ? refs.bounce->get_param(Light3D::PARAM_ENERGY) : -1.0f),
                                String(" rim="),
                                (refs.rim != nullptr ? refs.rim->get_param(Light3D::PARAM_ENERGY) : -1.0f),
                                String(" rim_az="),
                                (refs.rim != nullptr ? refs.rim->get_rotation_degrees().y : -1.0f));
    }
}


// ------------------------------------------------------- 主入口
void build_scene(Node3D *root, SceneRefs &out) {
    out.root = root;

    // ---- 天空 / 环境 / 光照（三点布光 + 三级调色，见 build_environment）----
    build_environment(root, out);


    // ---- 地面 ----
    Node3D *ground = memnew(Node3D);
    ground->set_name("Ground");
    {
        Ref<PlaneMesh> pm = memnew(PlaneMesh);
        // 铺到 700 m：原来只比战场大 40 m（≈150×105），地平线其实就是"地板的边"，
        // 一眼就看出场景很小。铺大之后交给雾去收尾，再叠远景山脊做层次。
        pm->set_size(Vector2(700.0f, 700.0f));
        pm->set_subdivide_width(1);
        pm->set_subdivide_depth(1);
        Ref<StandardMaterial3D> gm;
        gm.instantiate();
        gm->set_albedo(Color(1, 1, 1));
        gm->set_texture(StandardMaterial3D::TEXTURE_ALBEDO, tex_grass());
        gm->set_uv1_scale(Vector3(132, 132, 1));   // 贴图密度与原来一致（700/150×28≈132）
        gm->set_roughness(0.98f);
        MeshInstance3D *mi = memnew(MeshInstance3D);
        mi->set_mesh(pm);
        mi->set_position(to3(va::CFG.W * 0.5f, va::CFG.H * 0.5f));
        mi->set_material_override(gm);
        ground->add_child(mi);
    }
    root->add_child(ground);

    // ---- 远景山脊：打破死板地平线，配合雾形成层叠剪影 ----
    {
        Ref<ArrayMesh> rm = make_ridges_mesh(va::CFG.W * 0.5f * S, va::CFG.H * 0.5f * S);
        Ref<StandardMaterial3D> rmm = mat_solid(Color(0.105f, 0.125f, 0.115f), 1.0f);
        add_mesh(root, rm, Vector3(), rmm);
    }

    // ---- 河流 ----
    {
        Ref<BoxMesh> rm = memnew(BoxMesh);
        rm->set_size(Vector3((va::CFG.riverX2 - va::CFG.riverX1) * S, 0.10f, va::CFG.H * S));
        Ref<StandardMaterial3D> wm;
        wm.instantiate();
        wm->set_albedo(Color(0.9f, 0.95f, 1.0f));
        wm->set_texture(StandardMaterial3D::TEXTURE_ALBEDO, tex_water());
        wm->set_uv1_scale(Vector3(3, 40, 1));
        wm->set_roughness(0.12f);
        wm->set_metallic(0.25f);
        MeshInstance3D *mi = memnew(MeshInstance3D);
        mi->set_mesh(rm);
        mi->set_position(to3((va::CFG.riverX1 + va::CFG.riverX2) * 0.5f, va::CFG.H * 0.5f, -0.04f));
        mi->set_material_override(wm);
        ground->add_child(mi);
    }

    // ---- 公路（沿 ROAD_PATH 铺一条带） ----
    Node3D *road = memnew(Node3D);
    road->set_name("Road");
    for (int i = 0; i + 1 < va::ROAD_PATH_N; ++i) {
        const float x1 = va::ROAD_PATH[i][0], y1 = va::ROAD_PATH[i][1];
        const float x2 = va::ROAD_PATH[i + 1][0], y2 = va::ROAD_PATH[i + 1][1];
        const float len = va::distf(x1, y1, x2, y2);
        if (len < 1.0f) continue;
        const float cx = (x1 + x2) * 0.5f, cy = (y1 + y2) * 0.5f;
        const float ang = std::atan2(y2 - y1, x2 - x1);
        Ref<BoxMesh> bm = memnew(BoxMesh);
        bm->set_size(Vector3(len * S + 0.2f, 0.12f, (va::CFG.roadBot - va::CFG.roadTop) * S));
        Ref<StandardMaterial3D> rmat;
        rmat.instantiate();
        rmat->set_albedo(Color(1, 1, 1));
        rmat->set_texture(StandardMaterial3D::TEXTURE_ALBEDO, tex_road());
        rmat->set_uv1_scale(Vector3(len * S * 0.35f, 2.2f, 1));
        rmat->set_roughness(0.92f);
        MeshInstance3D *mi = memnew(MeshInstance3D);
        mi->set_mesh(bm);
        mi->set_position(to3(cx, cy, 0.02f));
        mi->set_rotation_degrees(Vector3(0, -ang * 180.0f / 3.141592653589793f, 0));
        mi->set_material_override(rmat);
        road->add_child(mi);
    }
    // 中央虚线
    {
        Ref<StandardMaterial3D> line_m = mat_solid(Color(0.62f, 0.60f, 0.50f), 0.85f);
        for (int i = 0; i + 1 < va::ROAD_PATH_N; ++i) {
            const float x1 = va::ROAD_PATH[i][0], y1 = va::ROAD_PATH[i][1];
            const float x2 = va::ROAD_PATH[i + 1][0], y2 = va::ROAD_PATH[i + 1][1];
            const float len = va::distf(x1, y1, x2, y2);
            if (len < 1.0f) continue;
            const int n = (int)(len / 80.0f);
            const float ang = std::atan2(y2 - y1, x2 - x1);
            for (int k = 0; k < n; k += 2) {
                const float t = (k + 0.5f) / (float)n;
                const float cx = va::lerpf(x1, x2, t), cy = va::lerpf(y1, y2, t);
                Ref<BoxMesh> bm = memnew(BoxMesh);
                bm->set_size(Vector3(1.5f, 0.03f, 0.14f));
                MeshInstance3D *mi = memnew(MeshInstance3D);
                mi->set_mesh(bm);
                mi->set_position(to3(cx, cy, 0.05f));
                mi->set_rotation_degrees(Vector3(0, -ang * 180.0f / 3.141592653589793f, 0));
                mi->set_material_override(line_m);
                road->add_child(mi);
            }
        }
    }
    root->add_child(road);

    // ---- 桥 ----
    {
        Ref<BoxMesh> bm = memnew(BoxMesh);
        bm->set_size(Vector3((va::CFG.riverX2 - va::CFG.riverX1 + 24) * S, 0.30f,
                             (va::CFG.bridgeY2 - va::CFG.bridgeY1) * S));
        MeshInstance3D *mi = memnew(MeshInstance3D);
        mi->set_mesh(bm);
        mi->set_position(to3((va::CFG.riverX1 + va::CFG.riverX2) * 0.5f,
                             (va::CFG.bridgeY1 + va::CFG.bridgeY2) * 0.5f, 0.16f));
        mi->set_material_override(mat_solid(Color(0.33f, 0.32f, 0.30f), 0.92f));
        ground->add_child(mi);
    }

    // ---- 掩体物件 ----
    Node3D *props = memnew(Node3D);
    props->set_name("Props");
    for (const auto &p : va::W.props) add_prop(props, p);
    root->add_child(props);
    out.props = props;
    out.root = root;

    // ---- 动态实体挂载点 ----
    out.units = memnew(Node3D);    out.units->set_name("Units");       root->add_child(out.units);
    out.vehicles = memnew(Node3D); out.vehicles->set_name("Vehicles"); root->add_child(out.vehicles);
    out.decals = memnew(Node3D);   out.decals->set_name("Decals");     root->add_child(out.decals);
}

} // namespace volunteer_army
