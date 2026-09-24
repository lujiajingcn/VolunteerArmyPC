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

// ============================================================================
// 峡谷地形
// ============================================================================
//
// 【为什么地形只存在于渲染层】
// 逻辑层 (src/sim/*) 是一张 2D 平面：单位只有 (x,y)，掩体只有 (x,y,r)，没有高度维；
// 子弹的遮挡同样是 2D 的 —— `va_combat.cpp` 用 segCircle 撞掩体圆，不看高度。
// 所以这里加的山坡**只改画面**：单位的 (x,y)、掩体的 (x,y) 与 p.r、判定顺序
// 一个都没动，`src/sim/` 一行未改。
//
// 高度由一个**纯函数** ground_h() 给出，地形网格与单位/掩体摆位**共用同一个函数**：
// 两边各写一份，迟早出现"人埋在坡里 / 石头浮在半空"这类只有靠截图才能发现的偏差。
//
// 【形状】公路（逻辑 y=650 → 32.5 米）压在谷底正中，两侧按剖面抬升成山坡，
// 于是公路位于一条两侧上坡夹出的峡谷里；河道（逻辑 x 306~414 → 15.3~20.7 米）
// 处把两侧谷壁切出一道缺口，让河从谷壁里穿出来（水口），桥正好架在缺口上。
//
// 【一个必须交代的取舍：为什么地图内的抬升是克制的】
// 子弹挡在掩体上靠 2D 圆判定、不看高度。若可走区域里把地面抬得很高，站在高处的射手
// 就会遇到"瞄准线明明越过了石头，子弹却停在石头上"——视线与判定脱节（平地没有这个问题，
// 因为所有人都是同一高度）。所以剖面把**两个高度拆成两个旋钮**：
//   · VA_TERRAIN_IN  = 地图边界（d = 32.5 米，`passable()` 把单位锁在地图里）处的高度，
//     也就是**玩家走得到的最高点**，默认 **4.5 米** —— 脱节幅度被压在 1 米量级。
//   · VA_TERRAIN_OUT = 谷壁峰值（d = 48 米，地图外，玩家走不到），默认 **18 米**。
//     纯背景，不受上面那条约束；而它正是玩家从谷底看出去的那道天际线（视高角 ≈ 18°），
//     峡谷感由它承担。
// 想更激进就调大 VA_TERRAIN_OUT（只动画面）；调大 VA_TERRAIN_IN 则要同时接受上面那条脱节。
//
// 【一个必须交代的第二个取舍：峡谷为什么在东西两端收口】
// 地形网格是有限的。若剖面沿 x 一路铺到网格边界，边界处就凭空多一道 15 米断崖。
// 所以 x ∈ [-6, 122] 米保持全高、往两端 smoothstep 收口 —— 顺带也讲得通：
// 公路从谷口穿出去（东边来车、西边是 C 点撤离线）。
const float kRoadCy  = va::CFG.roadCY * S;                              // 32.5 米
const float kRiverCx = (va::CFG.riverX1 + va::CFG.riverX2) * 0.5f * S;  // 18.0 米

struct TerrainKnobs {
    bool  on    = true;
    // 这两个高度必须分开给，因为受的约束完全不同（见上面那段取舍说明）：
    float h_in  = 4.5f;    // d = 地图边界（32.5 米）处的高度 = **玩家走得到的最高点**
    float h_out = 18.0f;   // d = 48 米处的谷壁峰值 = **地图外，纯背景**
    float bump  = 0.35f;   // 横向起伏强度（0 = 完美棱柱，越大越自然）
    bool  gorge = true;    // 河道处是否开缺口
};

// 「默认开、写 0 才关」—— 与工程里其它开关同一套语义（见 README 的旋钮表）
static bool env_off(const char *p_name) {
    const char *e = std::getenv(p_name);
    return e != nullptr && e[0] == '0' && e[1] == '\0';
}

static const TerrainKnobs &tknobs() {
    static const TerrainKnobs k = [] {
        TerrainKnobs t;
        t.on    = !env_off("VA_TERRAIN");
        t.gorge = !env_off("VA_TERRAIN_GORGE");
        if (const char *v = std::getenv("VA_TERRAIN_IN"))   t.h_in  = va::clampf((float)std::atof(v), 0.0f, 40.0f);
        if (const char *v = std::getenv("VA_TERRAIN_OUT"))  t.h_out = va::clampf((float)std::atof(v), 0.0f, 90.0f);
        if (const char *v = std::getenv("VA_TERRAIN_BUMP")) t.bump  = va::clampf((float)std::atof(v), 0.0f, 1.0f);
        return t;
    }();
    return k;
}

