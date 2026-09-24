#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
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
    godot::Vector3 art_sight[MAX_SKINS] = {};   // 各外观的照门位置（gun 局部系）
    int  art_slots = 0;                          // 有效槽位数 = 武器键数
    int  art_index = 0;
    float art_muzzle_z = 0.0f;                   // 当前外观的枪口 z（gun 局部系）
    godot::Node *proto_parent = nullptr;         // 原始场景根的挂载点（场景里的容器）

    // ---- 姿态基准（build 时读环境变量覆盖，默认值见 viewmodel.cpp 的注释推导）----
    //
    // 这一组是**腰射**的基准。它改过一轮（2026-09-19）：换真模型之后枪长从
    // 0.748 m 涨到 1.232 m，而这一组还是照程序化枪模调的，于是支撑手被顶到
    // 眼前 0.78 m、前臂横跨 0.6 m，画面上读出来是"一根撑杆 + 一块木头"。
    // 新值 + vm_scale=0.62 把支撑手拉回 0.41 m（真持枪约 0.35~0.45 m）。
    godot::Vector3 hip_pos{0.105f, -0.088f, -0.130f};
    godot::Vector3 aim_pos{0.018f, -0.075f, -0.290f};
    godot::Vector3 hip_rot{3.0f, 13.0f, -6.0f};
    godot::Vector3 aim_rot{0.0f, 0.0f, 0.0f};
    // VA_VM_AIM 是否被显式给过。给过就整套照它的算（取证/找位用），
    // 没给就按"枪自己的照门落在屏幕中心"反推。
    bool aim_env_over = false;

    // ---------------------------------------------------------------------
    // 【视图模型统一缩放 + "眼睛在枪的哪儿"】
    //
    // 真模型是**真实尺寸**（莫辛 1.232 m、DP-27 1.275 m），而腰射姿态最早是照
    // 0.748 m 的程序化枪模调的。一把真长度的步枪搁在眼前 0.33 m 处，必然
    // 又细又长：支撑手被顶到眼睛前 0.78 m，前臂得横跨 0.6 m 才够得着，
    // 画面上就是一根**长宽比 14:1 的细棍**（真人前臂约 5:1）。
    //
    // 关键认识：**均匀缩放本身不改变观感**。"整体乘 s + 距离也乘 s"是一个
    // 相似变换，屏幕上的角度一模一样。真正决定观感的是**眼睛在枪局部系里的位置**：
    //     Z0 = -hip_pos.z / vm_scale      （眼睛在枪原点后方多少米，枪局部单位）
    // 想让支撑手落到 0.42 m 处、同时枪托又不能在近裁剪面（0.06 m）后面，
    // 只能靠 s < 1 把枪本身做小 —— 这是真 FPS 给视图模型单独一套变换的常规做法。
    //
    // 三者是**一起**用的：s 定"枪相对人有多大"，hip_pos 定"枪摆在画面哪儿"。
    // ---------------------------------------------------------------------
    float vm_scale = 1.0f;

    // ---- 开镜对准 ----
    // 开镜时"眼睛要穿过的那一点"（照门/瞄具）在**枪局部系**里的位置。
    // aim_pos.x/y 由它反推：aim = -sight * vm_scale，这样枪自己的照门正好落在
    // 屏幕中心（准心处），而不是像上一版那样落在 0.73 屏高。
    // 每把枪由 kWpnArt 给（tools/glb_preview.py --probe 量的剖面顶面）；
    // 程序化枪模用它的红点瞄具 (0, +0.075)。
    float aim_sight_x = 0.0f;
    float aim_sight_y = 0.075f;
    // 开镜时枪托尾端到眼睛的距离（米）。太远则枪缩成一小条，太近则托底撞近裁剪面 ——
    // 而且**太近会把开镜画面下半屏整个糊掉**：0.200 时托底占 820 px（见 .cpp 里 build() 的注释）。
    float aim_dist = 0.270f;

    // 前臂 / 腕口。**不挂在 hand_r / hand_l 里** —— 那两组会随每把枪的握持点
    // 整体平移，而肘长在人身上、不跟着枪走。挂进 hands，换枪时只重摆腕端。
    godot::MeshInstance3D *fore_r = nullptr;
    godot::MeshInstance3D *cuff_r = nullptr;
    godot::MeshInstance3D *fold_r = nullptr;
    godot::MeshInstance3D *fore_l = nullptr;
    godot::MeshInstance3D *cuff_l = nullptr;
    godot::MeshInstance3D *fold_l = nullptr;
    godot::Vector3 cur_hand_r;   // 当前外观的右手掌心（枪局部系）
    godot::Vector3 cur_hand_l;   // 当前外观的左手掌心

    // ---- 双手的"姿态让位"（开镜时用，见 update()）----
    // 真手模是**实尺**的，而开镜时枪轴与视轴重合，握把上的扳机手与护木上的支撑手
    // 都会投影到画面中心附近 —— 实测**两只手都压住照门**（中心就是瞄准点）。
    // 这不是落位能修的（扫过 5 档落位，"最近的手像素到中心"始终 0 px；把任一只手
    // 移出画面，另一只仍然压住中心）。所以按真 FPS 的通行做法给视图模型**分腰射/
    // 开镜两套姿态**：随 ads 把双手缩一点、往中轴外让一点。
    // 腰射时 ads = 0 → 缩放 1、位移 0，是**构造保证**的逐位不变。
    float          hand_pose_scale = 1.0f;
    godot::Vector3 hand_pose_off;
    // 【2026-09-24：0.65 → 1.00，从"缩小让位"改成"缩到看不见"】
    //
    // 0.65 那一档（缩 0.65 + 让 (0.06,-0.04,0.02)）标定时用的是**程序化手套**：
    // 它是一团没有内部结构的几何，缩到 35% 仍然读作"握着的拳头"。换成**真手模**
    // 之后同一档读出来的是**一块悬在机匣右侧的橙色残片** —— 腕口朝下像个小支架，
    // 不握任何东西（`sweep/glove/ads_hand_ab.png` 有 3 倍特写）。
    //
    // 为什么不能"干脆别缩"：开镜时相机在枪轴上、离握把只有约 **22 cm**
    // （gun 局部 z ≈ +0.08 → 相机空间 -0.220；见 update() 里 aim_dist 的推导），
    // 实尺手模 0.11 m 宽在这个距离上占 **0.55 屏高** —— 不是"挡照门"，是糊掉半个屏。
    // 所以"开镜时看得见一只完整的手"在这个相机位上本来就不成立，行业里的做法是
    // **开镜把双手收掉**。实测 `VA_VM_HANDS=0` + 开镜（`sweep/glove/ads_try4.png`）
    // 是这批候选里唯一干净的：悬浮手套与前臂圆柱一起消失，只剩枪、照门、目标。
    //
    // 所以保留了"缩小"这条通路但把终点设成 0（缩放连续，不会突跳），
    // 并在 place_arms() 里低于 SHOW 门限时整组藏掉 —— 既避开 0 缩放的退化变换，
    // 也避免缩到 3% 时那一堆子像素抖动。
    // 键级覆盖仍在：VA_VM_ADS_HSHRINK / VA_VM_ADS_HOFF。
    float          hand_ads_shrink = 1.0f;    // 开镜时双手缩小的比例（1 = 缩到无）
    godot::Vector3 hand_ads_off = godot::Vector3(0.060f, -0.040f, 0.020f);   // 位移（枪局部系，米）

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
    // 按当前 cur_hand_r / cur_hand_l 重摆两条前臂与腕口。换枪后必须调一次。
    void place_arms();
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
