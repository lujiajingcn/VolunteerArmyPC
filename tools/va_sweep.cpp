// VolunteerArmyPC —— 无头平衡扫描：脱离引擎跑逻辑层
//
// 为什么需要它
// ------------
// README 一直写着「逻辑层可以脱离引擎单独跑」，但在这之前仓库里没有任何东西
// **证明**过这句话：平衡数据（10 种子胜率 4/10、车顶机枪占我方伤亡 33%–59%）
// 是从网页版的 _devcheck/sweep.js 上量的，而 PC 端改完常数只能靠人肉跑图对比 ——
// 而真跑一局要 620 秒模拟时间，10 个种子根本没法手工比。
// 本工具把同一件事搬到 PC 的逻辑层上：不链接 godot-cpp、不开窗口、不走渲染，
// 直接把 src/sim/* 编进来把 10 局跑完。
//
// 与网页版的关系
// --------------
// 剧本逐条对应 sweep.js 的 SCRIPT_A（同一套口令、同一触发时刻、同一个
// issueCommand 顺序：**先 step 再下命令**）。所以 base 那一行必须能与网页版
// 记录的 4/10 对上 —— 对不上就说明移植有偏差，此时任何"调整后变好了"的结论
// 都不可信。这是本工具的第一用途：当基线校验器。
//
// 用法
// ----
//   va_sweep                跑全部对照方案 × 10 种子
//   va_sweep base           只跑某个方案（名字前缀匹配）
//   va_sweep -v             逐局打印明细（默认只打印汇总）
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "sim/va_world.h"
#include "sim/va_campaign.h"

using namespace va;

namespace {

/* 种子表与网页版 sweep.js 完全一致 —— 同种子 → 同天气、同车队顺序、同箱位置，
   两边的逐局结果应当逐行可比，而不只是"胜率差不多"。 */
const int   SEEDS[] = { 20240915, 1, 7, 42, 123, 777, 2024, 31337, 555, 90901 };
const int   NSEED   = (int)(sizeof(SEEDS) / sizeof(SEEDS[0]));
const float DT      = 0.05f;
const float T_MAX   = 620.0f;

/* 编译期默认值的一份快照，main 开跑前抓一次。
   "BAL 默认" 那一行靠它**还原**默认值，而不是靠"排在第一个所以还没被改过"——
   后者只要有人重排方案表就会静默失效。 */
BalanceCfg g_bal_defaults;

struct Scenario {
    const char *name;
    float mg_dmg;      // BAL.veh_mg_dmg
    float mg_gap;      // BAL.veh_mg_gap
    float ally_hp;     // BAL.ally_hp
    int   evac;        // BAL.evac_need
    /* true = 用 va_config.h 里的**当前默认值**（不管本行前面写了什么）。
       这一行是本工具的第二用途：证明"代码里的默认值"就是"扫描里被选中的那一行"。
       少了它就会出现"扫描报告 8/10、实机却还是 3/10"这种偏差而无人发现 ——
       改完 va_config.h 忘了让扫描跟着走，两边就永久性地各说各话。 */
    bool  bal_default = false;