static float sstep(float t) { t = va::clampf(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }

// 距路中心多远开始起坡（米）。路本身宽 7 米，所以坡脚离路肩还有 4.5 米平地。
static constexpr float kFoot = 8.0f;

// 横向剖面。用错开的 smoothstep 相加而不是查表插值：查表 + 逐段 smoothstep 会在每个
// 控制点留下零导数（画面上一圈圈"梯田"），错开相加既光滑又只有几行。
static float profile_h(float d, float h_in, float h_out) {
    const float kEdge  = va::CFG.H * 0.5f * S;   // 32.5 米：地图边界，正好是"能走到的最高处"
    const float kCrest = 48.0f;                  // 谷壁峰值所在的横向距离（地图外）
    const float kBack  = 100.0f;                 // 缓降回平地
    float h = h_in * sstep((d - kFoot) / (kEdge - kFoot));
    h += (h_out - h_in) * sstep((d - kEdge) / (kCrest - kEdge));
    h -= h_out * sstep((d - (kCrest + 4.0f)) / (kBack - kCrest - 4.0f));
    return h;
}

// 平滑起伏。必须是**纯函数、无状态**：网格建顶点和每帧摆单位会各调一次，
// 用带状态的 va::Rng 就会两边取到不同的值 —— 那正是"人陷进坡里"的来源。
static float bnoise(float x, float y) {
    float s = 0.52f * std::sin(x * 0.081f + y * 0.062f);
    s += 0.27f * std::sin(x * 0.163f - y * 0.131f + 1.7f);
    s += 0.13f * std::sin(x * 0.297f + y * 0.271f + 3.1f);
    s += 0.08f * std::sin(x * 0.612f - y * 0.533f + 5.2f);
    return va::clampf(0.5f + 0.5f * s, 0.0f, 1.0f);
}

// 东西两端收口（见上文的第二个取舍）
static float x_taper(float x) {
    if (x < -6.0f)  return sstep((x + 55.0f) / 49.0f);    // -55 → -6 米由 0 升到 1
    if (x > 122.0f) return sstep((172.0f - x) / 50.0f);   // 122 → 172 米由 1 降到 0
    return 1.0f;
}

// 河道缺口：河心两侧 5 米内削平（河宽 5.4 米 + 两条岸），13 米外恢复原坡
static float gorge_mask(float x) {
    const float xr = std::fabs(x - kRiverCx);
    if (xr <= 5.0f) return 0.0f;
    return sstep((xr - 5.0f) / 8.0f);
}

// 米制入参（x 沿路、y 垂直路），返回地面高度（米）
static float terrain_raw(float x, float y) {
    const TerrainKnobs &k = tknobs();
    const float d = std::fabs(y - kRoadCy);
    float h = profile_h(d, k.h_in, k.h_out);
    if (h <= 0.0f) return 0.0f;                 // 谷底严格 0：公路/桥/河道不能被顶起来
    h *= x_taper(x);
    if (k.gorge) h *= gorge_mask(x);
    if (k.bump > 0.0f) h *= (1.0f + k.bump * (bnoise(x, y) - 0.5f));
    return h;
}

bool terrain_enabled() { return tknobs().on; }

// ---- 对外入口：逻辑层坐标进，地面高度（米）出 ----
float ground_h(float lx, float ly) {
    if (!tknobs().on) return 0.0f;
    return terrain_raw(lx * S, ly * S);
}

// 非均匀网格：缺口处加密（x 方向高度靠缺口掩码变化），坡面陡段加密（y 方向靠剖面变化）。
static Ref<ArrayMesh> make_terrain_mesh() {
    std::vector<float> xs, ys;
    auto fill = [](std::vector<float> &v, float lo, float hi, float flo, float fhi, float fs, float cs) {
        for (float x = lo; x < hi - 1e-3f; ) {
            v.push_back(x);
            x += (x >= flo && x < fhi) ? fs : cs;
        }
        v.push_back(hi);
    };
    fill(xs, -58.0f, 175.0f,  6.0f,  30.0f, 0.8f, 2.5f);
    fill(ys, -80.0f, 145.0f, -40.0f, 105.0f, 1.0f, 4.0f);

    const float TEX_M = 700.0f / 132.0f;   // 与原 700×700 地板同样的贴图密度（每 5.3 米一重复）
    const float wall = std::max(tknobs().h_out, 0.001f);

    Ref<SurfaceTool> st;
    st.instantiate();
    st->begin(Mesh::PRIMITIVE_TRIANGLES);
    auto vtx = [&](float x, float y) {
        const float h = terrain_raw(x, y);
        const float t = va::clampf(h / wall, 0.0f, 1.0f);
        const float nv = bnoise(x * 1.9f, y * 1.9f);
        /* 顶点色只做很轻的染色（三通道均值都≈1、只在高处略偏干土）：
           这套布光/曝光是标定过的（见 README 的材质三旋钮那段），染色一大就把标定推翻。
           ACES 有跨通道耦合，所以这里只敢给"方向 + 很小幅度"，不按通道反推目标色。 */
        const Color c(va::clampf(0.985f + 0.055f * t + 0.035f * (nv - 0.5f), 0.0f, 1.0f),
                      va::clampf(0.995f + 0.015f * t + 0.035f * (nv - 0.5f), 0.0f, 1.0f),
                      va::clampf(0.955f - 0.115f * t + 0.035f * (nv - 0.5f), 0.0f, 1.0f));
        st->set_uv(Vector2(x / TEX_M, y / TEX_M));
        st->set_color(c);
        st->add_vertex(Vector3(x, h, y));   // 注意：单位是**米**，与 to3 的 h 同一套（不乘 S）
    };
    for (size_t i = 0; i + 1 < xs.size(); ++i) {
        for (size_t j = 0; j + 1 < ys.size(); ++j) {
            const float x0 = xs[i], x1 = xs[i + 1], y0 = ys[j], y1 = ys[j + 1];
            vtx(x0, y0); vtx(x1, y0); vtx(x1, y1);
            vtx(x0, y0); vtx(x1, y1); vtx(x0, y1);
        }
    }
    /* index() 把重合顶点合并，generate_normals() 才会给**平滑**法线；
       不 index 的话每个三角形各算各的，坡面会是一片片硬边。 */
    st->index();
    st->generate_normals();
    return st->commit();
}

// 只重建掩体层（见头文件说明）
// add_prop 定义在文件后半段，这里先声明（掩体建模函数很长，不搬家了）
// 第二个参数是**地物三维模型原型的挂载点**（传 refs.units，场景里已有的容器）：
// 原型必须挂进场景树，否则它持有的 mesh / 材质 / 贴图在进程退出时会被 Godot
// 报成 "RID allocations ... leaked at exit"，而"日志里有没有 ERROR"正是本工程的
// 回归判据，被这种假错误污染之后真问题就看不见了（角色/武器/载具三条都踩过）。
static void add_prop(Node3D *parent, Node *p_proto_parent, const va::Prop &p);

void rebuild_props(Node3D *root, SceneRefs &refs) {
    if (refs.props != nullptr) {
        refs.props->queue_free();
        refs.props = nullptr;
    }
    Node3D *props = memnew(Node3D);
    props->set_name("Props2");
    for (const auto &p : va::W.props) add_prop(props, refs.units, p);
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

static Node3D *load_glb_root(const String &p_path, const char *p_tag) {
    // 先判存在再解析：直接解析一个不存在的路径会在 stderr 打一行 ERROR，
    // 而"日志里有没有 ERROR"是本工程的回归判据之一，不能被这种假错误污染。
    // 整条函数都用 print 而不是 push_error，理由同上：缺素材是**可预期**的情况
    // （武器参考图不入库、模型还没生成），不该把它变成"回归失败"。
    if (!FileAccess::file_exists(p_path)) {
        UtilityFunctions::print(String("[") + String(p_tag) + String::utf8("] 缺模型文件 "), p_path);
        return nullptr;
    }

    Ref<GLTFDocument> doc;
    doc.instantiate();
    Ref<GLTFState> st;
    st.instantiate();
    if (doc.is_null() || st.is_null()) {
        UtilityFunctions::print(String("[") + String(p_tag) + String::utf8("] GLTFDocument 不可用"));
        return nullptr;
    }

    const Error err = doc->append_from_file(p_path, st);
    if (err != OK) {
        UtilityFunctions::print(String("[") + String(p_tag) + String::utf8("] 模型解析失败 "),
                                p_path, " err=", (int)err);
        return nullptr;
    }
    Node *scene = doc->generate_scene(st);
    Node3D *raw = Object::cast_to<Node3D>(scene);
    if (raw == nullptr) {
        UtilityFunctions::print(String("[") + String(p_tag) + String::utf8("] 模型没有可用的场景根 "), p_path);
        return nullptr;
    }
    // 注意：这里**不判"有没有网格"**。角色要按身高缩放、武器要按全长缩放，
    // 两者对"空模型"的判据不一样，交给各自的调用方。
    return raw;
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
    Node3D *raw = load_glb_root(path, "unit");
    if (raw == nullptr) {
        s_unit_failed[p_key] = true;
        return nullptr;
    }

    // 量包围盒。glTF 的节点自带 +90°绕X（Z-up → Y-up），
    // 所以这里量到的就是引擎空间的尺寸，不必再自己转。
    AABB box;
    bool has = false;
    collect_aabb(raw, Transform3D(), box, has);
    if (!has || box.size.y <= 1e-5f) {
        UtilityFunctions::print(String::utf8("[unit] 模型没有网格 "), path);
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
        UtilityFunctions::print(String::utf8("[unit] 警告：没有原型挂载点，模型资源会在退出时报泄漏"));
    }

    UtilityFunctions::print(String::utf8("[unit] 模型 "), String::utf8(p_key.c_str()),
                            String::utf8(" 原始包围盒 "), box.size,
                            String::utf8(" 缩放 "), k, " yaw ", yaw_deg);
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

// ---------------------------------------------- 武器三维模型（图生3D 产物）
//
// 参考图与生成方式：tools/fetch_wpn_refs.sh（取图）+ tools/gen3d_batch.py --kind wpn
// （生成）→ assets/art/wpn/model/<键>.glb。
//
// 【与角色模型最本质的差别：枪有明确的"前后"】
// 角色模型是"正面朝哪"的问题，枪是"枪口朝哪"的问题 —— 后者判错了，
// 玩家手里就横着一根棍子，而且枪口焰会从枪托那头喷出来。
//
// 【归一化的目标坐标系】= viewmodel.cpp 里 gun 节点的局部系：
//     原点在**枪托尾端平面**，枪口朝 **-Z**，y≈0 是枪管轴线，单位是**米**。
// 与那套程序化枪模完全同构，所以接真模型**不需要改动任何姿态/后坐/开镜数学** ——
// 位移与姿态全都作用在 root / gun 两层上，与挂在这一层下面的是什么网格无关。
//
// 【为什么按"真枪全长"缩放，而不是塞进某个固定长度】
// 世界坐标里 1 单位 = 0.05 米、士兵 1.68 米，枪也必须按米来才是对的尺寸。
// 塞进固定长度会让莫辛-纳甘（1232 mm）和波波沙（843 mm）一样长，
// 而"长步枪看起来就是长"恰恰是这三把枪彼此最直观的区别。
// 注意这**不是**"把枪做大一点更显眼"那类审美选择：图生3D 输出的尺度是它自己定的
// （通常归一化到一个单位盒），不换算就必然错。

// 枪托尾端平面在 gun 局部系的 z，与程序化枪托的 +0.15 对齐。
constexpr float WPN_STOCK_Z = 0.15f;

// 朝向校正的默认值。**这是实测项，不是推导项**：图生3D 把"图像的哪个方向"
// 映到"模型的哪个轴"由服务端决定，只能量出来。与角色模型的 VA_MODEL_YAW 同理，
// 能靠截图量出来改，就不该靠重编译猜。
//
// 默认 90° 的来历：角色模型那批的实测结论是"把 +Z 转到 +X 正好绕 Y 转 +90°"，
// 而武器参考图是**侧视图**（图像横向 = 枪身长度方向），图像横向在生成结果里
// 与角色的"正面方向"是同一个轴族 —— 所以先按同一套角度试，再按截图修正。
constexpr float WPN_YAW_DEG = 90.0f;

struct WpnArtDef {
    const char *key;
    const char *label;
    float       len_mm;     // 真枪全长（毫米）—— 归一化按它缩放
    float       place[3];   // 归一化之后的落位微调（米）：x 左右 / y 上下 / z 前后
    /* 双手在**枪的局部系**里的落点 = 手掌方块的中心（米）。
       【为什么必须每把枪单独给】程序化枪模上那对手是照**它自己**的握把与护木摆的
       （右手 z=-0.015、左手 z=-0.260）。换成真模型之后，握持点随枪而异：
       莫辛的扳机在枪托后 0.33 m、波波沙在 0.28 m、DP-27 在 0.35 m ——
       照抄同一组数会让手悬在枪身外面。

       【为什么是三维的、而不是原来那个只有 z 的标量】
       只有 z 的时候，x/y 只能沿用程序化那对手的 -0.080 / -0.032，而真模型的
       握把高度完全不是这个数：实测三把枪在各自握把处，木头的下缘分别在
       y = -0.095 / -0.101 / -0.131（见下表注释）。
       照抄 y 的后果是**手整块陷进枪身里**：从背后看只剩两侧各 2 mm 的边，
       实测手套像素只有 525 px（胳膊是 64500 px）—— "手不见了"。
       所以 y 与 x 都必须按真模型的剖面来。

       量法：tools/glb_preview.py --probe <z> 会按 make_wpn_node 的同一套
       归一化把顶点换算到引擎局部系，并报该 z 切片上 y 的分位数。
       判读口径：**取"低 ~ 中"那一簇**（握把/护木是下面那块木头/钢管），
       最高点通常是机匣或照门，拿它当握持中心手会飘到枪上面去。
       手心的 y = 该处下缘 + 0.022（手半高 0.037 → 手指搭住工件、底下露 1.5 cm）。 */
    float       hand_r[3];  // 右手（握把/机匣颈）
    float       hand_l[3];  // 左手（护木）
    /* 开镜时"眼睛要穿过的那一点"（照门）在枪局部系里的位置（米）。
       【为什么要它】开镜时要把这一点摆到屏幕中心，枪自己的机械瞄具才压得住准心。
       上一版没有这个量，`aim_pos` 是照**程序化枪模的红点瞄具**（枪局部 y=+0.075）
       定的常数偏移 —— 换成真模型之后每把枪的照门高度都不一样，于是开镜时枪
       落在画面下方。

       取哪个数：与 hand_* 同一份 --probe 剖面，但取**顶面**（"高"那一列）。
       握持点取"低~中那一簇"（下面那块木头），照门恰恰相反，取最上面那道脊。

       ⚠️ **量的时候必须把 --place 一起传给工具**。tools/glb_preview.py 的
       engine_space() 是 `(y - c)·k + place.y`，不给 --place 就等于按 place=0 换算，
       整条 y 会平移 place.y —— 莫辛偏 6.55 cm、DP-27 偏 14.6 cm。
       本轮就踩了这个：照门高度一度被读成 0.075 / 0.082 / 0.160，
       而真值全部落在 **+0.015 附近**（正好是"枪管轴线之上 1.5 cm"，
       三把枪归一化时都把轴线对到了 y=0，顶面自然都在同一个高度上）。
       判据：量完拿 hand_l 的木头区间反查一遍 —— 若与表里已有那组差一个常数，
       那就是漏了 --place。 */
    float       sight[3];
};

// 全部武器键 + 标定参数。顺序 = 游戏里 V 键切换的顺序，也是检阅台/自检的顺序。
//
// 全长取的是**整枪长度**（枪托到尾端，不含刺刀）：
//   莫辛-纳甘 M91/30  1232 mm   波波沙-41  843 mm   DP-27  1275 mm
//
// place.y 的来历：归一化时竖直方向按包围盒居中，而枪的重心明显偏下
//   （枪托、弹鼓、脚架都垂在枪管下方），不补这一下枪管会比原点高出一截、
//   枪口焰从枪管下方喷出来。数值 = -(枪管轴线 y − 包围盒中心 y)，
//   两个量都由 tools/glb_preview.py 报出。
static const WpnArtDef kWpnArt[] = {
    // place 的两个横向/竖向分量都由 tools/glb_preview.py 量出来（"最前端 3% 切片"
    // 在 Y 轴与 Z 轴上的均值 − 包围盒中心）：
    //   Y → 引擎的 y（竖直，决定枪管是浮在原点上方还是陷进手里）
    //   Z → 引擎的 x（横向，因为绕 Y 转 90° 后 原始Z→+X）
    // 横向那一项容易漏，但它是实的：归一化是按**包围盒**居中的，而包围盒会被
    // 只长在一边的突出物带偏。三把枪实测下来只有莫辛偏（它的拉机柄伸在外侧），
    // 波波沙的弹鼓与 DP-27 的圆盘弹匣都在轴线上，所以那两把的横向量 ≈ 0。
    // 莫辛： 竖向 轴线 0.1558、中心 0.0903 -> -0.0655 ／ 横向 -0.0012、中心 0.0148 -> +0.0159
    //
    // ★ 双手落点（每一条都是 tools/glb_preview.py --probe 量出来的，不是试出来的）
    //   量与判读的口径见上面 WpnArtDef::hand_r 的注释。三把枪在各自握持 z 上的剖面：
    //     莫辛   z=-0.180（握把）木头 y ∈ [-0.095, +0.001]  -> 手心 y=-0.073
    //            z=-0.450（护木）木头 y ∈ [-0.052, +0.016]  -> 手心 y=-0.030
    //     波波沙 z=-0.130（握把）木头 y ∈ [-0.101, -0.010] -> 手心 y=-0.079
    //            z=-0.480（护木）钢管 y ∈ [-0.048, -0.009]  -> 手心 y=-0.026
    //     DP-27  z=-0.200（握把）木头 y ∈ [-0.131, +0.009] -> 手心 y=-0.109
    //            z=-0.620（护木）钢管 y ∈ [-0.057, +0.016]  -> 手心 y=-0.033
    //
    //   ⚠️ 波波沙与 DP-27 的**左手 z 与原来那组不同**，原因不是握把变了，
    //   而是它们各自有个"挡在护木位置上"的凸出物：
    //     · 波波沙的 71 发弹鼓占住 z ∈ [-0.30, -0.42]（剖面 x 宽到 ±0.073），
    //       原来的 -0.350 正好把左手放进鼓里；
    //     · DP-27 的 47 发圆盘弹匣占住 z ∈ [-0.30, -0.55]（x 宽到 ±0.135）。
    //   所以左手一律往前挪到"干净的管段"上：波波沙 -0.480、DP-27 -0.620。
    //   判据就是剖面的 x 范围 —— 明显宽于一根枪管，就是撞上了附件。
    { "wpn_mosin", "莫辛-纳甘 M91/30", 1232.0f, { 0.0159f, -0.0655f, 0.0f },
      { -0.016f, -0.073f, -0.180f }, { -0.026f, -0.030f, -0.450f },
      { 0.0f, 0.015f, -0.420f } },
    // 波波沙：竖向 轴线 0.2377、中心 0.1474 -> -0.0903 ／ 横向 -0.0006、中心 -0.0003 -> +0.0002
    { "wpn_ppsh",  "波波沙-41",         843.0f, { 0.0f, -0.0903f, 0.0f },
      { -0.016f, -0.079f, -0.130f }, { -0.026f, -0.026f, -0.480f },
      { 0.0f, 0.000f, -0.460f } },
    // DP-27：竖向 轴线 0.3135、中心 0.1674 -> -0.1461 ／ 横向 -0.0016、中心 -0.0014 -> +0.0001
    { "wpn_dp27",  "DP-27 轻机枪",     1275.0f, { 0.0f, -0.1461f, 0.0f },
      { -0.016f, -0.109f, -0.200f }, { -0.026f, -0.033f, -0.620f },
      { 0.0f, 0.016f, -0.450f } },
};
constexpr int kWpnCount = (int)(sizeof(kWpnArt) / sizeof(kWpnArt[0]));

const std::vector<std::string> &all_wpn_keys() {
    // 【为什么要有一份总表】游戏里的切换顺序、检阅台的陈列顺序、
    // "模型齐备"的自检 —— 三处各抄一遍键名，加第四把枪时必然漏掉其中一处，
    // 而漏掉的那一处只表现为"检阅台上少一把枪"，不报错。
    static const std::vector<std::string> k = { "wpn_mosin", "wpn_ppsh", "wpn_dp27" };
    return k;
}

static std::map<std::string, Node3D *> s_wpn_raw;    // 键 -> 未归一化的原始场景根
static std::map<std::string, bool> s_wpn_failed;

static const WpnArtDef *wpn_art_def(const std::string &p_key) {
    for (int i = 0; i < kWpnCount; ++i) {
        if (p_key == kWpnArt[i].key) return &kWpnArt[i];
    }
    return nullptr;
}

const char *wpn_label(const std::string &p_key) {
    const WpnArtDef *d = wpn_art_def(p_key);
    return (d != nullptr) ? d->label : "未知武器";
}

// 原始场景根：挂在 p_parent 下并隐藏（生命周期交给场景树）。
// 不进场景树的话，它持有的 mesh / material / 贴图会在进程退出时被 Godot
// 报成 "RID allocations ... leaked at exit" 的 ERROR —— 与角色原型同一个理由。
static Node3D *load_wpn_raw(const std::string &p_key, Node *p_parent) {
    auto it = s_wpn_raw.find(p_key);
    if (it != s_wpn_raw.end()) return it->second;
    if (s_wpn_failed.count(p_key) != 0) return nullptr;

    const String path = String("res://assets/art/wpn/model/") +
                        String::utf8(p_key.c_str()) + String(".glb");
    Node3D *raw = load_glb_root(path, "wpn");
    if (raw == nullptr) {
        s_wpn_failed[p_key] = true;
        return nullptr;
    }
    if (p_parent != nullptr) {
        p_parent->add_child(raw);
        raw->set_visible(false);
    } else {
        UtilityFunctions::print(String::utf8("[wpn] 警告：没有原型挂载点，模型资源会在退出时报泄漏"));
    }
    s_wpn_raw[p_key] = raw;
    return raw;
}

Node3D *make_wpn_node(const std::string &p_key, Node *p_proto_parent, WpnNodeInfo &out) {
    Node3D *raw = load_wpn_raw(p_key, p_proto_parent);
    if (raw == nullptr) return nullptr;

    // 做成实例：原始根是隐藏的、且被缓存复用；直接拿去摆会连原型一起动。
    Node *dup = raw->duplicate();
    Node3D *inst = Object::cast_to<Node3D>(dup);
    if (inst == nullptr) {
        if (dup != nullptr) dup->queue_free();
        return nullptr;
    }
    // **必须显式打开可见性**：duplicate() 会把原型上的 visible=false 一起复制过来，
    // 而原型是隐藏的（它挂在场景里纯粹为了不报 RID 泄漏）。
    // 漏了这一行的症状是"手里什么都没有"，但日志里一切正常 ——
    // 角色模型那条路踩过同一个坑（make_unit_node_by_key 里有一模一样的注释），
    // 本轮又踩了一次。
    inst->set_visible(true);

    // 0) 量**旋转之前**的包围盒：最长边就是枪身长度方向，用它算缩放最稳。
    //    （旋转之后再量的话，一旦朝向校正给错，"最长边"会变成枪的厚度，缩放就飞了 ——
    //      而缩放错了会连带枪口位置一起错，两个症状叠在一起更难查。）
    AABB raw_box;
    bool has = false;
    collect_aabb(inst, Transform3D(), raw_box, has);
    if (!has || raw_box.size.length() <= 1e-6f) {
        UtilityFunctions::print(String::utf8("[wpn] 模型没有网格 "), String::utf8(p_key.c_str()));
        inst->queue_free();
        return nullptr;
    }
    const float raw_long = std::max(raw_box.size.x, std::max(raw_box.size.y, raw_box.size.z));

    // 1) 全长：默认取真枪全长（毫米 → 米），VA_WPN_LEN 可整体覆盖。
    const WpnArtDef *def = wpn_art_def(p_key);
    float len_m = (def != nullptr) ? def->len_mm / 1000.0f : 1.0f;
    // 真值只在这一处算 —— 归一化与对外报告的"全长 / 枪口 z"都从它派生，
    // 不会出现"模型按 1.232 缩放、枪口焰按 0.9 摆"这种对不上的情况。
    if (const char *e = std::getenv("VA_WPN_LEN")) {
        const float f = (float)std::strtod(e, nullptr);
        if (f > 0.05f && f < 10.0f) len_m = f;
    }

    // 2) 朝向校正（VA_WPN_ROT="俯仰,偏航,侧倾"，现场调朝向用）
    Vector3 euler(0.0f, WPN_YAW_DEG, 0.0f);
    if (const char *e = std::getenv("VA_WPN_ROT")) {
        float a = 0.0f, b = 0.0f, c = 0.0f;
        if (std::sscanf(e, "%f,%f,%f", &a, &b, &c) == 3) euler = Vector3(a, b, c);
    }
    const float D2R = 3.14159265358979323846f / 180.0f;
    inst->set_transform(Transform3D(Basis::from_euler(euler * D2R), Vector3()));

    Node3D *norm = memnew(Node3D);
    norm->add_child(inst);

    // 3) 量旋转之后的包围盒（此时 norm 还是单位变换），据此定缩放与落位。
    AABB box;
    bool has2 = false;
    collect_aabb(norm, Transform3D(), box, has2);
    if (!has2 || box.size.z <= 1e-6f) {
        UtilityFunctions::print(String::utf8("[wpn] 模型在 Z 轴上没有厚度，朝向校正可能给错了 "),
                                String::utf8(p_key.c_str()), String::utf8(" 校正后尺寸 "), box.size);
        norm->queue_free();
        return nullptr;
    }

    const float k = len_m / std::max(raw_long, 1e-6f);

    Vector3 place;
    if (def != nullptr) place = Vector3(def->place[0], def->place[1], def->place[2]);
    if (const char *e = std::getenv("VA_WPN_PLACE")) {
        float a = 0.0f, b = 0.0f, c = 0.0f;
        if (std::sscanf(e, "%f,%f,%f", &a, &b, &c) == 3) place = Vector3(a, b, c);
    }

    // 4) 落位：x/y 以包围盒中心为准（再用 place 微调），z 把**包围盒的 +Z 端**
    //    顶到 WPN_STOCK_Z。于是枪口自然落在 WPN_STOCK_Z - 全长，
    //    与程序化枪模的"原点=机匣后端、枪口在 -Z"完全一致。
    const Vector3 c = box.get_center();
    const float pos_z = WPN_STOCK_Z - (box.position.z + box.size.z) * k;
    Basis sb;
    sb.scale(Vector3(k, k, k));
    norm->set_transform(Transform3D(sb, Vector3(-c.x * k + place.x,
                                                -c.y * k + place.y,
                                                pos_z + place.z)));

    // 5) **不在这里设渲染层与阴影**。枪模要挂在"只照枪的那五盏灯"所在的层上，
    //    而那个层是 ViewModel 的光照安排的产物 —— 由它自己走一遍子树去设。
    //    写在这里的话，scene_builder 就得知道 viewmodel.cpp 里的 VM_LAYER，
    //    两个模块会为了一个常量互相依赖。
    out.length = box.size.z * k;
    out.muzzle_z = WPN_STOCK_Z - out.length;
    out.raw_size = raw_box.size;
    out.scale = k;
    // 双手在枪上的落点：没有登记过的键就退回程序化枪模那组位置
    // （宁可手放得不准，也不要让手消失）。
    out.hand_r_pos = (def != nullptr) ? Vector3(def->hand_r[0], def->hand_r[1], def->hand_r[2])
                                      : Vector3(0.0f, -0.080f, WPN_HAND_R0);
    out.hand_l_pos = (def != nullptr) ? Vector3(def->hand_l[0], def->hand_l[1], def->hand_l[2])
                                      : Vector3(-0.010f, -0.032f, WPN_HAND_L0);
    // 照门落点：没有登记的键退回**程序化枪模红点瞄具**的位置 (0, 0.075)。
    out.sight_pos = (def != nullptr) ? Vector3(def->sight[0], def->sight[1], def->sight[2])
                                     : Vector3(0.0f, 0.075f, -0.095f);

    UtilityFunctions::print(String::utf8("[wpn] 模型 "), String::utf8(p_key.c_str()),
                            String::utf8(" 原始包围盒 "), raw_box.size,
                            String::utf8(" 校正后 "), box.size,
                            String::utf8(" 缩放 "), k,
                            String::utf8(" 全长 "), out.length,
                            String::utf8(" 枪口 z "), out.muzzle_z);
    return norm;
}

// ---------------------------------------------- 第一人称手模（图生3D 产物）
//
// 参考图与生成方式：tools/gen3d_batch.py --kind vm → assets/art/vm/model/<键>.glb。
// 接口与取舍见 scene_builder.h 那一节；这里只记**实测出来的那部分**。
//
// 【实测记录（2026-09-21，vm_hand_r）】
//   原始包围盒 (0.723, 0.916, 0.416) —— **长轴在 Y**，不是 X。
//   三视图判读（tools/glb_preview.py，图见 ref/vm/SOURCES.md）：
//     XY 正视图：手**背**朝 +Z（看得到指节、拇指搭在食指上）
//     ZY 侧视图：**腕在 −Y、指节在 +Y**
//     XZ 俯视图：手指**卷曲轴 = X**、拇指在 −X
//   即"一只右手、手背朝 +Z、手指指 +Y、拇指在左"。
//   ⚠️ **"自然读法"不等于"拿在手里就自然"**：对轴（Y→−Z）只保证长轴躺平，
//   剩下的滚转/翻面它一概不管。首版残余角给 0，渲染出来是"一大块肉"（腕口
//   切面正对相机）。所以残余角是**解出来的**，不是 0，也不是扫出来的 ——
//   见下面 kVmHandArt 的注释与 VA_VM_HAND_ROT。
//
// ⚠️ **不要假定"长轴在 X"**。武器那批恰好都在 X，人形也在 X，于是很容易顺手写成
// "最长轴 = X"；载具那批已经用坦克（长轴在 Z）证明过这个假定的代价是静默放大 7 倍、
// 一条错都不报。所以这里的对轴是**算了再转**，不是常量。
//
// ⚠️ **两个键共用一份模型时，左手必须镜像**。右拳模型直接套给左手，拇指会跑到
// 手背的错侧 —— 而拇指是"这是哪只手"最省笔墨的判据。镜像用负缩放实现，
// 同时把材质设成 CULL_DISABLED（负行列式会翻转三角形绕序，不关剔除会看穿手掌）。

struct VmHandArtDef {
    const char *key;
    const char *label;
    float       len_m;     // 手长（腕→指节，米）—— 归一化按它缩放
    float       rot[3];    // 对轴之后**残余**的朝向校正（俯仰,偏航,侧倾，度）
    float       place[3];  // 落位微调（米）
    // 自己的 glb 缺失时借谁的模型（nullptr = 不借，直接回退程序化图元）。
    // 借来的一律左右镜像 —— 见上面那条 ⚠️。真给左手生成了自己的模型之后
    // 这个字段就自动失效（先查自己的文件），不必改代码。
    const char *borrow;
};

// 【2026-09-24：手模换成"土黄棉手套"（文生3D），三档全部重标】
//
// **为什么必须换件**：09-21 那个模型（空手握拳实拍 → 图生3D）的几何是一整团
// 钝圆体 —— 识别色图下手指只在轮廓上有浅浅起伏，指头的"样子"完全靠贴图糊出来，
// 放大 3 倍还能看到手背上一块贴图白斑。根因与「树冠叶簇」同构：
// 图生3D 是**单视图重建**，而"握拳"这个姿势里**指缝之间的空腔在任何单张照片里
// 都看不见**（能看到的永远是"手指叠在一起的那一面"），重建只能把那些区域蒙成一块。
// 所以改走文生3D（`tools/gen3d_batch.py --kind vm`），prompt 见那边 PROMPTS。
//
// **三档为什么全变**：新件的原始朝向与旧件**不同**。两件都拍 rot=0 并排看：
//   旧件腕口朝 **+X**（横躺）  vs  新件腕口朝 **+Z**（正对相机）
// 而两者的对轴变换**完全相同**（日志"对轴后"都是 Y→Z、Z→Y），
// 差异纯粹来自模型自身。拿旧件的 rot 直接套，结果是那个腕口切面正对相机
// （画面里出现一个大圆筒）—— 与旧件注释里"另一组解会让腕口切面正对相机"同一个症状。
// 判别方法不是猜：**两件都在 rot=0 下渲染一次并排看**。
//
//   rot = 90, 0, 90
//     · pitch 90 是**解**出来的：把腕口从 +Z 翻到 −Y（朝肘）。
//       这一步单独就同时满足了"手背朝相机"—— 因为手背在模型里与腕口正交，
//       绕同一根轴转 90° 时两个约束是同一个方程。
//     · roll 90 是**扫**出来的：让**四根手指沿横向排列**。roll=0 时手指竖直排列，
//       读出来是"举手"而不是"攥住"；roll=±90 才与旧件的姿态同构（也是横排）。
//   len_m = 0.110 —— 按**同口径面积**反推（不是拍的）：同一 ROI 下新手套的
//     识别色像素在 len=0.105 时是 78695，旧件是 87547 →
//     要面积持平需 len = 0.105 × √(87547/78695) = 0.1108，取 0.110。
//   place = 0, −0.050, 0 —— 新件的长轴里含那个**腕口台座**（约占长轴 25%），
//     包围盒中心因此比旧件更偏腕侧。落位要往下补 5 cm 才把"拳"落到枪身上；
//     不补的话手是**浮在枪右上方**的（这一步是本轮最大的一项）。
//   tint = 0.71,0.61,0.43 —— 也重标了，见 hand_tint() 的注释：新模型的贴图
//     **本身就是深赭黄的粗棉布**，旧件那套"皮肤→皮革棕"的乘数会把棉布二次压暗成
//     深棕（L90），而 0.80 那档又亮到把布纹冲平。0.71 档实测受光面 RGB(193,133,94)
//     L142 —— 最贴"土黄"，且腕口罗纹编织在 1.55 倍缩放下还看得见。
//
// 【旧件（2026-09-21，已隔离到 sweep/gen3d_vm/rejected/）的实测那三档】
//   len_m = 0.115 m —— 扫过 0.185/0.145/0.135/0.115/0.095 后 0.115 最好。
//     **不要用"腕→中指尖 18.5 cm"**：那个模型的长轴里含一截腕柱，按 18.5 cm
//     归一化等于整只手放大 1.6 倍，画面上是一大块肉。0.115 恰好接近"腕横纹→
//     指节"的真人口径（~0.10 m），也就是它长轴的真实语义。
//   rot = 4.1,20.2,48.5 —— 不是扫出来的，是**解**出来的：
//     要求"腕端(模型 −Y)指向肘、手背(模型 +Z)指向相机"，联立解得欧拉角，
//     回代误差 2.2e-16。另一组解 (−31.0,−20.7,39.6) 会让腕口切面正对相机。
//   place = −0.016,0.015,0.007 —— 包围盒中心 ≠ 握拳中心（拳在长轴末端）。
//   对照数据（同一掩码口径 |常规−藏|）：程序化手 VM 合计 5.76%、手套 1.59%(L=92)、
//     枪身 3.28%(L=155)；真手模 5.81% / 2.46%(L=76) / 2.46%(L=133)。
//     即**总占屏几乎不变** —— 真手是"盖住了一部分枪身"，不是额外糊上去一块。
static const VmHandArtDef kVmHandArt[] = {
    { "vm_hand_r", "右手（扳机手）", 0.110f, { 90.0f, 0.0f, 90.0f }, { 0.0f, -0.050f, 0.0f }, nullptr },
    { "vm_hand_l", "左手（支撑手）", 0.110f, { 90.0f, 0.0f, 90.0f }, { 0.0f, -0.050f, 0.0f }, "vm_hand_r" },
};
constexpr int kVmHandCount = (int)(sizeof(kVmHandArt) / sizeof(kVmHandArt[0]));

const std::vector<std::string> &all_vm_hand_keys() {
    static const std::vector<std::string> k = { "vm_hand_r", "vm_hand_l" };
    return k;
}

static const VmHandArtDef *vm_hand_def(const std::string &p_key) {
    for (int i = 0; i < kVmHandCount; ++i) {
        if (p_key == kVmHandArt[i].key) return &kVmHandArt[i];
    }
    return nullptr;
}

const char *vm_hand_label(const std::string &p_key) {
    const VmHandArtDef *d = vm_hand_def(p_key);
    return (d != nullptr) ? d->label : "未知手模";
}

static std::map<std::string, Node3D *> s_vm_hand_raw;      // 键 -> 未归一化的原始场景根
static std::map<std::string, bool>     s_vm_hand_borrowed; // 键 -> 这份是借来的吗
static std::map<std::string, bool>     s_vm_hand_failed;

// 把**最长的那一维**转到 −Z 的对轴基。
// 只做坐标轴之间 90° 整数倍的旋转，所以结果仍是轴对齐的，不会把模型转出斜角。
// 返回的基作用在"已经转过残余角"的模型上（见 make_vm_hand_node 的合成顺序）。
static Basis vm_hand_align(const Vector3 &p_size) {
    const float x = p_size.x, y = p_size.y, z = p_size.z;
    const float PI2 = 1.57079632679489661923f;
    if (y >= x && y >= z) return Basis(Vector3(1, 0, 0), -PI2);   // 长轴 Y -> −Z
    if (x >= y && x >= z) return Basis(Vector3(0, 1, 0),  PI2);   // 长轴 X -> −Z
    return Basis();                                              // 长轴已经在 Z
}

// 原始场景根：挂在 p_parent 下并隐藏（生命周期交给场景树）。
// 与武器那条同构 —— 不进场景树的话，它持有的 mesh / material / 贴图会在进程退出时
// 被 Godot 报成 "RID allocations ... leaked at exit" 的 ERROR，
// 而"日志里有没有 ERROR"正是本工程的回归判据。
//
// 【借用链】自己的文件不存在时读 def->borrow 那份（左手借右手），并置 out_borrowed。
// **先判存在再解析**，所以正常情况下不会为"左手还没生成"刷一条 ERROR 级噪声。
static Node3D *load_vm_hand_raw(const std::string &p_key, Node *p_parent, bool &out_borrowed) {
    out_borrowed = false;
    {
        auto it = s_vm_hand_raw.find(p_key);
        if (it != s_vm_hand_raw.end()) {
            out_borrowed = s_vm_hand_borrowed[p_key];
            return it->second;
        }
    }
    if (s_vm_hand_failed.count(p_key) != 0) return nullptr;

    const VmHandArtDef *def = vm_hand_def(p_key);
    String path = String("res://assets/art/vm/model/") +
                  String::utf8(p_key.c_str()) + String(".glb");
    Node3D *raw = nullptr;
    if (FileAccess::file_exists(path)) {
        raw = load_glb_root(path, "vm");
    } else if (def != nullptr && def->borrow != nullptr) {
        const String bp = String("res://assets/art/vm/model/") +
                          String::utf8(def->borrow) + String(".glb");
        raw = load_glb_root(bp, "vm");
        out_borrowed = (raw != nullptr);
        if (raw != nullptr) {
            UtilityFunctions::print(String::utf8("[vm] 手模 "), String::utf8(p_key.c_str()),
                                    String::utf8(" 没有自己的模型，借 "),
                                    String::utf8(def->borrow), String::utf8(" 并左右镜像"));
        }
    }
    // 不再自己打一条"缺手模文件"：load_glb_root 已经会把路径打出来，
    // 两条对着同一件事报两遍（实测日志里就是连着两行），只会淹掉真正的信息。
    if (raw == nullptr) {
        s_vm_hand_failed[p_key] = true;
        return nullptr;
    }
    if (p_parent != nullptr) {
        p_parent->add_child(raw);
        raw->set_visible(false);
    } else {
        UtilityFunctions::print(String::utf8("[vm] 警告：没有原型挂载点，手模资源会在退出时报泄漏"));
    }
    s_vm_hand_raw[p_key] = raw;
    s_vm_hand_borrowed[p_key] = out_borrowed;
    return raw;
}

// 逐键旋钮：VA_VM_HAND_<X>_ROT / _LEN / _PLACE，其中 <X> 取键名的**最后一个字母**
// 的大写（vm_hand_r → VA_VM_HAND_R_ROT）。找不到键级变量时才回落到全局的
// VA_VM_HAND_*（全局一次改两只，扫档用；键级用来分开微调）。
//
// 【为什么要有键级】两只手共用一份 .glb，但**落位的容错空间不是一回事**：模型是
// "握拳 + 一截腕柱"，包围盒中心落在腕柱上，于是把中心对到握持点时，拳会被顺着
// 长轴推出去一截。右手那边多出来的正好是拳、压在握把上；左手那边就是腕柱顶进
// 护木里。2026-09-21 扫过 8 档（4 朝向 × 4 落位），结论是**这一版两只手用同一组
// 值最好** —— 看着更"松"的那几档要么把腕口切面转正对相机（画面里出现一块平板），
// 要么把支撑手拖到与扳机手重叠。所以键级旋钮留着当**重标定工具**：
// 换手模、换枪的握持点之后，先全局扫一遍再单独校一只，不必改代码。
static bool hand_env3(const std::string &p_key, const char *p_field, float p_out[3]) {
    if (p_key.empty()) return false;
    std::string n = "VA_VM_HAND_";
    n += (char)std::toupper((unsigned char)p_key[p_key.size() - 1]);
    n += "_";
    n += p_field;
    const char *e = std::getenv(n.c_str());
    if (e == nullptr) return false;
    return std::sscanf(e, "%f,%f,%f", &p_out[0], &p_out[1], &p_out[2]) == 3;
}

static bool hand_env1(const std::string &p_key, const char *p_field, float &p_out) {
    if (p_key.empty()) return false;
    std::string n = "VA_VM_HAND_";
    n += (char)std::toupper((unsigned char)p_key[p_key.size() - 1]);
    n += "_";
    n += p_field;
    const char *e = std::getenv(n.c_str());
    if (e == nullptr) return false;
    const float f = (float)std::strtod(e, nullptr);
    if (!(f > 0.02f && f < 1.0f)) return false;
    p_out = f;
    return true;
}

Node3D *make_vm_hand_node(const std::string &p_key, Node *p_proto_parent, VmHandNodeInfo &out) {
    bool borrowed = false;
    Node3D *raw = load_vm_hand_raw(p_key, p_proto_parent, borrowed);
    if (raw == nullptr) return nullptr;

    // 做成实例：原始根是隐藏的、且被缓存复用；直接拿去摆会连原型一起动。
    Node *dup = raw->duplicate();
    Node3D *inst = Object::cast_to<Node3D>(dup);
    if (inst == nullptr) {
        if (dup != nullptr) dup->queue_free();
        return nullptr;
    }
    // **必须显式打开可见性**：duplicate() 会把原型上的 visible=false 一起复制过来
    // （原型是隐藏的，它挂在场景里纯粹为了不报 RID 泄漏）。武器那条踩过同一个坑。
    inst->set_visible(true);

    // 0) 量**旋转之前**的包围盒，用来定"哪一维是长轴"。
    AABB raw_box;
    bool has = false;
    collect_aabb(inst, Transform3D(), raw_box, has);
    if (!has || raw_box.size.length() <= 1e-6f) {
        UtilityFunctions::print(String::utf8("[vm] 手模没有网格 "), String::utf8(p_key.c_str()));
        inst->queue_free();
        return nullptr;
    }
    const float raw_long = std::max(raw_box.size.x, std::max(raw_box.size.y, raw_box.size.z));

    // 黄牌：最长轴与次长轴几乎相等 → 对轴是"抛硬币"，朝向可能整体差 90°。
    // 载具那批的教训：轴向错了不会报错，只会静默地把模型转错方向。
    {
        float a = raw_box.size.x, b = raw_box.size.y, c = raw_box.size.z;
        if (a > b) { const float t = a; a = b; b = t; }
        if (b > c) { const float t = b; b = c; c = t; }
        if (a > b) { const float t = a; a = b; b = t; }
        // 现在 a <= b <= c：c 最长、b 次长
        if (b > c * 0.97f) {
            UtilityFunctions::print(String::utf8("!! 手模 "), String::utf8(p_key.c_str()),
                                    String::utf8(" 的长轴与次长轴几乎相等（原始包围盒 "), raw_box.size,
                                    String::utf8("）—— 自动对轴在抛硬币，朝向可能整体差 90°。"
                                                 "请用 VA_VM_HAND_ROT 现场校，别直接采信。"));
        }
    }

    // 1) 朝向 = 残余角 × 对轴。**合成顺序**：先转残余角（在模型自己的坐标系里），
    //    再做对轴。于是对轴永远是"把最长的那一维送到 −Z"，而残余角负责
    //    "手背朝哪边、拇指在哪侧"这类几何上定不了的事。
    const VmHandArtDef *def = vm_hand_def(p_key);
    Vector3 euler(0.0f, 0.0f, 0.0f);
    if (def != nullptr) euler = Vector3(def->rot[0], def->rot[1], def->rot[2]);
    // VA_VM_HAND_ROT="俯仰,偏航,侧倾"（度）：覆盖残余角。**注意它是在模型自己的
    // 坐标系里给的**，所以 "0,90,0" 读作"把这只手绕它自己的 Y 轴转 90°"。
    // 键级 VA_VM_HAND_R_ROT / VA_VM_HAND_L_ROT 优先（见 hand_env3 的说明）。
    {
        float v[3];
        if (hand_env3(p_key, "ROT", v)) {
            euler = Vector3(v[0], v[1], v[2]);
        } else if (const char *e = std::getenv("VA_VM_HAND_ROT")) {
            float a = 0.0f, b = 0.0f, c = 0.0f;
            if (std::sscanf(e, "%f,%f,%f", &a, &b, &c) == 3) euler = Vector3(a, b, c);
        }
    }
    const float D2R = 3.14159265358979323846f / 180.0f;
    inst->set_transform(Transform3D(Basis::from_euler(euler * D2R) * vm_hand_align(raw_box.size),
                                    Vector3()));

    Node3D *norm = memnew(Node3D);
    norm->add_child(inst);

    // 2) 量旋转之后的包围盒（此时 norm 还是单位变换），据此定缩放与落位。
    AABB box;
    bool has2 = false;
    collect_aabb(norm, Transform3D(), box, has2);
    if (!has2 || box.size.z <= 1e-6f) {
        UtilityFunctions::print(String::utf8("[vm] 手模在 Z 轴上没有厚度，对轴可能给错了 "),
                                String::utf8(p_key.c_str()), String::utf8(" 对轴后尺寸 "), box.size);
        norm->queue_free();
        return nullptr;
    }

    // 3) 手长：默认取真人口径，VA_VM_HAND_LEN 可整体覆盖（排查"手太大/太小"用）。
    float len_m = (def != nullptr) ? def->len_m : 0.185f;
    if (!hand_env1(p_key, "LEN", len_m)) {
        if (const char *e = std::getenv("VA_VM_HAND_LEN")) {
            const float f = (float)std::strtod(e, nullptr);
            if (f > 0.02f && f < 1.0f) len_m = f;
        }
    }
    // 用**旋转之前**的长边算缩放，与武器那条同一个理由：一旦朝向给错，
    // 旋转后的"最长边"会变成手的厚度，缩放跟着一起飞。对轴是算出来的，
    // 所以这两个量在正常情况下只差数值噪声。
    const float k = len_m / std::max(raw_long, 1e-6f);

    Vector3 place;
    if (def != nullptr) place = Vector3(def->place[0], def->place[1], def->place[2]);
    {
        float v[3];
        if (hand_env3(p_key, "PLACE", v)) {
            place = Vector3(v[0], v[1], v[2]);
        } else if (const char *e = std::getenv("VA_VM_HAND_PLACE")) {
            float a = 0.0f, b = 0.0f, c = 0.0f;
            if (std::sscanf(e, "%f,%f,%f", &a, &b, &c) == 3) place = Vector3(a, b, c);
        }
    }

    // 4) 落位：把包围盒中心搬到原点（再按 place 微调）。
    //    【为什么是按包围盒居中，而不是"把手腕对到原点"】手腕那一点在模型里
    //    没有可靠标记，而包围盒是量出来的。而 ViewModel 那边对前臂的处理本来就
    //    允许有偏差：前臂从 `掌心 + WRIST_*_OFF` 起笔，真手比程序化手大 3.4 倍，
    //    起笔点必然落在手掌实体**内部** —— 那正好，前臂从拳头里长出来，
    //    不会露出接缝。所以这里只要"手掌中心 ≈ 原点"就够了。
    //
    //    ⚠️ place 是**在引擎系里加的**（见 scene_builder.h 的坐标系说明），
    //    所以它要按"校准之后想让手往哪挪"来给，而不是按模型自己的轴。
    //    VA_VM_HAND_NOMIRROR=1 借来的模型**不做镜像**（A/B 用：判定"左右手
    //    亮度不一样"到底是镜像翻了法线，还是那只手本来就更背光）。
    bool mirror = borrowed;
    if (mirror && std::getenv("VA_VM_HAND_NOMIRROR") != nullptr) mirror = false;
    const float mx = mirror ? -k : k;
    const Vector3 c = box.get_center();
    Basis sb;
    sb.scale(Vector3(mx, k, k));
    norm->set_transform(Transform3D(sb, Vector3(-mx * c.x + place.x,
                                                -k * c.y + place.y,
                                                -k * c.z + place.z)));

    out.length = raw_long * k;
    out.scale = k;
    out.raw_size = raw_box.size;
    out.mirrored = mirror;

    UtilityFunctions::print(String::utf8("[vm] 手模 "), String::utf8(p_key.c_str()),
                            " ", String::utf8(vm_hand_label(p_key)),
                            String::utf8(" 原始包围盒 "), raw_box.size,
                            String::utf8(" 对轴后 "), box.size,
                            String::utf8(" 缩放 "), k,
                            String::utf8(" 手长 "), out.length,
                            String::utf8(" 镜像 "), String::utf8(mirror ? "是" : "否"));
    return norm;
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
    // 站到地面上：地形抬起来了，人就得跟着抬 —— 这里改一处，全场单位（含检阅台）一起生效。
    return Transform3D(b, to3(p_x, p_y, ground_h(p_x, p_y)));
}

// ---------------------------------------------- 载具三维模型（图生3D 产物）
//
// 参考图与生成方式：tools/prep_veh_refs.sh（取图 + 统一车头朝向 + 补方）
// → tools/gen3d_batch.py --kind veh → assets/art/veh/model/veh_<type>.glb。
//
// 【归一化目标坐标系】= make_vehicle_node 那套程序化车体的局部系：
//     原点在**车体中心的地面投影**（车轮着地 y=0）、车头朝 **+X**、
//     车长沿 X、车宽沿 Z，单位是**米**。
// 与程序化车模完全同构，所以 world_sim 那两处 `set_rotation(0,-angle,0)`
// 一行都不用改 —— 这也是"车头必须朝 +X"的由来（依据见头文件注释）。
//
// 【朝向校正角是实测项，不是推导项】
// 图生3D 把"图像的哪个方向"映到"模型的哪个轴"由服务端决定，只能量出来。
// 武器那批的实测结论是"图像右 = +X"（三张参考图枪口朝右，成品枪口也在 +X）。
// 载具的参考图同样是**侧视图**（图像横向 = 车长方向），所以先按 **0°** 起步 ——
// 与武器那条"把 +X 转到 -Z 需要 90°"并不矛盾：那 90° 是为了转到**另一个**目标系
// （枪口朝 -Z），而这里的目标系本来就是"车头朝 +X"。
// 能靠截图量出来改，就不该靠重编译猜，所以留 VA_VEH_YAW 现场扫。
//
// ⚠️ 【但"长轴在 X 上"这件事**不能**只靠一个常量假定 —— 它逐辆不同】
// 实测四辆里 jeep / apc 出来长轴就在 X，而 **tank 出来长轴在 Z**
// （原始包围盒 (0.526, 0.532, 1.085)）。所以这里把朝向拆成两段：
//   ① `align`（自动，几何决定，无猜测）：把**最长的水平轴**转到 X；
//   ② `veh_extra_yaw_deg()`（逐辆实测的残余角）：只管剩下的 180°
//      ——"哪一端是车头"几何上定不了（车头和车尾都是一样的长），只能看图。
float veh_extra_yaw_deg(const std::string &p_type) {
    // 四个值都是 2026-09-20 用 `VA_UNIT_SHOW=veh:<键>` 看出来的
    // （判据：偏航 0° 时画面里应当是**车头正面** —— 格栅 / 大灯 / 保险杠正对镜头）。
    static const std::map<std::string, float> k = {
        { "jeep",  0.0f },   // 实测 0° 看到格栅 + 大灯 + 挡风玻璃 ✓；侧视车头朝左、备胎在尾
        { "apc",   0.0f },   // 实测 0° 看到绞盘保险杠 + 天线 ✓；侧视前轮 + 后履带（确为半履带车）
        { "tank",  0.0f },   // 长轴在 Z → align 自动转 90°；残余 0°，侧视主动轮在前、炮管与车头同向 ✓
        { "truck", 0.0f },   // 长轴在 X（align 0）；侧视六轮（三轴）+ 帆布车厢 + GMC 百叶窗机盖，
                             // 车头朝左、与其余三辆一致 ✓
    };
    auto it = k.find(p_type);
    return (it == k.end()) ? 0.0f : it->second;
}

static std::map<std::string, Node3D *> s_veh_proto;   // 键 -> 外层原型（含完整的归一化子树）
static std::map<std::string, bool> s_veh_failed;      // 失败过就别每辆车再试一次

static Node3D *load_vehicle_proto(const std::string &p_type, Node *p_parent) {
    auto it = s_veh_proto.find(p_type);
    if (it != s_veh_proto.end()) {
        return it->second;
    }
    if (s_veh_failed.count(p_type) != 0) {
        return nullptr;
    }

    const String path = String("res://assets/art/veh/model/veh_") +
                        String::utf8(p_type.c_str()) + String(".glb");
    Node3D *raw = load_glb_root(path, "veh");
    if (raw == nullptr) {
        s_veh_failed[p_type] = true;
        return nullptr;
    }

    AABB box;
    bool has = false;
    collect_aabb(raw, Transform3D(), box, has);
    // 判据用"两个水平尺寸都非零"而不是"体积非零"：载具是躺着的东西，
    // 万一生成成一个竖片（贴图平面），体积照样非零，但摆到地上就是一块纸板。
    if (!has || box.size.x <= 1e-4f || box.size.z <= 1e-4f) {
        UtilityFunctions::print(String::utf8("[veh] 模型没有网格或尺寸退化 "), path,
                                String::utf8(" 包围盒 "), box.size);
        raw->queue_free();
        s_veh_failed[p_type] = true;
        return nullptr;
    }

    /* ---- ① 先把"车长"对齐到局部 X（逐辆自动，不靠常量假定）----
       判据是**水平两轴的实测长度**：车长必然是水平方向上更长的那一维
       （四辆车的实物长宽比都在 2:1 以上），所以只有 0° / 90° 两个候选，
       没有猜的成分。不先做这一步就会踩到下面这个坑 ——
       它是**静默**的，所以值得把症状写清楚：
         tank 原始包围盒 (0.526, 0.532, 1.085)，长轴在 Z；
         若直接拿 size.x=0.526 当车长，缩放到 3.7 m 需要 k=7.03，
         于是坦克变成 **7.6 m 长、3.7 m 高**的巨物（目标其实是 3.7 × 1.9）。
         全程不报一条错，只表现为"这坦克怎么比房子还大"。 */
    const float align_deg = (box.size.z > box.size.x) ? 90.0f : 0.0f;

    float yaw_deg = align_deg + veh_extra_yaw_deg(p_type);
    if (const char *e = std::getenv("VA_VEH_YAW")) {
        // 现场扫角度用：覆盖的是**总**偏航（align 也算在内）。
        // 别拿它单独覆盖残余角 —— 那样 align 会被一起冲掉，越扫越乱。
        yaw_deg = (float)std::atof(e);
    }

    constexpr float PI = 3.14159265358979323846f;
    Basis b(Vector3(0.0f, 1.0f, 0.0f), yaw_deg * PI / 180.0f);

    // 车长按 spec 走（理由见头文件）。`spec.len` 自 2026-09-20 起就是实车尺寸，
    // 所以这条同时是"按实车长度归一化"——与武器那套口径一致。
    // 它**同时**是 sim 判遮挡/碰撞的车体盒，改它会连带一串按旧车长调过的常量
    // （README「载具形象」有清单），不要只改这里。
    const va::VehicleSpec *sp = va::vehicle_of(p_type);
    const float target_len = sp->len * S;

    // 先量"转过之后"的包围盒，再按它定缩放 —— 顺序反了就会拿原始 X 当车长，
    // 而原始 X 在 yaw=±90 时其实是车宽。
    const AABB rbox = Transform3D(b).xform(box);
    const float cur_len = std::max(rbox.size.x, 1e-5f);
    const float k = target_len / cur_len;
    b.scale(Vector3(k, k, k));
    const AABB kbox = Transform3D(b).xform(box);

    /* 黄牌：车高不该超过车长。四辆车的实物都是"长 > 高"（比值 1.5~2.4），
       正常成品不该越过 0.95。越过了就说明"车长"这一维仍然取错
       （上面那步对齐没生效 / 生成器给了个竖着的东西），
       这时宁可日志里喊一声也不要默默摆一辆巨车上场。 */
    if (kbox.size.y > kbox.size.x * 0.95f) {
        UtilityFunctions::print(String::utf8("[veh] ⚠ 车高 "), kbox.size.y,
                                String::utf8(" 不短于车长 "), kbox.size.x,
                                String::utf8(" —— 长轴可能仍未对齐，请核对该行日志"));
    }

    const Vector3 c = kbox.get_center();

    Node3D *outer = memnew(Node3D);
    Node3D *norm = memnew(Node3D);
    // 一次给全：绕 Y 转 yaw、等比缩放到目标车长、车底落到 y=0、水平居中到原点。
    // 平移量取**缩放之后的包围盒中心**，而不是原包围盒中心乘 k ——
    // yaw=±90 时两者并不相等（x/z 互换），照抄角色那套会横着偏出半个车长。
    norm->set_transform(Transform3D(b, Vector3(-c.x, -kbox.position.y, -c.z)));
    norm->add_child(raw);
    outer->add_child(norm);

    if (p_parent != nullptr) {
        p_parent->add_child(outer);
        outer->set_visible(false);
    } else {
        UtilityFunctions::print(String::utf8("[veh] 警告：没有原型挂载点，模型资源会在退出时报泄漏"));
    }

    UtilityFunctions::print(String::utf8("[veh] 模型 "), String::utf8(p_type.c_str()),
                            String::utf8(" 原始包围盒 "), box.size,
                            String::utf8(" 校正后 "), kbox.size,
                            String::utf8(" 缩放 "), k,
                            String::utf8(" 对齐角 "), align_deg,
                            String::utf8(" 总偏航 "), yaw_deg,
                            String::utf8(" 目标车长 "), target_len);
    s_veh_proto[p_type] = outer;
    return outer;
}

Node3D *make_vehicle_node_by_key(const std::string &p_type, Node *p_proto_parent) {
    Node3D *proto = load_vehicle_proto(p_type, p_proto_parent);
    if (proto == nullptr) {
        return nullptr;
    }
    Node *dup = proto->duplicate();
    Node3D *n = Object::cast_to<Node3D>(dup);
    if (n != nullptr) {
        // 与角色那条路同一个坑：duplicate() 会把原型上的 visible=false 一起复制过来，
        // 不显式打开的话整支车队在画面上集体消失。
        n->set_visible(true);
        return n;
    }
    if (dup != nullptr) {
        dup->queue_free();
    }
    return nullptr;
}

Node3D *make_vehicle_node(const std::string &type, Node *p_proto_parent) {
    if (Node3D *real = make_vehicle_node_by_key(type, p_proto_parent)) {
        return real;
    }
    // 回退：程序化图元车（少一个模型文件不该让战场上少一辆车）。
    // 这套图元同时是"真模型接坏了吗"的对照基准，所以**不要删**。
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

// ------------------------------------------------------- 战场地物模型（图生3D，2026-09-22）
/* 【这一档换的是什么】场上**数量最多**的三类掩体：岩石（每关三十来块）、
   树（二十来棵）、灌木（十几丛）。它们原先全是程序化图元 ——
   岩石是"极坐标球 + 逐顶点半径扰动"的多面体，树是"圆柱 + 斜插分枝 + 球簇树冠"，
   灌木是"3~5 个小球"。三样在近景里都读得出是几何拼的，最出戏的是树冠：
   一根杆顶三个绿球。

   【归一化口径：把"占多大地方"原样搬过来】这是本档唯一不能错的一件事。
   逻辑层的 p.r 决定这块东西遮住多大一片，而程序化那套的尺寸**就是**从 p.r 推的
   （岩石网格的顶点半径 = r = p.r × S，再压扁 flat=0.50）。所以换模型时不能按
   模型自己的尺寸摆，而要把它的包围盒拉到"原来那套图元的包围盒"：

     岩石   宽 = 2·r         高 = 1.00·r       底沉 0.24·r（= 原来"中心抬 0.26·r"）
     灌木   宽 = 2.2·r       高 = 0.80·r       底沉 0.10·r
     松树   只定高 = 7.0 + rng·3.0（沿用原公式），冠幅跟着模型走

   两款"按宽"、一款"按高"不是随手定的：
     · 岩石/灌木低矮，它们的"遮挡"就是那个水平圆 —— 按水平尺度归一化、再把高度
       压到固定比例。高了矮了都会立刻改变"能不能藏住"的读感，而那是玩法；
     · 树只按高度归一化。逻辑层对树的 p.r **只**约束"俯视遮挡半径"、高度本来就不受
       它约束（见下面 Tree 分支那句注释），若按冠幅归一化，8 米的树会缩成 3 米的灌木。
       代价是树冠比原来宽（原来约 2.1 m、现在约 4 m）—— **那正是要改的东西**：
       原来是一根杆顶三个球。另注意"视觉比逻辑宽"这件事**改动前后同向**，
       改动前 2.1 m 也已经 > 2·p.r（1.6 m），不是这一次新引入的偏差。

   【变体：一份模型 + hash 轮换】两块石头是**两份不同剪影**（棱角裸岩 / 苔覆圆石），
   由 add_prop 按 hash 交替取，偏航也按 hash 撒开 —— 三十多块石头不该是同一块
   复制出来的。生成额度有限（内置通道 5 次/天），"多做几个变体"太贵，
   而"一份模型 + hash 轮换"是同样效果里最便宜的。

   【回退链】任何一步失败（文件缺失 / 解析失败 / 尺寸退化）一律返回 nullptr，
   由 add_prop 回退到原来的程序化图元 —— 少一个模型文件不该让战场上少一块掩体。
   那套程序化图元同时是"真模型接坏了吗"的对照基准，**不要删**。 */

struct PropArtDef {
    const char *key;
    // true ：原型归一化到"高 = 1"，实例按目标**高**等比缩放（树）
    // false：原型归一化到"水平尺度 = 1"且高/宽压到 hw，实例按目标**宽**等比缩放
    bool  by_height;
    float hw;          // by_height = false 时的高宽比（压扁量）
};

static const PropArtDef kPropArt[] = {
    { "prop_rock_a", false, 0.50f },   // 棱角裸岩
    { "prop_rock_b", false, 0.50f },   // 苔覆圆石（矮而宽）
    { "prop_pine",   true,  0.00f },   // 针叶树
    { "prop_bush",   false, 0.36f },   // 低矮灌丛
};
static const int kPropArtN = (int)(sizeof(kPropArt) / sizeof(kPropArt[0]));

static const PropArtDef *prop_art_def(const std::string &p_key) {
    for (int i = 0; i < kPropArtN; ++i) {
        if (p_key == kPropArt[i].key) return &kPropArt[i];
    }
    return nullptr;
}

static std::map<std::string, Node3D *> s_prop_proto;   // 键 -> 单位原型（已归一化，隐藏）
static std::map<std::string, bool> s_prop_failed;      // 失败过就别每块石头再试一次

static Node3D *load_prop_proto(const std::string &p_key, Node *p_parent) {
    auto it = s_prop_proto.find(p_key);
    if (it != s_prop_proto.end()) {
        return it->second;
    }
    if (s_prop_failed.count(p_key) != 0) {
        return nullptr;
    }

    const String path = String("res://assets/art/prop/model/") +
                        String::utf8(p_key.c_str()) + String(".glb");
    Node3D *raw = load_glb_root(path, "prop");
    if (raw == nullptr) {
        s_prop_failed[p_key] = true;
        return nullptr;
    }

    AABB box;
    bool has = false;
    collect_aabb(raw, Transform3D(), box, has);

    const PropArtDef *def = prop_art_def(p_key);
    if (def == nullptr) {
        UtilityFunctions::print(String::utf8("[prop] 表里没有这个键 "), String::utf8(p_key.c_str()));
        raw->queue_free();
        s_prop_failed[p_key] = true;
        return nullptr;
    }

    // 判据要求**三个方向都非零**：地物是坐在地上的东西，万一生成成一张竖片
    // （贴图平面）或一条线，体积照样非零，摆到地上就是一块纸板 / 一根杆。
    if (!has || box.size.x <= 1e-4f || box.size.y <= 1e-4f || box.size.z <= 1e-4f) {
        UtilityFunctions::print(String::utf8("[prop] 模型没有网格或尺寸退化 "), path,
                                String::utf8(" 包围盒 "), box.size);
        raw->queue_free();
        s_prop_failed[p_key] = true;
        return nullptr;
    }

    /* ---- 尺寸退化的第二道闸：**相对**判据 ----
       上面那条用的是绝对阈值 1e-4，**实测拦不住"饼 / 杆"**：图生3D 对**半透明叶簇**
       （针叶、灌木）会把体积塌掉，本项目 2026-09-22 实测两个（都在同一批、同一组参数下）：
         prop_bush  世界包围盒 (1.0017, 0.0016, 0.9878)  高 = 最大维的 0.16%  → 一张绿饼
         prop_pine  世界包围盒 (0.0703, 1.1956, 0.0752)  宽 = 高的 6.3%        → 一根绿杆
       bush 那个数离 1e-4 还差 16 倍，**照样过闸**，摆到场上就是一块饼。
       所以这里补两条**比例**判据 —— 判据必须是比例，因为生成器的输出不带单位：
       同一个模型整体缩放 1000 倍，绝对阈值会给出相反的答案，而"它是不是饼"不变。
       ① 任何一维 < 最大维的 2%  → 塌了（纸片 / 杆 / 饼）；
       ② 按高归一化的地物（树）还要"横向展得开"：真实针叶树的冠幅 / 树高通常 0.3~0.6，
          取 0.15 作下限 —— 只有"只剩一根树干"那类才会低于它。
       ⚠️ 判据②只对 by_height 的地物成立。日后若有**又高又细**的正当道具
       （旗杆 / 电线杆 / 独腿支架）想走这条通道，得给它单独一档，别放宽这条。 */
    const float mx = std::max(std::max(box.size.x, box.size.y), box.size.z);
    const float mn = std::min(std::min(box.size.x, box.size.y), box.size.z);
    const char *degen_why = nullptr;
    if (mx <= 0.0f || mn < 0.02f * mx) {
        degen_why = "某一维 < 最大维的 2%（饼 / 纸片 / 杆）";
    } else if (def->by_height) {
        const float spread = std::max(box.size.x, box.size.z) / std::max(box.size.y, 1e-5f);
        if (spread < 0.15f) {
            degen_why = "按高归一化但横向过窄（冠幅 / 树高 < 0.15）";
        }
    }
    if (degen_why != nullptr) {
        UtilityFunctions::print(String::utf8("[prop] 模型尺寸退化，回退程序化图元 "), path,
                                String::utf8(" 包围盒 "), box.size,
                                String::utf8(" —— "), String::utf8(degen_why));
        raw->queue_free();
        s_prop_failed[p_key] = true;
        return nullptr;
    }

    /* ---- ① 偏航对轴：把**较长的那个水平轴**转到 X ----
       地物没有"正面"，这一步只为一个目的：让归一化读到的水平尺度
       （max(size.x, size.z)）落在模型的真实长轴上，免得拿短边当宽度缩。
       （载具那批的对轴是同一件事，但那边还多一个"哪一端是车头"的残余角 ——
       地物不需要，偏航转过去也没人看得出差别。） */
    const float align_deg = (box.size.z > box.size.x) ? 90.0f : 0.0f;
    constexpr float PI = 3.14159265358979323846f;
    Basis b(Vector3(0.0f, 1.0f, 0.0f), align_deg * PI / 180.0f);
    const AABB rbox = Transform3D(b).xform(box);

    const float hw_meas = std::max(std::max(rbox.size.x, rbox.size.z), 1e-5f);
    const float hy_meas = std::max(rbox.size.y, 1e-5f);

    /* ---- ② 归一化到"单位原型" ----
       按宽：先等比缩到水平尺度 = 1，再把 Y 单独压/拉到 hw（高宽比）。
       按高：等比缩到高 = 1，宽交给模型自己。
       压扁是**在同一个 Basis 上追加一次 scale**，一次给全 —— 与角色那条一样，
       不拆成 set_scale + set_rotation 两步（Node3D 会拿内部缓存的 euler/scale
       重新合成基，顺序与语义都要额外确认，而这里要表达的是一个明确的仿射变换）。 */
    float k = 0.0f, ys = 1.0f;
    if (def->by_height) {
        k = 1.0f / hy_meas;
    } else {
        k = 1.0f / hw_meas;
        ys = (def->hw * hw_meas) / hy_meas;   // 使 (高 × k × ys) / (宽 × k) == hw
    }
    b.scale(Vector3(k, k * ys, k));
    const AABB kbox = Transform3D(b).xform(box);
    const Vector3 c = kbox.get_center();

    Node3D *outer = memnew(Node3D);
    Node3D *norm = memnew(Node3D);
    // 一次给全：绕 Y 对轴 → 单位归一化（含压扁）→ **底面中心挪到原点**。
    // 原点取底面而不是包围盒中心：地物要按"底贴地面"摆，取中心的话每换一个模型
    // 都要重算一次下沉量，而那个数只能靠试。
    norm->set_transform(Transform3D(b, Vector3(-c.x, -kbox.position.y, -c.z)));
    norm->add_child(raw);
    outer->add_child(norm);

    if (p_parent != nullptr) {
        p_parent->add_child(outer);
        outer->set_visible(false);
    } else {
        UtilityFunctions::print(String::utf8("[prop] 警告：没有原型挂载点，模型资源会在退出时报泄漏"));
    }

    UtilityFunctions::print(String::utf8("[prop] 模型 "), String::utf8(p_key.c_str()),
                            String::utf8(" 原始包围盒 "), box.size,
                            String::utf8(" 水平尺度 "), hw_meas,
                            String::utf8(" 高 "), hy_meas,
                            String::utf8(" 归一化 "),
                            def->by_height ? String::utf8("按高 1.0")
                                           : String::utf8("按宽 1.0"),
                            String::utf8(" 高宽比 "), def->by_height ? 0.0f : def->hw,
                            String::utf8(" 对轴 "), align_deg);
    s_prop_proto[p_key] = outer;
    return outer;
}

Node3D *make_prop_node(const std::string &p_key, float p_target, Node *p_proto_parent) {
    Node3D *proto = load_prop_proto(p_key, p_proto_parent);
    if (proto == nullptr) {
        return nullptr;
    }
    Node *dup = proto->duplicate();
    Node3D *n = Object::cast_to<Node3D>(dup);
    if (n == nullptr) {
        if (dup != nullptr) dup->queue_free();
        return nullptr;
    }
    // 与角色/载具同一条坑：duplicate() 会把原型上的 visible=false 一起复制过来，
    // 不显式打开的话整片林子与石头在画面上集体消失。
    n->set_visible(true);
    // 等比缩放。目标语义（宽还是高）由 kPropArt 决定，调用方只把数传进来 ——
    // 让"哪一维是目标"这件事只在一处定义。
    n->set_scale(Vector3(p_target, p_target, p_target));
    return n;
}

// ------------------------------------------------------- 掩体物件
static void add_prop(Node3D *parent, Node *p_proto_parent, const va::Prop &p) {
    // 种在坡面上：掩体的 (x,y) 与 r 都由逻辑层给、一个没动，这里只把它按地形抬起来。
    const Vector3 pos = to3(p.x, p.y, ground_h(p.x, p.y));
    const float r = p.r * S;
    // 弧度换算：地物的偏航是"按 hash 撒开"，不是逻辑层给的 —— 地物没有正面，
    // 转多少只影响"这块石头看起来跟旁边那块不一样"。
    constexpr float DEG2RAD = 3.14159265358979323846f / 180.0f;
    switch (p.type) {
        case va::PropType::Rock: {
            const uint32_t sd = (uint32_t)(p.x * 31 + p.y * 17);
            /* 真模型：两份剪影（棱角裸岩 / 苔覆圆石）按 hash 交替，偏航也按 hash 撒开。
               尺寸照"占多大地方"给：宽 2·r（与 make_rock_mesh 的顶点半径口径一致），
               高由 kPropArt 的 hw=0.50 定成 1.0·r，底面下沉 0.24·r ——
               三条合起来正是原来"网格中心抬 0.26·r、网格自身高 1.0·r"的复现。 */
            if (Node3D *m = make_prop_node((sd & 1u) ? "prop_rock_b" : "prop_rock_a",
                                           2.0f * r, p_proto_parent)) {
                m->set_position(pos + Vector3(0, -0.24f * r, 0));
                m->set_rotation(Vector3(0, (float)(sd % 360u) * DEG2RAD, 0));
                parent->add_child(m);
                break;
            }
            // 回退：低模不规则多面体 + 扁平化，贴地而不是"立起来的蛋"。
            // 这套图元同时是"真模型接坏了吗"的对照基准，**不要删**。
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
            /* 真模型：只按**高度**归一化 —— 而 h 用的还是上面那条原公式，
               所以地平线上的树高分布一个字没改，变的只是"树长什么样"。
               冠幅交给模型自己（约 4 m）：逻辑层的 p.r 对树只管"俯视遮挡半径"、
               高度不受它约束，若改按冠幅归一化，8 米的树会缩成 3 米的灌木。
               偏航按 hash 撒开 —— 针叶树是旋转体，但要的是"这片林子不是同一棵"。 */
            if (Node3D *m = make_prop_node("prop_pine", h, p_proto_parent)) {
                m->set_position(pos);
                m->set_rotation(Vector3(0, (float)((uint32_t)(p.x * 7 + p.y * 13) % 360u) * DEG2RAD, 0));
                parent->add_child(m);
                break;
            }
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
            /* 真模型：按**水平尺度**归一化 —— 2.2·r 是原来那丛小球的水平轮廓量级，
               高压到 0.80·r、底沉 0.10·r。低矮地物"能不能藏住"读的是水平轮廓，
               高度一改那件事就跟着变，所以按宽给、由 kPropArt 的 hw 压扁。 */
            if (Node3D *m = make_prop_node("prop_bush", 2.2f * r, p_proto_parent)) {
                m->set_position(pos + Vector3(0, -0.10f * r, 0));
                m->set_rotation(Vector3(0, (float)((uint32_t)(p.x * 11 + p.y * 23) % 360u) * DEG2RAD, 0));
                parent->add_child(m);
                break;
            }
            // 回退：一丛 3~5 个小球，不是一个孤零零的大球
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
        /* 峡谷网格在谷底恒为 h=0，与这块地板共面 → 会 z-fighting。
           把地板压下去 6 厘米（远处看不出这 6 厘米），峡谷网格就稳稳盖在上面；
           关掉地形时地板回到 0，与改动前逐像素一致，A/B 才有意义。 */
        mi->set_position(to3(va::CFG.W * 0.5f, va::CFG.H * 0.5f,
                             terrain_enabled() ? -0.06f : 0.0f));
        mi->set_material_override(gm);
        ground->add_child(mi);
    }
    root->add_child(ground);

    // ---- 峡谷地形（公路两侧的山坡） ----
    if (terrain_enabled()) {
        Node3D *terr = memnew(Node3D);
        terr->set_name("Terrain");
        Ref<StandardMaterial3D> tm;
        tm.instantiate();
        tm->set_albedo(Color(1, 1, 1));
        tm->set_texture(StandardMaterial3D::TEXTURE_ALBEDO, tex_grass());
        tm->set_roughness(0.98f);
        // UV 已经按"每 5.3 米一重复"烘进了顶点，这里不能再叠 uv1_scale（会二次缩放）
        // 枚举名注意：godot-cpp 里叫 FLAG_ALBEDO_FROM_VERTEX_COLOR（引擎文档里写作
        // vertex_color_use_as_albedo），FLAG_VERTEX_COLOR_USE_AS_ALBEDO 这个名字不存在。
        tm->set_flag(StandardMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
        Ref<ArrayMesh> tmesh = make_terrain_mesh();
        if (std::getenv("VA_DBG_TERRAIN") != nullptr) {
            /* 参数读回。教训（见本文件 build_environment 里那段）：改完只截图看画面，
               是看不出"这次改的值到底有没有编进去"的 —— 画面"看起来差不多"就白跑一轮。
               这里把剖面与几个地标点的地面高度打成可读数字，既自证也留下回归基线。
               判据：h(路心)=0（公路必须平）、h(32.5)=VA_TERRAIN_IN、h(48)=VA_TERRAIN_OUT、
               水口中心恒为 0（河不能被埋）。 */
            const TerrainKnobs &k = tknobs();
            int verts = 0, tris = 0;
            if (tmesh.is_valid() && tmesh->get_surface_count() > 0) {
                const Array arr = tmesh->surface_get_arrays(0);
                const PackedVector3Array vs = arr[Mesh::ARRAY_VERTEX];
                const PackedInt32Array ix = arr[Mesh::ARRAY_INDEX];
                verts = vs.size();
                /* index() 之后顶点是**去重**的，所以顶点数 ≠ 3×三角形数。
                   拿 vs.size()/3 当三角形数会少报一个数量级（实测 6308 vs 真值 37064）——
                   自检里的数字必须可信，否则它比没有更糟。 */
                tris = (ix.size() > 0) ? ix.size() / 3 : vs.size() / 3;
            }
            UtilityFunctions::print(String::utf8("[terrain] 开关 开  地图内最高 "), k.h_in,
                                    String::utf8(" m  谷壁峰值 "), k.h_out,
                                    String::utf8(" m  起伏 "), k.bump,
                                    String::utf8("  水口 "), k.gorge ? 1 : 0,
                                    String::utf8("  网格顶点 "), verts,
                                    String::utf8(" / 三角形 "), tris);
            struct P { const char *name; float lx, ly; };
            const P pts[] = {
                { "路心(1100,650)",      1100.0f,  650.0f },
                { "桥/水口(360,650)",     360.0f,  650.0f },
                { "水口北(360,200)",      360.0f,  200.0f },
                { "玩家出生(872,872)",    872.0f,  872.0f },
                { "北侧岩石(1128,350)",  1128.0f,  350.0f },
                { "南侧树林(880,1240)",   880.0f, 1240.0f },
                { "地图北界(1100,6)",    1100.0f,    6.0f },
                { "地图南界(1100,1294)", 1100.0f, 1294.0f },
                { "谷壁峰值(1100,1610)", 1100.0f, 1610.0f },   // 逻辑坐标，已在地图外
            };
            for (const P &p : pts) {
                UtilityFunctions::print(String::utf8("[terrain] "), String::utf8(p.name),
                                        String::utf8(" 地面 "), ground_h(p.lx, p.ly), String::utf8(" m"));
            }
        }
        add_mesh(terr, tmesh, Vector3(), tm);
        root->add_child(terr);
    } else if (std::getenv("VA_DBG_TERRAIN") != nullptr) {
        // 关掉时也要出声：没有这一行，"VA_TERRAIN=0 确实生效了"只能靠看图，
        // 而 A/B 里最容易犯的错正是"以为自己关掉了、其实环境变量没传进去"。
        UtilityFunctions::print(String::utf8("[terrain] 开关 关（VA_TERRAIN=0，平地，地板 h=0）"));
    }

    // ---- 远景山脊：打破死板地平线，配合雾形成层叠剪影 ----
    {
        Ref<ArrayMesh> rm = make_ridges_mesh(va::CFG.W * 0.5f * S, va::CFG.H * 0.5f * S);
        Ref<StandardMaterial3D> rmm = mat_solid(Color(0.105f, 0.125f, 0.115f), 1.0f);
        add_mesh(root, rm, Vector3(), rmm);
    }

    // ---- 河流 ----
    {
        /* 水面长度：原来只有地图那么长（CFG.H），因为再往外是一片平地、看不出断头。
           峡谷把河道所在的缺口一直削到离路 100 米，水若还停在地图边界，从谷底顺着
           缺口看过去就是"河在半空中截断"。所以开地形时把水面延到缺口全长。
           关地形时仍用原长度 —— 否则 VA_TERRAIN=0 的 A/B 里连河都不一样宽，
           那就分不清差异是地形造成的还是这条改动造成的。 */
        const float river_len = terrain_enabled() ? 205.0f : va::CFG.H * S;
        Ref<BoxMesh> rm = memnew(BoxMesh);
        rm->set_size(Vector3((va::CFG.riverX2 - va::CFG.riverX1) * S, 0.10f, river_len));
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
        const float x1 = va::ROAD_PATH[(size_t)i].x, y1 = va::ROAD_PATH[(size_t)i].y;
        const float x2 = va::ROAD_PATH[(size_t)(i + 1)].x, y2 = va::ROAD_PATH[(size_t)(i + 1)].y;
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
            const float x1 = va::ROAD_PATH[(size_t)i].x, y1 = va::ROAD_PATH[(size_t)i].y;
            const float x2 = va::ROAD_PATH[(size_t)(i + 1)].x, y2 = va::ROAD_PATH[(size_t)(i + 1)].y;
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
    // 原型挂载点传 root：这一段跑在 out.units 建出来之前，而原型只需要"在场景树里
    // 且隐藏"。静态缓存按键存，所以谁先跑谁挂 —— build_scene 恒先于 rebuild_props，
    // 于是原型这辈子都挂在 root 下，不随换关重建的 Props 层一起消失。
    for (const auto &p : va::W.props) add_prop(props, root, p);
    root->add_child(props);
    out.props = props;
    out.root = root;

    // ---- 动态实体挂载点 ----
    out.units = memnew(Node3D);    out.units->set_name("Units");       root->add_child(out.units);
    out.vehicles = memnew(Node3D); out.vehicles->set_name("Vehicles"); root->add_child(out.vehicles);
    out.decals = memnew(Node3D);   out.decals->set_name("Decals");     root->add_child(out.decals);
}

} // namespace volunteer_army
