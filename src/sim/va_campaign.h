#pragma once
// VolunteerArmyPC —— 铁原战役层：六关战役 / 逐关目标 / 队伍跨关继承
//
// 【为什么要有这一层】
// 原来的关卡是「一张硬编码地图 + 一套硬编码目标」：地图常量（战术点、掩体、公路、
// 撤离点）直接写在 va_config.cpp，目标写死在 va_flow.cpp 的 check_objectives()。
// 铁原阻击战的性质不一样 —— 它是**逐次抵抗**：同一支部队在 13 天里从涟川一路
// 打到铁原以北，**换的是阵地不是人**。所以必须让「关卡」变成数据，
// 让「队伍」能跨关带走。
//
// 【三件事由这一层负责】
//   1. 关卡表 LEVELS[]：每关的地形主题、时间轴、敌军编成、目标清单、史实文案；
//   2. 按关卡铺图：战术点 / 掩体 / 部署区 / 撤离点 / 公路 由主题 + 关卡尺寸生成
//      （手写 6 份掩体表既笨又不可复现，程序生成才有"同一 seed 同一张图"）；
//   3. 跨关继承：撤离成功的人带着血量/弹药/士气走下一关，阵亡的不复活。
//
// 【不负责什么】
// 判定顺序、掩体评分、伤害公式一律不动 —— 本层只提供"这一关的数据与目标"，
// 战斗本身还是原来那一套。改判定顺序会跟旧基线失去可比性（见 MEMORY 红线）。
//
// 逻辑层零引擎依赖：本文件不 include 任何 godot 头。
#include <string>
#include <vector>

#include "sim/va_config.h"

namespace va {

// ------------------------------------------------------------------ 目标
/* 目标种类。逐关按史实设目标，所以不能沿用"坦克 + 密码箱 + 撤离 6 人"那一套
   固定三件 —— 加齿项是"拖到点就撤"（不要求歼灭），内外加山是"炸坝拦住装甲
   集群"，种子山是"白天丢了夜里夺回来"。每一种在这里有一个判据。 */
enum class GoalKind {
    Hold,         // 守住阵地 N 秒（从交火起算）—— 涟川山口、207 高地
    Delay,        // 拖到本关第 N 秒（活到点就算达成）—— 加齿项、内外加山
    DestroyKind,  // 摧毁 N 辆某型车 —— 各关的"打掉坦克/打掉先头车"
    DestroyAny,   // 摧毁任意 N 辆车 —— 给"打不掉坦克"留的替代路径（见 L2 注释）
    BlowDam,      // 炸开水坝 —— 内外加山「水淹七军」
    Retake,       // 夜袭夺回阵地 —— 种子山
    ExtractItem,  // 带出指定物件（电台密码本 / 团部文件 / 伤员名册…）
    Evac,         // 撤离 N 人
};

struct LevelGoal {
    GoalKind     kind  = GoalKind::Evac;
    const char  *text  = "";      // HUD 目标清单里的文案
    bool         main  = true;    // 主目标必须全达成才能过关
    float        need  = 0;       // Hold/Delay=秒，DestroyKind/Evac=数量
    const char  *vtype = "";      // DestroyKind 的车型：jeep / apc / tank / truck
};

// ------------------------------------------------------------------ 地形主题
/* 主题决定"这一关长什么样"，也决定掩体的生成配方：
   Gorge     峡谷公路（断头谷的原型，两侧缓坡）
   Pass      山口（涟川山口：正面不足 3 公里，两侧陡岩，岩石多树少）
   Ridges    高地群（种子山 / 233.2 / 加齿项：几座高地 + 相连鞍部）
   LoneHill  孤山（207 高地、内外加山：平原上一座小山，掩体稀疏）
   Reservoir 水库（内外加山：南侧一片水，大坝横在谷口） */
enum class TerrainTheme { Gorge, Pass, Ridges, LoneHill, Reservoir };

struct LevelDef {
    // ---- 文案 ----
    const char *id       = "";    // 关卡键：l1_yunv / l2_lianchuan / …
    const char *name     = "";    // "第二关 · 涟川山口"
    const char *date     = "";    // "1951.6.1"
    const char *place    = "";    // "涟川山口 · 第一道防线"
    const char *ourUnit  = "";    // 我方番号
    const char *enemyUnit = "";   // 当面之敌
    const char *brief    = "";    // 简报正文（进关前给玩家看）
    const char *history  = "";    // 史实注（结算面板一行）