    void apply() const {
        if (bal_default) { BAL = g_bal_defaults; return; }
        BAL.veh_mg_dmg = mg_dmg;
        BAL.veh_mg_gap = mg_gap;
        BAL.ally_hp    = ally_hp;
        BAL.evac_need  = evac;
    }
};

/* 对照方案。
   base 是**改动前的原值**（= 网页版数值），它必须能与记录里的旧数据对上，
   否则"调整后变好了"就无从谈起 —— 这是本工具的第一用途：当基线校验器。
   2026-09-18 实测：base 3/10、车顶机枪占我方伤亡 66%（记录里 4/10、33%–59%
   来自网页版；PC 端实测更差，说明这个问题一点没被移植误差稀释）。
   A 已被采用为 BAL 的默认值（见 va_config.h 里的表），
   所以 "BAL 默认" 这一行的数字必须与 "A-mg" 那一行**完全相同**。 */
const Scenario SCEN[] = {
    { "BAL 默认", 0.00f, 0.00f, 0.00f, 0, true },   // = va_config.h 当前默认值（应等于 A-mg）
    { "base",     1.00f, 1.00f, 1.00f, 6 },   // 原值（改动前）
    { "A-mg",     0.55f, 1.80f, 1.00f, 6 },   // 削弱车顶机枪 —— 已采用
    { "B-hp",     1.00f, 1.00f, 1.40f, 6 },   // 提高队友耐久
    { "C-evac",   1.00f, 1.00f, 1.00f, 5 },   // 降低撤离门槛
    { "A+B",      0.55f, 1.80f, 1.40f, 6 },
    { "A+C",      0.55f, 1.80f, 1.00f, 5 },
    /* ---- 车长改实车尺寸（2026-09-20）之后的重新搜索 ----
       实车尺寸让车队从"6 个 3 米小盒"变成"一道 35 米、会吸子弹的墙"：
       队友绕不过去、子弹被车体吃掉，A-mg 从 8/10 掉到 6/10（殉爆半径钉回原值后）。
       下面四行是"要把 8/10 找回来，旋钮得拧到哪"的候选，**尚未采用**。 */
    { "P1-hp18",  0.55f, 1.80f, 1.80f, 6 },
    { "P2-mg45",  0.45f, 2.20f, 1.00f, 6 },
    { "P3-mg45hp", 0.45f, 2.20f, 1.40f, 6 },
    { "P4-mg40hp", 0.40f, 2.40f, 1.40f, 6 },
};
const int NSCEN = (int)(sizeof(SCEN) / sizeof(SCEN[0]));

// --------------------------------------------------------------- 剧本
std::string box_order() {
    if (W.boxWhere == "truck") return "全体，打卡车";
    if (W.boxWhere == "apc")   return "反坦克组，打装甲车";
    return "全体，打军官";
}

/* SCRIPT_A 等价物：第 i 条指令该不该发。
   网页版是 if (i === k && cond) 的链，这里保持同样的"先判 i 再判条件"顺序 ——
   顺序反了会让本该等到某时刻才发的指令提前发出，整局节奏就变了。 */
bool script_a(int i, std::string &out) {
    const Vehicle *lead = W.vehicles.empty() ? nullptr : &W.vehicles[0];
    const float T = W.triggerT;
    if (i == 0 && W.t > 5) { out = "全体，隐蔽"; return true; }
    if (i == 1 && lead != nullptr && lead->x < 1180) {
        trigger_ambush("mine");
        out = "老白，起爆";
        return true;
    }
    if (!W.triggered) return false;
    if (i == 2 && W.t > T + 14)  { out = "全体，开火"; return true; }
    if (i == 3 && W.t > T + 34)  { out = "反坦克组，打坦克"; return true; }
    if (i == 4 && W.t > T + 56)  { out = "老周，压制"; return true; }
    if (i == 5 && W.t > T + 78)  { out = box_order(); return true; }
    if (i == 6 && W.t > T + 110) { out = "铁头，搬密码箱"; return true; }
    if (i == 7 && W.t > T + 200) { out = "小满，救伤员"; return true; }
    if (i == 8 && W.t > T + 230) { out = "全体，撤离"; return true; }
    return false;
}

// --------------------------------------------------------------- 单局
struct Row {
    int seed = 0;
    std::string kind = "未结束";
    int alive = 0, dead = 0, downed = 0;
    int casMG = 0;              // 其中"最后一次挨的是车载机枪"的人数
    bool tank = false, box = false, boxEvac = false;
    int evac = 0, enemyDead = 0, apc = 0;
    std::string weather, boxWhere;

