#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>

namespace volunteer_army {

// 第一人称武器视图模型。
//
// 节点层次：
//     Camera3D
//       └ root      位置 = 枪在相机空间的落点（含 sway / bob / 后坐位移）
//         └ gun     位置 = 0，旋转 = 持枪姿态角（腰射↔开镜插值）
//           ├ proc_body   程序化枪身（有真模型时整块隐藏）
//           ├ hands       双手与前臂（**默认显示**；VA_VM_HANDS=0 藏掉）
//           ├ art_holder  真模型的挂载点（gun 局部系，见下）
//           ├ muzzle
//           └ flash_group
//
// 为什么要把"位移"和"姿态"分成两层：位移要跟着呼吸/走路/后坐抖，
// 姿态角只在腰射↔开镜之间平滑过渡。混在一层里，开镜过渡会被后坐的
// 旋转叠加上去，准心对不准。
//
// ---------------------------------------------------------------------------
// 【武器外观（纯表现层）】
//
// 三把 1951 年志愿军制式武器（莫辛-纳甘 / 波波沙 / DP-27），按键循环切换。
// 它们**共用同一个 gun 节点**：位移与姿态的数学一行都不改，每把枪只是
// "挂在 gun 下面的一棵子树 + 一个枪口位置"。
//
// 为什么这件事能绕开"不能动平衡基线"：手里拿的模型属于表现，
// 换模型不改变任何伤害 / 射速 / 弹匣 —— src/sim/ 一行都不用动，
// 于是"不能改判定顺序"这条约束天然成立。这也是当初选它作为落点的原因。
//
// 真模型的坐标系由 scene_builder 的 make_wpn_node 归一化成与程序化枪模**同构**的：
// 原点在枪托尾端平面、枪口朝 -Z、单位是米。所以两种外观可以随时对调，
// 枪口焰的位置由 skin 的 muzzle_z 决定。
struct ViewModel {
    godot::Camera3D *cam = nullptr;
    godot::Node3D *root = nullptr;
    godot::Node3D *gun = nullptr;
    godot::Node3D *muzzle = nullptr;
    godot::OmniLight3D *flash_light = nullptr;
    godot::Node3D *flash_group = nullptr;

    // ---- 外观切换 ----
    static constexpr int MAX_SKINS = 8;
    godot::Node3D *proc_body = nullptr;
    godot::Node3D *hands = nullptr;
    godot::Node3D *hand_r = nullptr;             // 右手 + 右前臂（整组沿 z 平移）
    godot::Node3D *hand_l = nullptr;             // 左手 + 左前臂
    godot::Node3D *art_holder = nullptr;
    godot::Node3D *art_nodes[MAX_SKINS] = {};   // 懒加载，建好就留着（切换只是切可见性）
    float art_mz[MAX_SKINS] = {};               // 各外观的枪口 z（gun 局部系）
    float art_len[MAX_SKINS] = {};              // 各外观归一化后的全长（米，日志用）
    godot::Vector3 art_hand_r[MAX_SKINS] = {};  // 各外观的右手手掌中心（gun 局部系）
    godot::Vector3 art_hand_l[MAX_SKINS] = {};  // 各外观的左手手掌中心
    int  art_slots = 0;                          // 有效槽位数 = 武器键数
    int  art_index = 0;
    float art_muzzle_z = 0.0f;                   // 当前外观的枪口 z（gun 局部系）
    godot::Node *proto_parent = nullptr;         // 原始场景根的挂载点（场景里的容器）

    // ---- 姿态基准（build 时读环境变量覆盖，默认值见 viewmodel.cpp 的注释推导）----
    godot::Vector3 hip_pos{0.150f, -0.130f, -0.330f};
    godot::Vector3 aim_pos{0.018f, -0.075f, -0.290f};
    godot::Vector3 hip_rot{2.5f, 7.0f, -5.0f};
    godot::Vector3 aim_rot{0.0f, 0.0f, 0.0f};

    float sway_yaw = 0, sway_pitch = 0;     // 视角惯性滞后
    float recoil = 0, recoil_v = 0;         // 弹簧-阻尼后坐
    float ads = 0, bob = 0, breath = 0;
    float flash_t = 0, reload_t = 0, reload_dur = 0;

    // ---- 枪口焰的取证计数（VA_DBG_VM=1 时打进日志）----
    // 闪光只活 0.045 秒，**肉眼、定时截图、事件落盘三种手段都抓不住它**
    // （事件落盘的等待是 0.05 秒墙钟 > 0.045 秒）。所以"到底画没画出来"这件事
    // 只能靠"亮了几帧、每帧 dt 多少"来回答 —— 而它恰好由 dt 决定。
    int   flash_frames = 0;                 // 本次闪光实际被画出来的帧数
    float flash_dt_max = 0.0f;              // 本次闪光期间的最大帧间隔（秒）
    float dbg_dt_acc = 0.0f;                // VA_DBG_VM 的帧率心跳累加器
    int   dbg_dt_frames = 0;
    float last_yaw = 0, last_pitch = 0;
    float fov_base = 65.0f, fov_ads = 45.5f;

    // 五盏只照枪模的平行光，以及天气亮度倍率（见 .cpp 里"为什么夜战要压暗"）
    static constexpr int VM_LIGHT_COUNT = 5;
    godot::DirectionalLight3D *vm_lights[VM_LIGHT_COUNT] = {};
    float vm_energy[VM_LIGHT_COUNT] = {};
    float vm_wmul = -1.0f;

    // p_proto_parent：原始模型场景根的挂载点（传场景里的容器节点）。
    // 必须挂进场景树 —— 只存在静态 map 里的话，它持有的 mesh / material / 贴图
    // 会在进程退出时被 Godot 报成 "RID allocations ... leaked at exit" 的 ERROR，
    // 而"日志里有没有 ERROR"正是本工程的回归判据。
    void build(godot::Camera3D *p_cam, godot::Node *p_proto_parent);
    void update(double dt, float move01, bool running, float pitch, float yaw, bool ads_want);
    void on_shot();
    void on_reload(float dur);
    bool valid() const { return root != nullptr; }

    // ---- 外观 ----
    // 切到下一把（p_step 可为负）。返回是否真的换掉了。
    bool next_skin(int p_step = 1);
    // 挂上第 p_index 个外观（懒加载）。失败返回 false，调用方保持原样。
    bool load_skin(int p_index);
    // 当前外观的键与显示名。没有真模型时返回空串（表示"正在用程序化枪模"）。
    const char *skin_key() const;
    const char *skin_label() const;
    // 当前是否**真的**在用手里的真模型（而不是程序化枪模）。
    //
    // 【别写成 art_holder != nullptr】art_holder 是 build() 里**无条件**建的挂载点
    // （见 viewmodel.cpp 里 `art_holder = memnew(Node3D)`），它一直非空 ——
    // 拿它当判据等于恒为 true。这个错实测发生过：HUD 的"枪上有没有瞄具"
    // 于是永远答"有"，开镜时准星永远让位，连程序化枪模那份也一起让掉了。
    // 真正该问的是"这个槽位的模型节点建出来了吗"：load_skin 才会建它，
    // 而 VA_VM_NO_ART 走的分支根本不调 load_skin。
    bool using_art() const {
        return art_index >= 0 && art_index < MAX_SKINS && art_nodes[art_index] != nullptr;
    }
};

} // namespace volunteer_army
