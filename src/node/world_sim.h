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

#include "node/audio.h"
#include "node/hud.h"
#include "node/scene_builder.h"
#include "node/unit_anim.h"
#include "node/unit_leg.h"
#include "node/viewmodel.h"
#include "sim/va_world.h"
#include "sim/va_campaign.h"   // 战役：关卡表 / 跨关花名册（CarryOver）

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
    /* ---- 输入：把"按住"与"重复"分开 ------------------------------------------
       逻辑层只有"键现在是不是按着"一个概念（va::IN 里全是 bool），
       而引擎侧的事件流有三类：按下 / **系统按键重复** / 松开。
       重复（InputEventKey::is_echo()）说的是"键还按着"，把三条塞进两个状态里
       是本工程踩过的坑（按住 W 只走半秒就被自己的重复按停），详见 _input 与
       clear_held_input 的注释。
         apply_key        写状态（含 R/G/F/Z/R/Esc 这些"一次动作"键）
         is_hold_key      该键是不是"按住"语义（决定 echo 能不能用来重建状态）
         clear_held_input 丢 keyup 的三条路径（失焦 / 进菜单 / 每帧兜底）统一从这里清
    */
    void apply_key(godot::Key p_code, bool p_down);
    static bool is_hold_key(godot::Key p_code);
    void clear_held_input(bool p_reacquire_ok);
    bool reacquire_ = false;       // 闩锁被清过、还没与真实键态对上 → 允许 echo 重建一次
    bool focus_was_ = true;        // 上一帧窗口是否聚焦（只为识别"刚失焦"那一刻）
    void push_subtitle(const std::string &who, const std::string &text, const std::string &cls);
    // 开发用截图探针：设了环境变量 VA_CAPTURE=<秒,秒,...> 时，
    // 在指定的战局时刻把视口存成 PNG 到 res://captures/，全部拍完后自动退出。
    // 这是验证「3D 战场真的渲染出来了」最直接的手段（无头运行也能取证）。
    void capture_step();
    void capture_setup();
    void aim_at_road();
    // 战斗事件取证（VA_AUTO / VA_DOWN_AT）：
    //   命中标记 / 击杀飘字 / 倒地倒计时这三条 HUD 路径都由战斗事件驱动，
    //   而无人值守跑图没有键鼠输入 —— 玩家一枪不发，三条路径永远不会被触发，
    //   "到底画没画对"就只能靠读代码猜。这两个旋钮**顶替玩家操作**，
    //   不伪造 HUD 状态：命中判定、hits/kills 自增、伤害与死亡全走真实战斗代码。
    void autoplay_step();          // 每帧朝最近活敌对准 + 按住开火
    void hud_down_inject();        // 到点让玩家被真实伤害击倒
    // 把当前视口存成 PNG。capture_step（按战局秒数）与战斗事件取证（按事件）共用。
    void save_shot(const godot::String &tag);
    // 角色模型检阅台（取证）：VA_UNIT_SHOW 时把 11 个角色的三维模型
    // 等距、正面摆成两排在玩家起点前方，供近景取证朝向 / 比例 / 脚底落地 / 倒地姿态。
    // 战场截图判不了这些 —— 单位在画面里只有几十像素高，还大半时间背对或被掩体挡住。
    void build_unit_showcase();

    SceneRefs refs_;
    godot::Camera3D *cam_ = nullptr;
    ViewModel vm_;                 // 第一人称武器视图模型（相机的子节点）
    Audio snd_;                    // 音效层：SimEvents::on_sfx 的落点（见 node/audio.h）
    /* 单位跑动节奏（见 node/unit_anim.h）。与 snd_ 同一类：只读逻辑层状态、
       挂在 sync_entity_nodes 里、删掉不影响任何逻辑。 */
    UnitAnim anim_;
    /* 双腿交替（见 node/unit_leg.h）：用着色器顶点位移把左右腿按相位交替前后摆，
       相位由 anim_ 提供（PoseVals::leg_phase / leg_deg）。
       与 anim_ 同一类：只读逻辑层状态、挂在 sync_entity_nodes 里、
       **VA_LEG=0 时完全不动作**（连材质都不换），删掉不影响任何逻辑。 */
    UnitLeg leg_;
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

    /* ---- 铁原战役：关卡推进 ------------------------------------------------
       为什么状态放在表现层而不是逻辑层：逻辑层只认"这次给我哪一关、带哪些人进来"
       （init_world(seed, levelIdx, carry)），**它不需要知道"打完该去哪"** ——
       那是一次会话级的选择（玩家按了转进、还是从菜单重开），属于外壳的职责。
       放在这里还有一个好处：逻辑层的 va_campaign 保持零引擎依赖，
       而 tools/va_sweep 用同一套接口从任意一关起跑，不需要模拟"按了什么键"。 */
    int  camp_level_ = 0;          // 当前关卡下标
    bool camp_cleared_ = false;    // 本关已"转进"（结算面板上的按钮要变成"转进下一阵地"）
    bool camp_won_ = false;        // 最后一关也打下来了（按钮变成"重新入伍"）
    /* 进当前这一关时带进来的那份花名册。**"重打本关"要退回它** ——
       否则第一次打得好不好会污染重试，失败几次之后越打越弱、最后无解。 */
    va::CarryOver carry_in_;
    // 战役口令的节流时刻（炸坝 / 夜袭 / 撤离）
    float camp_last_dam_ = -100.0f, camp_last_retake_ = -100.0f, camp_last_evac_ = -100.0f;
    void init_level_world();       // 只铺逻辑世界（_ready 用：那时静态场景还没建）
    void begin_level();            // 按 camp_level_ 铺关并把队伍带进去
    void advance_level();          // 结算面板按 R：过关就转进下一阵地，否则重打本关

    // 音频节流（同一音效 id 在极短时间内不重复触发）
    double last_sfx_t_ = 0.0;
    // VA_DBG_SFX=1：音效链路诊断（载入数 / 触发数 / 未登记 id 的丢弃数）。
    // 注意它和 VA_DBG_VM 是两件事，判读场合不重叠。
    bool dbg_sfx_ = false;

    // 视图模型手感：靠「观测量跳变」判断开火/换弹，不去侵入逻辑层。
    // 逻辑层删掉开火事件回调也不影响这里 —— 渲染层只读状态。
    float prev_fire_cd_ = -1.0f;
    float prev_reload_t_ = -1.0f;
    bool force_ads_ = false;       // VA_ADS=1：取证时强制据枪（截图时按不了鼠标右键）

    // 战斗事件取证旋钮（见 autoplay_step / hud_down_inject 的说明）
    bool   autoplay_   = false;    // VA_AUTO=1
    double down_at_    = -1.0;     // VA_DOWN_AT=<战局秒数>，<0 = 关闭
    bool   down_done_  = false;    // 幂等：只击倒一次
    /* VA_FF=<倍率>：快进。逻辑层是按「真实经过时间」推进的，而车队要到
       CFG.convoyIn = 175 秒才进场 —— 不快进的话，一次"打到交火"的取证
       就是五分钟真实时间起步。只改推进多快，不改怎么推进：
       步长仍是固定 1/60，所以同一战局秒数下的状态与不快进时逐位相同。 */
    int    ff_         = 1;

    // 战斗事件取证（VA_CAPTURE_EV，见 hud.h 的 EvShot）：
    // 事件当帧不立刻截图 —— 这一帧的 _draw 还没跑，读视口纹理拿到的是上一帧
    // （正好是没有命中标记的那一帧）。所以先记下"该截哪一张"，稍后再落盘。
    //
    // 「稍后」是**按墙钟秒**算的，不是按帧数 —— 这是实测踩出来的坑：
    // HUD 的瞬时动画按墙钟衰减（`hud_->update(p_delta)`，见 hud.cpp 的 hit_t_），
    // 命中标记只活 0.24 秒墙钟。而"隔 2 帧"在 1080p + VA_FF=6 下会换算成
    // 2 × 0.133 = 0.267 秒墙钟 > 0.24 秒 → 标记已经灭了。
    // 症状：那一组日志里命中/击杀打得清清楚楚，截图目录里却一张 ev_hit 都没有，
    // 只有命中频率更高的 ff=3 那一组碰巧能截到。
    // 改成"等固定墙钟时长 + 至少跨一帧"就与帧率解耦：
    // 60fps 时约 3 帧，低到 7.5fps 时 1 帧就落盘（0.133 秒 < 0.24 秒，仍在寿命内）。
    godot::String ev_tag_;
    float  ev_wait_sec_ = -1.0f;   // <0 = 空闲；>0 = 还要等这么多**墙钟**秒
    int    ev_wait_frames_ = 0;    // 至少还要跨几帧（本帧 _draw 还没跑，当帧读纹理必是旧图）
    double ev_from_t_ = 0.0;       // 事件被检测到时的战局秒数（仅用于落盘时自证时序）
    double ev_wall_   = 0.0;       // 在途期间累计的**墙钟**秒（落盘日志自证用）
    int    ev_frames_ = 0;         // 在途期间跨过的帧数（落盘日志自证用）
    bool   ev_cap_ = false;        // VA_CAPTURE_EV：事件当帧自动落盘（本层落盘侧开关）
    /* 在途截图的"重要性"，用来决定新事件能不能抢位（击杀 4 > 倒地 3 > 队友阵亡 2 > 命中 1）。
       为什么需要它：命中每 0.1~0.7 秒一次，全收会一直排队存不完，所以在途期间必须丢事件；
       但"一刀切地丢"会把击杀这种一闪而过、又最难得的路径整条丢掉 ——
       实测踩到过：日志里 `t=228.52 击杀 (kills=1)` 明明发生了，截图目录里一张 ev_kill 都没有。 */
    int    ev_prio_ = 0;

    /* VA_SCRIPT_A=1：把离线扫描（tools/va_sweep.cpp）里那套剧本搬到渲染端执行。
       为什么必须有它：VA_AUTO 只顶替玩家的"手"（瞄准 + 扣扳机），而扣扳机要先有射界，
       伏击又是由"第一枪"触发的 —— 车队在伏击圈外一路向西时玩家根本没有目标，
       于是伏击永不触发、车队冲出西口直接判负。实测就是这么失败的：
       「[结算] 失败：车队冲过了西侧出口，伏击失败。」
       而离线扫描里伏击是按**位置**起爆的（先头车 x < 1180 → trigger_ambush("mine")），
       所以渲染端也必须照做，否则"屏幕上能看到的这一局"和"扫描里量到的那一局"
       根本不是同一局。 */
    bool   script_a_   = false;    // VA_SCRIPT_A=1
    int    script_cmd_ = 0;        // 已发出的剧本指令序号
    void   script_a_step();        // 每个固定步之后调用一次（与 va_sweep 的次序一致）
    void   script_campaign_step(); // 战役模式下的目标驱动口令（见 .cpp 的说明）

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

    /* 第一人称是否画"自己的身体"。默认不画 —— 见 sync_entity_nodes 里那段长注释。
       VA_SHOW_SELF=1 可以把它放回来：那是**复现"视野被大片遮挡"**用的，
       不是可选的画面风格（放回来就是镜头被自己的模型包住）。 */
    bool   show_self_ = false;

    /* VA_DBG_UNITS=1：把"镜头最近的是谁"打成数字（见 dbg_units_dump）。
       分层消融（VA_HIDE_PROPS/UNITS/VEH）只能答"属于哪一层"，
       而"视野被谁挡了"要落到**具体哪一个单位**上。 */
    bool   dbg_units_ = false;
    bool   dbg_first_ = true;      // 相机落地后的第一帧再打一次（此前 cam_ 还在原点）
    void   dbg_units_dump(const char *p_when) const;

    /* VA_DBG_INPUT=1：逐事件打印按键流 + 每 0.25 秒墙钟打一次"位置心跳"。
       「按住方向键该不该一直走 / 松开该不该立刻停」这类问题，**只看画面判不了** ——
       屏幕上是"走得慢"还是"走了两步就停"读不出量级；而键鼠事件流里
       pressed / echo 两个标志位是**排他的原因**（echo=1 是操作系统的按键重复）。
       两张表一对（下面 dbg_input_key 与 dbg_input_heartbeat），
       就能把"手感"换成"哪一类事件把 IN 改成了什么"。
       判读口径：echo=1 的事件**不得**改变 IN（重复不等于松开）；
       心跳里"位移=0.000 m"必须只在 release 之后出现。 */
    bool   dbg_input_ = false;
    double dbg_in_wall_ = 0.0;     // 墙钟累计（心跳按它触发，与是否快进无关）
    double dbg_in_next_ = 0.25;    // 下一次心跳的墙钟时刻
    double dbg_in_x_ = 0.0, dbg_in_y_ = 0.0;   // 上次心跳时的玩家位置
    bool   dbg_in_have_prev_ = false;          // 第一条心跳只建基准，不报位移
    void   dbg_input_key(int64_t p_code, bool p_pressed, bool p_echo);
    void   dbg_input_heartbeat(double p_delta);

    // UI：全套使命召唤风格 HUD，手绘在一个 Control 里（见 node/hud.h）
    Hud *hud_ = nullptr;
};

} // namespace volunteer_army