    bool win() const { return kind == "成功"; }
};

Row run_one(const Scenario &sc, int seed) {
    sc.apply();

    init_world((uint32_t)seed);
    W.smokes.clear();
    W.started = true;
    W.deployDone = true;

    int cmdIdx = 0;
    const int steps = (int)(T_MAX / DT);
    for (int i = 0; i < steps; ++i) {
        // 顺序与 sweep.js 一致：先 step，再决定要不要下命令
        step_once(DT);
        std::string c;
        if (script_a(cmdIdx, c)) { run_command_text(c, "voice", true); ++cmdIdx; }
        if (W.over) break;
    }

    Row r;
    r.seed = seed;
    r.kind = W.over ? (W.overKind.empty() ? "未结束" : W.overKind) : "未结束";
    /* 归因不靠计数器，靠"这个人最后一次挨的是谁打的"：
       damage_unit 会在每次伤害时整个覆写 lastHurtVehSpec，
       阵亡/倒地之后不可能再挨打（damage_unit 直接早退），
       所以任务结束时读一次就是死因 —— 不会重复计，也不会漏计。 */
    for (auto &u : W.units) {
        if (u.team != Team::Ally) continue;
        if (u.dead)        { r.dead++;   if (u.lastHurtVehSpec) r.casMG++; }
        else if (u.downed) { r.downed++; if (u.lastHurtVehSpec) r.casMG++; }
        else               { r.alive++; }
    }
    r.tank     = W.stats.tankKilled;
    r.box      = W.stats.boxTaken;
    r.boxEvac  = W.stats.boxEvacuated;
    r.evac     = W.stats.evacCount;
    r.enemyDead= W.stats.enemyDead;
    r.apc      = W.stats.apcKilled;
    r.weather  = W.weather;
    r.boxWhere = W.boxWhere;
    return r;
}

// --------------------------------------------------------------- 战役模式
/* 逐关打一遍：每关结束把撤出来的人交给下一关（capture_carry → CARRY → init_world）。
   它回答的是"关卡能不能连起来"，**不是**"这六关难度合不合适" ——
   剧本是同一套机械口令，不带玩家的临场判断，所以胜率没有参考价值；
   有价值的是：六关都能铺出来、目标都能判、人确实在一关关减少、不崩、不卡死。 */
bool goals_but_evac_done() {
    for (const auto &g : cur_level().goals) {
        if (g.main && g.kind != GoalKind::Evac && !goal_done(g)) return false;
    }
    return true;
}

struct LvRow {
    int   lv = 0;
    std::string name, kind;
    int   alive = 0, dead = 0, downed = 0, evac = 0;
    int   carry = 0;          // 带进下一关的人数
    float t = 0;
    std::string goals;        // 目标完成情况
    std::string extra;        // 关卡机制的诊断
    std::string text;         // 结束文案（失败原因）
};

LvRow run_level(int lv, uint32_t seed, bool verbose) {
    init_world(seed, lv, (lv == 0) ? nullptr : &CARRY);
    W.smokes.clear();
    W.started = true;
    W.deployDone = true;

    int cmdIdx = 0;
    float lastEvacCmd = -100.0f, lastDamCmd = -100.0f, lastRetakeCmd = -100.0f;
    bool damOrdered = false, retakeOrdered = false;
    const float tMax = CFG.missionEnd + 2.0f;
    const int steps = (int)(tMax / DT) + 2;
    for (int i = 0; i < steps; ++i) {
        step_once(DT);
        std::string c;
        bool fire = false;
        const float T = W.triggerT;
        const Vehicle *lead = W.vehicles.empty() ? nullptr : &W.vehicles[0];
        if (cmdIdx == 0 && W.t > 5) { c = "全体，隐蔽"; fire = true; }
        else if (cmdIdx == 1 && lead != nullptr && lead->x < CFG.convoyStopX + 320) {
            trigger_ambush("mine"); c = "老白，起爆"; fire = true;
        }
        if (!fire && W.triggered) {
            if (cmdIdx == 2 && W.t > T + 12) { c = "全体，开火"; fire = true; }
            else if (cmdIdx == 3 && W.t > T + 30) { c = "反坦克组，打坦克"; fire = true; }
            else if (cmdIdx == 4 && W.t > T + 70) { c = box_order(); fire = true; }
            else if (cmdIdx == 5 && W.t > T + 100) { c = "铁头，搬密码箱"; fire = true; }
            else if (cmdIdx == 6 && W.t > T + 130) { c = "小满，救伤员"; fire = true; }
        }
        // 关卡特有：炸坝（内外加山）与夜袭夺回（种子山）
        if (!fire && cur_level().hasDam && W.triggered && W.t > T + 25 && W.t - lastDamCmd > 25.0f) {
            /* 呼号写"老白"没关系：issue_command 对 blowDam 有一整条候选链
               （老白 → 石头 → 带炸药的人 → 任何活人），爆破是班组能力。 */
            c = "老白，炸水库"; lastDamCmd = W.t; damOrdered = true; fire = true;
        }
        /* **撤退令优先于其它一切**：一旦下过撤离令，就不能再有人被派回阵地。
           原先"夜袭夺回"的循环无条件每 20 秒发一次"前往A点"，把刚刚发出的
           撤离令当场覆盖掉 —— 实测第三关 313 秒下令撤离，322 / 342 秒又把全队
           叫回主峰，40 秒窗口里**没有一个人往撤离点走**（0 人撤离，整关判负）。
           真人不会这么干：喊了撤就不会再把人往山上推。 */
        const bool retreating = W.evacOrdered;
        if (!fire && W.triggered && !retreating
            && (goals_but_evac_done() || W.t > CFG.missionEnd - 90.0f)
            && W.t - lastEvacCmd > 30.0f) {
            c = "全体，撤离"; lastEvacCmd = W.t; fire = true;
        }
        /* 夜袭夺回要**反复下令**：一次"前往 A 点"之后，队伍会在半路被火力打散，
           站上主峰需要持续压上去。史实里敢死队也是一波波冲的。 */
        if (!fire && !retreating && LV.retakeArmed && W.t - lastRetakeCmd > 20.0f) {
            c = "全体，前往A点"; lastRetakeCmd = W.t; retakeOrdered = true; fire = true;
        }
        if (fire && !c.empty()) {
            if (cmdIdx < 7 && c != "老白，炸水库" && c != "全体，前往A点" && c != "全体，撤离") ++cmdIdx;
            const ParsedCmd pc = run_command_text(c, "voice", true);
            if (verbose) {
                std::printf("   [%.0fs] %-14s -> act=%-10s 呼号=%s 地点=%s ok=%d\n",
                            W.t, c.c_str(), pc.actId.c_str(), pc.csId.c_str(),
                            pc.hasLoc ? pc.locKey.c_str() : "-", (int)pc.ok);
            }
        }
        if (W.over) break;
    }

    LvRow r;
    r.lv = lv;
    r.name = cur_level().name;
    r.kind = W.over ? (W.overKind.empty() ? std::string("未结束") : W.overKind) : std::string("未结束");
    for (auto &u : W.units) {
        if (u.team != Team::Ally) continue;
        if (u.dead) r.dead++;
        else if (u.downed) r.downed++;
        else r.alive++;
    }
    r.evac = W.stats.evacCount;
    r.t = W.t;
    r.text = W.overText;
    if (cur_level().hasDam) {
        Unit *lb = ally_by_id("laobai");
        /* "已炸(安放)" = 人跑上去安放了 8 秒炸药；"已炸(非安放)" = 被别的爆炸波及 ——
           后者意味着"水淹七军"在玩家没做那个两难选择的情况下就被打勾了。 */
        r.extra = std::string("坝:") + (LV.damBlown ? (LV.damByCharge ? "已炸(安放)" : "已炸(非安放!)")
                                                    : "未炸");
        r.extra += lb ? (lb->dead ? " 老白:阵亡" : (lb->downed ? " 老白:失能" : " 老白:在")) : " 老白:不在";
        /* 这两个是**复用同一个字段的末刻快照**：damTask 在安放完成那一刻就被清掉，
           所以"炸成功过"的局这里必然读 0/8 —— 别把它当成"没人干过活"。 */
        r.extra += " 末刻派工" + std::to_string(LV.damWorkers) + "人 安放" +
                   std::to_string((int)LV.damPlantT) + "/8";
    }
    if (cur_level().forceNight) {
        r.extra = std::string("夜袭:") + (LV.retakeArmed ? "已发起" : "未到") +
                  (LV.retaken ? " 已夺回" : " 未夺回") +
                  " 站稳" + std::to_string((int)LV.retakeHold) + "s" +
                  " 阵地 " + std::to_string(LV.holdAlly) + "比" + std::to_string(LV.holdEnemy) +
                  " 已守" + std::to_string((int)LV.holdT) + "s";
    }
    for (const auto &g : cur_level().goals) {
        r.goals += (goal_done(g) ? "[√]" : "[ ]");
        r.goals += g.text;
        r.goals += " ";
    }
    /* CARRY 在 check_end 里就抓好了（过关那一刻），这里只读。
       **只在真的"转进"时才有意义** —— 没过关（失败）时 CARRY 还是上一关留下的，
       直接打出来会被读成"这一关带出了这么多人"，所以标成 -1、表格里打 "-"。 */
    r.carry = (W.overKind == "转进") ? (int)CARRY.units.size() : -1;
    return r;
}

int run_campaign(int seed, bool verbose) {
    CARRY = CarryOver{};
    CAM = CampaignState{};
    std::printf("铁原战役 · 六关连续跑（种子 %d，无敌方干预的机械剧本）\n\n", seed);
    /* 「撤」= 本关结算时走出去的人数（统计口径）；「带」= 实际被 carry 进下一关的人数
       （继承口径）。两者应当只差玩家 1 人（玩家永远进下一关，但不一定"撤出"）。
       分开打出来，是为了让 capture_carry() 的判据（u.evacuated || 距离）出问题时
       当场能看见 —— 实测踩过「撤 7 / 带 5」：撤离点改到南侧树林后，
       已经走出旧撤离点的人被判成没带出来。 */
    std::printf("%-4s %-22s %-6s %4s %4s %4s %4s %4s %6s  %s\n",
                "关", "阵地", "结果", "活", "亡", "倒", "撤", "带", "用时", "目标");
    /* VA_CAMP_START：从第几关开始（0 基）。**单独验证后面几关用** ——
       第六关的炸坝、第三关的夜袭夺回，要一路打到那里才能测太费事，
       而且前几关打输了就根本走不到。这只是验证入口，不是玩法。 */
    int start = 0;
    if (const char *e = std::getenv("VA_CAMP_START")) start = std::atoi(e);
    if (start > 0) {
        start = std::min(start, level_count() - 1);
        CAM.levelIndex = start;
        std::printf("（从第 %d 关开始，队伍按满编起算）\n", start + 1);
    }
    int cleared = 0;
    int carryBad = 0;   // 「带」与「撤」对不上的关数（不变式见下）
    for (int lv = start; lv < level_count(); ++lv) {
        const LvRow r = run_level(lv, (uint32_t)seed + (uint32_t)lv * 7919u, verbose);
        /* 「带」只在转进时有值；失败/末关打 "-"，免得把上一关的 CARRY 误读成本关成绩。 */
        char carryStr[8];
        if (r.carry < 0) std::snprintf(carryStr, sizeof carryStr, "-");
        else             std::snprintf(carryStr, sizeof carryStr, "%d", r.carry);
        std::printf("%-4d %-22s %-6s %4d %4d %4d %4d %4s %5.0fs  %s\n",
                    r.lv + 1, r.name.c_str(), r.kind.c_str(),
                    r.alive, r.dead, r.downed, r.evac, carryStr, r.t, r.goals.c_str());
        if (!r.extra.empty()) std::printf("     └ %s\n", r.extra.c_str());
        /* 结束原因：`W.overText` 是 end_game 写下的原话（"全队失能" / "时限已到" …）。
           关卡"失败"有好几种成因，先看这句再猜是哪一条。 */
        if (!r.text.empty()) std::printf("     × 结束原因：%s\n", r.text.c_str());
        /* **不变式**：过了关的话，`带` 必须等于 `撤` 或 `撤+1`。
           加 1 的那一种来自玩家 —— 玩家恒进下一关，但收拢时可能已经倒在场上、
           没从撤离点走出去（所以 `撤` 里没有他）。除此之外任何差额都是 bug：
           `撤 > 带` = capture_carry 把已经走出去的人漏了（撤离点中途改址踩过，
           实测「撤 7 / 带 5」）；`带 > 撤+1` = 把人重复收了。 */
        if (r.kind == "转进" && (r.carry < r.evac || r.carry > r.evac + 1)) {
            std::printf("     !! 不变式破了：撤 %d / 带 %d（应满足 撤 <= 带 <= 撤+1）\n",
                        r.evac, r.carry);
            ++carryBad;
        }
        if (r.kind != "转进") {
            /* VA_CAMP_FORCE：打输了也往下走一关。
               **只是链路验证用的开关，不是玩法** —— 机械剧本打不掉坦克是常事
               （真人玩家 8/10 能打掉，剧本只有一次口令、不会集火），
               如果因此第二关就断掉，第五关的孤山、第六关的炸坝就永远测不到。 */
            if (!std::getenv("VA_CAMP_FORCE")) break;
            capture_carry();
        }
        ++cleared;
    }
    if (carryBad) std::printf("⚠ 有 %d 关的「撤/带」对不上，见上面 !! 行\n", carryBad);
    std::printf("\n→ 过关 %d/%d   累计阵亡 %d   累计撤离 %d 人次   总用时 %.0f 秒\n",
                cleared, level_count(), CAM.totalDead, CAM.totalEvac, CAM.totalTime);
    for (const auto &l : CAM.log) std::printf("   · %s\n", l.c_str());
    return cleared;
}

const char *weather_cn(const std::string &w) {
    if (w == "sunny") return "晴";
    if (w == "rain")  return "雨";
    if (w == "night") return "夜";
    return w.c_str();
}

} // namespace

