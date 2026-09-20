#pragma once
// VolunteerArmyPC —— 逻辑层数据结构（对应网页版 World / makeUnit / buildConvoy 等）
#include <deque>
#include <string>
#include <vector>
#include <cstdint>

#include "sim/va_math.h"

namespace va {

// ------------------------------------------------------------------ 基础
struct Vec2 { float x = 0, y = 0; };

// 武器规格（对应 WEAPONS 表）
struct WeaponSpec {
    const char *name = "";
    float dmg = 13, range = 720, rof = 0.12f;
    int   burst = 3;
    float burstGap = 0.34f, spread = 0.045f;
    int   mag = 30, ammo = 240;
    float reload = 2.2f, pen = 0.12f, sup = 7, speed = 950;
};

struct RocketSpec { const char *name; float speed, dmgVeh, dmgInf, splash, rof; int ammo; float reload, range; };
struct ShellSpec  { const char *name; float speed, dmg, splash, rof, warn; };
struct VehicleSpec {
    const char *name;
    float hp, len, wid, speed, armor;
    int   crew, cap;
    bool  has_mg = false;
    float mg_dmg = 0, mg_range = 0, mg_rof = 0, mg_spread = 0, mg_sup = 0;
    bool  cannon = false;
    /* 殉爆半径。**0 = 按 len × 0.9 自动算**（历史行为）。
       单独列出来的原因：`len` 原先身兼三职 —— 渲染尺寸、碰撞/遮挡盒、**殉爆半径**。
       前两个是"车多大"，第三个是"打爆它伤多大范围"，是**玩法量**、跟车的美术尺寸
       没有必然关系。2026-09-20 把 len 改成实车尺寸时，殉爆半径跟着悄悄放大了
       1.5~2.1 倍（坦克 2.1→2.9 m 半径仍在 160 伤害），10 种子胜率因此掉 1 胜。
       让玩法量跟着美术尺寸漂移就是"一个数干两件事"，所以钉成显式字段。 */
    float blast_r = 0.0f;
};

// ------------------------------------------------------------------ 地形物件
enum class PropType { Rock, Tree, Bush, Barrel, Wall, Trench };

struct Prop {
    PropType type = PropType::Rock;
    float x = 0, y = 0, r = 0;
    float cover = 1.0f;
    bool  blocksLos = true;
    bool  blocksBullet = true;
    bool  hard = false;
    bool  soft = false;
    bool  low = false;
    bool  explosive = false;
    bool  destroyed = false;
    float hp = 0;
    float w = 0, h = 0;   // trench 用
};

// ------------------------------------------------------------------ 战斗体
// None = 中立爆炸（炸桥 / 油桶殉爆）：对应网页版传 null team，
// 此时 `u.team === team` 对任何单位都不成立 —— 也就是**敌我不分、范围内通吃**。
// 若用 Ally 代替会让队友免疫，行为就变了。
enum class Team { Ally, Enemy, None };
enum class ProjKind { Bullet, Rocket, Grenade, SmokeG, Shell, TankMG };

struct Unit;
struct Vehicle;

// ------------------------------------------------------------- 目标句柄
// 网页版里 perceive / allyTargetSelect / pickTargetByType / fireAt 处理的目标
// **既可能是步兵（Unit）也可能是载具（Vehicle）** —— JS 靠鸭子类型统一读写
// `t.x / t.y / t.dead / t.destroyed / t.name / t.type / t.spec`。
// C++ 里用这个双指针句柄表达同一语义，逐行对照时 `t.x` 对应 `t.tx()`。
// gone() 对应 JS 里的 `!t.dead && !t.destroyed`（步兵的 downed 也算「不可打」）。
//
// 注意：解引用 Unit/Vehicle 的访问器只能在两个结构体**定义之后**再实现，
// 所以这里只声明，实现在文件末尾。
struct Target {
    Unit    *u = nullptr;
    Vehicle *v = nullptr;