    // ---- 地形 ----
    TerrainTheme theme   = TerrainTheme::Gorge;
    float mapW = 2200, mapH = 1300;
    float roadCY = 650, roadHalf = 70;          // 公路中心线 y 与半宽
    float hIn = 4.5f, hOut = 18.0f, bump = 0.5f; // 谷壁高度（表现层 ground_h 用）
    const char *pointNames[7] = { "", "", "", "", "", "", "" };  // A~G 的阵地名

    // ---- 时间轴（秒）----
    float tIntel     = 30;    // 情报阶段
    float tDeploy    = 120;   // 部署阶段
    float convoyIn   = 175;   // 敌人进入地图
    float missionEnd = 420;   // 本关时限（同时也是 Delay 类目标的判据）
    float reinforceAt = 480;  // 增援到达

    // ---- 关卡机制开关 ----
    bool forceNight = false;   // 种子山：整关夜间（史实是凌晨夜袭）
    bool hasDam     = false;   // 内外加山：地图上有可炸的水坝
    bool earlyFlank = false;   // 207 高地：侧翼迂回来得早（史实是 6/6 敌分两路迂回）
    bool boxAlways  = true;    // 是否随机"物件在哪"（卡车 / 装甲车 / 军官身上）

    // ---- 敌军 ----
    int  infantryMin = 10, infantryMax = 16;
    std::vector<std::vector<std::string>> convoyOrders;  // 候选编成，开局抽一个

