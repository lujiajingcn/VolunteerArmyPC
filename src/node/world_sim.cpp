// VolunteerArmyPC —— WorldSim 实现：逻辑层 ↔ Godot 的桥
#include "node/world_sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace volunteer_army {

// 站在地面上的眼高：1 世界单位 = 0.05 米，网页版 EYE = 33 → 1.65 米
static constexpr float EYE_H = 1.65f;

// 崩溃诊断：设了 VA_TRACE=<文件路径> 时把执行轨迹写进去（每步 fclose，崩了也留得下）
static void va_trace(const char *msg) {
    const char *p = std::getenv("VA_TRACE");
    if (p == nullptr || *p == '\0') return;
    FILE *f = std::fopen(p, "a");
    if (f == nullptr) return;
    std::fprintf(f, "[T] %s\n", msg);
    std::fclose(f);
}

// 战局随机种子。
// 默认取毫秒时钟（每局都不一样，这是网页版"天气变体带来重玩性"的设计）；
// 但取证/回归时必须能复现，所以留 VA_SEED 覆盖：
//     VA_SEED=12345   → 固定同一张地图布局、同一场天气
// 想扫遍三种天气又不想靠运气，就试 VA_SEED=1/2/3/4/5…（天气由首个 RNG 决定）
static uint32_t new_seed() {
    const char *v = std::getenv("VA_SEED");
    if (v != nullptr && *v != '\0') return (uint32_t)std::strtoul(v, nullptr, 10);
    return (uint32_t)Time::get_singleton()->get_ticks_msec();
}

// ---- 取证机位（VA_CAM_*）-----------------------------------------------
//
// 相机默认固定在玩家的 (x,y) 上、离地 1.65 米，于是**没有任何办法拍到战场全貌** ——
// 峡谷这种大尺度地形的验收要靠俯视/抬高机位，靠"站在原地转视角"是拍不到的：
// 谷壁的天际线、道路两侧的对称性、水口缺口，站在谷底一张都看不全。
//
//   VA_CAM_H=<米>      在眼高之上再抬多少（如 40 → 近似航拍）
//   VA_CAM_PITCH=<度>  强制俯仰（负值向下；-25 是常用的"半俯视"）
//   VA_CAM_YAW=<度>    强制偏航（0 = 朝北=路的北侧；-90 = 朝东=车队来向）
//
// 【不写回状态】这三个旋钮只覆盖**相机变换**，不写 va::W.viewPitch / 不改 IN.* ——
// 与 VA_ADS 只做视效、绝不写回 IN.ads 是同一条纪律（写回会污染 VA_DBG_INPUT 的取证）。
// 代价是：强制俯仰时"子弹有效射程"仍按玩家真实视角算，取证图里别拿它验证命中率。
static bool env_set(const char *p_name) {
    const char *e = std::getenv(p_name);
    return e != nullptr && *e != '\0';
}
static float env_f(const char *p_name, float p_def) {
    const char *e = std::getenv(p_name);
    return (e != nullptr && *e != '\0') ? (float)std::atof(e) : p_def;
}

WorldSim::WorldSim() = default;
WorldSim::~WorldSim() = default;

void WorldSim::_bind_methods() {
    ClassDB::bind_method(D_METHOD("start_mission", "skip_deploy"), &WorldSim::start_mission);
    ClassDB::bind_method(D_METHOD("reset_mission"), &WorldSim::reset_mission);
    ClassDB::bind_method(D_METHOD("get_status"), &WorldSim::get_status);
    ClassDB::bind_method(D_METHOD("get_diag"), &WorldSim::get_diag);
}

// 开局把视线转向公路来向（东北），让玩家一睁眼就看见要伏击的那条路
void WorldSim::aim_at_road() {
    const va::Unit *p = va::W.player;
    if (p == nullptr) return;
    const float a = std::atan2(va::CFG.roadCY - p->y, 2020.0f - p->x);
    va::W.player->facing = a;
    yaw_ = -a - 1.5707963267948966f;
    pitch_ = -0.04f;
}

/* ----------------------------------------------------- 角色模型检阅台（取证）
   两种模式，都由 VA_UNIT_SHOW 选：
     VA_UNIT_SHOW=1 | row        陈列排：11 个角色等距摆两排，看彼此差异与整体齐备度
     VA_UNIT_SHOW=one:<键>[:<度>]  近景单体：把<键>那个模型单独摆到镜头前 5 米，
                                  看朝向 / 脚底 / 比例（可加第三段临时偏航角度）
     VA_UNIT_SHOW=run:<键>[:<速度>] 跑动排：同一个模型按一个步态周期均分相位摆一排
                                  （默认 6 格 / 满速），一张静态截图看完整个跑动循环。
                                  相位是给定值，不依赖帧对齐 —— 见该分支里的说明。
                                  格数 / 速度：VA_RUN_DEMO_N / VA_RUN_DEMO_SPEED。

   载具另有两条（前面多了 veh: 前缀，不与角色撞键）：
     VA_UNIT_SHOW=veh:<类型>[:<度>]  单车近景，类型 = jeep | apc | tank | truck
     VA_UNIT_SHOW=veh:row           四辆车的陈列排（按 jeep/apc/tank/truck 固定顺序）

   地物还有两条（前缀 prop:，2026-09-22 加）：
     VA_UNIT_SHOW=prop:<键>[:<度>]  近景单体，键 = prop_rock_a | prop_rock_b |
                                    prop_pine | prop_bush（按场上典型尺寸摆）
     VA_UNIT_SHOW=prop:row          四件地物的陈列排（固定顺序，看彼此差异）
   地物这条的取景按**物体自身的实际高度**推（灌木 0.6 m 与松树 8 m 没法共用一套距离），
   且相机高取 0.5×H —— 低矮地物的判据是"底下那条地面余量"，相机太高就俯视、余量被压没。
   
   【载具为什么要单独一条通道】战场截图答不了载具的三个问题：车头朝哪个方向、
   跟人比有多大、轮子是踩在地上还是悬在半空。车队在公路上、离玩家几十到上百米，
   画面上就是一小块，还常被地形切掉一半 —— 与角色那轮的处境一模一样。

   近景里 dyaw=0 时看到的**应当是车头正面**（推导见 veh: 分支里的注释），
   这既是"车头在 +X"这条约定的判据，也是 VEH_MODEL_YAW_DEG 该取几度的判据。
   扫角度：VA_UNIT_SHOW=veh:jeep:90 就是"把模型再绕 Y 转 90°"。

   为什么需要它：把角色换成三维模型之后，"朝向校对了没有 / 身高比例对不对 /
   脚是踩在地上还是悬空 / 倒地的姿态是躺下还是穿模"这四件事，**战场截图一件都答不了**。
   场上每个单位在 2560 宽的画面上只有几十像素高，还大半时间背对镜头或被掩体挡住；
   上一轮看 cap_40s.png 只能得出"那是个人形"，朝向对不对完全看不出来。

   与战场共用同一套 make_unit_node_by_key / unit_transform，
   所以这里看到的**就是**战场上那个模型，不是另做的一份预览 ——
   预览和实物分家的话，这里全对、场上全错，是最坏的情况。

   【先近景单体、再批量生成】近景单体存在的第一个理由是**省钱**：
   朝向标定如果错了，整批模型都要重做（单张 40 积分 × 11）。所以流程是
   "生成 1 张 → 近景确认朝向 → 确认无误 → 再批量"。这一条是实打实踩出来的，
   不是预防性的教条。

   逻辑层全程冻结（_process 里 show_mode_ 直接早退），原因有两个：
     1. 敌人会走进陈列排里，把"第几个是谁"搅得读不出来；
     2. 停摆之后一帧不用算 AI，取证速度快一个数量级。            */