    Target() = default;
    Target(Unit *p) : u(p) {}        // NOLINT(*-explicit-constructor) —— 刻意保留隐式转换
    Target(Vehicle *p) : v(p) {}     // NOLINT(*-explicit-constructor)

    bool ok() const { return u != nullptr || v != nullptr; }
    explicit operator bool() const { return ok(); }
    bool veh() const { return v != nullptr; }
    uint32_t key() const { return (uint32_t)(uintptr_t)u ^ ((uint32_t)(uintptr_t)v << 1); }
    bool same(const Target &o) const { return u == o.u && v == o.v; }

    float tx() const;
    float ty() const;
    bool  gone() const;          // 步兵 dead||downed / 载具 destroyed
    bool  dead_only() const;     // 只判 dead / destroyed
    bool  is_player() const;
    const VehicleSpec *spec() const;
    const std::string &tname() const;
    const std::string &ttype() const;   // 步兵恒为空串（对应 JS 的 undefined）
};

// ----------------------------------------------------- 解析后的语音指令
// 对应网页版 Parser.parse() 的返回值 cmd。
struct ParsedCmd {
    bool  ok = false;
    std::string raw;
    float confidence = 0;
    std::vector<std::string> notes;

    // 呼号
    std::string csKind = "member", csId = "auto", csLabel = "默认（最近队友）";

    // 动作
    std::string actId, actLabel, typeTarget;
    bool  needLoc = false;

    // 地点
    bool  hasLoc = false;
    std::string locKey, locName;
    float locX = 0, locY = 0;

    // 方向
    bool  hasDir = false;
    float bearing = 0;
    std::string dirRaw;

    std::vector<std::string> mentioned;
    std::string source = "voice";
    float noise = 0;
    bool  hasMarker = false;
    float markerX = 0, markerY = 0;

    bool valid() const { return ok && !actId.empty(); }
};

// 指令对象（对应 applyOrder 里的 u.order）
struct Order {
    std::string actId, actLabel;
    std::string csKind, csId, csLabel;
    float x = 0, y = 0;
    bool  hasPoint = false;
    float bearing = 0;
    bool  hasBearing = false;
    std::string targetType;              // tank/apc/jeep/truck/mg/officer/inf
    std::vector<std::string> mentioned;
    float t = 0;
    bool  active = false;

    static Order from(const ParsedCmd &c, float now, const std::string &targetTypeOverride = std::string()) {
        Order o;
        o.actId = c.actId; o.actLabel = c.actLabel;
        o.csKind = c.csKind; o.csId = c.csId; o.csLabel = c.csLabel;
        o.x = c.locX; o.y = c.locY; o.hasPoint = c.hasLoc;
        o.bearing = c.bearing; o.hasBearing = c.hasDir;
        o.targetType = targetTypeOverride.empty() ? c.typeTarget : targetTypeOverride;
        o.mentioned = c.mentioned;
        o.t = now; o.active = true;
        return o;
    }
};

struct Unit {
    // 身份
    std::string id, name, role, group;
    bool isPlayer = false;
    Team team = Team::Ally;

    // 运动
    float x = 0, y = 0, vx = 0, vy = 0, facing = 0;
    float radius = 5.2f;
    bool  hasDest = false; Vec2 dest{};
    bool  hasMoveGoal = false; Vec2 moveGoal{};
    bool  hasCoverPos = false; Vec2 coverPos{};
    bool  hasPushPt = false; Vec2 pushPt{};
    bool  hasFlankPt = false; Vec2 flankPt{};
    bool  hasPatrol = false; Vec2 patrol{};
    bool  hasDragGoal = false; Vec2 dragGoal{};
    bool  moving = false, aiming = false, arrived = false;

    // 生命
    float hp = 100, maxHp = 100, morale = 78, suppression = 0, armor = 0;
    bool  downed = false, dead = false;
    float downTimer = 0, reviveT = 0, hitFlash = 0, stun = 0;

