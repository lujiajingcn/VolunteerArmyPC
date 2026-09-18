#pragma once
// VolunteerArmyPC —— 逻辑层中枢：世界状态 + 事件出口 + 全部模块函数声明
//
// 移植原则：网页版用「一个全局 World 对象 + 一堆自由函数」，这里保持同构 ——
// 全局单例 va::W 对应 World，函数名按原样直译（findCover / damageUnit / applyOrder…），
// 这样逐行对照网页版即可核对行为，不会因为"重构"引入语义漂移。
//
// 逻辑层不认识 Godot：需要播声音 / 弹字幕 / 出提示的地方，一律通过 SimEvents 回调出去。
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "sim/va_config.h"
#include "sim/va_types.h"

namespace va {

// ------------------------------------------------------------- 事件出口
// 逻辑层要"对外说话"时调用这些；由引擎侧（world_sim.cpp）实现。
struct SimEvents {
    virtual ~SimEvents() = default;
    virtual void on_say(const std::string &who, const std::string &text, const std::string &cls) {}
    virtual void on_log(const std::string &text, const std::string &cls) {}
    virtual void on_sfx(const std::string &id, float x, float y, float gain, bool local) {}
    virtual void on_toast(const std::string &text) {}
    virtual void on_alert(const std::string &text, float dur) {}
    virtual void on_end(const std::string &kind, const std::string &text) {}
    virtual void on_subs_dirty() {}
    virtual void on_objectives_dirty() {}
};
extern SimEvents *EV;

void set_events(SimEvents *e);

// ------------------------------------------------------------- 世界状态
struct WorldState {
    // 时间 / 流程
    float t = 0;
    std::string phase = "INTEL";
    std::string phaseName = "情报";
    float speed = 1;
    bool  paused = false, started = false, over = false;
    std::string overKind, overText;
    uint32_t seed = 0;

    // 实体（deque：push_back 不会让已有指针失效，AI 里大量持有 Unit*/Vehicle*）
    std::deque<Prop> props;
    std::deque<Unit> units;
    std::deque<Vehicle> vehicles;
    std::vector<Projectile> projectiles;
    std::vector<FxItem> fx;
    std::vector<Mine> mines;
    std::vector<Decal> decals;
    std::vector<Smoke> smokes;
    std::vector<Subtitle> subs;
    std::vector<LogEntry> logs;

    // 玩家 / 标记 / 撤离
    Unit *player = nullptr;
    bool  hasMarker = false;
    Vec2  marker{};
    EvacPoint evac = EVAC_DEFAULT;
    bool  evacArmed = false;
    bool  bridgeAlive = true;
    bool  hasBox = false;
    BoxItem box;

    // 车队
    bool  convoyStarted = false, convoyEscaped = false;
    float convoyProgress = 0;
    std::string boxWhere;                 // truck / apc / officer
    std::vector<std::string> convoyOrder;
    int   infantryTotal = 0;
    Vehicle *officerVeh = nullptr;
    bool  officerSpawned = false;
    Mine *mineEast = nullptr, *mineWest = nullptr;
    bool  mineWarned = false;

    // 伏击 / 增援 / 炮击
    bool  triggered = false;
    float triggerT = 0;
    bool  reinforceDone = false;
    float reinforceT = 0;
    bool  barrage = false;
    float barrageCd = 0;
    struct PendingShell { float x = 0, y = 0, t = 0; };
    std::vector<PendingShell> pendingShells;

    // 指挥
    ParsedCmd lastCmd;                    // 上一条成功下发的指令（供"重复"用）
    bool  hasLastCmd = false;
    Unit *handoverUnit = nullptr;
    bool  rangedSel = false;

    // 天气 / 噪声
    std::string weather = "sunny";
    float noise = 0, noiseT = 0, alertT = 0;

    // 部署
    bool  deployDone = false;

    // 统计
    MissionStats stats;

    // 结果：目标清单（HUD 用）
    struct Objective { std::string text, extra; bool done = false, main = false; };
    std::vector<Objective> objState;

    // 结算
    struct Score { std::string g = "失败"; int cas = 0, dead = 0; bool win = false; };
    Score score;