void WorldSim::build_unit_showcase() {
    const char *e = std::getenv("VA_UNIT_SHOW");
    if (e == nullptr || *e == '\0') return;
    const va::Unit *p = va::W.player;
    if (p == nullptr) return;
    const std::string mode(e);
    if (mode == "0") return;
    show_mode_ = true;

    // 相机前方 / 右方（逻辑层 2D）。
    // 推导：相机 rotation.y = yaw_ = -a - π/2，Godot 里偏航 θ 的相机朝 -Z 旋转后
    // 前方 = (-sinθ, 0, -cosθ)；代入 θ 得 (cos a, 0, sin a) ——
    // 即逻辑层的 (cos a, sin a)。右方 = (cosθ, 0, -sinθ) → 逻辑层 (-sin a, cos a)。
    // aim_at_road() 已经把 a 写进 player->facing，直接读回来，不再算第二遍。
    const float a = p->facing;
    const float fx = std::cos(a), fy = std::sin(a);      // 前
    const float rx = -fy,        ry = fx;                // 右
    constexpr float U = 20.0f;   // 逻辑层 1 米 = 20 单位（to3 里乘 S=0.05 之前的那套坐标）
    constexpr float PI = 3.14159265358979323846f;

    Node3D *stage = memnew(Node3D);
    stage->set_name("UnitShowcase");
    add_child(stage);

    // 枪模是相机的子节点，不藏的话会横在画面正中挡住人
    if (vm_.root != nullptr) vm_.root->set_visible(false);

    if (mode.rfind("one:", 0) == 0) {
        // ---- 近景单体：<键>[:<临时偏航角>] ----
        std::string rest = mode.substr(4);
        std::string key = rest, yaw_s;
        const size_t c = rest.find(':');
        if (c != std::string::npos) { key = rest.substr(0, c); yaw_s = rest.substr(c + 1); }
        float dyaw = 0.0f;
        if (!yaw_s.empty()) dyaw = (float)std::strtod(yaw_s.c_str(), nullptr) * PI / 180.0f;

        constexpr float DIST_M = 4.8f;
        constexpr float CAM_H  = 1.05f;
        Node3D *nd = make_unit_node_by_key(key, refs_.units);
        if (nd == nullptr) {
            UtilityFunctions::print(String::utf8("[show] 近景：没有模型 "), String::utf8(key.c_str()));
        } else {
            const float lx = p->x + fx * DIST_M * U;
            const float ly = p->y + fy * DIST_M * U;
            nd->set_transform(unit_transform(lx, ly, a + PI + dyaw, false));
            /* 站立单体也要挂上腿摆层（摆角 0）—— 不是为了让它动，是为了**材质一致**：
               若这里走原始 StandardMaterial3D、而跑动排走着色器材质，
               两边的取证图就不可比，"换了材质外观到底有没有变"这个问题会答不了。
               挂上之后 VA_LEG=0/1 的对照才落在同一套取景与姿态上。 */
            leg_.apply(nd, key, 0.0f, 0.0f);
            stage->add_child(nd);
        }
        /* 取景推导（这几行数字不是拍的，是算的，改之前先算一遍）：
             相机高 1.05（**不是** 1.65 眼高）—— 眼高平视时人身只占画高 26%，
             因为要同时容纳脚下 1.65 m 与头顶 0.03 m，垂直半视场被迫开到 ±1.9 m，
             1.68/3.8 = 44% 就是上限，实际还更小。把相机降到 1.05 再微微抬头，
             包围盒变成 [-1.05, +0.63]，同样的半视场能装下的"人"就大得多。
             FOV 32（垂直半角 16°，tan=0.2867）、距离 4.8 m ⇒ 半视场 1.376 m：
               脚底 -1.05 → 在框内，且**脚下还留 0.33 m 地面**（"脚有没有踩在地上"
               全靠这条地面余量，压到 0 就看不出来了）；
               头顶 +0.63 → 在框内，人身占画高 1.68/2.752 = 61%。
             抬头 0.04 rad ≈ 2.3°：人身中心在 0.84 m，比相机低 0.21 m，
             atan(0.21/4.8)=2.5°，抬回来正好把人居中。 */
        cam_->set_position(to3(p->x, p->y, CAM_H));
        cam_->set_rotation(Vector3(0.04f, yaw_, 0));
        // VA_FOV 显式给了就以它为准（想试别的取景时不必改代码）
        if (std::getenv("VA_FOV") == nullptr) cam_->set_fov(32.0f);
        UtilityFunctions::print(String::utf8("[show] 近景 "), String::utf8(key.c_str()),
                                String::utf8(" 距离 "), DIST_M, String::utf8("m 机高 "), CAM_H,
                                String::utf8(" 临时偏航 "), dyaw * 180.0f / PI, String::utf8(" 度"));
        if (refs_.units != nullptr) refs_.units->set_visible(false);
        return;
    }

    if (mode.rfind("run:", 0) == 0) {
        /* ---- 跑动排：run:<键>[:<速度 m/s>] ----
         把**同一个模型**按一个步态周期均分相位摆一排，一张静态截图就能看到
         整段跑动循环的动作幅度与相位关系。

         【为什么不做成"动的那一个"】要证明"在跑"，最直觉的办法是隔 0.15 秒拍三张。
         但那要求两次/多次拍摄的**帧对齐**完全一致 —— 本工程踩过：`acc_ += p_delta*ff`，
         ff>1 时不同运行的帧数根本不对齐，截到的战局时刻差零点几秒，单位早移动了。
         摆成一排则是**静态**的：相位是给定值，不依赖任何时序，可重复、可逐格比对。
         这也是当年"角色模型检阅台"存在的同一个理由 —— 战场截图答不了姿态问题。 */
        std::string rest = mode.substr(4);
        std::string key = rest, spd_s;
        const size_t c = rest.find(':');
        if (c != std::string::npos) { key = rest.substr(0, c); spd_s = rest.substr(c + 1); }
        float spd = 0.0f;
        if (!spd_s.empty()) spd = (float)std::strtod(spd_s.c_str(), nullptr);
        if (spd <= 0.0f) spd = env_f("VA_RUN_DEMO_SPEED", anim_.full_speed_mps());
        int N = (int)env_f("VA_RUN_DEMO_N", 6.0f);
        if (N < 2) N = 2;
        if (N > 12) N = 12;

        constexpr float DIST_M = 6.6f;
        constexpr float CAM_H  = 1.05f;
        constexpr float SPACING_M = 1.05f;   // 相邻两格的横向间距
        if (refs_.units != nullptr) refs_.units->set_visible(false);

        const float row_w = SPACING_M * (float)(N - 1);
        const float lx0 = p->x + fx * DIST_M * U - rx * (row_w * 0.5f) * U;
        const float ly0 = p->y + fy * DIST_M * U - ry * (row_w * 0.5f) * U;

        int placed = 0;
        for (int k = 0; k < N; ++k) {
            Node3D *nd = make_unit_node_by_key(key, refs_.units);
            if (nd == nullptr) break;
            const float lx = lx0 + rx * (SPACING_M * (float)k) * U;
            const float ly = ly0 + ry * (SPACING_M * (float)k) * U;
            const float phase = 2.0f * PI * (float)k / (float)N;
            const UnitAnim::PoseVals pv = anim_.pose_at(phase, spd);
            nd->set_transform(unit_transform(lx, ly, a + PI, false)
                              * UnitAnim::pose_transform(pv));
            /* 腿摆：这一排本来就是把**一个步态周期按相位均分**摆开的，
               把相位喂进去之后，**一张静态截图就能看到左右腿的交替** ——
               不依赖帧对齐，也不需要 VA_FF（那两条正是"拍三张对照"最容易翻车的地方）。 */
            leg_.apply(nd, key, pv.leg_phase, pv.leg_deg);
            stage->add_child(nd);
            ++placed;
            UtilityFunctions::print(String::utf8("[show] 跑动排 #"), k,
                                    String::utf8(" 相位 "), phase * 180.0f / PI, String::utf8("°"),
                                    String::utf8(" 起伏 "), pv.bob, String::utf8("m 前倾 "),
                                    pv.lean, String::utf8("° 摇 "), pv.roll, String::utf8("°"),
                                    String::utf8(" 腿摆 "), pv.leg_deg, String::utf8("°"));
        }
        cam_->set_position(to3(p->x, p->y, CAM_H));
        cam_->set_rotation(Vector3(0.04f, yaw_, 0));
        if (std::getenv("VA_FOV") == nullptr) cam_->set_fov(32.0f);
        UtilityFunctions::print(String::utf8("[show] 跑动排 "), String::utf8(key.c_str()),
                                String::utf8(" 共 "), placed, String::utf8(" 格，速度 "),
                                spd, String::utf8(" m/s（满速 "), anim_.full_speed_mps(),
                                String::utf8(" m/s），距离 "), DIST_M, String::utf8("m"));
        return;
    }

    if (mode.rfind("veh:", 0) == 0) {
        /* ---- 载具近景 / 陈列排：veh:<type>[:<临时偏航角>] 或 veh:row ----
         【为什么载具要单独一条通道】战场截图答不了载具的三个问题：
         朝向对不对（车头朝哪）、比例对不对（跟人比多大）、轮子有没有踩在地上。
         车队在公路上、离玩家几十到上百米，画面上就一小块，还常被地形切掉一半。
         取证口径与角色那条完全一致（同一套 make_vehicle_node_by_key + 同一套
         量化日志），只是取景要重算 —— 载具比人矮而长（吉普 2.3×1.2 m、
         坦克 3.7×1.9 m），照抄角色那套 4.8 m / 1.05 m 会把车拍到画面下缘外。 */
        std::string rest = mode.substr(4);
        std::string key = rest, yaw_s;
        const size_t c = rest.find(':');
        if (c != std::string::npos) { key = rest.substr(0, c); yaw_s = rest.substr(c + 1); }
        float dyaw = 0.0f;
        if (!yaw_s.empty()) dyaw = (float)std::strtod(yaw_s.c_str(), nullptr) * PI / 180.0f;

        const bool row = (key == "row");
        const char *kinds[4] = { "jeep", "apc", "tank", "truck" };
        const int   n = row ? 4 : 1;

        /* 取景（2026-09-20 车长改实车尺寸后重新推的 —— 旧值是按 2~3.7 m 的小车算的，
           照抄会让四辆车直接出框）。
           横向半视场 = 0.2867 × (2560/1369) ≈ 0.536（FOV 32 的竖直角换算来的）。
           单车 —— 侧视时屏幕跨度 ≈ 车长 L（最长 6.95 m，卡车），再加参照兵横移：
                   0.536×D ≥ (L + 参照兵 2.4)×0.5×1.2  →  D ≈ 7.0 + 0.6×L
           陈列排 —— **一字排开且全部朝镜头时，横向占位是车宽、不是车长**
                   （车长朝镜头里延伸，由透视体现）。四辆宽度 1.56+2.32+2.84+2.62
                   = 9.34 m，加 3 段 3 m 间隙 = 18.3 m：
                   0.536×D ≥ 18.3/2×1.15 → D ≥ 19.6，取 20.5。
                   间距取 3 m 的依据是**相邻半宽之和**（最坏 1.42+1.50 = 2.92），
                   不是车长 —— 按车长算会得出 7 m 间距、D 要 46 m，人跟车都小到看不清。
           （第一版实车取景就是拿车长当横向占位推的，结果间距 1.5 m 让四辆车互相遮挡。）
           机高 —— "相机不能比车顶低"这条对实车更要紧（实车本体 2.3~3.0 m 高），
                   取 1.6 + 0.30×L：吉普 2.6 / 半履带 3.46 / 坦克 3.36 / 卡车 3.69。
           单车的 D 与机高**按该车的实际车长算**，所以换车型不用再回来调常数。 */
        const va::VehicleSpec *fsp = va::vehicle_of(row ? "truck" : key);
        const float frameL = (fsp != nullptr) ? fsp->len / U : 3.0f;   // 米
        const float DIST_M = row ? 20.5f : (7.0f + 0.60f * frameL);
        const float CAM_H  = row ? 4.60f : (1.60f + 0.30f * frameL);
        const float PITCH  = row ? -0.13f : -0.13f;
        const float GAP_M  = row ? 3.0f : 0.0f;
        const float REF_OFF_M = 2.4f;   // 比例尺参照兵与车心的横向间距（仅单车模式）

        int built = 0;
        for (int i = 0; i < n; ++i) {
            const std::string type = row ? std::string(kinds[i]) : key;
            Node3D *nd = make_vehicle_node_by_key(type, refs_.vehicles);
            if (nd == nullptr) {
                // 与角色那条同一个口径：缺模型**留空位**并打一行日志，
                // 而不是把后面的往前挪 —— 挪了就再也看不出"少哪一个"。
                UtilityFunctions::print(String::utf8("[show] 缺载具模型，第 "), i,
                                        String::utf8(" 格（"), String::utf8(type.c_str()),
                                        String::utf8("）空置"));
                continue;
            }
            const float off_m = row ? ((float)i - (n - 1) * 0.5f) * GAP_M : 0.0f;
            const float lx = p->x + fx * DIST_M * U + rx * off_m * U;
            const float ly = p->y + fy * DIST_M * U + ry * off_m * U;
            // 与角色同口径：模型前方 = +X，facing = a + π 就是把+X转过来对着相机。
            // 所以 dyaw=0 时看到的应当是**车头正面** —— 这正是"朝向标定对不对"的判据。
            nd->set_transform(unit_transform(lx, ly, a + PI + dyaw, false));
            stage->add_child(nd);
            ++built;
        }

        /* 比例尺参照兵（只在单车模式）。
           "跟人比多大"是这条通道三个待答问题里的第二个，而它**只能靠同框回答** ——
           分开两张图各量像素再去比，中间隔着两次取景、两个距离，误差比结论还大。
           用具名模型而不是画个高度标尺：标尺能被当成装饰忽略，一个人不会被忽略；
           而且他与陈列排里的角色是同一个模型，读数可以直接互相印证。
           站右侧而不是左侧：横向视场在 dx>0 方向与本工程其余取证图的读数习惯一致。
           注意必须在下面 hide 之前建 —— duplicate() 会把原型的 visible 一起复制。 */
        if (!row) {
            if (Node3D *ref = make_unit_node_by_key("char_rifleman", refs_.units)) {
                const float lx = p->x + fx * DIST_M * U + rx * REF_OFF_M * U;
                const float ly = p->y + fy * DIST_M * U + ry * REF_OFF_M * U;
                ref->set_transform(unit_transform(lx, ly, a + PI, false));
                stage->add_child(ref);
                UtilityFunctions::print(String::utf8("[show] 比例尺参照兵 char_rifleman 距车心 "),
                                        REF_OFF_M, "m");
            } else {
                UtilityFunctions::print(String::utf8("[show] 比例尺参照兵缺失（char_rifleman）"));
            }
        }

        cam_->set_position(to3(p->x, p->y, CAM_H));
        cam_->set_rotation(Vector3(PITCH, yaw_, 0));
        if (std::getenv("VA_FOV") == nullptr) cam_->set_fov(32.0f);
        UtilityFunctions::print(String::utf8("[show] 载具 "), String::utf8(key.c_str()),
                                String::utf8(" 距离 "), DIST_M, String::utf8("m 机高 "), CAM_H,
                                String::utf8(" 临时偏航 "), dyaw * 180.0f / PI,
                                String::utf8(" 度 建出 "), built, "/", n);
        /* 藏掉真单位、真车队、以及**道具层**。
           道具这一条在角色那条通道里是"保留"的（当尺寸参照），载具这边必须反过来藏：
           实测第一张吉普近景里，一块岩石**正好压在车身右半侧**上，
           车顶线与岩石轮廓糊在一起，"车厢有多高、轮胎露几个"当场读不出来 ——
           与角色那轮"一根树干立在视线中轴"是同一个坑。
           载具的尺寸参照已经由上面那个人给了，不再需要岩石，所以道具在这里是纯干扰。 */
        if (refs_.units != nullptr) refs_.units->set_visible(false);
        if (refs_.vehicles != nullptr) refs_.vehicles->set_visible(false);
        if (refs_.props != nullptr) refs_.props->set_visible(false);
        return;
    }

    if (mode.rfind("prop:", 0) == 0) {
        /* ---- 地物近景 / 陈列排：prop:<键>[:<临时偏航角>] 或 prop:row ----
         【为什么地物也要一条通道】岩石/树/灌木换真模型之后，同样有三件事
         战场截图答不了：**比例对不对**（跟 1.68 m 的兵比多大）、
         **底是踩在地上还是悬空**、**压扁有没有把模型压成一块饼**。
         而地物恰恰是"数量最多、每块都很小"的那一类 —— 场上三十块石头里
         随便挑一块，画面上就是几像素，什么都读不出来。

         【取景按"物体自身的实际高度"推，不能照抄角色那套】
         两个原因：
           ① 地物的高宽比跨度极大：灌木 0.6 m、松树 8 m，同一套距离必然
              要么把树拍出框、要么把灌木拍成一个点；
           ② 相机高度取 0.5×H（而不是固定的 1.05 m）—— 低矮地物的判据是
              "**底下那条地面余量**"，相机太高就俯视，地面余量被压没了。
         数字：FOV 32 ⇒ 竖半角 16°、tan = 0.2867，横半视场 = 0.2867×(2560/1369)
         ≈ 0.536（同一个换算见 veh 分支）。要求"物体高 H 在框内且脚下留 0.25H"：
           半视场 ≥ 0.65H  ⇒ D ≥ 2.27H
         再由参照兵定下限（参照兵横向偏 1.6 m，还要留 0.4 m 边距）：
           0.536D ≥ W/2 + 1.6 + 0.4
         取两者的大者。岩石落到横向下限（D≈6）、松树落到纵向上限（D≈18.5），
         正是上面那个"跨度极大"的直接后果。 */
        std::string rest = mode.substr(5);
        std::string key = rest, yaw_s;
        const size_t c = rest.find(':');
        if (c != std::string::npos) { key = rest.substr(0, c); yaw_s = rest.substr(c + 1); }
        float dyaw = 0.0f;
        if (!yaw_s.empty()) dyaw = (float)std::strtod(yaw_s.c_str(), nullptr) * PI / 180.0f;

        const bool row = (key == "row");
        const char *pk[4] = { "prop_rock_a", "prop_rock_b", "prop_pine", "prop_bush" };
        // 代表尺寸：岩石/灌木给的是**水平尺度**、树给的是**高**（与 kPropArt 的
        // by_height 同口径）。取的是场上最常见的档：岩石 p.r≈22、灌木 p.r≈15、树 8 m。
        const float pt[4] = { 2.20f, 2.20f, 8.00f, 1.65f };
        // 高宽比只为算取景用，必须与 kPropArt 保持一致（岩石 0.50 / 灌木 0.36）
        const float phw[4] = { 0.50f, 0.50f, 0.0f, 0.36f };

        /* 【单体模式必须按**点中的那个键**取尺寸，不能一律用 pt[0]】（2026-09-23 修）
           原来取景与建节点两处都写成 `row ? pt[i] : pt[0]`，于是 `prop:prop_pine`
           把 8 m 的树按 2.2 m 建出来、又按 2.2 m 取景 —— 截图上就是"树怎么跟人差不多高"，
           而战场上它是按 `7.0 + rng·3.0` 摆的，两边对不上。
           这条通道存在的唯一理由就是回答"**比例对不对**"，所以这个 bug 恰好
           把它的判据毁掉：拍出来偏小，看着像"树接错了"，其实是取景表取错了行 ——
           最坏的一种错，因为它会让人去改一个本来正确的模型。
           键写错时静默退回 pt[0]（= 改动前的行为），所以顺手把解析出的下标喊进日志。 */
        int pick[4] = { 0, 1, 2, 3 };
        if (!row) {
            int idx = 0;
            for (int i = 0; i < 4; ++i) {
                if (key == pk[i]) { idx = i; break; }
            }
            pick[0] = idx;
            if (key != pk[idx]) {
                UtilityFunctions::print(String::utf8("[show] 未知地物键 "),
                                        String::utf8(key.c_str()),
                                        String::utf8("（临时按 prop_rock_a 的尺寸取景）"));
            }
        }
        const int n = row ? 4 : 1;

        float max_h = 0.0f, sum_w = 0.0f;
        for (int i = 0; i < n; ++i) {
            const int j = pick[i];
            const float h = (phw[j] > 0.0f) ? pt[j] * phw[j] : pt[j];
            const float w = (phw[j] > 0.0f) ? pt[j] : pt[j] * 0.5f;   // 树冠估算 0.5H
            max_h = std::max(max_h, h);
            sum_w += w;
        }
        const float GAP_P = row ? 1.60f : 0.0f;
        const float row_w = sum_w + GAP_P * (float)(n - 1);
        const float CAM_H  = std::max(0.90f, 0.50f * max_h);
        const float D_vert = 2.27f * max_h;
        const float D_horz = (row ? (row_w * 0.5f * 1.15f) : (row_w * 0.5f + 2.0f)) / 0.536f;
        const float DIST_M = std::max(6.0f, std::max(D_vert, D_horz));
        const float PITCH  = -0.06f;
        const float REF_OFF_M = 1.60f;

        int built = 0;
        float off_acc = -row_w * 0.5f;
        for (int i = 0; i < n; ++i) {
            const int j = pick[i];
            const std::string k = row ? std::string(pk[i]) : key;
            const float t = pt[j];
            Node3D *nd = make_prop_node(k, t, refs_.units);
            if (nd == nullptr) {
                // 与角色/载具同口径：缺模型**留空位**而不是把后面的往前挪 ——
                // 挪了就再也看不出"少哪一个"，而"少哪个"恰恰是要一眼看出来的事。
                UtilityFunctions::print(String::utf8("[show] 缺地物模型，第 "), i,
                                        String::utf8(" 格（"), String::utf8(k.c_str()),
                                        String::utf8("）空置"));
                continue;
            }
            const float w_i = (phw[j] > 0.0f) ? pt[j] : 4.0f;
            const float off_m = row ? (off_acc + w_i * 0.5f) : 0.0f;
            off_acc += w_i + GAP_P;
            const float lx = p->x + fx * DIST_M * U + rx * off_m * U;
            const float ly = p->y + fy * DIST_M * U + ry * off_m * U;
            // 地物没有"正面"，偏航只用来核对"压扁/对轴有没有把它转歪"。
            /* 【缩放必须合进 Basis，不能 set_transform 之后再想】set_transform 是
               **整体替换**：它会把 make_prop_node 里设好的 Vector3(t,t,t) 一并抹成 1。
               于是检阅台里每一件地物都按"归一化后的单位原型"渲染 —— 岩石 1.0 m 宽
               （而不是 2.2 m）、针叶树 1.0 m 高（而不是 8 m）。
               而**战场侧是对的**：add_prop 用的是 set_position / set_rotation，
               这两个不动缩放。所以这是一个**只影响取证**的 bug —— 偏偏检阅台存在的
               唯一理由就是回答"比例对不对"，它一错，判据就变成反的：
               拍出来树只有人一半高，看着像"模型接错了"，其实该改的是取景（这里）。
               2026-09-23 实测：树 135 px / 参照兵 236 px，反推 0.96 m，恰好是单位原型。 */
            Basis pb(Vector3(0, 1, 0), a + PI + dyaw);
            pb.scale(Vector3(t, t, t));
            nd->set_transform(Transform3D(pb, to3(lx, ly, ground_h(lx, ly))));
            stage->add_child(nd);
            ++built;
        }

        // 比例尺参照兵：与载具那条同一个理由 —— "跟人比多大"只能靠同框回答。
        if (!row) {
            if (Node3D *ref = make_unit_node_by_key("char_rifleman", refs_.units)) {
                const float lx = p->x + fx * DIST_M * U + rx * REF_OFF_M * U;
                const float ly = p->y + fy * DIST_M * U + ry * REF_OFF_M * U;
                ref->set_transform(unit_transform(lx, ly, a + PI, false));
                stage->add_child(ref);
                UtilityFunctions::print(String::utf8("[show] 比例尺参照兵 char_rifleman 距物心 "),
                                        REF_OFF_M, "m");
            } else {
                UtilityFunctions::print(String::utf8("[show] 比例尺参照兵缺失（char_rifleman）"));
            }
        }

        cam_->set_position(to3(p->x, p->y, CAM_H));
        cam_->set_rotation(Vector3(PITCH, yaw_, 0));
        if (std::getenv("VA_FOV") == nullptr) cam_->set_fov(32.0f);
        UtilityFunctions::print(String::utf8("[show] 地物 "), String::utf8(key.c_str()),
                                String::utf8(" 距离 "), DIST_M, String::utf8("m 机高 "), CAM_H,
                                String::utf8(" 横向跨 "), row_w,
                                String::utf8(" 临时偏航 "), dyaw * 180.0f / PI,
                                String::utf8(" 度 建出 "), built, "/", n);
        /* 藏掉真单位、真车队、以及**道具层**。
           这里道具层是**必藏**的（与载具那条同理、与角色那条相反）：
           场上本来就散着三十块程序化/真模型石头与二十棵树，而检阅台这套
           用的又是同一批模型、还冻在出生点附近 —— 同框会把"这一排到底几个"
           彻底搅乱（载具那轮实测一块岩石正好压在吉普车身上、
           角色那轮实测一根树干正好立在视线中轴上）。 */
        if (refs_.units != nullptr) refs_.units->set_visible(false);
        if (refs_.vehicles != nullptr) refs_.vehicles->set_visible(false);
        if (refs_.props != nullptr) refs_.props->set_visible(false);
        return;
    }

    // ---- 陈列排 ----
    const std::vector<std::string> &keys = all_art_keys();    // 末位那一个是**倒在地上的步枪手** —— 倒地姿态只有摆出来才能确认，
    // 而战场上要等到有人被打倒才看得到，取证时等不起（也未必等得到）。
    const int n = (int)keys.size() + 1;

    constexpr float GAP_M   = 1.40f;   // 同排间距
    constexpr float ROW0_M  = 6.50f;   // 前排距离
    constexpr float ROW1_M  = 9.20f;   // 后排距离（错开半格，避免前排挡住后排）
    constexpr int   PER_ROW = 6;

    int built = 0;
    for (int i = 0; i < n; ++i) {
        const int row = i / PER_ROW;
        const int col = i % PER_ROW;
        const int row_cnt = (row == 0) ? PER_ROW : (n - PER_ROW);
        // 每排各自以相机中轴居中；后排再右移半格，让后一排的人落在前一排的空隙里
        const float off_m = (col - (row_cnt - 1) * 0.5f) * GAP_M + (row == 0 ? 0.0f : GAP_M * 0.5f);
        const float dep_m = (row == 0) ? ROW0_M : ROW1_M;

        const bool downed = (i == n - 1);
        const std::string key = downed ? std::string("char_rifleman") : keys[(size_t)i];

        Node3D *nd = make_unit_node_by_key(key, refs_.units);
        if (nd == nullptr) {
            // 模型缺失时**留一格空位**并打一行日志，而不是把后面的往前挪 ——
            // 挪位之后"第 4 格是医疗兵"这种读图方式就失效了，
            // 而"少一个模型"恰恰是最需要一眼看出来的事。
            UtilityFunctions::print(String::utf8("[show] 缺模型，第 "), i, String::utf8(" 格（"),
                                    String::utf8(key.c_str()), String::utf8("）空置"));
            continue;
        }
        const float lx = p->x + fx * dep_m * U + rx * off_m * U;
        const float ly = p->y + fy * dep_m * U + ry * off_m * U;
        // 正面朝向相机：模型前方 = (cos f, sin f)，要指向相机就得取 a + π。
        // 倒地的那一个额外叠 84° 侧翻（unit_transform 内部处理），
        // 让他**沿排面**倒下（facing 与同排一致），躺姿才读得出来。
        nd->set_transform(unit_transform(lx, ly, a + PI, downed));
        /* 站立陈列也要挂上腿摆层（摆角 0）—— 与 one: / run: 同口径。
           不是为了让它动，而是为了让**所有角色渲染路径共用同一套材质**：
           哪条路径漏挂，那条路径就会退回原始 StandardMaterial3D，
           于是两个模式下的取证图无法互相比对，而这类偏差在截图上根本看不出来。 */
        leg_.apply(nd, key, 0.0f, 0.0f);
        stage->add_child(nd);
        ++built;
    }

    /* 相机与视图模型。
       复用第一人称相机（_process 里 show_mode_ 早退，不会再被 sync 覆盖回玩家身上），
       不另立一台 —— 多一台相机就多一处"哪台是 current"的隐性状态，
       而本工程已经被"这块黑东西到底属于哪一层"坑过一次。 */
    cam_->set_position(to3(p->x, p->y, 1.65f));
    cam_->set_rotation(Vector3(pitch_, yaw_, 0));

    UtilityFunctions::print(String::utf8("[show] 检阅台：陈列 "), built, "/", n, String::utf8(" 个模型，间距 "),
                            GAP_M, String::utf8("m，前排 "), ROW0_M, String::utf8("m 后排 "), ROW1_M, "m");

    /* 藏掉场上**真单位**、**真车队**、以及**道具层**。
       真单位用的是同一批模型，又正好冻在出生点（大多就在玩家身边几米内），
       同框会读不出"这一排到底几个、第几格是谁"—— 而这恰恰是检阅台唯一要回答的问题。
       真车队是同一个道理的另一半：四辆车停在公路上，`veh:row` 那一排同样四辆，
       同框就会数出**八辆**，而"这一排到底几辆、顺序对不对"正是载具陈列排唯一要回答的问题。
       道具层又是另一半：上一版就这么拍了一张，
       一根树干**正好立在视线中轴**上，把中间两格劈成两半。
       道具在陈列排这种"整排平铺"的取景里没有净收益（尺寸参照用不上整排），
       要参照尺寸请走近景单体模式，那里背景道具是保留的。 */
    if (refs_.units != nullptr) refs_.units->set_visible(false);
    if (refs_.vehicles != nullptr) refs_.vehicles->set_visible(false);
    if (refs_.props != nullptr) refs_.props->set_visible(false);
}

