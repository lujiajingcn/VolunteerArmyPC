#pragma once
// VolunteerArmyPC —— 3D 战场场景构建
//
// 坐标映射：逻辑层是 2D 平面 (x, y)，比例尺 1 米 ≈ 20 世界单位。
//   Godot 位置 = (x * S, height, y * S)，S = 0.05
//   于是 2200×1300 的世界 → 110×65 米的战场，人的肩宽 0.52 米、眼高 1.65 米，
//   与网页版完全一致 —— 逻辑层不需要任何换算。
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include <string>
#include <vector>

#include "sim/va_world.h"

// 只用到指针，前向声明即可，不必把 environment.hpp / directional_light3d.hpp 拉进头文件
namespace godot {
class WorldEnvironment;
class DirectionalLight3D;
} // namespace godot

namespace volunteer_army {

// 1 世界单位 = 0.05 米
constexpr float S = 0.05f;

inline godot::Vector3 to3(float x, float y, float h = 0.0f) {
    return godot::Vector3(x * S, h, y * S);
}

// 场景静态部分构建完成后，返回各实体的挂载点
struct SceneRefs {
    godot::Node3D *root = nullptr;
    godot::Node3D *props = nullptr;
    godot::Node3D *units = nullptr;
    godot::Node3D *vehicles = nullptr;
    godot::Node3D *decals = nullptr;

    // 光照四件套（主光 / 补光 / 反弹光 / 补洞光）—— 留引用是为了运行时还能调
    // （例如夜战 / 爆炸闪光临时提亮、剧情转折时换色温）
    godot::WorldEnvironment *world_env = nullptr;
    godot::DirectionalLight3D *sun = nullptr;
    godot::DirectionalLight3D *fill = nullptr;
    godot::DirectionalLight3D *bounce = nullptr;
    // RIM：专补太阳与补光之间那个"两边都照不到"的方位扇区，见 scene_builder.cpp
    // 里 RIM 那段的长注释 —— 少了它，竖直面朝那个方位时三盏灯 N·L 全为负，
    // 只剩微弱环境光，在画面上就是一根纯黑剪影树干（实测 RGB(9,15,26)）。
    godot::DirectionalLight3D *rim = nullptr;
    godot::Ref<godot::Material> sky_mat;   // ProceduralSkyMaterial，随天气换色
};

// 构建整个静态世界（天空/光照/地形/公路/河流/桥/掩体），并把引用填进 out
void build_scene(godot::Node3D *root, SceneRefs &out);

// 只重建掩体层（油桶会被殉爆打掉、掩体会被毁，重开一局必须把视觉层一并刷新）。
// 调用前提：va::init_world() 已经跑过，W.props 已是新一局的掩体表。
void rebuild_props(godot::Node3D *root, SceneRefs &refs);

// 按 logic 层当前的 va::W.weather 重新打一整套布光/雾/天空参数。
// 天气在 init_world() 里随机抽（重玩性），所以重开一局必须重调一次。
void apply_weather(SceneRefs &refs);

// 单个实体的可视化节点（供动态增删）
godot::Node3D *make_soldier_node(bool enemy, bool downed);
godot::Node3D *make_vehicle_node(const std::string &type);
godot::Node3D *make_box_node();

// ---- 角色三维模型（图生3D 产物，tools/gen3d.py + tools/slim_glb.py）----
//
// 约定：assets/art/char/model/<键>.glb。键由 unit_model_key() 从 Unit 导出。
// 载入失败（文件缺失 / 解析失败 / 场景为空 / 没有网格）一律回退到 make_soldier_node ——
// 少一个模型文件不该让战场上少一个人。
//
// 返回的是**外层节点**：摆位与偏航由 sync_entity_nodes 负责，模型自身的
// 缩放/脚底归零/朝向校正放在内层，免得被每帧的覆盖写掉。
//
// p_proto_parent 是模型**原型**的挂载点（传场景里已有的容器节点，如 units 层）。
// 原型必须挂进场景树，不能只 memnew 后存在静态 map 里 —— 那样它持有的
// mesh / material / 3 张贴图在进程退出时会被 Godot 报成
// "RID allocations ... leaked at exit" 共 8 条 ERROR，而"日志里有没有 ERROR"
// 正是本工程的回归判据，被这种假错误污染之后真问题就看不见了。
godot::Node3D *make_unit_node(const va::Unit &u, godot::Node *p_proto_parent);

// 按美术键直接建（检阅台 VA_UNIT_SHOW 用：手里只有键，没有 Unit）。
// make_unit_node 内部就是先 unit_model_key(u) 再转到这里。
godot::Node3D *make_unit_node_by_key(const std::string &p_key, godot::Node *p_proto_parent);

// 单位在场景里的姿态：偏航（模型前方 = +X，故绕 Y 转 -facing）+ 倒地时绕**自身前方轴**
// 的 84° 侧翻；p_x / p_y 是逻辑层的 2D 坐标。
//
// 【为什么抽成公共函数】这个姿态现在有**两个**施加者：sync_entity_nodes 每帧施加，
// 检阅台（VA_UNIT_SHOW）建节点时就施加。两处各写一份，迟早出现"战场上倒下、
// 检阅台上还站着"这种只改了一边的偏差 —— 而这类偏差在截图上极难发现，
// 因为两条路径从不出现在同一张图里。
godot::Transform3D unit_transform(float p_x, float p_y, float p_facing, bool p_downed);

// 角色美术键：三维模型文件名与胸像纹理名**共用同一个键**（char_*）。
// 拆成"按 id"和"按军官/机枪标志"两个入口，是因为两个调用方拿到的东西不同：
//   · sync_entity_nodes 手里是 Unit（含 id / officer / mg）
//   · 简报名册手里是花名册的 id 字符串
// 两处各写一份映射迟早会漂移，所以只有这里一份真值。
std::string ally_art_key(const std::string &p_id);
std::string enemy_art_key(bool p_officer, bool p_mg);

// 全部 11 个美术键，顺序 = 检阅台的陈列顺序（我方 8 种职务 → 敌方 3 种）。
// 检阅台与"模型是否齐备"的自检都用它，避免再抄一遍键名列表。
const std::vector<std::string> &all_art_keys();

// Unit -> 模型键
std::string unit_model_key(const va::Unit &u);

// 程序化纹理（保持"零外部资源"）
godot::Ref<godot::Texture2D> tex_grass();
godot::Ref<godot::Texture2D> tex_road();
godot::Ref<godot::Texture2D> tex_water();

} // namespace volunteer_army
