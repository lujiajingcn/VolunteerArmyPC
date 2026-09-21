#pragma once
// VolunteerArmyPC —— 关卡 / 武器 / 车辆 / 花名册 / 语音指令文法 常量表
// 逐项对应网页版 index.html 458~620、615~913 行，数值一律保持原样。
#include <string>
#include <vector>

#include "sim/va_types.h"

namespace va {

// ------------------------------------------------------------ 世界 / 关卡配置
constexpr float PX_PER_M = 8.0f;

struct LevelCfg {
    float W = 2200, H = 1300;
    float roadTop = 580, roadBot = 720, roadCY = 650;
    float riverX1 = 306, riverX2 = 414;
    float bridgeY1 = 566, bridgeY2 = 734;
    float tIntel = 30;         // 0:00-0:30 情报
    float tDeploy = 120;       // 0:30-2:00 部署
    float convoyIn = 175;      // 2:55 先头车进入地图
    float reinforceAt = 480;   // 8:00 增援
    float missionEnd = 600;    // 10:00 强制结束
    float tailSpeedUp = 250;   // 车队接近西侧出口
    float convoyStopX = 1020;  // 伏击圈停车线
    float convoyGap = 145;     // 各车间距（= 位置真值，生成与停车排队共用）
};
extern LevelCfg CFG;

// ------------------------------------------------------------ 平衡旋钮
/* 玩法平衡的可调项。**默认值 = 当前采用的平衡值**（见下面的实测依据）。
   为什么不写死成常数：平衡是"量出来的"，不是"想出来的" ——
   同一个可执行文件里换一组数值就能重跑一遍 10 种子扫描，
   不必改常数 → 重新编译 → 再等一轮。tools/va_sweep.cpp 就是靠它
   一次跑完「原值 / 削弱车顶机枪 / 提高队友耐久 / 降低撤离门槛」几套对照。
   约束：只动数值，**不动任何判定顺序、不动掩体** —— 动了判定顺序，
   扫描结果就与旧数据失去可比性，等于把基线扔掉。

   2026-09-18 实测（tools/va_sweep.cpp，10 种子 × 标准伏击剧本，620 秒上限）：

     方案                  胜率   全灭   我方伤亡   其中车顶机枪
     原值（网页版数值）     3/10   1 局    71 人       66%
     A 削弱车顶机枪        8/10   1 局    36 人       72%   ← 采用
     B 提高队友耐久        3/10   0 局    56 人       82%
     C 降低撤离门槛        3/10   1 局    71 人       66%
     A+B                  8/10   0 局    26 人       73%

   结论：**根因是车顶机枪**（占我方伤亡 66%，而它只有把车打毁才停火，
   步枪/狙击对它无解）。A 单独一项就把胜率从 3/10 抬到 8/10、伤亡砍掉一半；
   B 只减少死亡不增加胜场（说明失败原因是"被压制到打不完目标"而不是"死光了"）；
   C 完全没有影响 —— 撤离门槛不是瓶颈，能撤的都撤了，撤不了的是全灭。
   故只采用 A：一个根因、一个杠杆，改动越小越容易归因。
   注意 A 之后车顶机枪占比仍是 72% —— 它依然是头号威胁，
   只是强度回到了"能打、要躲、可以压制"的位置，而不是"谁先打爆车"。 */
struct BalanceCfg {
    float veh_mg_dmg = 0.55f;  // 车载机枪伤害倍率（A：-45%）
    float veh_mg_gap = 1.80f;  // 车载机枪射击间隔倍率（A：+80% = 打得更慢）
    float ally_hp    = 1.00f;  // 队友生命上限倍率（不含玩家）—— 方案 B 未采用
    int   evac_need  = 6;      // 撤离人数门槛 —— 方案 C 实测无效，未采用
};
extern BalanceCfg BAL;

struct TacPoint { const char *key; const char *name; const char *shortName; float x, y, r; const char *desc; };
/* 逐关重写（apply_level）。**恒为 7 个点** A~G —— 指令文法里的地点词（LOC_KEYS）、
   AI 里写死的下标（POINTS[2] 是撤离点、POINTS[6] 是西路出口、POINTS[0] 是主阵地）
   全都依赖这个顺序，所以"每关换名字换坐标"可以，"每关换数量"不行。 */
extern std::vector<TacPoint> POINTS;
const TacPoint *point_of(const std::string &key);

struct EvacPoint { std::string key, name; float x, y; };
extern EvacPoint EVAC_DEFAULT;
extern EvacPoint EVAC_ALT;

extern std::vector<Vec2>  ROAD_PATH;   // 由东向西，车队沿它走
extern int                ROAD_PATH_N;
struct WayPt { float x = 0, y = 0; };
extern std::vector<WayPt> CONVOY_WAY;      // 由 build_convoy_way() 填充
void build_convoy_way();

// ------------------------------------------------------------------ 花名册
// 注意：这里刻意用 std::string 而不是 const char* —— 敌军下车时要用
// "e" + 车号 + "_" + 序号 现场拼出 ID，const char* 存不住这种临时串。
struct RosterDef {
    std::string id, name, role, group, weapon;
    bool deputy = false, leader = false;
    std::vector<const char *> aliases;
};
extern const std::vector<RosterDef> ROSTER;
const RosterDef *roster_of(const std::string &id);

struct GroupDef { const char *name; std::vector<const char *> members; };
extern const std::vector<GroupDef> GROUPS;

struct RoleCall { const char *role; std::vector<const char *> ids; };
extern const std::vector<RoleCall> ROLE_CALL;

// ------------------------------------------------------------------ 武器表
const WeaponSpec *weapon_of(const std::string &key);
extern const RocketSpec ROCKET;
extern const ShellSpec  SHELL;

const VehicleSpec *vehicle_of(const std::string &type);

// ------------------------------------------------------------------ 地图物件
Prop make_rock(float x, float y, float r);
Prop make_tree(float x, float y, float r = 15);
Prop make_bush(float x, float y, float r = 17);
Prop make_barrel(float x, float y);
Prop make_wall(float x, float y, float r);
Prop make_dam(float x, float y, float w);      // 内外加山水库大坝（可炸，见 chain_barrel）
Prop make_trench(float x, float y, float w, float h);
extern std::vector<Prop> BASE_PROPS;

struct DeployZone { const char *name; float x1, y1, x2, y2; const char *desc; };
extern std::vector<DeployZone> DEPLOY_ZONES;

struct RecommendPos { const char *id; float x, y; };
extern std::vector<RecommendPos> RECOMMEND;
bool recommend_of(const std::string &id, float &ox, float &oy);

// ------------------------------------------------------- 语音指令文法
enum class ActKind { Combat, Move, Atk, Support, Report, Misc, Special };

struct ActionDef {
    const char *id, *label;
    std::vector<const char *> keys;
    ActKind kind;
    int  pri;
    bool needLoc = false;
    const char *target = nullptr;   // tank/apc/jeep/truck/mg/officer/inf
};
extern const std::vector<ActionDef> ACTIONS;
const ActionDef *action_by_id(const std::string &id);

extern const std::vector<const char *> CS_ALL;
struct CsGroup { const char *key, *value; };
extern const std::vector<CsGroup> CS_GROUP;
struct LocKey { const char *key, *point; };
extern const std::vector<LocKey> LOC_KEYS;
// 绝对方位用角度；相对方位用负哨兵（对应网页版里的 'L'/'R'/'FL'/'FR'）
constexpr float DIR_L = -1, DIR_R = -2, DIR_FL = -3, DIR_FR = -4;
struct DirKey { const char *key; float bearing; };
extern const std::vector<DirKey> DIR_KEYS;
struct TargetNoun { const char *noun, *type; };
extern const std::vector<TargetNoun> TARGET_NOUNS;
const char *target_act_of(const std::string &type);   // target type → action id

// 队友回复（无线电）
struct Replies {
    std::vector<const char *> ok, delay, refuse, panic, down;
};
extern const Replies REPLIES;

// 中文数字归一 + 去标点（对应 normalizeOrder）
std::string normalize_order(const std::string &t);

// 字符相似度（最长公共子序列比率，对应 charSim）
float char_sim(const std::string &a, const std::string &b);

} // namespace va