void WorldSim::_ready() {
    va_trace("_ready:enter");
    va::build_convoy_way();
    va::set_events(this);

    /* 顺序是关键：世界必须先于静态场景建立。
       build_scene() 会读 va::W.props 来生成全部掩体，而 W.props 是在
       init_world() 里由 BASE_PROPS 填充的 —— 反过来写的话，
       掩体节点层会建成一个空容器：逻辑层有 69 处掩体、画面上一个都看不见，
       「掩体评分 / 掩体减伤」这套核心玩法在视觉上完全不可读。
       （实测第一版就是这么错的，截图里光秃秃一片。） */
    /* **必须走 init_level_world 而不是裸 init_world(seed)**：
       裸调用等于 levelIdx < 0 → CAM.active = false → 整局跑的还是改动前那一关，
       简报上的目标清单、撤离门槛、结算文案全都按旧单关卡走 ——
       "六关战役"在游戏里就一次也不会发生（只有离线工具能跑到）。
       这里只初始化逻辑世界：静态场景还在下面才建，而 build_scene 要读 W.props。 */
    init_level_world();
    force_ads_ = (std::getenv("VA_ADS") != nullptr);
    autoplay_  = (std::getenv("VA_AUTO") != nullptr);
    if (const char *dt = std::getenv("VA_DOWN_AT"); dt != nullptr && *dt != '\0') {
        down_at_ = std::atof(dt);
    }
    /* VA_RETREAT_AT=<战局秒数>：到点强制弹出「伤亡过半 → 1 撤 / 2 守」选择条。
       为什么需要它：真实玩到"伤亡过半"要靠运气（机械剧本最硬的一局也只伤亡 3/10），
       而这条 HUD 是一个**等玩家决策的常驻状态**，只有真弹出一次才验得到它画得对、
       按 1/2 有没有反应。它只置 `W.retreatOffered` 这一个标志 —— 判定、结算、
       转进全都还是走 `src/sim/` 的真实代码，本层不伪造任何战果。
       离线侧的对口开关是 VA_SW_CAS（减员）+ VA_SW_RETREAT（选撤退）。 */
    if (const char *rt = std::getenv("VA_RETREAT_AT"); rt != nullptr && *rt != '\0') {
        retreat_at_ = std::atof(rt);
    }
    /* 快进（VA_FF）。加它的原因是实测：一次"打到交火"的取证跑图 = 300 秒真实时间，
       因为逻辑层按真实经过时间推进，而车队要等到 CFG.convoyIn = 175 秒才进地图
       —— 在那之前战场上一个人都没有（这一点是本轮踩到的：t=50 的取证跑图里
       一条命中都没有，不是自动战斗坏了，是那时还没敌人）。 */
    if (const char *v = std::getenv("VA_FF"); v != nullptr && *v != '\0') {
        int k = std::atoi(v);
        if (k < 1) k = 1;
        if (k > 32) k = 32;          // 上限只为防手滑，正常用 4~8
        ff_ = k;
    }
    if (retreat_at_ >= 0.0) {
        UtilityFunctions::print(String::utf8("[combat-ev] 取证注入：强制弹出撤退选择条 t="),
                                String::num(retreat_at_, 1), String::utf8(" 秒（VA_RETREAT_AT）"));
    }
    if (autoplay_ || down_at_ >= 0.0 || ff_ > 1) {
        UtilityFunctions::print(String::utf8("[combat-ev] 取证注入：自动战斗="), autoplay_ ? String::utf8("开") : String::utf8("关"),
                                String::utf8("  强制击倒时刻="), down_at_ >= 0.0 ? String::num(down_at_, 1) : String("-"),
                                String::utf8("  快进="), ff_, "x");
    }
    /* 剧本 A（VA_SCRIPT_A）：与 tools/va_sweep.cpp 的 SCRIPT_A 逐条对应。
       没有它就跑不到交火 —— 见 world_sim.h 里的说明（伏击由第一枪触发，
       而"有没有第一枪"取决于车队有没有进射界，这不是自动战斗能保证的事）。 */
    if (std::getenv("VA_SCRIPT_A") != nullptr) {
        script_a_ = true;
        UtilityFunctions::print(String::utf8("[combat-ev] 启用剧本 A（与离线扫描同一套口令/触发条件）"));
    }
    /* VA_CAPTURE_EV 的本层开关。HUD 侧读同一个变量（决定要不要把事件报上来），
       这里再读一次只为能打印 "[capture-ev] 落盘 …" 那行时序自证 ——
       没有它，下次再遇到"截不到某条路径"，就只能靠猜是帧率问题还是标记寿命问题。 */
    ev_cap_ = (std::getenv("VA_CAPTURE_EV") != nullptr);
    if (ev_cap_) {
        UtilityFunctions::print(String::utf8("[combat-ev] 启用事件当帧落盘（VA_CAPTURE_EV）：等 0.05 秒墙钟 + 至少跨 1 帧，与帧率解耦"));
    }
    va_trace("_ready:init_world ok");

    build_scene(this, refs_);
    va_trace("_ready:build_scene ok");
    // 取证用消融开关：把某一层藏起来，用来判定"画面上这块到底是什么"。
    // 本机已经在"肉眼认几何体"上错过好几次（把树当成枪、把枪当成草地……），
    // 分层消融是唯一可靠的归属判断手段。
    if (std::getenv("VA_HIDE_PROPS") != nullptr && refs_.props != nullptr) refs_.props->set_visible(false);
    if (std::getenv("VA_HIDE_UNITS") != nullptr && refs_.units != nullptr) refs_.units->set_visible(false);
    if (std::getenv("VA_HIDE_VEH") != nullptr && refs_.vehicles != nullptr) refs_.vehicles->set_visible(false);
    // 自己身体的开关（默认隐藏）与"谁挡了镜头"的数字探针，都见 sync_entity_nodes 的注释
    show_self_ = (std::getenv("VA_SHOW_SELF") != nullptr);
    dbg_units_ = (std::getenv("VA_DBG_UNITS") != nullptr);
    if (show_self_) {
        UtilityFunctions::print(String::utf8("[self] VA_SHOW_SELF：刻意把玩家自己的身体画回来（复现视野遮挡用）"));
    }
    dbg_input_ = (std::getenv("VA_DBG_INPUT") != nullptr);
    if (dbg_input_) {
        UtilityFunctions::print(String::utf8("[dbg-input] 逐事件记录按键流 + 每 0.25 秒墙钟打一次位置心跳"));
    }
    spawn_entity_nodes();
    va_trace("_ready:spawn ok");
    setup_runtime_ui();
    va_trace("_ready:ui ok");

    /* 界面外壳的初始屏。
       默认先进主菜单；但取证 / 回归必须直接进战斗 ——
       截图探针是按「战局秒数」触发的（capture_step 依赖逻辑层 t 推进），
       而菜单期间 t 是冻结的：不跳过菜单，所有截图都会拍到菜单，
       整条取证管线会当场失效。所以 VA_CAPTURE 一出现就等价于 VA_SKIP_MENU。

       VA_SCREEN=menu|brief|play 可以**显式**指定初始屏，且优先级高于上面的跳过规则。
       加这个旋钮是因为否则新加的两屏根本无法取证：想看菜单就得跑 VA_CAPTURE，
       而 VA_CAPTURE 又强制跳过菜单，形成死结。
       菜单/简报期间截图改用墙钟驱动（见 shell_t_），所以照样能拍到。 */
    if (hud_ != nullptr) {
        hud_->load_art();
        const char *scr = std::getenv("VA_SCREEN");
        const bool have_scr = (scr != nullptr && *scr != '\0');
        const bool skip = !have_scr && ((std::getenv("VA_SKIP_MENU") != nullptr)
                                     || (std::getenv("VA_CAPTURE") != nullptr));
        Hud::Screen s0 = Hud::SCREEN_PLAY;
        if (have_scr) {
            const std::string v(scr);
            if (v == "menu")       s0 = Hud::SCREEN_MENU;
            else if (v == "brief") s0 = Hud::SCREEN_BRIEF;
        } else if (!skip) {
            s0 = Hud::SCREEN_MENU;
        }
        hud_->set_screen(s0);
        if (s0 != Hud::SCREEN_PLAY) {
            Input *in = Input::get_singleton();
            if (in != nullptr) in->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
        }
        UtilityFunctions::print(String::utf8("[ui] 初始屏 = "),
                                String(s0 == Hud::SCREEN_MENU ? "MENU"
                                       : (s0 == Hud::SCREEN_BRIEF ? "BRIEF" : "PLAY")));
    }

    /* VA_END=win|lose：直接把战局按指定结局收尾。
       用途是给结算面板取证 —— 真打到结束要跑满一局（最长 10 分钟模拟时间），
       无人值守下不可能反复跑。这里只设结果与统计，不碰逻辑判定，
       所以看到的就是真实的 draw_end_panel，而不是另一个"预览版面板"。 */
    if (const char *fe = std::getenv("VA_END"); fe != nullptr && *fe != '\0') {
        const bool win = (std::string(fe) == "win");
        va::MissionStats &s = va::W.stats;
        if (win) {
            s.tankKilled = true; s.boxTaken = true; s.boxEvacuated = true;
            s.evacCount = 7; s.apcKilled = 3; s.enemyDead = 18;
            s.allyDead = 1; s.allyDown = 2; s.tankShells = 2; s.minesUsed = 1;
            s.cmdIssued = 9; s.cmdExec = 7; s.cmdRefused = 2;
        } else {
            s.tankKilled = false; s.boxTaken = false;
            s.apcKilled = 1; s.enemyDead = 9;
            s.allyDead = 4; s.allyDown = 1; s.evacCount = 2;
            s.tankShells = 4; s.minesUsed = 0;
            s.cmdIssued = 11; s.cmdExec = 4; s.cmdRefused = 7;
        }
        va::W.over = true;
        va::W.overKind = win ? "成功" : "失败";
        va::score_mission();
        UtilityFunctions::print(String::utf8("[ui] VA_END 强制结局 = "), String(win ? "win" : "lose"),
                                String::utf8(" 评级="), String::utf8(va::W.score.g.c_str()));
    }

    // 第一人称相机
    cam_ = memnew(Camera3D);
    // FOV 65（垂直）→ 水平约 100°。
    // 之前用 78 是照搬网页版，但那是 2D 俯视时代的遗留：垂直 78 在 2560×1369 下
    // 水平高达 113°，属于广角畸变区，靠近相机的东西会被拉得又大又歪，枪模怎么摆
    // 都不像枪。这是第一人称射击的常规区间，也是使命召唤的手感来源。
    // VA_FOV 可覆盖（想看广角的战场感就 VA_FOV=78）。
    cam_->set_fov(65.0f);
    {
        const char *fv = std::getenv("VA_FOV");
        if (fv != nullptr && *fv != '\0') {
            const float f = (float)std::strtod(fv, nullptr);
            if (f > 20.0f && f < 140.0f) cam_->set_fov(f);
        }
    }
    // near 收到 0.06：视图模型最靠后的枪托底板在开镜时为 z=-0.135、腰射时 z=-0.175，
    // 再加后坐最大 +0.045 的退让，最坏也还有 4 cm 余量。
    cam_->set_near(0.06f);
    cam_->set_far(600.0f);
    add_child(cam_);
    // 第二个参数是武器模型**原始场景根**的挂载点：模型原型必须挂进场景树，
    // 否则它持有的 mesh / material / 贴图会在退出时被 Godot 报成 8 条
    // "RID allocations ... leaked at exit" 的 ERROR，污染"日志里有没有 ERROR"这条回归判据。
    // 传 this（WorldSim 自己）而不是 cam_：原型是场景资产，不该挂在会动的相机下面。
    vm_.build(cam_, this);
    va_trace("_ready:cam ok");

    /* 音频层。必须在 cam_ 建好之后 —— 听者是相机的子节点（跟着转头），
       传 cam_ 而不是 this。素材与声学口径见 node/audio.h。 */
    dbg_sfx_ = (std::getenv("VA_DBG_SFX") != nullptr);
    snd_.setup(this, cam_);
    /* 听者初始位置：不设就停在原点，而一局开始的 "ambush" 号角立刻就要用，
       第一声会被按「到世界原点的距离」误判（地图 110×65 m，起点离原点可以很远）
       —— 表现为「开局那一下没声」，很难归因。 */
    if (va::W.player != nullptr) snd_.set_listener(va::W.player->x, va::W.player->y);
    va_trace("_ready:snd ok");

    /* 跑动节奏层（见 node/unit_anim.h）。与音频层同性质：只读逻辑层状态。
       放在 sync 之前 —— 第一帧就要能算出姿态。 */
    anim_.setup();

    /* 双腿交替层（见 node/unit_leg.h）：着色器顶点位移，把左右腿按相位交替前后摆。
       摆角由 anim_ 算（PoseVals::leg_phase / leg_deg），所以必须排在 anim_.setup() 之后。
       VA_LEG=0 时内部直接返回 —— 不加载着色器、不换材质，画面与未加本层时逐像素相同。 */
    leg_.setup();

    /* 射击光效层（见 node/fx_layer.h）：枪口焰 / 曳光弹 / 弹着火花，按阵营分色。
       挂在 this（WorldSim 自己）而不是 cam_ —— 光效属于世界，不跟着镜头走。
       传 cam_ 只为近距剔除（玩家自己那发枪口焰在相机前 0.4 m，不剔除会糊屏）。
       与 snd_ 同性质：只读逻辑层的 fx / projectiles，一行逻辑都不改。 */
    fx_.setup(this, cam_);

    aim_at_road();
    sync_entity_nodes();
    va_trace("_ready:sync ok");

    // 检阅台必须在 cam_ 与 vm_ 都建好之后（它要摆相机、要藏枪模），
    // 也必须在 aim_at_road 之后（它读 player->facing 当排面朝向）。
    build_unit_showcase();

    capture_setup();

    UtilityFunctions::print("[VolunteerArmyPC] ", get_diag());
}

