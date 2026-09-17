#pragma once
// VolunteerArmyPC —— WorldSim：逻辑层与 Godot 场景之间的唯一桥梁
//
// 职责边界（严格）：
//   逻辑层 src/sim/* —— 不认识 Godot，可脱离引擎单独跑
//   本类             —— 驱动逻辑步进、把状态同步到 3D 节点、把输入喂回逻辑层、
//                       实现 SimEvents 把"说/播/提示/结算"转成引擎侧的音频与 UI
#include <vector>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string.hpp>

#include "node/hud.h"
#include "node/scene_builder.h"
#include "node/viewmodel.h"
#include "sim/va_world.h"

namespace volunteer_army {

class WorldSim : public godot::Node3D, public va::SimEvents {
    GDCLASS(WorldSim, godot::Node3D)

public:
    WorldSim();
    ~WorldSim() override;

    void _ready() override;
    void _process(double p_delta) override;
    void _input(const godot::Ref<godot::InputEvent> &p_event) override;
    void _notification(int p_what);

    // ---- SimEvents 实现 ----
    void on_say(const std::string &who, const std::string &text, const std::string &cls) override;
    void on_log(const std::string &text, const std::string &cls) override;
    void on_sfx(const std::string &id, float x, float y, float gain, bool local) override;
    void on_toast(const std::string &text) override;
    void on_alert(const std::string &text, float dur) override;
    void on_end(const std::string &kind, const std::string &text) override;
    void on_subs_dirty() override;
    void on_objectives_dirty() override;

    // ---- 供 GDScript ----
    void start_mission(bool p_skip_deploy);
    void reset_mission();
    godot::Dictionary get_status() const;
    godot::String get_diag() const;

protected:
    static void _bind_methods();

private:
    void spawn_entity_nodes();
    void sync_entity_nodes();
    void setup_runtime_ui();
    // ---- 界面外壳（主菜单 / 任务简报）----
    // 详见 node/hud.h 顶部：Hud 同时充当"界面外壳"，这三件事由本类代劳 ——
    //   enter_play()    从简报切进战斗：接管鼠标、把实体节点对齐到新一局的布局
    //   shell_owns_input()  shell 期间键鼠一律不喂给逻辑层（否则菜单里按 WASD
    //                      会把战斗中的角色一起推着走）
    void enter_play();
    bool shell_owns_input() const;
    void push_subtitle(const std::string &who, const std::string &text, const std::string &cls);
    // 开发用截图探针：设了环境变量 VA_CAPTURE=<秒,秒,...> 时，
    // 在指定的战局时刻把视口存成 PNG 到 res://captures/，全部拍完后自动退出。
    // 这是验证「3D 战场真的渲染出来了」最直接的手段（无头运行也能取证）。
    void capture_step();
    void capture_setup();
    void aim_at_road();
    // 角色模型检阅台（取证）：VA_UNIT_SHOW 时把 11 个角色的三维模型
    // 等距、正面摆成两排在玩家起点前方，供近景取证朝向 / 比例 / 脚底落地 / 倒地姿态。
    // 战场截图判不了这些 —— 单位在画面里只有几十像素高，还大半时间背对或被掩体挡住。
    void build_unit_showcase();

    SceneRefs refs_;
    godot::Camera3D *cam_ = nullptr;
    ViewModel vm_;                 // 第一人称武器视图模型（相机的子节点）
    std::vector<godot::Node3D *> unit_nodes_;
    std::vector<godot::Node3D *> veh_nodes_;
    godot::Node3D *box_node_ = nullptr;

    // 视角
    float yaw_ = 0.0f, pitch_ = 0.0f;
    float sens_ = 0.0022f;
    float bob_t_ = 0.0f;

    // 主循环
    double acc_ = 0.0;
    bool  mission_started_ = false;

    // 音频节流（同一音效 id 在极短时间内不重复触发）
    double last_sfx_t_ = 0.0;

    // 视图模型手感：靠「观测量跳变」判断开火/换弹，不去侵入逻辑层。
    // 逻辑层删掉开火事件回调也不影响这里 —— 渲染层只读状态。
    float prev_fire_cd_ = -1.0f;
    float prev_reload_t_ = -1.0f;
    bool force_ads_ = false;       // VA_ADS=1：取证时强制据枪（截图时按不了鼠标右键）

    // 截图探针
    std::vector<double> cap_times_;
    int    cap_i_ = 0;
    double cap_sim_t_ = 0.0;
    bool   cap_enabled_ = false;
    int    cap_frame_skip_ = 0;
    // 界面外壳期间逻辑层的 t 是冻结的，截图探针按 t 触发就永远不会响。
    // 所以菜单/简报里改用这个墙钟累计值来驱动截图 —— 否则新加的这两屏
    // 一条证据都拿不到，只能靠"我看着是对的"。
    double shell_t_ = 0.0;

    // 角色模型检阅台（VA_UNIT_SHOW）。逻辑层同样冻结（否则敌人会走进陈列排里，
    // 把"谁是谁"搅得读不出来），所以截图也按墙钟驱动，和外壳期同一套办法。
    bool   show_mode_ = false;
    double show_t_ = 0.0;

    // UI：全套使命召唤风格 HUD，手绘在一个 Control 里（见 node/hud.h）
    Hud *hud_ = nullptr;
};

} // namespace volunteer_army
