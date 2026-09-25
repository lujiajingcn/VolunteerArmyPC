// VolunteerArmyPC —— 世界状态 / 单位工厂 / 车队 / 地形与视线（对应网页版第二章）
#include "sim/va_world.h"
#include "sim/va_campaign.h"

#include <algorithm>
#include <cmath>

namespace va {

WorldState W;
Rng RNG(1u);
PlayerInput IN;

static SimEvents *g_events = nullptr;
SimEvents *EV = nullptr;

void set_events(SimEvents *e) {
    /* 两个名字指向同一个对象，务必一起更新 ——
       va_world 内部的 say/sfx 走 g_events（经 ev() 兜底），
       而 va_flow 里的 check_objectives/end_game 直接解引用 EV。
       只赋值其中一个会让另一条路径拿到 nullptr：曾经就是这样，
       step_once 第一次跑到 check_objectives() 就空指针崩溃。 */
    g_events = e;
    EV = e;
}

// 保证 EV 始终可安全调用
static SimEvents *ev() {
    static SimEvents dummy;
    return g_events ? g_events : &dummy;
}

void sfx(const std::string &id, float x, float y, float gain, bool local) {
    (void)id; (void)x; (void)y; (void)gain; (void)local;
    ev()->on_sfx(id, x, y, gain, local);
}
void toast(const std::string &text) { ev()->on_toast(text); }
void set_alert(const std::string &text, float dur) { ev()->on_alert(text, dur); }

// ------------------------------------------------------------------ 查询
std::vector<Unit *> allies() {
    std::vector<Unit *> v;
    for (auto &u : W.units) if (u.team == Team::Ally && !u.dead) v.push_back(&u);
    return v;
}
std::vector<Unit *> alive_allies() {
    std::vector<Unit *> v;
    for (auto &u : W.units) if (u.team == Team::Ally && !u.dead && !u.downed) v.push_back(&u);
    return v;
}
std::vector<Unit *> combat_allies() {
    std::vector<Unit *> v;
    for (auto &u : W.units) if (u.team == Team::Ally && !u.dead && !u.downed && !u.isPlayer) v.push_back(&u);
    return v;
}
std::vector<Unit *> enemies() {
    std::vector<Unit *> v;
    for (auto &u : W.units) if (u.team == Team::Enemy && !u.dead) v.push_back(&u);
    return v;
}
std::vector<Vehicle *> enemy_vehicles() {
    std::vector<Vehicle *> v;
    for (auto &x : W.vehicles) if (x.team == Team::Enemy && !x.destroyed) v.push_back(&x);
    return v;
}
std::vector<Unit *> downed_allies() {
    std::vector<Unit *> v;
    for (auto &u : W.units) if (u.team == Team::Ally && u.downed && !u.dead) v.push_back(&u);
    return v;
}
Unit *unit_by_id(const std::string &id) {
    for (auto &u : W.units) if (u.id == id) return &u;
    return nullptr;
}
Unit *ally_by_id(const std::string &id) {
    for (auto &u : W.units) if (u.id == id && u.team == Team::Ally) return &u;
    return nullptr;
}

// 对应 JS 的 pick(arr)：从字符串常量表里随机取一条（用于无线电回复语）
const char *pick_cstr(const std::vector<const char *> &v) {
    if (v.empty()) return "";
    return v[(size_t)(RNG.next() * (float)v.size()) % v.size()];
}

Unit *nearest_enemy_at(float x, float y, Team team, const std::function<bool(Unit *)> &filter) {
    Unit *best = nullptr;
    float bd = 1e9f;
    for (auto &u : W.units) {
        if (u.dead || u.downed || u.team == team) continue;
        if (filter && !filter(&u)) continue;
        const float d = dist2f(x, y, u.x, u.y);
        if (d < bd) { bd = d; best = &u; }
    }
    /* 原版在这里还会遍历载具，且只在「带 filter」时才可能把载具当成结果返回 ——
       但返回后的调用点全部按 Unit 使用（读 name/target 等），载具一旦胜出就是类型错配。
       实际受影响的只有 calcObey（filter=!x.dead），而它只取距离；
       因此这里不再让载具参与，改由 enemy_threat_dist() 单独提供"含载具的最近威胁距离"。*/
    return best;
}

float enemy_threat_dist(float x, float y, Team team) {
    float bd = 1e9f;
    for (auto &u : W.units) {
        if (u.dead || u.downed || u.team == team) continue;
        bd = std::min(bd, dist2f(x, y, u.x, u.y));
    }
    for (auto &v : W.vehicles) {
        if (v.destroyed || v.team == team) continue;
        bd = std::min(bd, dist2f(x, y, v.x, v.y));
    }
    return bd >= 1e8f ? 9999.0f : std::sqrt(bd);
}

// --------------------------------------------------------------- 对外消息
void say(const std::string &who, const std::string &text, const std::string &cls) {
    Subtitle s; s.who = who; s.text = text; s.cls = cls; s.t = 6.5f;
    W.subs.push_back(s);
    while (W.subs.size() > 7) W.subs.erase(W.subs.begin());
    ev()->on_say(who, text, cls);
    ev()->on_subs_dirty();
    if (W.started) sfx(who == "你" ? "radioTx" : "radioRx", 0, 0);
}
void logline(const std::string &text, const std::string &cls) {
    LogEntry e; e.t = W.t; e.text = text; e.cls = cls;
    W.logs.push_back(e);
    ev()->on_log(text, cls);
}
FxItem &fx_push(FxItem item) {
    item.t = 0;
    if (item.life <= 0) item.life = 0.5f;
    W.fx.push_back(item);
    return W.fx.back();
}
void decal_push(float x, float y, float r, float cr, float cg, float cb, float ca) {
    Decal d; d.x = x; d.y = y; d.r = r; d.cr = cr; d.cg = cg; d.cb = cb; d.ca = ca; d.t = 0;
    W.decals.push_back(d);
    if (W.decals.size() > 260) W.decals.erase(W.decals.begin());
}

// ------------------------------------------------------------------ 地形
bool in_river(float x, float y) { (void)y; return x > CFG.riverX1 && x < CFG.riverX2; }
bool on_bridge(float x, float y) {
    return x > CFG.riverX1 - 10 && x < CFG.riverX2 + 10 && y > CFG.bridgeY1 && y < CFG.bridgeY2;
}
bool passable(float x, float y, bool isVehicle) {
    if (x < 6 || y < 6 || x > CFG.W - 6 || y > CFG.H - 6) return false;
    if (isVehicle) {
        if (in_river(x, y) && !on_bridge(x, y)) return false;
        if (!W.bridgeAlive && in_river(x, y)) return false;
    }
    return true;
}
bool vehicle_blocked(float x, float y, float r, const Vehicle &v) {
    const float dx = x - v.x, dy = y - v.y;
    const float c = std::cos(-v.angle), s = std::sin(-v.angle);
    const float lx = dx * c - dy * s, ly = dx * s + dy * c;
    return std::fabs(lx) < v.len / 2 + r * 0.5f && std::fabs(ly) < v.wid / 2 + r * 0.5f;
}
bool prop_blocked(float x, float y, float r) {
    for (const auto &p : W.props) {
        if (p.destroyed || !p.hard) continue;
        const float rr2 = p.r + r;
        if (dist2f(x, y, p.x, p.y) < rr2 * rr2) return true;
    }
    for (const auto &v : W.vehicles) {
        if (v.destroyed) continue;
        if (vehicle_blocked(x, y, r, v)) return true;
    }
    return false;
}

bool los_core(float x1, float y1, float x2, float y2, bool hard, bool soft, bool smoke) {
    for (const auto &p : W.props) {
        if (p.destroyed) continue;
        if (hard && p.blocksBullet) {
            const float r2 = p.r + 4;
            if (dist2f(x1, y1, p.x, p.y) < r2 * r2) continue;
            if (dist2f(x2, y2, p.x, p.y) < r2 * r2) continue;
            if (segCircle(x1, y1, x2, y2, p.x, p.y, p.r)) return true;
        } else if (soft && p.soft && p.blocksLos) {
            if (segCircle(x1, y1, x2, y2, p.x, p.y, p.r * 0.7f)) return true;
        }
    }
    if (smoke) {
        for (const auto &s : W.smokes) {
            if (segCircle(x1, y1, x2, y2, s.x, s.y, s.r * 0.8f)) return true;
        }
    }
    return false;
}
bool los_fire(float x1, float y1, float x2, float y2) { return los_core(x1, y1, x2, y2, true, false, true); }
bool los_sight(float x1, float y1, float x2, float y2) { return los_core(x1, y1, x2, y2, true, true, true); }

float cover_score(float x, float y, float tx, float ty) {
    float s = 0;
    for (const auto &p : W.props) {
        if (p.destroyed) continue;
        const float d = distf(x, y, p.x, p.y);
        const float R = p.r + 66;
        if (d > R) continue;
        float w = 1 - d / R;
        if (p.type == PropType::Trench) {
            w *= 1.5f;
        } else if (p.hard) {
            const float vx = p.x - x, vy = p.y - y;
            const float L = hypot2f(vx, vy) + 1e-6f;
            const float txn = tx - x, tyn = ty - y;
            const float L2 = hypot2f(txn, tyn) + 1e-6f;
            const float dot = (vx / L) * (txn / L2) + (vy / L) * (tyn / L2);
            w *= dot > 0.15f ? 1.5f : 0.3f;
        } else {
            w *= 0.55f;
        }
        s += w * p.cover;
    }
    if (!prop_blocked(x + (x - tx) * 0.25f, y + (y - ty) * 0.25f, 6)) s += 0.25f;
    if (los_sight(x, y, tx, ty)) s -= 1.2f;
    Unit *th = nearest_enemy_at(x, y, Team::Ally);
    if (th) s -= clampf((260 - distf(x, y, th->x, th->y)) / 400.0f, 0, 0.5f);
    return s;
}

bool find_cover(float fromX, float fromY, float tx, float ty, float maxDist, float &ox, float &oy) {
    if (maxDist <= 0) maxDist = 80;
    bool found = false;
    float bs = -1e9f;
    for (int r2 = 1; r2 <= 3; ++r2) {
        const float rad = maxDist * r2 / 3.0f;
        for (int i = 0; i < 16; ++i) {
            const float a = i * 3.141592653589793f / 8.0f + (RNG.next() - 0.5f) * 0.3f;
            const float x = fromX + std::cos(a) * rad, y = fromY + std::sin(a) * rad;
            if (!passable(x, y) || prop_blocked(x, y, 6)) continue;
            const float sc = cover_score(x, y, tx, ty) - rad / maxDist * 0.35f;
            if (sc > bs) { bs = sc; ox = x; oy = y; found = true; }
        }
    }
    return found;
}

float cover_protect(const Unit &u) {
    if (u.dead || u.downed) return 0;
    float best = 0;
    for (const auto &p : W.props) {
        if (p.destroyed) continue;
        const float reach = p.hard ? 38 : 26;
        const float d = distf(u.x, u.y, p.x, p.y);
        if (d >= p.r + reach) continue;
        const float near = 1 - std::max(0.0f, d - p.r) / reach;
        best = std::max(best, near * p.cover);
    }
    if (u.state == "隐蔽" || u.state == "待命") best = std::max(best, 0.4f);
    return clampf(best, 0, 1);
}

float vis_range(const Unit &u) {
    float r = (u.team == Team::Ally) ? 780 : 620;
    if (u.weaponKey == "sniper") r = 1160;
    else if (u.weaponKey == "mg") r = 840;
    if (W.weather == "rain") r *= 0.82f;
    if (W.weather == "night") r *= 0.62f;
    if (u.dead || u.downed) r = 0;
    return r;
}
bool can_see(const Unit &u, float tx, float ty) {
    const float r = vis_range(u);
    const float d = distf(u.x, u.y, tx, ty);
    if (d > r) return false;
    if (d > 150) {
        if (los_sight(u.x, u.y, tx, ty)) return false;
    } else if (los_fire(u.x, u.y, tx, ty)) return false;
    return true;
}
// 对应 JS 的 canSee(u, tgt) —— tgt 可能是步兵也可能是载具，取坐标后语义完全一致
bool can_see_t(const Unit &u, const Target &t) {
    if (!t.ok()) return false;
    return can_see(u, t.tx(), t.ty());
}

// ------------------------------------------------------------ 单位工厂
Unit *make_unit(const RosterDef &def, float x, float y, Team team, bool isPlayer) {
    W.units.emplace_back();
    Unit &u = W.units.back();
    const WeaponSpec *w = weapon_of(def.weapon);
    u.id = def.id; u.name = def.name;
    u.role = def.role.empty() ? std::string("步枪手") : def.role;
    u.group = def.group;
    u.isPlayer = isPlayer;
    u.team = team;
    u.x = x; u.y = y;
    u.radius = 5.2f;
    /* 队友耐久旋钮（BAL.ally_hp）只作用于我方**非玩家**单位 ——
       "提高队友耐久"这条方案的落点是队友，玩家的手感与容错是另一条设计线，
       不该被一次平衡调整顺手改掉。 */
    const float hpBase = 100.0f * ((team == Team::Ally && !isPlayer) ? BAL.ally_hp : 1.0f);
    u.hp = hpBase; u.maxHp = hpBase; u.morale = 78;
    u.weaponKey = def.weapon;
    u.wpn = w;
    u.ammo = w->ammo; u.magAmmo = w->mag;
    u.rockets = (u.weaponKey == "at") ? ROCKET.ammo : 0;
    u.grenades = (u.weaponKey == "mg") ? 1 : 2;
    u.smokes = (u.weaponKey == "rifle") ? 1 : 0;
    u.hasCharge = (u.weaponKey == "demo" || u.weaponKey == "at");
    u.state = (team == Team::Ally) ? "部署" : "警戒";
    u.wobble = RNG.next() * 6.28f;
    return &u;
}

// 玩家的合成花名册项（网页版直接内联字面量）
/* 玩家的合成花名册项（网页版直接内联字面量）。
   art = char_leader：**队长那一套模型/胸像与职务绑定、不随阵地变** ——
   玩家阵亡后接任的是下一阵地的队长，用的也是这一套，
   所以换人之后画面上不会出现"同一个人换了张脸"。 */
static const RosterDef PLAYER_DEF{ "player", "你（队长）", "队长", "1组", "rifle", "char_leader", false, false, {} };

void init_world(uint32_t seed, int p_level, const CarryOver *p_carry) {
    RNG.reseed(seed);
    /* 铺关必须排在所有 RNG 抽取**之前**：apply_level 用的是自己那条独立随机流
       （见 va_campaign.cpp），所以插在这里不会平移主 RNG 的序列 ——
       天气 / 地雷 / 密码箱 / 车队顺序与改动前逐次相同。 */
    if (p_level >= 0) {
        apply_level(p_level, seed);
        CAM.active = true;
    } else {
        CAM.active = false;
    }
    W.seed = seed;

    W.props.assign(BASE_PROPS.begin(), BASE_PROPS.end());
    W.units.clear();
    W.vehicles.clear();
    W.projectiles.clear();
    W.fx.clear();
    W.decals.clear();
    W.smokes.clear();
    W.mines.clear();
    W.subs.clear();
    W.logs.clear();
    W.objState.clear();

    W.t = 0; W.phase = "INTEL"; W.phaseName = "情报";
    W.over = false; W.overKind.clear(); W.overText.clear();
    W.triggered = false; W.triggerT = 0;
    W.reinforceDone = false; W.reinforceT = CFG.reinforceAt;
    W.convoyStarted = false; W.convoyEscaped = false;
    W.bridgeAlive = true; W.evac = EVAC_DEFAULT; W.evacArmed = false; W.evacOrdered = false;
    /* 「撤 / 守」的选择是**每一关一次**的：换关必须清回去，
       否则第二关一开局就带着上一关那个"已经选了死守"的状态，
       提示条再也不弹 —— 而玩家只会以为这一关还没打到伤亡过半。 */
    W.retreatOffered = false; W.retreatChoice = 0; W.retreatOfferT = -1.0f;
    W.playerLost = false;
    W.hasMarker = false; W.deployDone = false; W.noise = 0; W.noiseT = 0;
    W.started = false; W.paused = false;

    // 运行态清零（对应原版那段"漏了会串局"的注释）
    W.barrage = false; W.barrageCd = 0; W.pendingShells.clear();
    W.hasBox = false; W.box = BoxItem{};
    W.officerVeh = nullptr; W.officerSpawned = false;
    W.handoverUnit = nullptr; W.convoyProgress = 0; W.mineWarned = false;
    W.alertT = 0; W.rangedSel = false;
    W.lastCmd = ParsedCmd{}; W.hasLastCmd = false;
    W.stats = MissionStats{};

    // 天气变体（重玩性）—— 注意只调用一次 RNG
    const float wr = RNG.next();
    W.weather = wr < 0.45f ? "sunny" : (wr < 0.8f ? "rain" : "night");
    /* 种子山那一关是**夜战**（史实：6 月 3 日凌晨夜袭夺回阵地），所以强制夜间。
       放在抽取之后覆盖而不是跳过抽取：少一次 RNG.next() 就会把后面的地雷、
       密码箱、车队顺序整体平移一格，旧基线的数据全废。 */
    if (p_level >= 0 && level_at(p_level).forceNight) W.weather = "night";

    /* ---- 先定"这一关出场的是哪 10 个人" ----
       编制 = 队长位（玩家）+ 9 名队员，队员池 = 本阵地花名册的 men[1..9]。
       继承优先：上一阵地活下来的先上场，缺额用本阵地**还没上过场**的名字补齐 ——
       于是"每个阵地 10 个人、名字事先定好"和"打得好的人能带到下一关"同时成立。 */
    const bool campaign = (p_level >= 0);
    const std::vector<RosterDef> &men =
        position_men(campaign ? level_at(p_level).id : POSITION_ROSTERS[0].levelId);
    const int    memberCap = campaign ? 9 : 10;    // 战役里队长位由玩家占
    const size_t poolStart = campaign ? 1u : 0u;   // 战役里 men[0] 就是那个队长位

    std::vector<RosterDef> squad;                  // 队员（不含玩家）
    const bool carried = (p_carry != nullptr && p_carry->valid);
    if (carried) {
        for (const auto &c : p_carry->units) {
            if (c.id == "player") continue;
            if ((int)squad.size() >= memberCap) break;
            const RosterDef *r = roster_def_anywhere(c.id);
            if (r == nullptr) continue;            // 查不到档案的人上不了场
            squad.push_back(*r);
            if (!c.name.empty()) squad.back().name = c.name;
        }
    }
    /* 补员分两轮，**都不随机** —— "事先创造出来"的含义就是"同一份名单可复现"：
       掺进随机之后"第二关是谁"会随上一关的战况漂移，复盘时对不上号。
         第一轮：优先补**现在没人担任的职务**。职务是班组能力而不是装饰 ——
           按花名册顺序硬补会让"这一队没有医疗兵"一直带到后面几关，
           于是倒地的人永远没人救（实测机械剧本第 5 关补进来的 4 个全是
           机枪/步枪，医疗兵与反坦克手双双缺席）。
         第二轮：按花名册原序把人数填满。 */
    auto already_has = [&](const std::string &role) {
        for (const auto &s : squad) if (s.role == role) return true;
        return false;
    };
    /* 第一轮只在**有继承队伍**时才跑。
       为什么：满编开局（第一关 / 非战役）时花名册本身就是完整编制，
       走"缺职务优先"会把顺序打乱（实测：满编时它会跳过第二个步枪手、
       先塞反坦克手，队伍顺序与改动前不同 —— 而建队顺序决定 RNG 抽取顺序，
       离线扫描的基线会被整体平移）。没有继承队伍时顺序就该等于名册顺序。 */
    if (carried) {
        for (size_t i = poolStart; i < men.size() && (int)squad.size() < memberCap; ++i) {
            bool taken = false;
            for (const auto &s : squad) if (s.id == men[i].id) { taken = true; break; }
            if (taken || already_has(men[i].role)) continue;
            squad.push_back(men[i]);
        }
    }
    for (size_t i = poolStart; i < men.size() && (int)squad.size() < memberCap; ++i) {
        bool taken = false;
        for (const auto &s : squad) if (s.id == men[i].id) { taken = true; break; }
        if (!taken) squad.push_back(men[i]);
    }

    /* ---- 队长位（玩家）----
       · 上一关活下来了 → **继续控制原角色**（id 恒为 "player"，姓名跟着人走）；
       · 上一关阵亡（CARRY 里没有 player）或第一关 → 由**本阵地的队长**接任。
       这两支正是需求第 4 条的两半，缺哪一半都会出现"人死了还在操控他"
       或者"人活着却莫名其妙换了个名字"。 */
    const CarryUnit *pc = nullptr;
    if (carried) {
        for (const auto &c : p_carry->units) if (c.id == "player") { pc = &c; break; }
    }
    RosterDef pdef = PLAYER_DEF;
    if (pc != nullptr) {
        if (!pc->name.empty()) pdef.name = pc->name;
    } else if (campaign) {
        pdef.name = men[0].name;                                   // 本阵地队长接任
        pdef.weapon = men[0].weapon;
    }
    pdef.art = "char_leader";
    pdef.leader = true;

    // 玩家 + 队员
    float px = 872, py = 872;
    recommend_of("player", px, py);
    Unit *pl = make_unit(pdef, px, py, Team::Ally, true);
    pl->facing = -3.141592653589793f / 2.0f;
    W.player = pl;

    /* 继承的队长带伤延续：血量 / 弹药 / 士气原样带过来。
       **但不带"倒地"进新关** —— 玩家开局躺着等于把操作权收走
       （要等医疗兵跑过来，期间只能看着），而且倒地计时一到就直接判阵亡 →
       触发"换人转进"，玩家会看到自己在还没接敌的时候就被换掉了。
       实测：第三关结束时有 3 个倒地（玩家在内），第四关开局 45 秒
       倒计时一到，队伍还没见到敌人就"你阵亡了 —— 只拖住 45 秒"。
       所以倒地状态按**带伤起立**处理（35% 血），伤势由血量表达。 */
    if (pc != nullptr) {
        if (pc->downed) {
            pl->hp = std::max(1.0f, pc->maxHp * 0.35f);
        } else {
            pl->hp = clampf(pc->hp, 1.0f, pc->maxHp);
        }
        pl->maxHp = pc->maxHp; pl->morale = pc->morale;
        pl->ammo = pc->ammo; pl->magAmmo = pc->magAmmo;
        pl->grenades = pc->grenades; pl->smokes = pc->smokes;
    }

    for (const auto &r : squad) {
        float rx = 880, ry = 900;
        recommend_of(r.id, rx, ry);
        Unit *u = make_unit(r, rx + rr(-8, 8), ry + rr(-8, 8), Team::Ally, false);
        u->hasHome = true; u->homePos = { rx, ry };
        if (!carried) continue;
        /* 找到这个人的继承档案，把血量 / 弹药 / 士气 / 战绩原样交回去。
           **阵亡的根本不在 CARRY 里**，所以这里"查不到"就是"上一关已经死了"，
           按新兵（满血满弹）建出来 —— 这就是补员。 */
        for (const auto &c : p_carry->units) {
            if (c.id != r.id) continue;
            u->hp = clampf(c.hp, 1.0f, c.maxHp); u->maxHp = c.maxHp;
            u->morale = c.morale;
            u->ammo = c.ammo; u->magAmmo = c.magAmmo;
            u->rockets = c.rockets; u->grenades = c.grenades; u->smokes = c.smokes;
            u->kills = c.kills; u->shots = c.shots; u->hits = c.hits;
            /* 继承过来的倒地伤员给 **120 秒**而不是新兵倒地那 45 秒。
               为什么：45 秒是"当场被打倒"的窗口，伤员是被抬下来的、
               开局就躺在部署区，医疗兵要先从阵位上过来；给 45 秒等于
               "上一关拼命救回来的人，下一关开局 45 秒集体断气"
               （实测第 4 关：带进来 3 个伤员，45 秒时全部阵亡，
               其中倒地的玩家因此触发转进）。救与不救都一样的话，
               "把伤员带出来"这件事就没有任何意义。 */
            if (c.downed) { u->downed = true; u->downTimer = 120; u->state = "失能"; u->hp = 0; }
            break;
        }
    }

    /* ROSTER = **当前这一关真正在场的人**（含玩家那条）。
       语音呼号（GROUPS / ROLE_CALL）与胸像、三维模型键（ally_art_key）全都按它走，
       所以必须在建队之后立刻重建 —— 留着上一阵地的人名，
       就等于可以对着一个不在场的人下命令。 */
    ROSTER.clear();
    ROSTER.push_back(pdef);
    for (const auto &r : squad) ROSTER.push_back(r);
    rebuild_roster_index();
    // 地雷
    const float mineSpots[4][2] = { { 1120, 662 }, { 1040, 672 }, { 620, 655 }, { 540, 658 } };
    const int eastIdx[2] = { 0, 1 }, westIdx[2] = { 2, 3 };
    const int east = eastIdx[(size_t)(RNG.next() * 2) % 2];
    const int west = westIdx[(size_t)(RNG.next() * 2) % 2];
    Mine me; me.x = mineSpots[east][0]; me.y = mineSpots[east][1]; me.team = Team::Ally; me.kind = "east";
    W.mines.push_back(me);
    Mine mw; mw.x = mineSpots[west][0]; mw.y = mineSpots[west][1]; mw.team = Team::Ally; mw.kind = "west";
    W.mines.push_back(mw);
    W.mineEast = &W.mines[0];
    W.mineWest = &W.mines[1];

    // 密码箱位置随机（卡车 / 装甲车 / 军官身上）—— 两次 RNG 调用的短路顺序必须保留
    if (RNG.next() < 0.62f) {
        W.boxWhere = "truck";
    } else {
        W.boxWhere = (RNG.next() < 0.6f) ? "apc" : "officer";
    }
    /* 五个阵地的任务只有"拖延"一条 —— 密码箱整体下线。
       抽取照旧（少一次 RNG.next() 会把后面的车队顺序整体平移一格，
       旧基线的数据全废），只把结论改成 "none"。 */
    if (p_level >= 0) W.boxWhere = "none";

    /* 车队顺序随机。**战役里换成关卡自己的编成表**：涟川山口来的是骑 1 师
       （两辆装甲车 + 一辆坦克），内外加山是上百辆车的装甲集群（三辆坦克）——
       逐关的"对面是谁"写在这张表里，而不是共用一个随机池。
       抽取次数保持一次，理由同上（不能平移主 RNG 序列）。 */
    const float orderRoll = RNG.next();
    if (p_level >= 0 && !level_at(p_level).convoyOrders.empty()) {
        const auto &os = level_at(p_level).convoyOrders;
        W.convoyOrder = os[(size_t)(orderRoll * (float)os.size()) % os.size()];
    } else if (orderRoll < 0.62f) {
        W.convoyOrder = { "jeep", "apc", "tank", "truck", "apc", "jeep" };
    } else if (orderRoll < 0.82f) {
        W.convoyOrder = { "jeep", "tank", "apc", "truck", "apc", "jeep" };
    } else {
        W.convoyOrder = { "apc", "jeep", "apc", "tank", "truck", "jeep" };
    }

    W.infantryTotal = (p_level >= 0) ? ri(level_at(p_level).infantryMin, level_at(p_level).infantryMax)
                                     : ri(10, 16);
    W.reinforceT = CFG.reinforceAt + std::round(rr(-30, 30));

    // 开局人数：撤离门槛按它算（effective_evac_need），必须在建队之后统计
    LV.startCount = 0;
    for (auto &u : W.units) if (u.team == Team::Ally && !u.dead) LV.startCount++;

    build_convoy();
    update_evac_marker();
}

// ------------------------------------------------------------ 车队生成
void build_convoy() {
    W.vehicles.clear();
    const char *names[4] = { "吉普", "装甲车", "坦克", "卡车" };
    int idx[4] = { 0, 0, 0, 0 };
    const int total = (int)W.convoyOrder.size();
    for (int i = 0; i < total; ++i) {
        const std::string &type = W.convoyOrder[i];
        int ti = 0;
        if (type == "jeep") ti = 0; else if (type == "apc") ti = 1;
        else if (type == "tank") ti = 2; else ti = 3;
        idx[ti]++;

        W.vehicles.emplace_back();
        Vehicle &v = W.vehicles.back();
        v.type = type;
        v.id = type + std::to_string(idx[ti]);
        v.name = std::string(names[ti]) + std::to_string(idx[ti]);
        v.team = Team::Enemy;
        /* 展开间距与停车排队间距（CFG.convoyGap）**必须是同一个数**：
           原先这里是硬编码的 132、而停车排队用 CFG.convoyGap=145，两个数各说各话。
           车长改实车尺寸后这条就绷不住了 —— 6.93 m 的卡车（139 单位）比 132 还长，
           行进中会与前后车穿模。现在统一走 CFG.convoyGap，只留一个真值。
           另外注意：领头车（i==0）的 dist 恒为 0，所以改间距**不影响**伏击触发的
           时刻（伏击判据用的是领头车的 x），只让后续车依次晚到。 */
        v.x = 2320 + (float)i * CFG.convoyGap; v.y = 700; v.angle = 3.141592653589793f;
        v.speed = 0;
        const VehicleSpec *sp = vehicle_of(type);
        v.baseSpeed = sp->speed * 0.30f;
        v.dist = -(float)i * CFG.convoyGap;
        v.hp = sp->hp; v.maxHp = sp->hp;
        v.spec = sp;
        v.len = sp->len; v.wid = sp->wid; v.armor = sp->armor;
        v.turret = 3.141592653589793f;
        v.fireCd = rr(2, 6); v.wpnCd = rr(0.5f, 2);
        v.capacity = sp->cap;
        v.line = (i == 0) ? -1 : (i == total - 1 ? 1 : 0);
        if (type == "apc" || type == "truck" || type == "jeep") {
            int n = (type == "apc") ? 6 : (type == "truck" ? 6 : 2);
            int used = 0;
            for (const auto &x : W.vehicles) used += x.troopPlan;
            int plan = std::min(n, std::max(0, W.infantryTotal - used));
            if (plan < 0) plan = 0;
            v.troopPlan = plan;
        } else {
            v.troopPlan = 0;
        }
    }
    // 卡车上放密码箱（优先）
    if (W.boxWhere == "truck" || W.boxWhere == "apc") {
        const bool wantTruck = (W.boxWhere == "truck");
        for (auto &v : W.vehicles) v.hasBox = false;
        for (auto &v : W.vehicles) {
            if (wantTruck ? v.type == "truck" : v.type == "apc") { v.hasBox = true; break; }
        }
    }
    // 军官
    std::vector<Vehicle *> cands;
    for (auto &v : W.vehicles) if (v.troopPlan > 0) cands.push_back(&v);
    if (!cands.empty()) W.officerVeh = cands[(size_t)(RNG.next() * cands.size()) % cands.size()];
    else if (!W.vehicles.empty()) W.officerVeh = &W.vehicles[0];
}

bool way_point_at(float d, float &ox, float &oy, float &oa) {
    float acc = 0;
    for (size_t i = 0; i + 1 < CONVOY_WAY.size(); ++i) {
        const WayPt &a = CONVOY_WAY[i], &b = CONVOY_WAY[i + 1];
        const float seg = distf(a.x, a.y, b.x, b.y);
        if (acc + seg >= d) {
            const float t = seg > 0 ? (d - acc) / seg : 0;
            ox = lerpf(a.x, b.x, t); oy = lerpf(a.y, b.y, t);
            oa = std::atan2(b.y - a.y, b.x - a.x);
            return true;
        }
        acc += seg;
    }
    if (CONVOY_WAY.empty()) { ox = 0; oy = 0; oa = 3.141592653589793f; return false; }
    const WayPt &last = CONVOY_WAY.back();
    ox = last.x; oy = last.y; oa = 3.141592653589793f;
    return true;
}

bool convoy_pos(const Vehicle &v, float progress, float &ox, float &oy) {
    int vi = 0;
    for (size_t i = 0; i < W.vehicles.size(); ++i) if (&W.vehicles[i] == &v) { vi = (int)i; break; }
    float d = progress - (float)((int)W.convoyOrder.size() - 1 - vi) * 132.0f;
    if (d < 0) d = 0;
    float a = 0;
    return way_point_at(d, ox, oy, a);
}

void update_evac_marker() {
    const EvacPoint &p = W.bridgeAlive ? EVAC_DEFAULT : EVAC_ALT;
    W.evac = p;
}

} // namespace va