// --------------------------------------------------------- 截图探针
// 用法：环境变量 VA_CAPTURE="2,30,120,240"（战局秒数，逗号分隔）
//       环境变量 VA_CAPTURE_DIR 可覆盖输出目录（默认 res://captures）
// 用途：在无头/无人值守环境下取证「3D 战场确实渲染出来了」。
void WorldSim::capture_setup() {
    const char *env = std::getenv("VA_CAPTURE");
    if (!env || !*env) return;
    cap_enabled_ = true;
    std::string s(env);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        if (comma == std::string::npos) comma = s.size();
        const std::string tok = s.substr(pos, comma - pos);
        if (!tok.empty()) cap_times_.push_back(std::atof(tok.c_str()));
        pos = comma + 1;
    }
    std::sort(cap_times_.begin(), cap_times_.end());
    UtilityFunctions::print(String::utf8("[VA_CAPTURE] 计划在 "), (int)cap_times_.size(), String::utf8(" 个时刻截图"));
}

/* ---- 视野遮挡取证：镜头最近的是谁（VA_DBG_UNITS）--------------------------------
   「用户视野有大片遮挡」这类问题，分层消融（VA_HIDE_PROPS / UNITS / VEH）只能
   答到"属于哪一层"。要落到**具体哪一个单位**上，得看距离 ——
   因为队友的出生点与玩家只差 1.4 m 左右，而一个 1.68 m 的模型在 2 m 处
   就已经占满 66% 画高：截图上"贴脸的一大块"既可能是自己、也可能是队友。

   量的是**水平距离**而不是到节点原点的三维距离：身体是竖直的，决定它占多大画面的
   是水平距离（节点原点在脚底，三维距离会把"人比我矮"算成"离我远"，玩家自己
   量出来会是 1.65 m = 眼高，反而看着不像 0 距离）。

   p_when 只用于标注这一行是哪个时刻打的（ready / 某张截图前）。 */
void WorldSim::dbg_units_dump(const char *p_when) const {
    if (!dbg_units_ || cam_ == nullptr || unit_nodes_.size() != va::W.units.size()) return;
    const Vector3 c = cam_->get_global_position();

    struct Row { float d; size_t i; };
    std::vector<Row> rows;
    rows.reserve(va::W.units.size());
    for (size_t i = 0; i < va::W.units.size(); ++i) {
        const Vector3 p = unit_nodes_[i]->get_global_position();
        const float dx = p.x - c.x, dz = p.z - c.z;
        rows.push_back({ std::sqrt(dx * dx + dz * dz), i });
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.d < b.d; });

    UtilityFunctions::print(String::utf8("[dbg-units] "), String::utf8(p_when),
                            String::utf8(" 镜头最近的 6 个单位（水平距离 / 米；vis=是否在画，self=是不是玩家自己）"));
    const size_t n = std::min<size_t>(6, rows.size());
    for (size_t k = 0; k < n; ++k) {
        const size_t i = rows[k].i;
        const va::Unit &u = va::W.units[i];
        const std::string key = unit_model_key(u);
        UtilityFunctions::print(String::utf8("    #"), (int)i, " ", String::utf8(key.c_str()),
                                String::utf8("  d="), String::num(rows[k].d, 2),
                                String::utf8(" m  vis="), unit_nodes_[i]->is_visible() ? 1 : 0,
                                " self=", u.isPlayer ? 1 : 0,
                                u.dead ? String::utf8("  [阵亡]") : String(""));
    }
}

/* ---- 输入取证：按住方向键时，事件流把 IN 改成了什么（VA_DBG_INPUT）--------------
   "按住不松开会一直走 / 松开应该立刻停" 这类问题必须看事件流，不能看画面：
   画面上"走得慢"和"走了两步就停"读不出量级，而键鼠事件里的两个标志位
   （pressed = 键现在是不是按下的；echo = 这条是**操作系统的按键重复**）
   恰好就是判据本身 —— 重复事件绝不等于松开。

   判读口径（两条，缺一不可）：
   ① echo=1 的事件后面，IN(w,s,a,d) 必须与之前**相同**；
   ② 心跳里"位移=0.000 m"只能出现在 release 之后，不能出现在按住期间。 */
void WorldSim::dbg_input_key(int64_t p_code, bool p_pressed, bool p_echo) {
    if (!dbg_input_) return;
    UtilityFunctions::print(String::utf8("[dbg-input] key 码="), p_code,
                            String::utf8(" pressed="), p_pressed ? 1 : 0,
                            String::utf8(" echo="), p_echo ? 1 : 0,
                            String::utf8("  →  IN(w,s,a,d)="),
                            va::IN.w ? 1 : 0, va::IN.s ? 1 : 0, va::IN.a ? 1 : 0, va::IN.d ? 1 : 0,
                            String::utf8(" shift="), va::IN.shift ? 1 : 0,
                            String::utf8(" ctrl="), va::IN.ctrl ? 1 : 0);
}

/* 位置心跳。报的是**两次心跳之间的位移**（米）而不是坐标 ——
   "按住了却位移 0" 与 "松开了还在位移" 是同一枚硬币的两面，
   坐标列本身读不出这件事，位移列一眼就能读出来。 */
void WorldSim::dbg_input_heartbeat(double p_delta) {
    if (!dbg_input_) return;
    dbg_in_wall_ += p_delta;
    if (dbg_in_wall_ < dbg_in_next_) return;
    dbg_in_next_ = dbg_in_wall_ + 0.25;

    const va::Unit *p = va::W.player;
    const double x = p != nullptr ? (double)p->x : 0.0;
    const double y = p != nullptr ? (double)p->y : 0.0;
    /* 第一条心跳只记基准、不报位移：基准位置是从 (0,0) 起算的，
       而玩家出生点离原点 1.2 km —— 不这样处理的话第一行会报"位移 1233 米"，
       看着像"按住键瞬移了"，其实只是基准没建。 */
    if (dbg_in_have_prev_) {
        const double dx = x - dbg_in_x_, dy = y - dbg_in_y_;
        UtilityFunctions::print(String::utf8("[dbg-input] 心跳 墙钟="), String::num(dbg_in_wall_, 2),
                                String::utf8("s 战局="), String::num((double)va::W.t, 2),
                                String::utf8("s IN(w,s,a,d)="),
                                va::IN.w ? 1 : 0, va::IN.s ? 1 : 0, va::IN.a ? 1 : 0, va::IN.d ? 1 : 0,
                                String::utf8(" shift="), va::IN.shift ? 1 : 0,
                                String::utf8(" moving="), (p != nullptr && p->moving) ? 1 : 0,
                                String::utf8(" 位移="), String::num(std::sqrt(dx * dx + dy * dy), 3),
                                String::utf8(" m"));
    } else {
        dbg_in_have_prev_ = true;
    }
    dbg_in_x_ = x;
    dbg_in_y_ = y;
}

// 把当前视口存成 PNG。两条取证通道共用：
//   ① capture_step —— 按「战局秒数」定时拍（看战场整体长什么样）
//   ② 战斗事件取证 —— 事件发生那一刻拍（命中标记只活 0.24 秒，定时拍撞不上）
void WorldSim::save_shot(const godot::String &tag) {
    // 每张截图前顺手打一次"镜头最近的是谁"，让截图与数字对得上 ——
    // 只看图判断"这块深色是谁"，本机已经错过好几次（把树看成枪、把枪看成草地）。
    dbg_units_dump("capture");
    const char *dir_env = std::getenv("VA_CAPTURE_DIR");
    const std::string dir = dir_env && *dir_env ? std::string(dir_env) : std::string("res://captures");
    const godot::String gdir = godot::String::utf8(dir.c_str());
    DirAccess::make_dir_recursive_absolute(gdir);

    const Viewport *vp = get_viewport();
    if (vp == nullptr) return;
    const Ref<ViewportTexture> tex = vp->get_texture();
    if (!tex.is_valid()) return;
    const Ref<Image> img = tex->get_image();
    if (!img.is_valid()) return;
    const godot::String path = gdir + godot::String("/cap_") + tag + godot::String(".png");
    const Error err = img->save_png(path);
    UtilityFunctions::print("[VA_CAPTURE] ", path, err == OK ? " OK" : " FAILED");
}

