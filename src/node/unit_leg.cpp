// VolunteerArmyPC —— 单位双腿交替实现（见 unit_leg.h 的完整设计说明）
#include "node/unit_leg.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector4.hpp>

using namespace godot;

namespace volunteer_army {
namespace {

// 着色器路径。放在 res://shaders/ 下而不是把源码内联进 C++：
// 着色器是文本资源，改它不必重新编译；写在 .cpp 里的字符串则每次都要走一遍
// 编译 → 链接 → 换 dll，标定期间这个差别很实在。
const char *kShaderPath = "res://shaders/unit_leg.gdshader";

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

} // namespace

// ============================================================ setup
void UnitLeg::setup() {
    enabled_ = env_flag("VA_LEG", true);
    if (!enabled_) {
        UtilityFunctions::print(String::utf8(
            "[leg] VA_LEG=0 —— 双腿交替关闭（材质保持原始 StandardMaterial3D，画面与未加本层时逐像素相同）"));
        return;
    }

    dbg_ = env_flag("VA_DBG_LEG", false);
    probe_ = (int)env_f("VA_LEG_PROBE", 0.0f);
    hip_frac_ = env_f("VA_LEG_HIP", 0.52f);
    band_frac_ = env_f("VA_LEG_BAND", 0.15f);
    knee_deg_ = env_f("VA_LEG_KNEE", 0.0f);
    knee_frac_ = env_f("VA_LEG_KNEE_POS", 0.28f);

    /* ⚠️ ResourceLoader 在 godot-cpp 里是**单例**（load 不是静态成员），
       必须经 get_singleton() 调。写成 `ResourceLoader::load(...)` 会报
       C2352「调用非静态成员函数需要一个对象」—— 这不影响运行期行为，
       纯粹是绑定层的表达方式，但编译期只给你一句 C2352，看不出该怎么改。 */
    ResourceLoader *rl = ResourceLoader::get_singleton();
    const Ref<Resource> r = (rl != nullptr) ? rl->load(String::utf8(kShaderPath))
                                            : Ref<Resource>();
    // Ref 的赋值有 operator=(const Ref<T>&) 与 operator=(const Variant&) 两个候选，
    // 直接 `shader_ = Object::cast_to<Shader>(...)` 会被判二义（C2593）。
    // 显式构造目标类型的 Ref 就没有这个问题。
    if (Shader *sp = Object::cast_to<Shader>(r.ptr())) {
        shader_ = Ref<Shader>(sp);
    }
    if (shader_.is_null()) {
        // **不 push_error**：日志里有没有 ERROR 是本工程的回归判据，
        // 而"着色器没加载上"是可预期的情况（文件缺失 / 编辑期）。降级即可。
        enabled_ = false;
        UtilityFunctions::print(String::utf8("[leg] 着色器不可用 "), String::utf8(kShaderPath),
                                String::utf8(" —— 双腿交替降级关闭（外观回退到原始材质）"));
        return;
    }

    /* ORM 兜底纹理（1×1）。这一条是防御性的，但必须要有：
       若某批模型将来没带 metallicRoughness 贴图，sampler2D 未设置时 Godot 给的是
       **纯白**，而白色在 glTF 布局里 = roughness 1 + **metallic 1**
       —— 整个角色会变成全金属黑块，而不是"没有粗糙度信息"。
       给一张 G=0.8（粗糙）、B=0（非金属）的中间值，最坏情况只是外观偏平。 */
    Ref<Image> img = Image::create(1, 1, false, Image::FORMAT_RGB8);
    img->set_pixel(0, 0, Color(1.0f, 0.8f, 0.0f));
    // 同上：显式构造 Ref<Texture2D>，避开 Ref 赋值的二义解析。
    fallback_orm_ = Ref<Texture2D>(ImageTexture::create_from_image(img).ptr());

    UtilityFunctions::print(
        String::utf8("[leg] 双腿交替就绪：髋 "), String::num((double)hip_frac_, 3),
        String::utf8("H 过渡带 "), String::num((double)band_frac_, 3),
        String::utf8("H 膝弯 "), String::num((double)knee_deg_, 1),
        String::utf8("° 探针 "), probe_, String::utf8("（1=权重染色 2=每实例参数染色）"));
}

// ============================================================ 找网格
MeshInstance3D *UnitLeg::find_mesh(Node3D *p_root) {
    if (p_root == nullptr) return nullptr;
    if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_root)) return mi;
    const int nc = p_root->get_child_count();
    for (int i = 0; i < nc; ++i) {
        if (MeshInstance3D *mi = find_mesh(Object::cast_to<Node3D>(p_root->get_child(i)))) {
            return mi;
        }
    }
    return nullptr;
}