    // ---- 目标 ----
    std::vector<LevelGoal> goals;
    int   evacNeed = 6;         // 撤离门槛（史实：越往后人越少，所以逐关下调）
    const char *itemName = "密码本";   // ExtractItem 的物件名
    /* 集结窗口（秒）：撤离门槛达标之后，再给队伍这么久把能走的人都带到撤离点。
       设 0 = 数够人头立刻收关（旧行为）。**逐关不同**：第一关敌人是搜索队、
       压力小，可以多等；越往后炮火越密，等越久死得越多，窗口要收短。 */
    float evacWindow = 20.0f;
};

// ------------------------------------------------------------------ 战役状态
/* 跨关继承的单位快照。**只带"活着撤出去的人"** —— 阵亡的不复活、倒地没被救走的
   算遗留（这正好把"救回所有伤员"这条加分项变成真的代价：你扔下的人下关就没有了）。
   为什么带弹药而不是每关补满：铁原打到后面弹药是真的不够（史料里"子弹、手榴弹
   优先供给前沿狙击小组，反坦克手雷严格管控"），把它做成机制比做成文案更有说服力。 */
struct CarryUnit {
    std::string id;
    float hp = 100, maxHp = 100, morale = 78;
    int   ammo = 0, magAmmo = 0, rockets = 0, grenades = 0, smokes = 0;
    bool  downed = false;   // 被抬下来的伤员：下一关开局仍处于失能状态
    int   kills = 0, shots = 0, hits = 0;
};

struct CarryOver {
    bool valid = false;                 // false = 第一关，按花名册新建
    std::vector<CarryUnit> units;
};

struct CampaignState {
    /* 是否走战役流程。**为什么不直接用 levelIndex**：tools/va_sweep 这类入口是
       init_world(seed) 一把梭，它要的是"原来那一关"的行为（时间轴、目标、撤离
       门槛都按旧值走），否则平衡扫描的基线会被战役改动整体平移，
       新扫出来的胜率跟旧数据再也比不了。只有真正进关（levelIdx >= 0）才置 true。 */
    bool  active = false;
    int   levelIndex = 0;               // 当前关卡下标
    int   cleared = 0;                  // 已过关数
    int   totalDead = 0;                // 战役累计阵亡
    int   totalEvac = 0;                // 战役累计撤离人次
    float totalTime = 0;                // 战役累计用时（秒）
    /* 集结窗口的开门时刻（-1 = 还没开门）。
       为什么需要这个：撤离门槛一达标就立刻收关，等于把**已经活着、还在往撤离点跑的人**
       判成"没带出来" —— 而严格继承之下，那些人正是下一关的步枪。
       实测第一关：撤 7 人过关，队伍 11 人里 10 个还活着，却只带走 8 个
       （7 + 玩家），第二关一开局就只剩 8 人，对着美骑 1 师直接崩掉（5 亡）。
       史实里逐次抵抗也是"收拢队伍、带上伤员再走"，不是数够人头就关灯。
       所以门槛达标后**再开一段窗口**，让能走的都走到；窗口长度按关卡给。 */
    float evacOpenT = -1.0f;
    std::vector<std::string> log;       // 逐关一行战报
};

extern const std::vector<LevelDef> LEVELS;
extern CampaignState CAM;
extern CarryOver     CARRY;

int  level_count();
const LevelDef &cur_level();
const LevelDef &level_at(int idx);
bool has_next_level();
int  next_level_index();

// 把 idx 这一关铺进全局关卡容器（CFG / POINTS / BASE_PROPS / DEPLOY_ZONES /
// EVAC / RECOMMEND / ROAD_PATH）。seed 只用于掩体抖动的抽取。
void apply_level(int idx, uint32_t seed);

// 目标判定与进度文案（va_flow 的 check_objectives 调这两个）
bool goal_done(const LevelGoal &g);
std::string goal_progress(const LevelGoal &g);
// 本关的撤离门槛。战役里逐关下调（人越打越少），非战役走平衡旋钮 BAL.evac_need。
int  effective_evac_need();

// 逐帧推进本关特有的机制（夜袭夺回的计时、洪水…）
void update_level_goals(float dt);

// 关卡结束时的收尾：统计 + 记录战报 + 生成下一关要带的队伍
void capture_carry();
void commit_level_result(bool win);

// 关卡特有机制的状态（放在 W 外面，避免给 WorldState 加一堆字段）
struct LevelRuntime {
    bool  damBlown = false;    // 水坝已炸
    /* **是被安放炸药炸的，还是被别的爆炸波及的**。
       为什么要分开记：坝体 150 宽、就落在侧射阵位附近，而 explosion() 的
       判定是 r+8 —— 如果流弹也能炸开，那"跑到坝上、自断退路"这个史实两难
       就会被一次偶然的汽油桶连锁悄悄替玩家完成，而画面上完全看不出来
       （无人派工、无人安放，目标却打勾了）。这个字段就是判据。 */
    bool  damByCharge = false;
    float damX = 0, damY = 0, damR = 75.0f;   // 坝体中心与半径（到达判定要算上它）
    float floodX1 = 0, floodY1 = 0, floodX2 = 0, floodY2 = 0;   // 洪水区（炸坝后生效）
    bool  retakeArmed = false; // 进入夜袭阶段
    bool  retaken = false;     // 已夺回阵地
    float retakeHold = 0;      // 在阵地上站稳的累计秒数
    float holdT = 0;           // Hold 类目标的累计秒数
    /* 主阵地（A 点附近）上的敌我人数 —— HUD 的"阵地 3 比 5"和排查共用。
       "守住/夺回"这类目标达不成时，第一个要看的就是这两个数：
       是我方根本没上去，还是上去了但被压着。 */
    int   holdAlly = 0, holdEnemy = 0;
    /* 炸坝进度（HUD 的"安放炸药 x/8"与故障排查共用）：
       带 damTask 的人数 + 安放进度。**进度必须能被外部读到** ——
       "为什么坝没炸开"这个问题，靠看图永远答不出来（人可能死在半路、
       也可能压根没被派出去），只有这两个数能区分。 */
    int   damWorkers = 0;
    float damPlantT = 0;
    /* 本关开局时我方的人数。撤离门槛按它算 ——
       见 effective_evac_need()：人越打越少，门槛必须跟着降，
       写死一个绝对值的话第二关就变成"只能损失两个人"，那不是难度是赌博。 */
    int   startCount = 0;
};
extern LevelRuntime LV;
void reset_level_runtime();

// 洪水对车辆速度的影响（update_vehicles 里乘）。返回 1 = 无影响。
float flood_speed_mul(float x, float y);

} // namespace va