void WorldSim::capture_step() {
    if (!cap_enabled_ || cap_i_ >= (int)cap_times_.size()) return;
    if (cap_sim_t_ < cap_times_[(size_t)cap_i_]) return;
    // 等两帧，确保这一帧的相机变换与场景同步都已生效（否则会拍到上一帧的画面）
    if (cap_frame_skip_ < 2) { cap_frame_skip_++; return; }
    cap_frame_skip_ = 0;

    save_shot(godot::String::num_int64((int)cap_times_[(size_t)cap_i_]) + godot::String("s"));
    cap_i_++;
    if (cap_i_ >= (int)cap_times_.size()) {
        UtilityFunctions::print(String::utf8("[VA_CAPTURE] 全部完成，退出"));
        get_tree()->quit();
    }
}

// ------------------------------------------- 剧本 A（VA_SCRIPT_A）
/* 逐条对应 tools/va_sweep.cpp 的 SCRIPT_A：同一套口令、同一触发条件、
   同一个"先 step 再下命令"的次序。

   两边唯一的差别是步长：离线扫描用 dt=0.05（为了与网页版 sweep.js 对齐），
   渲染端是固定 1/60。所以"同一局"指的是同一场景、同一阈值，
   不是逐位相同的轨迹 —— 要点在于**伏击由位置触发**（先头车 x < 1180），
   而不是像 VA_AUTO 那样押注"玩家恰好有射界开出第一枪"。 */
void WorldSim::script_a_step() {
    if (!script_a_) return;

    const va::Vehicle *lead = va::W.vehicles.empty() ? nullptr : &va::W.vehicles[0];
    const float T = va::W.triggerT;
    const int   i = script_cmd_;

    std::string out;
    bool fired = false;

    if (i == 0 && va::W.t > 5.0f) {
        out = "全体，隐蔽"; fired = true;
    } else if (i == 1 && lead != nullptr && lead->x < 1180.0f) {
        va::trigger_ambush("mine");          // 起爆在前、喊话在后 —— 与离线扫描一致
        out = "老白，起爆"; fired = true;
    } else if (i <= 1 && va::W.triggered) {
        /* 玩家抢在剧本之前开了第一枪（VA_AUTO 会这么干）。
           这时第 0/1 条已无意义，直接跳过，否则序号会永远卡在 1
           而后面 8 条口令一条都发不出去。 */
        script_cmd_ = 2;
        return;
    } else if (!va::W.triggered) {
        return;                              // 伏击还没开始，后面的指令都不到点
    } else if (i == 2 && va::W.t > T + 14.0f)  { out = "全体，开火"; fired = true; }
    else if (i == 3 && va::W.t > T + 34.0f)   { out = "反坦克组，打坦克"; fired = true; }
    else if (i == 4 && va::W.t > T + 56.0f)   { out = "老周，压制"; fired = true; }
    else if (i == 5 && va::W.t > T + 78.0f)   {
        out = (va::W.boxWhere == "truck") ? "全体，打卡车"
            : (va::W.boxWhere == "apc"   ? "反坦克组，打装甲车" : "全体，打军官");
        fired = true;
    }
    else if (i == 6 && va::W.t > T + 110.0f)  { out = "铁头，搬密码箱"; fired = true; }
    else if (i == 7 && va::W.t > T + 200.0f)  { out = "小满，救伤员"; fired = true; }
    /* 第 8 条（撤离）在战役下**不许走这条固定节奏**：它的"到点就喊撤"会在
       阵地还没守住、敌人还没打瘫的时候把全队打发去撤离点（这条口令不是"待命"，
       它直接把每个人的 moveGoal 指到撤离点），等于把主阵地拱手让出去。
       战役的撤离时机由下面的 script_campaign_step 按**目标达成情况**决定。 */
    else if (i == 8 && !va::CAM.active && va::W.t > T + 230.0f)  { out = "全体，撤离"; fired = true; }

    if (!fired) {
        script_campaign_step();     // 战役的那几条按目标驱动，不占固定序号
        return;
    }
    va::run_command_text(out, "voice", true);
    ++script_cmd_;
}

/* 战役模式下的口令（与 tools/va_sweep 的 run_level 同一套规则）。

   为什么实机也要有这一套：旧剧本是"发完 8 条就收工"的固定节奏，撤离令排到 T+230，
   而战役第一关的时限只有 300 秒 —— 按旧节奏**每一关都会被判成超时失败**，
   实机回归就再也验不到"过关 → 转进"这条链（离线工具验过了，但离线工具
   不经过 WorldSim 的推进逻辑，而那里恰恰是最需要实机证据的一段）。
   规则：非撤离类主目标一旦全达成就要撤；炸坝与夜袭夺回各自反复下令；
   **撤退令一旦发出就不再往回派人**（否则会把刚发出去的撤离令当场覆盖掉，
   实测离线工具踩过：第三关下了撤离令 40 秒里一个人都没往撤离点走）。 */
void WorldSim::script_campaign_step() {
    if (!va::CAM.active || !va::W.triggered) return;

    if (va::cur_level().hasDam && va::W.t > va::W.triggerT + 25.0f
        && va::W.t - camp_last_dam_ > 25.0f) {
        camp_last_dam_ = va::W.t;
        /* 呼号写"老白"没关系：issue_command 对 blowDam 有整条候选链
           （老白 → 石头 → 带炸药的人 → 任何活人），爆破是班组能力。 */
        va::run_command_text("老白，炸水库", "voice", true);
        return;
    }
    if (!va::W.evacOrdered && va::LV.retakeArmed && va::W.t - camp_last_retake_ > 20.0f) {
        camp_last_retake_ = va::W.t;
        va::run_command_text("全体，前往A点", "voice", true);
        return;
    }
    if (va::W.evacOrdered) return;          // 撤了就不再往回派
    bool tacticsDone = true;
    for (const auto &g : va::cur_level().goals) {
        if (g.main && g.kind != va::GoalKind::Evac && !va::goal_done(g)) { tacticsDone = false; break; }
    }
    if (tacticsDone || va::W.t > va::CFG.missionEnd - 90.0f) {
        if (va::W.t - camp_last_evac_ > 30.0f) {
            camp_last_evac_ = va::W.t;
            va::run_command_text("全体，撤离", "voice", true);
        }
    }
}

// ------------------------------------------- 战斗事件取证（VA_AUTO / VA_DOWN_AT）
void WorldSim::autoplay_step() {
    if (!autoplay_) return;
    va::Unit *p = va::W.player;
    if (p == nullptr || p->dead || p->downed) return;

    /* 选目标分两档：
         ① 有射界的**已下车**敌人 —— 只有打他们才会产生 `hits`（车上的人被
            弹道直接跳过），命中标记靠的就是它；
         ② 没有①时退而打**车上**的敌人：这是玩家开局会做的事（朝车队开枪），
            也是本局"伏击开始"的触发条件（`update_player` 里首发即 trigger_ambush）。
       少了②就没法自然起手 —— 伏击不触发，车队一路开出西侧，全场一枪不发。 */
    va::Unit *best = nullptr, *bestMounted = nullptr;
    float bd = 1e18f, bdM = 1e18f;
    for (auto &u : va::W.units) {
        if (u.team != va::Team::Enemy || u.dead || u.downed) continue;
        if (va::los_fire(p->x, p->y, u.x, u.y)) continue;    // los_fire 返回 true = 被挡住
        const float d = va::distf(p->x, p->y, u.x, u.y);
        if (u.mount) { if (d < bdM) { bdM = d; bestMounted = &u; } }
        else         { if (d < bd)  { bd  = d; best = &u; } }
    }
    if (best == nullptr) best = bestMounted;
    if (best == nullptr) return;

    // 与 _input 里鼠标视角那一条同源写法：yaw_ → player->facing
    const float a = std::atan2(best->y - p->y, best->x - p->x);
    yaw_ = -a - 1.5707963267948966f;
    // 俯仰归零：逻辑层按 viewPitch 折算有效射程，抬头会让子弹够不到地面目标
    pitch_ = 0.0f;
    va::W.viewPitch = 0.0f;
    p->facing = a;
    va::IN.fire = true;
    va::IN.w = va::IN.a = va::IN.s = va::IN.d = false;
}

void WorldSim::hud_down_inject() {
    if (down_at_ < 0.0 || down_done_) return;
    if ((double)va::W.t < down_at_) return;
    va::Unit *p = va::W.player;
    if (p == nullptr) return;
    down_done_ = true;
    if (p->downed || p->dead) return;

    /* 走真实的伤害入口（含掩体减伤 → down_player），而不是直接把 downed 置 true。
       这样验证的是"HUD 在真实倒地事件下画得对不对"，而不是"置了标志位以后画得对不对"。 */
    va::Unit *src = nullptr;
    float bd = 1e18f;
    for (auto &u : va::W.units) {
        if (u.team != va::Team::Enemy || u.dead || u.downed) continue;
        const float d = va::distf(p->x, p->y, u.x, u.y);
        if (d < bd) { bd = d; src = &u; }
    }
    UtilityFunctions::print("[combat-ev] t=", String::num((double)va::W.t, 2),
                            String::utf8(" 注入致命伤（真实 damage_unit）→ 击倒玩家"));
    va::damage_unit(p, 5000.0f, src, "bullet");
}

/* VA_RETREAT_AT：到点把「伤亡过半 → 1 撤 / 2 守」的选择条强制弹出来。
   这是这条 HUD 唯一的确定性取证入口 —— 真打到伤亡过半要看运气，
   而"选择条画得对不对 / 按 1 和按 2 有没有反应"这两件事只能真弹出一次才验得到。
   ⚠️ 只置 `W.retreatOffered` 与 `W.retreatOfferT` 两个字段，**不替玩家做选择**：
   撤还是守、什么时候结算、转进哪一关，全部仍由 `src/sim/` 的真实代码走完。
   ⚠️ 单关模式（`levelIdx < 0`）下 `check_end()` 不读 `retreatChoice`，
   这时按 1 不会有反应 —— 那是设计如此，不是开关坏了。 */
void WorldSim::retreat_inject() {
    if (retreat_at_ < 0.0 || retreat_done_) return;
    if ((double)va::W.t < retreat_at_) return;
    retreat_done_ = true;
    if (va::W.over || va::W.retreatChoice != 0) return;
    va::W.retreatOffered = true;
    va::W.retreatOfferT  = va::W.t;
    UtilityFunctions::print("[combat-ev] t=", String::num((double)va::W.t, 2),
                            String::utf8(" 强制弹出撤退选择条（1 撤 / 2 守），等待真实按键"));
}

void WorldSim::setup_runtime_ui() {
    CanvasLayer *layer = memnew(CanvasLayer);
    layer->set_layer(10);
    add_child(layer);

    // 整个 HUD 就是一个 Control，内容全部手绘。
    // 早期版本用 4 个 Label 平铺文字，能读但完全没有"界面"可言 ——
    // 使命召唤那套 UI 的信息有一半是用形状讲的（罗盘刻度、雷达、切角面板、
    // 动态准星），Label 表达不了。详见 node/hud.h 顶部说明。
    hud_ = memnew(Hud);
    hud_->set_name("Hud");
    // 铺满视口。HUD 只画不点，必须忽略鼠标事件，
    // 否则它会吃掉 _input 里那套鼠标视角控制。
    hud_->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    hud_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    layer->add_child(hud_);

    // 把视图模型挂给 HUD（在 hud_ 刚建好之后 —— vm_.build 在本函数更早处，
    // 所以顺序天然是对的）。用途只有一处：开镜时准星该不该让位给枪上的红点。
    // 收指针而不是收布尔量，是为了不必在"按 V 换枪"那条路上再同步一次。
    hud_->set_view_model(&vm_);

    // VA_ADS=1 的取证覆盖也要给 HUD：它**不写回 va::IN.ads**（写回去会污染
    // VA_DBG_INPUT 的输入取证，那是逻辑层的输入状态），只喂 ViewModel 的 ads 形参。
    // 不把同一个开关同步给 HUD 的话，"强制开镜"的取证图里枪是开镜姿态、
    // 准星却按腰射规则照画 —— 拿它验"开镜时准星让不让位"必然得出错结论
    // （实测踩过：日志里 开镜=0，而图看着像开镜）。force_ads_ 在本函数更早处已读。
    hud_->set_force_ads(force_ads_);

    // 取证用消融开关：把 HUD 整层藏掉，剩下的一定是 3D 画面。
    // 上一轮"画面正中央那根黑竖条到底是谁"靠肉眼认几何体认错过一次，
    // 所以凡是"这块黑东西属于哪一层"的问题，一律用消融来答，不靠眼看。
    if (std::getenv("VA_HIDE_HUD") != nullptr) hud_->set_visible(false);
}

void WorldSim::spawn_entity_nodes() {
    for (auto &u : va::W.units) {
        Node3D *n = make_unit_node(u, refs_.units);
        refs_.units->add_child(n);
        unit_nodes_.push_back(n);
    }
    for (auto &v : va::W.vehicles) {
        Node3D *n = make_vehicle_node(v.type, refs_.vehicles);
        refs_.vehicles->add_child(n);
        veh_nodes_.push_back(n);
    }
}