    // 武器
    std::string weaponKey;
    const WeaponSpec *wpn = nullptr;
    int   ammo = 0, magAmmo = 0;
    float reloadT = 0, fireCd = 0, burstCd = 0, aimT = 0;
    int   burstLeft = 0;
    int   rockets = 0, grenades = 0, smokes = 0;
    float grenadeCd = 0;
    bool  hasCharge = false;

    // 战术
    std::string state = "部署";
    float stateT = 0, thinkT = 0, aiT = 0, pickT = 0;
    Target target, focusTarget, atTarget, seen;
    float seenD = 0;
    float lastSeenT = -1000.0f, lastSeenX = 0, lastSeenY = 0;
    std::string preferType;
    std::string fireMode = "free";
    bool  holdPosition = false;
    Order order;
    ParsedCmd pendingCmd; bool hasPendingCmd = false; float pendingT = 0;
    float obeyBoost = 0, obeyBase = 0.8f, obeyRoll = 0;
    bool  formationFollow = false;
    Unit *followTarget = nullptr;
    std::string flankRole; int flankSide = 1;
    Vehicle *mount = nullptr;
    bool  officer = false, mg = false;
    float pushT = 0, holdT = 0, suppressFireT = 0;
    bool  moraleCheck = false;
    bool  noAutoFire = false, hideFire = false;
    Unit *rescueTarget = nullptr;
    std::string rescueMode;
    Unit *healTarget = nullptr;
    Unit *resupplyTarget = nullptr;
    bool  grenadeOrder = false, smokeOrder = false;
    bool  bridgeTask = false; float plantT = 0;
    Unit *dragging = nullptr;
    Unit *draggedBy = nullptr;
    bool  carryBox = false;
    float secReport = 0, speakCd = 0, reportCd = 0;
    Unit *lastHurtBy = nullptr;
    float lastHurtT = -1000.0f;
    /* 最后打中我的是哪一型车载武器（步兵子弹 / 爆炸为 nullptr）。
       为什么要单独一个字段而不是复用 lastHurtBy：车载机枪的弹丸不挂 owner（挂的是
       ownerVeh），所以 lastHurtBy 恒为 nullptr —— 于是"我方伤亡里有多少是车顶机枪
       打出来的"这个问题按现有字段根本答不出来，而它恰恰是平衡调整要盯的那个数。
       存 spec 而不是 Vehicle*：spec 指向静态常量表，不存在悬垂。 */
    const VehicleSpec *lastHurtVehSpec = nullptr;
    bool  evacuated = false;       // 已抵达撤离点并计数
    bool  boxTask = false;         // 被指派去捡密码箱
    bool  hasSpec = false;         // 占位：网页版用 u.spec 区分载具，步兵恒 false

    // 统计 / 表现
    int   kills = 0, shots = 0, hits = 0;
    float wobble = 0;

    // 部署
    bool  hasHome = false; Vec2 homePos{};
};

struct Vehicle {
    std::string id, name, type;
    Team team = Team::Enemy;
    float x = 0, y = 0, angle = 0, speed = 0, baseSpeed = 0, dist = 0;
    float hp = 0, maxHp = 0;
    const VehicleSpec *spec = nullptr;
    float len = 0, wid = 0, armor = 0;
    bool  destroyed = false;
    float burning = 0, turret = 0, fireCd = 0, wpnCd = 0;
    int   capacity = 0;
    std::vector<Unit *> troops;
    float dismountT = -1;
    bool  dismounted = false;
    std::string state = "drive";
    Target target;
    int   line = 0;
    float speedMul = 1;
    int   wayI = 0;
    bool  hasBox = false;
    float hitFlash = 0, stopT = 0, mgHeat = 0, report = 0;
    int   troopPlan = 0;

