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
    float convoyGap = 145;     // 各车间距
};
extern const LevelCfg CFG;

struct TacPoint { const char *key; const char *name; const char *shortName; float x, y, r; const char *desc; };
extern const TacPoint POINTS[7];       // A~G
const TacPoint *point_of(const std::string &key);

struct EvacPoint { std::string key, name; float x, y; };
extern const EvacPoint EVAC_DEFAULT;
extern const EvacPoint EVAC_ALT;

extern const float ROAD_PATH[][2];
extern const int   ROAD_PATH_N;
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
Prop make_trench(float x, float y, float w, float h);
extern std::vector<Prop> BASE_PROPS;

struct DeployZone { const char *name; float x1, y1, x2, y2; const char *desc; };
extern const DeployZone DEPLOY_ZONES[4];

struct RecommendPos { const char *id; float x, y; };
extern const std::vector<RecommendPos> RECOMMEND;
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