// 单位节点若数量对不上（例如增援/重开一局），整体重建
void WorldSim::sync_entity_nodes() {
    /* 跑动节奏：在**位置已更新之后**取一帧的相位与包络（时间基是 va::W.t）。
       放在这里而不是 _process 里，是因为本函数有 4 个调用点，而"同步了位置"
       才是动作的输入前提 —— 漏掉任何一处都会让那一帧的动作滞后一帧。
       同一帧被调多次时内部 dt=0，一切保持，不会把包络抖掉。 */
    anim_.step(va::W.units);

    if (unit_nodes_.size() != va::W.units.size()) {
        va_trace("sync:rebuild units");
        for (auto *n : unit_nodes_) n->queue_free();
        unit_nodes_.clear();
        for (auto &u : va::W.units) {
            Node3D *n = make_unit_node(u, refs_.units);
            refs_.units->add_child(n);
            unit_nodes_.push_back(n);
        }
        va_trace("sync:rebuild units done");
    }
    if (veh_nodes_.size() != va::W.vehicles.size()) {
        va_trace("sync:rebuild vehicles");
        for (auto *n : veh_nodes_) n->queue_free();
        veh_nodes_.clear();
        for (auto &v : va::W.vehicles) {
            Node3D *n = make_vehicle_node(v.type, refs_.vehicles);
            refs_.vehicles->add_child(n);
            veh_nodes_.push_back(n);
        }
        va_trace("sync:rebuild vehicles done");
    }

    for (size_t i = 0; i < va::W.units.size(); ++i) {
        const va::Unit &u = va::W.units[i];
        Node3D *n = unit_nodes_[i];
        /* 【第一人称不画自己的身体】
           逻辑层里玩家也是一个普通单位（W.units[0]，isPlayer=true），所以这个循环
           一开始给**包括玩家在内**的每个人都建了节点、每帧摆到 (x, y)。
           而相机就在同一个 (x, y) 上、高 1.65 m —— 角色模型归一化后高 1.68 m，
           于是镜头正好落在自己模型的**头里**：画面上就是一大块贴着脸的深色面，
           天空只剩左上角一条缝。实测的遮挡体量：一个 1.68 m 的模型在 2 m 处
           就已经占满 66% 画高，而自己这个的"距离"是 0。

           判据来自消融（VA_HIDE_UNITS 一藏、整块遮挡立刻消失），
           而要落到"具体是哪一个单位"上则靠 VA_DBG_UNITS 的打点 ——
           两者合起来才排除掉"其实是出生点旁边 1~2 米的队友挡的"这个很像的解释
           （队友出生点与玩家只差 ~1.4 m）。

           所以这里显式跳过玩家自己。VA_SHOW_SELF=1 可以放回来 ——
           那是**复现**这个问题用的，不是可选的画面风格。
           注意可见性要每帧重设：sync 的另一半（数量变化时重建节点）会新建节点，
           只在 spawn 时藏一次的话，重开一局遮挡就回来了。 */
        const bool self_hidden = u.isPlayer && !show_self_;
        n->set_visible(!u.dead && !self_hidden);
        if (u.dead) continue;
        // 姿态由 scene_builder 的 unit_transform 统一给出（偏航 + 倒地侧翻）。
        //
        // 【为什么姿态必须在这里施加】这段原来只写 position/rotation，而倒地姿态是在
        // make_soldier_node 里设的 —— 于是被这里的赋值逐帧覆盖，**倒地的士兵一直站着**，
        // 那段代码从未生效。现在两条渲染路径（图元士兵 / 三维模型）共用这一套姿态。
        //
        // 【为什么抽成公共函数】检阅台（VA_UNIT_SHOW）建节点时也要施加同一姿态。
        // 这里再抄一份的话，两处迟早出现"战场上倒下、检阅台上还站着"的偏差 ——
        // 而这类偏差极难在截图上发现，因为两条路径从不出现在同一张图里。
        //
        // 被隐藏的"自己"照样摆位（只是不画）：位置若停在原点，
        // VA_DBG_UNITS 就会把"自己 d=0.00 m"报成"自己 61 米外"，
        // 那正是这个探针唯一要回答的问题。
        //
        // 跑动节奏用**右乘**叠在姿态之后：unit_transform 仍然独占"人在哪、朝哪、
        // 倒没倒"这三件事，跑动层只加"自己在动"的局部摆动。
        // 这样倒地/朝向的判据不会因为加动作而改变（VA_UNIT_SHOW 检阅台也走这条）。
        const UnitAnim::PoseVals pv = anim_.pose_vals(i);
        n->set_transform(unit_transform(u.x, u.y, u.facing, u.downed)
                         * UnitAnim::pose_transform(pv));
        /* 双腿交替（见 node/unit_leg.h）：把**同一帧的同一个相位**喂给网格顶点位移层。
           显式取一次 pose_vals 再复用，而不是再走一遍 local_pose —— 后者内部会重算，
           一旦两处取到的不是同一帧的值，腿和躯干就会差一帧，快进时肉眼可见。 */
        leg_.apply(n, unit_model_key(u), pv.leg_phase, pv.leg_deg);
    }
    for (size_t i = 0; i < va::W.vehicles.size(); ++i) {
        const va::Vehicle &v = va::W.vehicles[i];
        Node3D *n = veh_nodes_[i];
        // 车队全程在公路（谷底）上跑，地形高度恒为 0；这里仍走同一个函数，
        // 免得以后有人把车队路线挪出谷底时漏掉一处抬升。
        const float gh = ground_h(v.x, v.y);
        if (v.destroyed) {
            n->set_visible(true);
            n->set_position(to3(v.x, v.y, gh - 0.18f));
            n->set_rotation(Vector3(0, -v.angle, 0));
            n->set_scale(Vector3(1, 0.86f, 1));
            continue;
        }
        n->set_position(to3(v.x, v.y, gh));
        n->set_rotation(Vector3(0, -v.angle, 0));
    }
}

void WorldSim::_process(double p_delta) {
    if (cam_ == nullptr) return;

    /* 界面外壳期间**冻结战局**：逻辑不步进、实体不同步、视图模型不更新。
       玩家在菜单里按 WASD 不该把正在潜伏的小队推着走；
       简报页上读到的目标清单必须是"还没开打"的那一份。
       但 HUD 仍要 update —— 菜单的悬停高亮、闪烁提示都靠它推进动画。
       注意这里直接 return 而不走下面的 capture_step：真跑取证时
       VA_CAPTURE 会让初始屏直接是 PLAY（见 _ready），外壳根本不会激活。 */
    if (shell_owns_input()) {
        if (hud_ != nullptr) {
            hud_->update(p_delta);
            if (hud_->take_start()) { enter_play(); return; }
            if (hud_->take_quit()) { get_tree()->quit(); return; }
        }
        /* 外壳期间逻辑层的 t 是冻结的，截图探针改由墙钟累计值驱动 ——
           否则菜单 / 简报这两个新屏一张证据都拿不到。
           （默认路径根本走不到这里：VA_CAPTURE 会让初始屏直接是 PLAY；
             只有显式给了 VA_SCREEN=menu|brief 才会进来。） */
        shell_t_ += p_delta;
        cap_sim_t_ = shell_t_;
        capture_step();
        return;
    }

    /* 检阅台：逻辑层完全冻结（不步进、不同步实体、不更新枪模）。
       代价是逻辑层的 t 停在 0，截图探针按 t 触发就永远不会响 ——
       所以这里和外壳期用同一招：改用墙钟 show_t_ 驱动。
       HUD 一并藏掉：这是给美术看的陈列照，罗盘/雷达/弹药板只会挡住下半排的脚，
       而"脚有没有踩在地上"正是要看的第一件事。 */
    if (show_mode_) {
        if (hud_ != nullptr) hud_->set_visible(false);
        show_t_ += p_delta;
        cap_sim_t_ = show_t_;
        capture_step();
        return;
    }

    /* 失焦兜底：只在**刚失焦的那一帧**清一次闩锁，不是每帧都清 ——
       每帧清会把 autoplay 每步刚写好的 IN.fire 也一起抹掉，还会把日志刷爆。
       （窗口一直不聚焦也是支持的用法：本工程的取证运行就是从命令行起的，
         那时窗口常常不在前台，而 autoplay 完全不依赖键鼠。） */
    {
        Window *w = get_window();
        const bool focused = (w == nullptr) || w->has_focus();
        if (!focused && focus_was_) clear_held_input(true);
        focus_was_ = focused;
    }

    // 固定步长推进逻辑（网页版是「按需要拆成 <=0.022s 的小步」，这里等价处理）
    va_trace("_process:enter");
    const double H = 1.0 / 60.0;
    const int step_cap = 8 * ff_;
    acc_ += p_delta * (double)ff_;     // VA_FF 只放大"喂进来的时间"，步长仍是 1/60
    int n = 0;
    while (acc_ >= H && n < step_cap) {
        /* 战斗事件取证注入（VA_AUTO / VA_DOWN_AT）放在**每个固定步之前**。
           原先放在循环外，快进时会出现"一帧跨过 0.13×ff 秒，枪口还停在上一帧的
           目标上"；放进循环里，瞄准的更新频率就与逻辑步一致 ——
           这样 VA_FF 才真的只是"跑得快"，而不是"跑得不一样"。
           放在 step 之前也保证了本次注入/瞄准在本步立即生效，
           下面的 hud_->update 才采样得到。 */
        autoplay_step();
        hud_down_inject();
        retreat_inject();
        va::step_once((float)H);
        script_a_step();      // 次序与 va_sweep 一致：先 step，再判该不该下命令
        acc_ -= H;
        ++n;
    }
    if (n == step_cap) acc_ = 0.0;   // 掉帧时放弃追帧，避免螺旋
    va_trace("_process:step ok");

    sync_entity_nodes();
    va_trace("_process:sync ok");

    // VA_DBG_RUN=1：每秒打一行"跑动人数 / 最快速度"。**静帧截图证明不了"在跑"**，
    // 这条数字才是"确实有单位被驱动"的判据（相位在动、幅度不为 0）。
    anim_.tick_diag(p_delta);

    // 相机跟随：位置取玩家单位，朝向由 yaw/pitch 决定
    const va::Unit *p = va::W.player;
    if (p) {
        // 走路摇晃
        const bool moving = (va::IN.w || va::IN.a || va::IN.s || va::IN.d);
        if (moving) bob_t_ += (float)p_delta * (va::IN.shift ? 13.0f : 9.0f);
        const float bob = moving ? std::sin(bob_t_) * 0.035f : 0.0f;
        /* 相机必须跟着地形抬：玩家站在坡上时若还按"绝对高度 1.65"摆，
           镜头就会埋进坡里（地形抬升最大 4.5 米 = 够埋掉整个头）。
           取证机位 VA_CAM_H 在此之上再抬（见文件开头的说明）。 */
        const float cam_h = (va::IN.ctrl ? 1.05f : EYE_H) + bob
                          + ground_h(p->x, p->y) + env_f("VA_CAM_H", 0.0f);
        cam_->set_position(to3(p->x, p->y, cam_h));
        // 听者位置与相机同步。用**逻辑层米数**（不是 to3 之后的世界单位），
        // 与 audio.cpp 里 ref/max 两个米制常数同口径 —— 见 audio.h 的说明。
        snd_.set_listener(p->x, p->y);

        // 逻辑层朝向回写：yaw = -facing - π/2  ⇒  facing = -yaw - π/2
        va::W.viewPitch = pitch_;
        const float PI = 3.14159265358979323846f;
        const float cam_pitch = env_set("VA_CAM_PITCH") ? env_f("VA_CAM_PITCH", 0.0f) * PI / 180.0f : pitch_;
        const float cam_yaw   = env_set("VA_CAM_YAW")   ? env_f("VA_CAM_YAW", 0.0f)   * PI / 180.0f : yaw_;
        cam_->set_rotation(Vector3(cam_pitch, cam_yaw, 0));
    }
    // 相机落地后的第一帧才量得出真数字 —— 在此之前 cam_ 还停在原点，
    // 量出来的"距离"全是到世界原点的距离（与开局的"谁挡在镜头前"没有关系）。
    if (dbg_first_) {
        dbg_first_ = false;
        dbg_units_dump("t=0");
    }

    va_trace("_process:cam ok");

    /* 射击光效（见 node/fx_layer.h）。位置有讲究：必须在**相机更新之后** ——
       近距剔除和曳光弹的条带朝向都要用当帧的相机位置；
       也必须在 sync_entity_nodes 之后 —— 枪口坐标是单位逻辑坐标推出来的。
       它只读逻辑层的 fx / projectiles，不写回任何状态，也不改任何判定。
       第三个参数是**本帧推进的逻辑秒数**（n 步 × 1/60），不是墙钟 ——
       枪口焰只有 0.055 s，掉帧或 VA_FF 快进时一帧能推进 0.1~0.13 s，
       用"当前时刻落在窗口内"去判会整帧跳过闪光。详见 fx_layer.h 的 step 说明。 */
    /* 玩家自己那发弹的**弹道起点** = 第一人称枪模的枪口世界坐标
       （见 fx_layer.h 的 set_player_muzzle）。不喂的话本层回退旧口径，
       玩家开枪时弹道线又从画面下方凭空冒出 —— 与右下角的枪口对不上。
       ⚠️ 这里读到的是**上一帧**的枪口变换：vm_.update() 在 _process 后段（输入
       处理之后），而本行在它之前。差一帧 = 16 ms，枪口在这个尺度上的位移是
       sway/bob 的量级（毫米级），对"弹道从枪口出发"没有可见影响；不值得为消掉
       这一帧去动 _process 的段落顺序（那一段有自己的理由：相机先落地，光效才量得准
       近距剔除）。muzzle_world() 在枪模未建好时返回 false，本层自动回退。 */
    {
        Vector3 mz;
        fx_.set_player_muzzle(vm_.muzzle_world(mz), mz);
    }

    fx_.step(va::W, (float)p_delta, (float)((double)n * H));

    // HUD：采样世界状态 + 推进动画。整屏内容一次 _draw() 画完，
    // 所以这里只需每帧调一次 update（它内部会 queue_redraw）。
    if (hud_ != nullptr) {
        hud_->update(p_delta);

        /* 战斗事件取证（VA_CAPTURE_EV）：HUD 声明"刚才发生了命中/击杀/倒地"，
           由本层落盘。**当帧不能存** —— 本帧的 _draw 还没跑，
           立刻读视口纹理只会拿到上一帧，也就是**没有那个标记的那一帧**。
           等多久见 world_sim.h 的 ev_wait_sec_：按**墙钟**等，不按帧数。
           在途期间只允许**更罕有**的事件抢位（击杀 > 倒地 > 队友阵亡 > 命中）：
           命中每 0.1~0.7 秒一次，全收会一直排队存不完；
           但一刀切地"在途就丢"会把击杀整条丢掉 —— 实测就是这么丢的：
           日志里 `t=228.52 击杀 (kills=1)` 明明发生了，截图目录里一张 ev_kill 都没有。 */
        constexpr float EV_SETTLE_SEC = 0.05f;   // 墙钟秒：60fps≈3 帧，且远小于命中标记的 0.24 秒
        const Hud::EvShot ev = hud_->take_ev_shot();
        bool ev_just_set = false;
        if (ev != Hud::EVSHOT_NONE) {
            int pri = 1;                                  // 默认按"命中"算
            switch (ev) {
                case Hud::EVSHOT_KILL: pri = 4; break;
                case Hud::EVSHOT_DOWN: pri = 3; break;
                case Hud::EVSHOT_ALLY: pri = 2; break;
                default:               pri = 1; break;     // HIT
            }
            if (ev_wait_sec_ < 0.0f || pri > ev_prio_) {
                switch (ev) {
                    case Hud::EVSHOT_HIT:  ev_tag_ = "ev_hit";     break;
                    case Hud::EVSHOT_KILL: ev_tag_ = "ev_kill";    break;
                    case Hud::EVSHOT_ALLY: ev_tag_ = "ev_allycas"; break;
                    case Hud::EVSHOT_DOWN: ev_tag_ = "ev_down";    break;
                    default: break;
                }
                ev_tag_ += godot::String("_t") + godot::String::num((double)va::W.t, 1);
                ev_prio_  = pri;
                // 抢位也重新计时：要拍的是"这一次"的 _draw
                ev_wait_sec_    = EV_SETTLE_SEC;
                ev_wait_frames_ = 1;
                ev_from_t_ = (double)va::W.t;
                ev_wall_   = 0.0;
                ev_frames_ = 0;
                ev_just_set = true;
            }
        }
        // 在途才推进。触发帧跳过递减 —— 这就保证了"当帧 _draw 已跑完"再读纹理（至少跨 1 帧）。
        if (!ev_just_set && ev_wait_sec_ >= 0.0f) {
            ev_wait_sec_ -= (float)p_delta;
            if (ev_wait_frames_ > 0) --ev_wait_frames_;
            ev_wall_ += p_delta;
            ++ev_frames_;
            if (ev_wait_sec_ <= 0.0f && ev_wait_frames_ <= 0) {
                /* 落盘。这一行日志是**取证时序的自证**：把"墙钟过了多久"和"仿真过了多久"
                   一起打出来。判读方法：**墙钟耗时必须小于被取证据的寿命**
                   （命中标记 0.24 秒 / 击杀标记 0.40 秒），否则截到的是已经衰减掉的画面，
                   甚至是完全消失的画面 —— 这正是当初 "ff=6 组一张 ev_hit 都没有" 的根因。 */
                const double now_t = (double)va::W.t;
                save_shot(ev_tag_);
                if (ev_cap_) {
                    UtilityFunctions::print(
                        String::utf8("[capture-ev] 落盘 "), ev_tag_,
                        String::utf8("  检测 t="), godot::String::num(ev_from_t_, 2),
                        String::utf8(" → 成像 t="), godot::String::num(now_t, 2),
                        String::utf8("  仿真 +"), godot::String::num(now_t - ev_from_t_, 2), "s",
                        String::utf8("  墙钟 +"), godot::String::num(ev_wall_, 3), "s",
                        "  ", ev_frames_, String::utf8(" 帧"));
                }
                ev_wait_sec_ = -1.0f;                      // 回到空闲
                ev_prio_ = 0;
            }
        }
    }

    // 视图模型：只读逻辑层的状态，反过来不影响逻辑
    {
        const va::Unit *pl = va::W.player;
        if (pl != nullptr) {
            const float move01 = (va::IN.w || va::IN.a || va::IN.s || va::IN.d) ? 1.0f : 0.0f;
            // 开火判定：fireCd 被重新赋成 rof 的那一刻就是"打了一发"。
            // 比监听 magAmmo 更可靠 —— 换弹会让 magAmmo 反向跳变。
            if (prev_fire_cd_ >= 0.0f && pl->fireCd > prev_fire_cd_ + 1e-4f) vm_.on_shot();
            prev_fire_cd_ = pl->fireCd;
            if (prev_reload_t_ >= 0.0f && pl->reloadT > prev_reload_t_ + 1e-4f) {
                vm_.on_reload(pl->wpn != nullptr ? pl->wpn->reload : 2.2f);
            }
            prev_reload_t_ = pl->reloadT;
            // 取证开关：VA_ADS=1 强制据枪，用来拍开镜姿态（截图时没法按鼠标右键）
            bool ads = va::IN.ads;
            if (force_ads_) ads = true;
            vm_.update(p_delta, move01, va::IN.shift, pitch_, yaw_, ads);
        }
    }

    // VA_DBG_INPUT 的位置心跳：按**墙钟**（p_delta）而不是战局秒数推进，
    // 这样 VA_FF 快进时"一秒墙钟走了几米"仍然读得出来。
    dbg_input_heartbeat(p_delta);
    va_trace("_process:hud ok");
    cap_sim_t_ = (double)va::W.t;
    capture_step();
    va_trace("_process:end");
}