    // 渲染层每帧写入（逻辑层只读，用于折算射程）
    float viewPitch = 0;
    Vec2  cam{};
};

extern WorldState W;
extern Rng RNG;

// ------------------------------------------------------------- 玩家输入
// 网页版直接把键盘事件塞进 Input.key['w'] 这样的字典；C++ 侧改成一个明确的结构体，
// 由引擎层（world_sim）每帧写入，逻辑层只读 —— 这样逻辑层依然零引擎依赖。
struct PlayerInput {
    bool w = false, a = false, s = false, d = false;
    bool shift = false, ctrl = false;
    bool fire = false, firePressed = false, ads = false;
    bool reload = false, grenade = false, smoke = false, rocket = false;
    bool hasMouseWorld = false;
    float mouseWorldX = 0, mouseWorldY = 0;
    bool fpsAim = true;         // 第一人称下朝向由视角驱动（对应 JS 的 Input.fpsAim）
    bool markerSet = false;     // 本帧请求设置标记点
};
extern PlayerInput IN;

// 便捷随机（对应网页版 rr / ri / pick）
inline float rr(float a, float b) { return RNG.range(a, b); }
inline int   ri(int a, int b) { return RNG.irange(a, b); }
template <class T> const T &pick_one(const std::vector<T> &v) { return v[(size_t)(RNG.next() * (float)v.size()) % v.size()]; }
template <class T> const T &pick_one(const std::deque<T> &v) { return v[(size_t)(RNG.next() * (float)v.size()) % v.size()]; }
const char *pick_cstr(const std::vector<const char *> &v);

// --------------------------------------------------------------- 查询
std::vector<Unit *> allies();
std::vector<Unit *> alive_allies();
std::vector<Unit *> combat_allies();
std::vector<Unit *> enemies();
std::vector<Vehicle *> enemy_vehicles();
std::vector<Unit *> downed_allies();
Unit *unit_by_id(const std::string &id);
Unit *ally_by_id(const std::string &id);      // 只在我方里找（对应 JS 的 find(x => x.id===id && team==='ally')）

Unit *nearest_enemy_at(float x, float y, Team team, const std::function<bool(Unit *)> &filter = {});
// 含载具的「最近威胁距离」（calcObey 用；原版那处 filter 分支会让载具胜出，此为其等价实现）
float enemy_threat_dist(float x, float y, Team team);

// --------------------------------------------------------------- 对外消息
void say(const std::string &who, const std::string &text, const std::string &cls = "");
void logline(const std::string &text, const std::string &cls = "");
FxItem &fx_push(FxItem item);
void decal_push(float x, float y, float r, float cr, float cg, float cb, float ca);

// --------------------------------------------------------------- 地形 / 视线
bool  in_river(float x, float y);
bool  on_bridge(float x, float y);
bool  passable(float x, float y, bool isVehicle = false);
bool  vehicle_blocked(float x, float y, float r, const Vehicle &v);
bool  prop_blocked(float x, float y, float r);
bool  los_core(float x1, float y1, float x2, float y2, bool hard, bool soft, bool smoke);
bool  los_fire(float x1, float y1, float x2, float y2);
bool  los_sight(float x1, float y1, float x2, float y2);
float cover_score(float x, float y, float tx, float ty);
bool  find_cover(float fromX, float fromY, float tx, float ty, float maxDist, float &ox, float &oy);
float cover_protect(const Unit &u);
float vis_range(const Unit &u);
bool  can_see(const Unit &u, float tx, float ty);        // 对应 JS canSee(u, {x,y})
bool  can_see_t(const Unit &u, const Target &t);         // 对应 JS canSee(u, tgt)
inline float dist_to(float x, float y, const Target &t) { return distf(x, y, t.tx(), t.ty()); }
inline float dist2_to(float x, float y, const Target &t) { return dist2f(x, y, t.tx(), t.ty()); }

// --------------------------------------------------------------- 单位 / 车辆
Unit *make_unit(const RosterDef &def, float x, float y, Team team, bool isPlayer);
void  init_world(uint32_t seed);
void  build_convoy();
bool  convoy_pos(const Vehicle &v, float progress, float &ox, float &oy);
bool  way_point_at(float d, float &ox, float &oy, float &oa);
void  update_evac_marker();

// --------------------------------------------------------------- 伤害 / 死亡
// srcVeh：伤害是否来自车载武器（车载机枪的弹丸不挂 owner，只挂 ownerVeh）。
// 只为平衡扫描的伤亡归因服务，传 nullptr 与旧行为完全一致。
void  damage_unit(Unit *u, float dmg, Unit *src, const std::string &kind,
                  const VehicleSpec *srcVeh = nullptr);
void  kill_unit(Unit *u, Unit *src);
void  down_player(Unit *u);
void  handover();
void  on_ally_lost();
void  damage_vehicle(Vehicle *v, float dmg, const std::string &kind, Unit *byUnit);
void  destroy_vehicle(Vehicle *v, Unit *byUnit);
void  drop_box(float x, float y);
void  explosion(float x, float y, float r, float dmg, Team team, const std::string &kind,
                float vehMul = -1.0f, const std::string &vehKind = "");
void  chain_barrel(Prop &p);

// --------------------------------------------------------------- 弹道
void  spawn_bullet(Unit *u, float tx, float ty, float spreadMul, float rangeMul = 1.0f);
void  spawn_rocket(Unit *u, float tx, float ty);
void  throw_grenade(Unit *u, float tx, float ty, const std::string &kind);
void  spawn_shell(Vehicle *v, float tx, float ty);
void  spawn_tank_mg(Vehicle *v, float tx, float ty);
void  update_projectiles(float dt);
bool  vehicle_hit(const Vehicle &v, float x, float y);

// --------------------------------------------------------------- 战斗判定
void  set_state(Unit *u, const std::string &s);
void  perceive(Unit *u);
Target ally_target_select(Unit *u);
bool  ineffective_target(const Unit &u, const Target &t);
bool  try_reload(Unit *u);
void  finish_reload(Unit *u);
float spread_mul(const Unit &u);
void  fire_at(Unit *u, const Target &tgt, float dt);
bool  ordered_to_fire(const Unit &u);
bool  at_may_use_rocket(const Unit &u, const Target &tgt, float d);
void  throw_grenade_auto(Unit *u, const Target &tgt);

// --------------------------------------------------------------- 指令
void  issue_command(const ParsedCmd &cmd, bool silent = false);
ParsedCmd parse_command(const std::string &text, float noise = 0, float asr = -1, bool typed = false,
                        const std::string &source = "voice");
std::vector<ParsedCmd> parse_candidates(const std::string &text, const ParsedCmd &cmd);
// 便捷入口：解析文本并下发，返回解析结果（供 HUD / 语音层调用）
ParsedCmd run_command_text(const std::string &text, const std::string &source = "voice", bool typed = true);

float calc_obey(const Unit &u, const std::string &actId);
std::vector<Unit *> resolve_targets(const ParsedCmd &cmd);
Unit *nearest_ally_to(const Unit &p);
Unit *nearest_downed(Unit *u);
Target nearest_enemy_to_marker(Unit *u);
Target pick_target_by_type(Unit *u, const std::string &type, float dirBearing, bool hasDir);
void  apply_order(Unit *u, const ParsedCmd &cmd);
void  detonate_mines(Unit *by);
struct Precheck { bool ok = true; std::string msg; };
Precheck precheck(Unit *u, const ParsedCmd &cmd);
std::string report_line(Unit *u, const std::string &id);
std::string bearing_name(float fx, float fy, float tx, float ty);

// --------------------------------------------------------------- AI
void  update_ally(Unit *u, float dt);
void  update_enemy(Unit *u, float dt);
void  update_player(float dt);
void  update_vehicles(float dt);
void  move_step(Unit *u, float dt, float speedMul = 1.0f);
float steer_angle(Unit *u, float tx, float ty);
bool  nearest_cover_spot_near(const Unit &p, const Unit &u, float &ox, float &oy);
void  ally_centroid(float &ox, float &oy);
void  dismount(Vehicle *v);
void  spawn_reinforcement();

// --------------------------------------------------------------- 流程
void  step_once(float dt);
void  trigger_ambush(const std::string &src);
void  update_mines(float dt);
void  update_barrage(float dt);
void  update_pending_shells(float dt);
void  check_objectives();
void  check_end();
void  update_phase_name();
void  update_flow(float dt);
void  end_game(const std::string &kind, const std::string &text);
std::string score_mission();

// 外部依赖：音频 / UI（由 world_sim 实现，逻辑层不关心细节）
void sfx(const std::string &id, float x, float y, float gain = -1.0f, bool local = false);
void toast(const std::string &text);
void set_alert(const std::string &text, float dur);

} // namespace va
