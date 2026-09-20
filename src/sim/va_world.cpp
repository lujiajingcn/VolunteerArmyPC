// VolunteerArmyPC —— 世界状态 / 单位工厂 / 车队 / 地形与视线（对应网页版第二章）
#include "sim/va_world.h"

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
static const RosterDef PLAYER_DEF{ "player", "你（队长）", "队长", "1组", "rifle", false, false, {} };

void init_world(uint32_t seed) {
    RNG.reseed(seed);
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
    W.bridgeAlive = true; W.evac = EVAC_DEFAULT; W.evacArmed = false;
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

    // 玩家 + 10 名队友
    float px = 872, py = 872;
    recommend_of("player", px, py);
    Unit *pl = make_unit(PLAYER_DEF, px, py, Team::Ally, true);
    pl->facing = -3.141592653589793f / 2.0f;
    W.player = pl;

    for (const auto &r : ROSTER) {
        float rx = 880, ry = 900;
        recommend_of(r.id, rx, ry);
        Unit *u = make_unit(r, rx + rr(-8, 8), ry + rr(-8, 8), Team::Ally, false);
        u->hasHome = true; u->homePos = { rx, ry };
    }

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

    // 车队顺序随机
    const float orderRoll = RNG.next();
    if (orderRoll < 0.62f)      W.convoyOrder = { "jeep", "apc", "tank", "truck", "apc", "jeep" };
    else if (orderRoll < 0.82f) W.convoyOrder = { "jeep", "tank", "apc", "truck", "apc", "jeep" };
    else                        W.convoyOrder = { "apc", "jeep", "apc", "tank", "truck", "jeep" };

    W.infantryTotal = ri(10, 16);
    W.reinforceT = CFG.reinforceAt + std::round(rr(-30, 30));

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
    if (W.boxWhere != "officer") {
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