void WorldSim::_input(const Ref<InputEvent> &p_event) {
    Input *in = Input::get_singleton();
    if (in == nullptr) return;

    /* 界面外壳期间：键鼠整条链路转交界面，一点都不能漏进逻辑层。
       漏的后果很具体：菜单里按 W 会写 va::IN.w，等玩家点"开始行动"进战斗时，
       角色会带着这个残留的按键状态直接开跑 —— 表现成"一进场就自己往前走"。
       所以这里在喂给逻辑层之前就整体 return。 */
    if (shell_owns_input()) {
        /* 外壳期间键鼠整条链路转交界面，一点都不能漏进逻辑层。
           漏的后果很具体：菜单里按 W 会写 va::IN.w，等玩家点"开始行动"进战斗时，
           角色会带着这个残留的按键状态直接开跑 —— 表现成"一进场就自己往前走"。
           注意这里连 **release 也一起被吃掉**了：战斗里按住 W 再进菜单松手，
           逻辑层的 IN.w 会永远停在 true。所以进外壳时必须把闩锁清掉，
           而且不允许 echo 重建（p_reacquire_ok=false）：菜单里的按键
           与战场无关。 */
        clear_held_input(false);
        if (Ref<InputEventKey> k = p_event; k.is_valid()) {
            if (k->is_pressed() && !k->is_echo()) hud_->shell_key((int64_t)k->get_keycode());
            return;
        }
        if (Ref<InputEventMouseMotion> mm = p_event; mm.is_valid()) {
            hud_->shell_hover(mm->get_position());
            return;
        }
        if (Ref<InputEventMouseButton> mb = p_event; mb.is_valid()) {
            if (mb->get_button_index() == MouseButton::MOUSE_BUTTON_LEFT && mb->is_pressed()) {
                hud_->shell_click(mb->get_position());
            }
            return;
        }
        return;
    }

    const bool captured = (in->get_mouse_mode() == Input::MOUSE_MODE_CAPTURED);

    if (Ref<InputEventMouseMotion> mm = p_event; mm.is_valid()) {
        if (!captured) return;
        yaw_ -= mm->get_relative().x * sens_;
        pitch_ -= mm->get_relative().y * sens_;
        pitch_ = va::clampf(pitch_, -1.45f, 1.45f);
        va::W.player->facing = -yaw_ - 1.5707963267948966f;
        return;
    }

    if (Ref<InputEventMouseButton> mb = p_event; mb.is_valid()) {
        if (mb->get_button_index() == MouseButton::MOUSE_BUTTON_LEFT && mb->is_pressed()) {
            if (!captured) { in->set_mouse_mode(Input::MOUSE_MODE_CAPTURED); return; }
            va::IN.fire = true;
            va::IN.firePressed = true;
            return;
        }
        if (mb->get_button_index() == MouseButton::MOUSE_BUTTON_LEFT && !mb->is_pressed()) {
            va::IN.fire = false;
            return;
        }
        if (mb->get_button_index() == MouseButton::MOUSE_BUTTON_RIGHT) {
            va::IN.ads = mb->is_pressed();
            return;
        }
        return;
    }

    if (Ref<InputEventKey> k = p_event; k.is_valid()) {
        const Key code = k->get_keycode();
        /* ---- 系统按键重复：它说的是"键还按着"，绝不是"松开了" ----------------
           按住不放时 Windows 每 ~30ms 重发一次 keydown，Godot 把它标成 echo=1
           （pressed 仍然是 1）。原来这里写的是
               const bool down = k->is_pressed() && !k->is_echo();
               ... va::IN.w = down;
           于是**每一条重复事件都把 IN.w 按成 false**：按住不放只走了 0.53 秒
           （Windows 默认重复延迟 500ms）就被自己的重复事件按停，之后一直到松开
           都不再动。实测数据（tools/inject_key.py + VA_DBG_INPUT，见 README）：
               6.4s pressed=1 echo=0 → IN.w=1，随后两条心跳位移 11.7 / 23.5 m
               ≈7.0s pressed=1 echo=1 → IN.w=0   ← 被重复事件按停
               7.0~8.4s 共 30 条 echo=1 → 位移 0.000 m
           现在的语义：echo 不写任何状态；只有"闩锁刚被清过"（见 clear_held_input）
           时才拿它把"键其实还按着"补回来 —— 那种情况下真正的 press 事件
           压根没送到过，echo 是唯一的线索。 */
        if (k->is_echo()) {
            if (reacquire_ && is_hold_key(code)) {
                reacquire_ = false;
                apply_key(code, true);
            }
            dbg_input_key((int64_t)code, true, true);
            return;
        }
        const bool down = k->is_pressed();
        // 真事件到了，键态就有了权威来源，不必再靠 echo 重建
        if (is_hold_key(code)) reacquire_ = false;
        apply_key(code, down);
        // VA_DBG_INPUT：事件原文（pressed/echo）与它写进逻辑层的结果打在一行上
        dbg_input_key((int64_t)code, down, false);
    }
}

/* 只有"按住"语义的键才允许被 echo 事件重建（W/A/S/D、方向键、Shift、Ctrl）。
   开关类是一次动作：R 换弹 / G 手雷 / F 烟雾 / Z 标记 —— 让重复事件去补一次
   按下，等于多打一枪、多丢一颗雷。 */
bool WorldSim::is_hold_key(Key p_code) {
    switch (p_code) {
        case Key::KEY_W: case Key::KEY_UP:
        case Key::KEY_S: case Key::KEY_DOWN:
        case Key::KEY_A: case Key::KEY_LEFT:
        case Key::KEY_D: case Key::KEY_RIGHT:
        case Key::KEY_SHIFT: case Key::KEY_CTRL:
            return true;
        default:
            return false;
    }
}

/* 把一条"键按下/松开"写进逻辑层。抽成函数是因为按键重复那条重建路径
   （见 _input 顶部）也要走同一套语义 —— 两处各抄一份，迟早会写歪一处。 */
void WorldSim::apply_key(Key p_code, bool p_down) {
    switch (p_code) {
        case Key::KEY_W: case Key::KEY_UP:    va::IN.w = p_down; break;
        case Key::KEY_S: case Key::KEY_DOWN:  va::IN.s = p_down; break;
        case Key::KEY_A: case Key::KEY_LEFT:  va::IN.a = p_down; break;
        case Key::KEY_D: case Key::KEY_RIGHT: va::IN.d = p_down; break;
        case Key::KEY_SHIFT: va::IN.shift = p_down; break;
        case Key::KEY_CTRL:  va::IN.ctrl = p_down; break;
        // R 有双重语义：战斗中换弹，结算界面上重开一局。
        // 用 W.over 分支而不是再占一个键 —— 结算时换弹毫无意义，
        // 键位重叠不会产生歧义。
        case Key::KEY_R:
            if (p_down) {
                /* 结算界面上 R = 继续战役：**过关了就转进下一阵地**，
                   没打过就重打这一关（advance_level 内部分这两支）。
                   这里必须是 advance_level 而不是 reset_mission ——
                   reset 只会重打当前关，那样"在一个关卡结束后率领队友撤往下一个阵地"
                   就永远发生不了。 */
                if (hud_ != nullptr && hud_->mission_over()) advance_level();
                else va::IN.reload = true;
            }
            break;
        case Key::KEY_G:     if (p_down) va::IN.grenade = true; break;
        case Key::KEY_F:     if (p_down) va::IN.smoke = true; break;
        case Key::KEY_Q:     if (p_down) { /* 指令面板（待接入） */ } break;
        /* V：切手里的武器外观（纯表现）。
           【为什么是 V】W/A/S/D 是移动、R 换弹、G 手雷、F 烟雾、Z 标记、Q 留给指令面板，
           余下的字母里 V 与"外观/装扮（visual）"对得上，也不会和上面任何一个撞。
           【为什么只在这个分支里做】apply_key 的调用点已经保证了两件事：
             ① 系统按键重复（echo）到不了这里 —— V 不是 is_hold_key，echo 那条路直接 return。
                否则按住 V 不放会以 ~30ms 的节奏把三把枪轮着切一遍。
             ② 界面外壳期间整条链路被 shell_owns_input() 拦掉 ——
                菜单里按 V 不会改战场上的枪。
           【为什么是纯表现】它只动 ViewModel（渲染层）的可见性，
           不碰 va:: 里的任何东西：伤害、射速、弹匣、判定顺序全都不变，
           所以平衡基线不受影响。 */
        case Key::KEY_V:
            if (p_down && vm_.next_skin(1) && hud_ != nullptr) {
                // 切完给个明确反馈。没有提示的话，玩家按了键只会怀疑"是不是没生效"
                // —— 三把枪在腰射姿态下的轮廓差异，一眼未必分得清。
                hud_->ev_alert(std::string("武器外观：") + vm_.skin_label(), 1.6f);
            }
            break;
        case Key::KEY_Z:     if (p_down) va::IN.markerSet = true; break;
        /* 1 / 2：伤亡过半后的「撤 / 守」选择。
           为什么必须是数字键：F 是烟雾弹、G 是手雷、Q/R/V/Z 各有其主，
           剩下的字母里没有一个能一眼对上"二选一"。
           **只在提示条已经弹出时生效** —— 平时按 1/2 不该有任何反应，
           否则玩家会在不知情的情况下把这一关提前结束掉（而提示只弹一次，
           那个"莫名其妙就转进了"的瞬间根本无从解释）。 */
        case Key::KEY_1:
            if (p_down && va::W.retreatOffered && va::W.retreatChoice == 0) {
                va::W.retreatChoice = 1;
                if (hud_ != nullptr) hud_->ev_alert("已下令：撤向下一个伏击阵地", 2.6f);
            }
            break;
        case Key::KEY_2:
            if (p_down && va::W.retreatOffered && va::W.retreatChoice == 0) {
                va::W.retreatChoice = 2;
                if (hud_ != nullptr) hud_->ev_alert("已下令：继续死守阵地", 2.6f);
            }
            break;
        case Key::KEY_ESCAPE:
            if (p_down) {
                /* 结算界面上 Esc = 回主菜单；战斗中 Esc = 释放鼠标。
                   和 R 键同一个思路：结算时"释放鼠标"毫无意义（已经没在瞄了），
                   键位重叠不产生歧义。顺带让界面外壳成为一个闭环，
                   而不是"进了战斗就再也回不到菜单"的单向门。 */
                if (hud_ != nullptr && hud_->mission_over()) {
                    /* Esc = 回主菜单。**顺手把"待转进"清掉**：回菜单意味着
                       这一局的转进没被确认，下次从菜单进来是重打本关，
                       而不是莫名其妙跳过关卡。 */
                    camp_cleared_ = false;
                    camp_won_ = false;
                    reset_mission();                    // 内部会把 W.over 清掉
                    hud_->set_screen(Hud::SCREEN_MENU);
                }
                Input *in = Input::get_singleton();
                if (in != nullptr) in->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
            }
            break;
        default: break;
    }
}