    // 运行态（对应网页版里在战局中途才被写入的那些字段）
    float px = 0, py = 0;          // 上一帧位置（渲染插值/残骸留痕用）
    float blockedT = 0;            // 被前方残骸堵住的累计时长
    bool  atZone = false;          // 已抵达伏击圈停车线
    bool  atBridge = false;        // 因桥被炸而停下
    bool  isReinforcement = false; // 增援车队
    bool  chaosSaid = false;
    float fireFxT = 0;             // 燃烧特效节流
    float aimT = 0;                // 坦克主炮瞄准倒计时
    bool  hasAimPt = false;
    Vec2  aimPt{};
    float lastFireCd = 0;
};

struct Projectile {
    ProjKind kind = ProjKind::Bullet;
    float x = 0, y = 0, vx = 0, vy = 0, speed = 0, life = 0, t = 0;
    float dmg = 0, pen = 0, sup = 0, splash = 0, dmgInf = 0;
    float angle = 0;
    // 投掷物（手雷/烟雾弹）的落点 —— 对应网页版 p.tx / p.ty
    float tx = 0, ty = 0;
    Team team = Team::Ally;
    Unit *owner = nullptr;
    Vehicle *ownerVeh = nullptr;   // 车载武器发射时记录（JS 里 owner 是多态的）
    Target ownerT;                 // 统一访问入口
    bool  fromPlayer = false;
    float rangeMul = 1.0f;
    bool  cracked = false;         // 掠弹音只响一次
    std::string src;               // 'rifle' / 'mine' / ...
    bool  alive = true;
};

struct FxItem {
    std::string type;    // 'boom' / 'spark' / 'smoke' ...
    float x = 0, y = 0, r = 0, t = 0, life = 0.5f;
    float vx = 0, vy = 0;
    float a = 0, b = 0, c = 0;
};

struct Decal { float x = 0, y = 0, r = 0; float cr = 0, cg = 0, cb = 0, ca = 1; float t = 0; };
struct Mine  { float x = 0, y = 0; bool armed = false, used = false; Team team = Team::Ally; std::string kind; };
struct Smoke { float x = 0, y = 0, r = 0, t = 0, life = 0; };
struct Subtitle { std::string who, text, cls; float t = 6.5f; };
struct LogEntry { float t = 0; std::string text, cls; };

struct BoxItem {
    float x = 0, y = 0;
    bool  active = false;
    bool  taken = false;
    bool  carried = false;
    Unit *by = nullptr;      // 对应网页版 box.carrier
    float t = 0;
    bool  onGround = false;
};

struct MissionStats {
    int   enemyDead = 0, apcKilled = 0;
    bool  tankKilled = false, boxTaken = false, boxEvacuated = false;
    int   evacCount = 0;
    int   cmdIssued = 0, cmdExec = 0, cmdRefused = 0;
    bool  reinforceTriggered = false, officerKilled = false;
    int   minesUsed = 0;
    bool  bridgeBlown = false;
    bool  shotFired = false, firstShotByPlayer = false;
    int   tankShells = 0;      // 坦克主炮发射次数（HUD 战报用）
    std::string triggerSrc;
    int   allyDead = 0, allyDown = 0;
};

// --------------------------------------------------- Target 访问器实现
// （必须放在 Unit / Vehicle 定义之后）
inline float Target::tx() const { return u ? u->x : v->x; }
inline float Target::ty() const { return u ? u->y : v->y; }
inline bool Target::gone() const { return u ? (u->dead || u->downed) : (v ? v->destroyed : true); }
inline bool Target::dead_only() const { return u ? u->dead : (v ? v->destroyed : true); }
inline bool Target::is_player() const { return u && u->isPlayer; }
inline const VehicleSpec *Target::spec() const { return v ? v->spec : nullptr; }
inline const std::string &Target::tname() const {
    static const std::string kEmpty;
    if (u) return u->name;
    if (v) return v->name;
    return kEmpty;
}
inline const std::string &Target::ttype() const {
    static const std::string kEmpty;
    return v ? v->type : kEmpty;
}

} // namespace va
