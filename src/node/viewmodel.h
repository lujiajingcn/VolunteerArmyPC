#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>

namespace volunteer_army {

// 第一人称武器视图模型。
//
// 节点层次：
//     Camera3D
//       └ root      位置 = 枪在相机空间的落点（含 sway / bob / 后坐位移）
//         └ gun     位置 = 0，旋转 = 持枪姿态角（腰射↔开镜插值）
//           ├ …枪械部件（局部原点在"枪托底板中心"，枪管沿 -Z 伸出）
//           ├ muzzle
//           └ flash_group
//
// 为什么要把"位移"和"姿态"分成两层：位移要跟着呼吸/走路/后坐抖，
// 姿态角只在腰射↔开镜之间平滑过渡。混在一层里，开镜过渡会被后坐的
// 旋转叠加上去，准心对不准。
struct ViewModel {
    godot::Camera3D *cam = nullptr;
    godot::Node3D *root = nullptr;
    godot::Node3D *gun = nullptr;
    godot::Node3D *muzzle = nullptr;
    godot::OmniLight3D *flash_light = nullptr;
    godot::Node3D *flash_group = nullptr;

    // ---- 姿态基准（build 时读环境变量覆盖，默认值见 viewmodel.cpp 的注释推导）----
    godot::Vector3 hip_pos{0.150f, -0.130f, -0.330f};
    godot::Vector3 aim_pos{0.018f, -0.075f, -0.290f};
    godot::Vector3 hip_rot{2.5f, 7.0f, -5.0f};
    godot::Vector3 aim_rot{0.0f, 0.0f, 0.0f};

    float sway_yaw = 0, sway_pitch = 0;     // 视角惯性滞后
    float recoil = 0, recoil_v = 0;         // 弹簧-阻尼后坐
    float ads = 0, bob = 0, breath = 0;
    float flash_t = 0, reload_t = 0, reload_dur = 0;
    float last_yaw = 0, last_pitch = 0;
    float fov_base = 65.0f, fov_ads = 45.5f;

    // 五盏只照枪模的平行光，以及天气亮度倍率（见 .cpp 里"为什么夜战要压暗"）
    static constexpr int VM_LIGHT_COUNT = 5;
    godot::DirectionalLight3D *vm_lights[VM_LIGHT_COUNT] = {};
    float vm_energy[VM_LIGHT_COUNT] = {};
    float vm_wmul = -1.0f;

    void build(godot::Camera3D *cam);
    void update(double dt, float move01, bool running, float pitch, float yaw, bool ads_want);
    void on_shot();
    void on_reload(float dur);
    bool valid() const { return root != nullptr; }
};

} // namespace volunteer_army