/* 清空"按住"类输入闩锁。三条路径会丢 keyup，一条都不能漏：
     ① 窗口失焦 —— Windows 把 keyup 送给了抢走焦点的那个窗口，本进程永远收不到。
        不处理的话：按住 W 时切出去，角色**自己一直走**（这就是"按住不放就一直
        往那个方向移动"的另一半原因）。
     ② 界面外壳 —— _input 在外壳期间整条 return（见上面的说明），release 也一起被吃掉。
     ③ 每帧的焦点核对 —— 兜底：通知（NOTIFICATION_APPLICATION_FOCUS_OUT）万一没送到，
        也不能让闩锁卡住。
   p_reacquire_ok 决定"要不要允许 echo 把键态补回来"：
     · 失焦 → true（键可能真的还按着，回来就该接着走）
     · 进菜单 → false（菜单里按住 W 不能变成"一进场就自己往前走"） */
void WorldSim::clear_held_input(bool p_reacquire_ok) {
    const bool any = va::IN.w || va::IN.s || va::IN.a || va::IN.d
                     || va::IN.shift || va::IN.ctrl || va::IN.fire || va::IN.ads;
    va::IN.w = va::IN.s = va::IN.a = va::IN.d = false;
    va::IN.shift = va::IN.ctrl = false;
    va::IN.fire = va::IN.ads = false;
    if (p_reacquire_ok) reacquire_ = true;
    // 什么都没按着就不吭声：这条路径每帧都可能被调用，否则日志会被刷屏
    if (any && dbg_input_) {
        UtilityFunctions::print(String::utf8("[dbg-input] 清空按键闩锁（"),
                                String::utf8(p_reacquire_ok ? "失焦" : "界面外壳"),
                                String::utf8("）—— 否则丢掉的 keyup 会让角色一直走"));
    }
}

// 从任务简报切进战斗。
void WorldSim::enter_play() {
    if (hud_ != nullptr) hud_->set_screen(Hud::SCREEN_PLAY);
    Input *in = Input::get_singleton();
    if (in != nullptr) in->set_mouse_mode(Input::MOUSE_MODE_CAPTURED);
    /* 简报期间逻辑层一步都没走过，实体节点还停在 _ready 建出来的位置上。
       这里对齐一次，并把视线重新指向公路来向 —— 保证"开打"这一帧画面就是对的，
       而不是先闪一帧歪的。 */
    aim_at_road();
    sync_entity_nodes();
    UtilityFunctions::print(String::utf8("[ui] 进入战斗"));
}

bool WorldSim::shell_owns_input() const {
    return hud_ != nullptr && hud_->shell_active();
}

void WorldSim::_notification(int p_what) {
    /* 窗口失焦：Windows 把 keyup 送给了抢走焦点的窗口，本进程收不到那条松开 ——
       "按住不放，角色就自己一直往那个方向走"就是这么来的。
       这里（以及 _process 每帧的焦点核对，兜通知没送到的情况）把闩锁清掉。
       用 p_reacquire_ok=true：键可能真的还按着，回到前台就该接着走，
       而 Windows 的按键重复会把"还按着"这件事再送一条 echo 过来（见 _input）。 */
    if (p_what == Node::NOTIFICATION_APPLICATION_FOCUS_OUT) {
        clear_held_input(true);
    }
}

// ------------------------------------------------------------- SimEvents
void WorldSim::on_say(const std::string &who, const std::string &text, const std::string &cls) {
    (void)cls;
    push_subtitle(who, text, cls);
}
void WorldSim::on_log(const std::string &text, const std::string &cls) { (void)text; (void)cls; }

void WorldSim::on_sfx(const std::string &id, float x, float y, float gain, bool local) {
    /* 音效的唯一落点。**逻辑层是在 va::step_once 内部同步调用这里的**，
       所以本函数必须便宜：真正的发声/限流/空间化都在 Audio 里（见 node/audio.h
       顶部那段口径说明），这里只转调 + 一个可选的诊断行。

       历史：这一格原来是一句空实现（注释「音频模块尚未接入」），只做去重节流。
       现在音频层接上了 —— 逻辑层那些 sfx(...) 调用点一行都没动，它们本来就
       按「id + 逻辑坐标 + 增益 + 是否居中」把话说清楚了。 */
    snd_.play(id, x, y, gain, local);

    if (!dbg_sfx_) return;
    /* 每秒一行累计量。**这里为什么要独立于 audio.cpp 的逐条打印**：
       逐条打印在交火高峰一秒能刷几十行，日志就没法当回归判据用了；
       而 `丢弃` 这个数字恰恰只能这样看趋势 —— 它非零 = 逻辑层发了某个
       audio.cpp 的 kSnd 表里没有的 id（改了逻辑层忘了加素材），是这条链路上
       最容易漏的一类 bug，静默失声不会有任何报错。 */
    const double now = (double)Time::get_singleton()->get_ticks_msec() / 1000.0;
    if (now - last_sfx_t_ < 1.0) return;
    last_sfx_t_ = now;
    UtilityFunctions::print(String::utf8("[snd] 累计声源 "), (int64_t)snd_.played(),
                            String::utf8("，未登记丢弃 "), (int64_t)snd_.dropped(),
                            snd_.dropped() > 0
                                ? String::utf8("（") + snd_.unknown_summary() + String::utf8("）")
                                : String());
}
void WorldSim::on_toast(const std::string &text) {
    if (hud_ != nullptr) hud_->ev_toast(text);
}
void WorldSim::on_alert(const std::string &text, float dur) {
    if (hud_ != nullptr) hud_->ev_alert(text, dur);
}
void WorldSim::on_end(const std::string &kind, const std::string &text) {
    /* 结算本身由 HUD 的结算面板呈现（成败、评级、统计）；
       这里只把逻辑层给的这段文案留个记录，方便对照逻辑输出。 */
    UtilityFunctions::print(String::utf8("[结算] "), String::utf8(kind.c_str()), String::utf8("："), String::utf8(text.c_str()));
    /* 音效链路的收尾诊断：载入数 + 实际触发分布 + 未登记丢弃。
       「哪些 id 一次都没响过」比「总共响了多少次」有用得多 ——
       它直接指出哪条 sfx(...) 链路没被走到（而不是合成得不对）。 */
    if (dbg_sfx_) {
        UtilityFunctions::print(String::utf8("[snd] 收尾：载入 "), snd_.loaded(), "/", snd_.attempted(),
                                String::utf8("，累计声源 "), (int64_t)snd_.played(),
                                String::utf8("，未登记丢弃 "), (int64_t)snd_.dropped());
        UtilityFunctions::print(String::utf8("[snd] 触发分布 "), snd_.played_summary());
        if (snd_.dropped() > 0) {
            /* P0 只做了 25 个 id，逻辑层一共会发 35 个 —— 剩下的 10 个
               （ambush/boxDrop/evac/grenadeThrow/incoming/pickup/reinforce/
                 rocketFire/radioTx/radioRx）会落在这一行里。这是**已知缺口**，
               不是故障；P1 批次补上素材与 kSnd 条目后这行自然消失。 */
            UtilityFunctions::print(String::utf8("[snd] 未登记 id（P1 待补）: "), snd_.unknown_summary());
        }
    }
    // 跑动层收尾：这一局到底有多少个单位被真正驱动过。VA_DBG_RUN=1 时打。
    if (std::getenv("VA_DBG_RUN") != nullptr) {
        UtilityFunctions::print(String::utf8("[run] 收尾："), anim_.dump());
        /* 腿摆层一并收尾。**「一条腿都没挂上」和「挂上了但摆角一直是 0」是两种
           完全不同的故障**，而画面上都表现为"腿没摆" —— 这个数就是把它们分开的那一条。 */
        UtilityFunctions::print(String::utf8("[leg] 收尾："), leg_.dump());
    }
    /* 光效层收尾：本局一共画过多少发枪口焰、其中敌方占几发、曳光弹峰值多少。
       **「一个光效都没画」和「画了但都落在镜头外」是两种故障**，
       而画面上都表现为"没看见光" —— 这个数把它们分开。 */
    if (std::getenv("VA_DBG_FX") != nullptr || std::getenv("VA_DBG_RUN") != nullptr) {
        UtilityFunctions::print(String::utf8("[fx] 收尾："), fx_.dump());
    }
    /* 「转进」= 这一关打下来了、还有下一关 —— 它和"成功/胜利/失败"不是一回事：
       战绩全达标但没过关（比如撤离人数不够）也会走 end_game，那种不能推进关卡。
       所以只认 end_game 给出的 kind，不自己看 stats 反推。 */
    camp_cleared_ = (kind == "转进");
    camp_won_     = (kind == "胜利");
}
void WorldSim::on_subs_dirty() {}
void WorldSim::on_objectives_dirty() {}

void WorldSim::push_subtitle(const std::string &who, const std::string &text, const std::string &cls) {
    (void)cls;
    UtilityFunctions::print(String::utf8("[无线电] "), String::utf8(who.c_str()), ": ", String::utf8(text.c_str()));
}

// ------------------------------------------------------------- 对外接口
/* 铺一关：把 camp_level_ 交给逻辑层，队伍从 carry_in_ 进来。
   **为什么要传 carry 而不是让逻辑层自己记着**：逻辑层的 va_campaign 只有"当前这一关"，
   没有"上一关"的概念 —— 跨关的那份花名册是外壳在推进关卡时抓下来、再交回去的。
   这样逻辑层的接口保持成"给我一关 + 给我一队人"，离线工具能从任意一关起跑。 */
/* 只把逻辑世界铺起来，不碰任何节点。
   分成两半的原因是 _ready：那时静态场景还没建（build_scene 要读 W.props），
   而 apply_weather / sync_entity_nodes 又都依赖已经建好的节点。 */
void WorldSim::init_level_world() {
    const va::CarryOver *c = (camp_level_ > 0 && carry_in_.valid) ? &carry_in_ : nullptr;
    va::init_world(new_seed(), camp_level_, c);
    va::W.started = true;
    va::W.deployDone = true;
    mission_started_ = true;
    /* 剧本游标与战役口令的节流时刻都是**每一局**的状态，必须在开新一局时归零。
       漏了 `script_cmd_` 的后果不是"少发一条口令"，而是**重开之后一条都不发**：
       游标停在 8，script_a_step 里那条 i==8 的分支再也不会命中，
       伏击不引爆、车队一路开过去 —— 实测连按 R 重开第一关 5 次，
       5 次都是同一句"车队冲过了西侧出口，伏击失败"。
       （这个 bug 在改动前就在，只是那时没人连着重开。是验"转进"时撞出来的：
        第一局正常、第二局开始全一样，正好说明游标没归零。） */
    script_cmd_ = 0;
    camp_last_dam_ = camp_last_retake_ = camp_last_evac_ = -100.0f;
    /* 一行"现在是哪一关、带了几个人"。**这条日志是判据**：
       战役有没有真的接进游戏，不看代码看这句 —— 只打"第 1 关"说明
       整局还是被当成单关卡在跑（那正是接入前 _ready 裸调 init_world(seed) 的样子）。 */
    UtilityFunctions::print(String::utf8("[战役] 第 "), camp_level_ + 1,
                            String::utf8(" 关 · "), String::utf8(va::cur_level().name),
                            String::utf8("  "), String::utf8(va::cur_level().date),
                            String::utf8("  带队 "), c != nullptr ? (int)c->units.size() : (int)va::CARRY.units.size(),
                            String::utf8(" 人"));
}

void WorldSim::begin_level() {
    init_level_world();
    // 每关的地形参数与掩体配方都不同（谷宽 / 路高 / 岩石与树林的撒法）
    // → 布光与静态几何必须跟着重打，否则第二关会顶着第一关的山谷。
    apply_weather(refs_);
    if (refs_.root != nullptr) rebuild_props(refs_.root, refs_);
    aim_at_road();
    /* 不在这里调 spawn_entity_nodes：那是"增援到齐"时补节点的，
       重复调用会在场景里堆出第二套模型。**换关的人数一定会变**
       （严格继承下只带撤出来的人），sync_entity_nodes 的"数量对不上就整体重建"
       正好覆盖这一条，所以换关只需要 sync 一次。 */
    sync_entity_nodes();
}

/* 结算面板上按 R：过关了就往下走，没打过就重打这一关。 */
void WorldSim::advance_level() {
    if (camp_won_) {
        /* 六关都打完了 → 重新入伍。**必须把 CARRY 清掉**：
           不清的话新战役第一关会带着上一轮最后那几个人进来。 */
        camp_level_ = 0;
        carry_in_ = va::CarryOver{};
        va::CARRY = va::CarryOver{};
    } else if (camp_cleared_) {
        /* 转进：**抓的是 check_end 里 capture_carry() 留下的那份快照** ——
           只有真走到撤离点的人在里面，阵亡的不复活、没抬走的伤员不留。
           这就是"率领队友撤往下一个阵地"的机制实现。 */
        carry_in_ = va::CARRY;
        camp_level_ = std::min(camp_level_ + 1, va::level_count() - 1);
    }
    camp_cleared_ = false;
    camp_won_ = false;
    reset_mission();
}

void WorldSim::start_mission(bool p_skip_deploy) {
    begin_level();
    va::W.deployDone = p_skip_deploy;
}

void WorldSim::reset_mission() {
    mission_started_ = false;
    /* 重打本关时**队伍要退回到"进这一关时"的那份花名册**（carry_in_），
       不是刚才被打烂之后的状态 —— 否则第一次打得好不好会污染重试，
       而且"失败 → 重打"会越打越弱，最后变成无解。
       camp_level_ 不动：reset 是"重来这一关"，转进才改关卡下标。 */
    begin_level();
    /* 光效层也要清空：逻辑层 init_world 里已经 W.fx.clear()、projectiles 也重建了，
       但表现层的池是有状态的（哪些槽在用、曳光弹 mesh 里还留着上一局的线段）。
       不清的话，重开的第一帧会闪出上一局最后那几发弹的曳光。 */
    fx_.reset();
}

Dictionary WorldSim::get_status() const {
    Dictionary d;
    d["t"] = va::W.t;
    d["phase"] = String::utf8(va::W.phaseName.c_str());
    d["weather"] = String::utf8(va::W.weather.c_str());
    d["alive_allies"] = (int)va::alive_allies().size();
    d["enemy_dead"] = va::W.stats.enemyDead;
    d["units"] = (int)va::W.units.size();
    d["vehicles"] = (int)va::W.vehicles.size();
    d["props"] = (int)va::W.props.size();
    return d;
}

String WorldSim::get_diag() const {
    String s = String::utf8("VolunteerArmyPC · Godot 4.5 + C++17 GDExtension");
    s += String::utf8(" · 单体 ") + String::num_int64((int64_t)va::W.units.size());
    s += String::utf8(" · 载具 ") + String::num_int64((int64_t)va::W.vehicles.size());
    s += String::utf8(" · 掩体 ") + String::num_int64((int64_t)va::W.props.size());
    s += String::utf8(" · 路线点 ") + String::num_int64((int64_t)va::CONVOY_WAY.size());
    s += String::utf8(" · ");
    s += anim_.dump();
    return s;
}

} // namespace volunteer_army