// ============================================================ 首次建材质
UnitLeg::KeyInfo &UnitLeg::ensure(const std::string &p_key, MeshInstance3D *p_mi) {
    auto it = keys_.find(p_key);
    if (it != keys_.end()) return it->second;

    // 先插入占位再填 —— 失败时留下的是一条 ok=false 的"墓碑"，
    // 否则每帧都会重走一遍失败路径（3 张贴图查询 + 一条日志刷屏）。
    KeyInfo &ki = keys_[p_key];

    const Ref<Mesh> mesh = p_mi->get_mesh();
    if (mesh.is_null() || mesh->get_surface_count() <= 0) {
        UtilityFunctions::print(String::utf8("[leg] "), String::utf8(p_key.c_str()),
                                String::utf8(" 没有可用网格 —— 该键不做腿摆"));
        return ki;
    }

    // 原材质：glTF 导入后贴在 mesh 的 surface 上，不是 override。
    // get_active_material 按 override > surface override > surface 取，一次到位。
    const Ref<Material> src = p_mi->get_active_material(0);
    StandardMaterial3D *sm = Object::cast_to<StandardMaterial3D>(src.ptr());

    Ref<Texture2D> alb, orm, nrm;
    if (sm != nullptr) {
        alb = sm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
        // glTF 的 metallicRoughness 只有一张图，Godot 导入时可能挂到
        // TEXTURE_ROUGHNESS、TEXTURE_METALLIC 或 TEXTURE_ORM 任一个槽上（版本相关），
        // 三个都试一遍，谁先有就用谁。布局都是 G=roughness / B=metallic，不必重排。
        orm = sm->get_texture(BaseMaterial3D::TEXTURE_ROUGHNESS);
        if (orm.is_null()) orm = sm->get_texture(BaseMaterial3D::TEXTURE_METALLIC);
        if (orm.is_null()) orm = sm->get_texture(BaseMaterial3D::TEXTURE_ORM);
        nrm = sm->get_texture(BaseMaterial3D::TEXTURE_NORMAL);
    }

    /* 几何参数全部由**网格自己的 AABB** 推出来，不写死常数 ——
       11 个角色的包围盒高度并不相同（实测 1.045 ~ 1.091），
       写死一个绝对值必然在某几个角色上把髋摆到膝盖或腰上。

       ⚠️ AABB 是 mesh 空间的，也就是**归一化之前、且 Z-up** 的那套坐标：
       z 轴上 **z=0 是脚、z=−H 是头顶**（−Z 朝上）。
       所以"从脚往上 hip_frac 身高"对应 z 变小：
           脚 z = position.z + size.z
           髋 z = 脚 z − hip_frac × size.z = position.z + (1 − hip_frac) × size.z */
    const AABB ma = mesh->get_aabb();
    const float foot_z = ma.position.z + ma.size.z;
    ki.hip_z = foot_z - hip_frac_ * ma.size.z;
    ki.band = std::max(band_frac_ * ma.size.z, 1e-4f);
    ki.knee_z = foot_z - knee_frac_ * ma.size.z;

    Ref<ShaderMaterial> m;
    m.instantiate();
    m->set_shader(shader_);
    if (alb.is_valid()) m->set_shader_parameter("albedo_tex", alb);
    m->set_shader_parameter("orm_tex", orm.is_valid() ? Variant(orm) : Variant(fallback_orm_));
    if (nrm.is_valid()) m->set_shader_parameter("normal_tex", nrm);
    m->set_shader_parameter("leg_probe", probe_);
    m->set_shader_parameter("knee_deg", knee_deg_);
    m->set_shader_parameter("knee_z", ki.knee_z);

    ki.mat = m;
    ki.ok = true;
    ++attached_;

    if (dbg_ || !ki.reported) {
        ki.reported = true;
        UtilityFunctions::print(
            String::utf8("[leg] "), String::utf8(p_key.c_str()),
            String::utf8(" 包围盒z["), String::num((double)ma.position.z, 3),
            String::utf8(", "), String::num((double)(ma.position.z + ma.size.z), 3),
            String::utf8("] H="), String::num((double)ma.size.z, 3),
            String::utf8(" ⇒ 髋z="), String::num((double)ki.hip_z, 3),
            String::utf8(" 过渡带="), String::num((double)ki.band, 3),
            String::utf8(" 膝z="), String::num((double)ki.knee_z, 3),
            String::utf8(" 贴图 反照="), alb.is_valid(), String::utf8(" 糙金="), orm.is_valid(),
            String::utf8(" 法线="), nrm.is_valid());
    }
    return ki;
}

// ============================================================ 每帧
void UnitLeg::apply(Node3D *p_unit_node, const std::string &p_key,
                    float p_phase, float p_swing_deg) {
    if (!enabled_ || p_unit_node == nullptr) return;

    MeshInstance3D *mi = find_mesh(p_unit_node);
    if (mi == nullptr) {
        if (dbg_) ++no_mesh_;
        return;
    }

    KeyInfo &ki = ensure(p_key, mi);
    if (!ki.ok) return;

    mi->set_material_override(ki.mat);
    /* 逐实例参数。用 instance uniform 而不是"每个单位克隆一份材质"：
       克隆材质每帧写参数会让材质版本号变化，进而重复提交渲染状态；
       instance uniform 本来就是为"同一份材质、每个实例不同值"设计的。 */
    mi->set_instance_shader_parameter(StringName("u_leg"),
                                      Vector4(p_phase, p_swing_deg, ki.hip_z, ki.band));
}

// ============================================================ 诊断
String UnitLeg::dump() const {
    String s = String::utf8("腿摆层：");
    if (!enabled_) return s + String::utf8("VA_LEG=0 关闭");
    s += String::utf8("已挂网格 ");
    s += String::num(attached_);
    if (dbg_) {
        s += String::utf8(" 未找到网格 ");
        s += String::num(no_mesh_);
    }
    s += String::utf8(" 髋H=");
    s += String::num((double)hip_frac_, 3);
    s += String::utf8(" 带H=");
    s += String::num((double)band_frac_, 3);
    return s;
}

} // namespace volunteer_army
