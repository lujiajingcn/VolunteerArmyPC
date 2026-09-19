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

// ---- 峡谷地形（见 scene_builder.cpp 顶部那段长注释）----
//
// 公路压在谷底正中，两侧按剖面抬升成山坡，公路因此位于一条两侧上坡夹出的峡谷内。
//
// 【纯表现层】逻辑层仍是 2D 平面：单位的 (x,y)、掩体的 (x,y) 与 r、判定顺序均未改动。
// 子弹的遮挡按 2D 圆判定、不看高度，所以地图内（玩家走得到的范围）的抬升被刻意压到
// 4.5 米量级，峡谷的天际线主要由地图外那段（走不到）承担 —— 详见 .cpp 里的取舍说明。
//
// ground_h 接受**逻辑层坐标**，返回地面高度（米，可直接当 to3 的第三个参数）。
// 地形网格、单位/掩体摆位、相机高度**共用**它，任何一处单独改都会立刻出现
// "人埋在坡里 / 石头浮空"。
float ground_h(float p_lx, float p_ly);

// 地形总开关（VA_TERRAIN=0 关）。关掉时地板回到原来的平地，用于做 A/B 对照。
bool terrain_enabled();

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

// ---- 武器三维模型（图生3D 产物，tools/fetch_wpn_refs.sh + tools/gen3d_batch.py）----
//
// 约定：assets/art/wpn/model/<键>.glb。载入失败一律返回 nullptr，由调用方回退程序化枪模 ——
// 少一个模型文件不该让玩家手里没有枪。
//
// 返回的节点已经**归一化到 ViewModel 的 gun 局部坐标系**：
//   原点在枪托尾端平面，枪口朝 -Z，y≈0 是枪管轴线，单位是米。
// 归一化放在这里而不是 ViewModel：它要用到与角色模型同一套"量包围盒 + 对轴 + 按实长缩放"，
// 而 collect_aabb 是本文件的内部函数。
//
// 注意：**渲染层与阴影不在这个函数里设**。枪模要挂到"只照枪的那几盏灯"所在的层上，
// 那是 ViewModel 的光照安排的产物，由它自己走一遍子树去设。
// 程序化枪模上双手**手掌方块中心**的落点（gun 局部系，米）。
// 【为什么放在头文件里】真模型的握持点是以它俩为基准做偏移的
// （见 WpnNodeInfo::hand_r_pos / load_skin），两边必须是同一个数 ——
// 各写一份就一定会漂移，而漂移的症状是"手慢慢从枪上滑开"，一次几厘米根本看不出来。
// viewmodel.cpp 里建那两只程序化手时也直接引用这两个常量。
constexpr float WPN_HAND_R0 = -0.015f;   // 右手（握把）z
constexpr float WPN_HAND_L0 = -0.260f;   // 左手（护木）z

struct WpnNodeInfo {
    float         length = 0.0f;     // 归一化后的全长（米）
    float         muzzle_z = 0.0f;   // 枪口在返回节点局部系的 z（负值）
    float         scale = 0.0f;      // 施加的缩放倍率（排查用）
    // 双手手掌方块的中心在**枪的局部系**里的位置（米）。由 tools/glb_preview.py --probe
    // 按真模型的剖面量出来，见 scene_builder.cpp 的 kWpnArt。
    godot::Vector3 hand_r_pos;
    godot::Vector3 hand_l_pos;
    godot::Vector3 raw_size;         // 归一化**之前**模型自身的尺寸（排查用）
};

// 全部武器键，顺序 = 游戏里切换外观的顺序，也是检阅台 / 自检的顺序。
const std::vector<std::string> &all_wpn_keys();

// 武器的中文显示名（HUD 提示用）。未知键返回"未知武器"而不是空串 ——
// 空串在界面上表现为"什么都没显示"，会被误读成"没切成"。
const char *wpn_label(const std::string &p_key);

// 造一个归一化的武器节点。p_proto_parent 是原始场景根的挂载点（要挂进场景树，
// 否则退出时会被 Godot 报 RID 泄漏）。失败返回 nullptr 并把 out 留空。
godot::Node3D *make_wpn_node(const std::string &p_key, godot::Node *p_proto_parent,
                             WpnNodeInfo &out);

// 程序化纹理（保持"零外部资源"）
godot::Ref<godot::Texture2D> tex_grass();
godot::Ref<godot::Texture2D> tex_road();
godot::Ref<godot::Texture2D> tex_water();

} // namespace volunteer_army