int main(int argc, char **argv) {
    // 无缓冲：万一中途崩了，也要能看见崩在哪一局（第一版就是缓冲吞掉了全部输出）
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const char *filter = nullptr;
    bool verbose = false;
    int  campSeed = 20240915;
    bool campaign = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0) verbose = true;
        else if (std::strcmp(argv[i], "campaign") == 0) campaign = true;
        else if (campaign) campSeed = std::atoi(argv[i]);
        else filter = argv[i];
    }

    /* 必须先挂上一个事件出口。
       va_world 内部的 say/sfx 走 ev() 兜底，但 va_flow 的 check_objectives /
       end_game 是**直接解引用 EV** 的 —— 不挂就是空指针崩溃。
       SimEvents 的 8 个回调都有默认空实现，所以直接实例化基类即可：
       这里要的正是"把声音、字幕、结算界面全部丢掉"。 */
    static va::SimEvents null_events;
    va::set_events(&null_events);

    build_convoy_way();

    /* 在任何 apply() 之前抓一次默认值 —— 必须在循环外，且必须在第一次 apply 之前。
       这一句就是"扫描数字"与"实机行为"之间的那根绳子。 */
    g_bal_defaults = BAL;

    if (campaign) { run_campaign(campSeed, verbose); return 0; }

    std::printf("VolunteerArmyPC · 无头平衡扫描（无渲染 / 无引擎）\n");
    std::printf("剧本 = 网页版 sweep.js 的 SCRIPT_A；种子表也一致；dt=%.2f 上限 %.0f 秒\n\n", DT, T_MAX);

    for (int s = 0; s < NSCEN; ++s) {
        if (filter != nullptr &&
            std::strncmp(SCEN[s].name, filter, std::strlen(filter)) != 0) continue;

        const Scenario &sc = SCEN[s];
        /* 先 apply 再打印：这样表头里那四个数**永远是实际生效的值**。
           否则 "BAL 默认" 那一行只能打出占位的 0.00，
           而它恰恰是最需要看清的一行。 */
        sc.apply();
        std::printf("=== %s   (mg_dmg×%.2f  mg_gap×%.2f  ally_hp×%.2f  evac=%d) ===\n",
                    sc.name, BAL.veh_mg_dmg, BAL.veh_mg_gap, BAL.ally_hp, BAL.evac_need);
        if (verbose) {
            std::printf("种子        结果   活 亡 倒 伤亡 机枪占  坦克 箱 撤离 敌伤亡 天气 箱位置\n");
        }

        int win = 0, totalCas = 0, totalCasMG = 0, wipe = 0;
        for (int k = 0; k < NSEED; ++k) {
            const Row r = run_one(sc, SEEDS[k]);
            if (r.win()) win++;
            const int cas = r.dead + r.downed;
            totalCas += cas;
            totalCasMG += r.casMG;
            if (r.alive == 0 && r.dead > 0) wipe++;
            if (verbose) {
                std::printf("%-11d %-6s %2d %2d %2d %4d %5.0f%%  %-4s %-2s %4d %6d %-4s %s\n",
                            r.seed, r.kind.c_str(), r.alive, r.dead, r.downed, cas,
                            cas > 0 ? 100.0 * r.casMG / cas : 0.0,
                            r.tank ? "毁" : "存", r.box ? "得" : "无",
                            r.evac, r.enemyDead, weather_cn(r.weather), r.boxWhere.c_str());
            }
        }
        const double mgShare = totalCas > 0 ? 100.0 * totalCasMG / totalCas : 0.0;
        std::printf("→ 胜率 %d/%d   全灭 %d 局   我方伤亡 %d 人（其中车顶机枪 %.0f%%）\n\n",
                    win, NSEED, wipe, totalCas, mgShare);
    }
    return 0;
}
