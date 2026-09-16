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

    // 光照三件套（主光 / 补光 / 反弹光）—— 留引用是为了运行时还能调
    // （例如夜战 / 爆炸闪光临时提亮、剧情转折时换色温）
    godot::WorldEnvironment *world_env = nullptr;
    godot::DirectionalLight3D *sun = nullptr;
    godot::DirectionalLight3D *fill = nullptr;
    godot::DirectionalLight3D *bounce = nullptr;
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

// 程序化纹理（保持"零外部资源"）
godot::Ref<godot::Texture2D> tex_grass();
godot::Ref<godot::Texture2D> tex_road();
godot::Ref<godot::Texture2D> tex_water();

} // namespace volunteer_army
