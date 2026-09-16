// VolunteerArmyPC —— 指令下发：呼号 → 队员；服从度 → 立即/延迟/拒绝
// 对应网页版 logic_ref.js 1016~1388 行（第三章）
#include "sim/va_world.h"

#include <algorithm>
#include <cmath>

namespace va {

// ------------------------------------------------------------- 方位描述
// 对应 bearingName(from, to)：把方位角折算成「N 点方向」
std::string bearing_name(float fx, float fy, float tx, float ty) {
    float brg = std::fmod(std::atan2(tx - fx, -(ty - fy)) * 180.0f / 3.141592653589793f + 360.0f, 360.0f);
    int h = (int)std::lround(brg / 30.0f) % 12;
    if (h == 0) h = 12;
    return std::to_string(h) + " 点方向";
}

// ------------------------------------------------------------- 服从度
float calc_obey(const Unit &u, const std::string &actId) {
    float o = u.obeyBase + (u.morale - 55) / 220.0f;
    /* 对应 JS：nearestEnemyAt(x, y, 'ally', x => !x.dead) 后取距离。
       该 filter 对载具恒为真，所以载具会参与 —— enemy_threat_dist 正是这个语义。 */
    const float d = enemy_threat_dist(u.x, u.y, Team::Ally);
    float danger = 0;
    if (d < 220) danger += 0.09f;
    if (d < 110) danger += 0.13f;
    if (u.suppression > 55) danger += 0.17f;
    if (u.hp < 45) danger += 0.10f;
    const std::string &id = actId;
    if (id == "takeCover" || id == "ceasefire" || id == "retreat" || id == "retreatTo" || id == "evac")
        danger *= 0.3f;
    const float supPen = (u.suppression / 100.0f) * 0.34f;
    o = o - danger - supPen + u.obeyBoost;
    if (u.role == "医疗兵" && (id == "rescue" || id == "heal" || id == "drag")) o += 0.30f;
    if (u.role == "反坦克手" && (id == "atTank" || id == "atAPC" || id == "rocket")) o += 0.25f;
    if (u.group == "火力组" && (id == "fire" || id == "suppress" || id == "focusFire")) o += 0.18f;
    if (u.group == "支援组" && id == "resupply") o += 0.3f;
    if (u.role == "爆破手" && (id == "detonate" || id == "blowBridge")) o += 0.35f;
    if (u.state == "恐慌") o -= 0.35f;
    if (u.downed || u.dead) o = 0;
    return clampf(o, 0, 1);
}

// ------------------------------------------------------------- 目标解析
std::vector<Unit *> resolve_targets(const ParsedCmd &cmd) {
    std::vector<Unit *> out;
    auto add = [&](const std::string &id) {
        Unit *u = ally_by_id(id);
        if (u) out.push_back(u);
    };
    const std::string &kind = cmd.csKind;
    const std::string &id = cmd.csId;
    if (kind.empty()) {
        Unit *a = nearest_ally_to(*W.player);
        if (a) out.push_back(a);
    } else if (kind == "all") {
        for (auto &u : W.units) if (u.team == Team::Ally) out.push_back(&u);
    } else if (kind == "group") {
        for (const auto &g : GROUPS) {
            if (id != g.name) continue;
            for (const char *mid : g.members) add(mid);
        }
    } else if (kind == "role") {
        for (const auto &rc : ROLE_CALL) {
            if (id != rc.role) continue;
            for (const char *mid : rc.ids) add(mid);
        }
    } else if (kind == "member" && id != "auto") {
        add(id);
    } else {
        Unit *a = nearest_ally_to(*W.player);
        if (a) out.push_back(a);
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](Unit *u) { return !u || u->dead; }), out.end());
    return out;
}

Unit *nearest_ally_to(const Unit &p) {
    Unit *best = nullptr;
    float bd = 1e9f;
    for (auto &u : W.units) {
        if (u.team != Team::Ally || u.dead || u.downed || &u == &p) continue;
        const float d = dist2f(u.x, u.y, p.x, p.y);
        if (d < bd) { bd = d; best = &u; }
    }
    return best;
}

/* 目标检索：按「打 X」的类型与方向修饰 */
Target pick_target_by_type(Unit *u, const std::string &type, float dirBearing, bool hasDir) {
    Target best;
    float bs = -1e9f;
    auto consider = [&](const Target &o) {
        if (o.gone()) return;
        const float d = dist_to(u->x, u->y, o);
        if (d > 900) return;
        float s = 1000.0f - d;
        if (hasDir) {
            const float brg = std::fmod(std::atan2(o.tx() - u->x, -(o.ty() - u->y)) * 180.0f / 3.141592653589793f + 360.0f, 360.0f);
            const float db = std::fabs(std::fmod(brg - dirBearing + 540.0f, 360.0f) - 180.0f);
            s -= db * 3.0f;
        } else {
            if (o.u && o.u->target.ok() && o.u->target.is_player()) s += 160.0f;
            const float th = (o.u && o.u->target.ok() && o.u->target.u && o.u->target.u->team == Team::Ally) ? 200.0f : 0.0f;
            s += th;
            if (o.u && (W.t - o.u->lastHurtT) < 6.0f) s += 80.0f;
        }
        if (s > bs) { bs = s; best = o; }
    };
    if (type == "inf" || type == "mg" || type == "officer") {
        for (auto &e : W.units) {
            if (e.team != Team::Enemy || e.dead) continue;
            if (e.mount) continue;
            if (type == "mg" && !e.mg) continue;
            if (type == "officer" && !e.officer) continue;
            consider(Target(&e));
        }
    } else {
        for (auto &v : W.vehicles) {
            if (v.destroyed || v.team != Team::Enemy) continue;
            const std::string nt = (v.type == "apc") ? "apc" : v.type;
            if (nt != type) continue;
            consider(Target(&v));
        }
    }
    return best;
}

Unit *nearest_downed(Unit *u) {
    Unit *best = nullptr;
    float bd = 1e9f;
    for (auto &d : W.units) {
        if (d.team != Team::Ally || !d.downed || d.dead) continue;
        const float v = dist2f(u->x, u->y, d.x, d.y);
        if (v < bd) { bd = v; best = &d; }
    }
    return best;
}

Target nearest_enemy_to_marker(Unit *u) {
    Target best;
    float bd = 1e9f;
    std::vector<Target> cand;
    for (auto &x : W.units) if (x.team == Team::Enemy && !x.dead && !x.mount) cand.push_back(Target(&x));
    for (auto &v : W.vehicles) if (v.team == Team::Enemy && !v.destroyed) cand.push_back(Target(&v));
    for (const auto &c : cand) {
        const float d = W.hasMarker ? dist2f(c.tx(), c.ty(), W.marker.x, W.marker.y)
                                    : dist2f(c.tx(), c.ty(), u->x, u->y);
        if (d < bd) { bd = d; best = c; }
    }
    return best;
}

void detonate_mines(Unit *u) {
    bool any = false;
    for (auto &m : W.mines) {
        if (m.used) continue;
        Vehicle *near = nullptr;
        for (auto *v : enemy_vehicles()) if (distf(v->x, v->y, m.x, m.y) < 60) near = v;
        if (near) {
            m.used = true; any = true; W.stats.minesUsed++;
            explosion(m.x, m.y, 70, 150, Team::Ally, "mine");
            damage_vehicle(near, 240, "mine", u);
        }
    }
    if (any) {
        for (auto &a : W.units) if (a.team == Team::Ally) a.morale = clampf(a.morale + 6, 0, 100);
    } else {
        say(u->name, "雷区里还没有车，等一下", "ok");
    }
}

// ------------------------------------------------------------- 指令落地
void apply_order(Unit *u, const ParsedCmd &cmd) {
    const std::string &id = cmd.actId;
    u->order = Order::from(cmd, W.t);
    u->hasPendingCmd = false;
    Unit *th = nearest_enemy_at(u->x, u->y, Team::Ally);

    if (id == "advance") {
        Vec2 goal;
        if (cmd.hasLoc) goal = { cmd.locX, cmd.locY };
        else if (W.hasMarker) goal = W.marker;
        else if (th) goal = { th->x, th->y };
        else goal = { u->x, u->y - 120 };
        u->moveGoal = goal; u->hasMoveGoal = true;
        u->holdPosition = false; u->state = "移动";
    } else if (id == "gotoPoint" || id == "retreatTo" || id == "enterBuilding") {
        if (cmd.hasLoc) {
            u->moveGoal = { cmd.locX + rr(-40, 40), cmd.locY + rr(-40, 40) };
            u->hasMoveGoal = true; u->holdPosition = false;
            u->state = (id == "retreatTo") ? "撤退" : "移动";
        } else if (W.hasMarker) {
            u->moveGoal = W.marker; u->hasMoveGoal = true;
            u->holdPosition = false;
            u->state = (id == "retreatTo") ? "撤退" : "移动";
        }
    } else if (id == "fallback") {
        const float tx = th ? th->x : u->x, ty = th ? th->y : u->y;
        const float back = std::atan2(u->y - ty, u->x - tx);
        u->moveGoal = { u->x + std::cos(back) * 130, u->y + std::sin(back) * 130 };
        u->hasMoveGoal = true;
        u->state = "移动";
    } else if (id == "retreat") {
        W.evacArmed = true;
        u->moveGoal = { W.evac.x + rr(-50, 50), W.evac.y + rr(-50, 50) };
        u->hasMoveGoal = true;
        u->state = "撤退"; u->fireMode = "free";
    } else if (id == "evac") {
        W.evacArmed = true;
        u->moveGoal = { W.evac.x + rr(-40, 40), W.evac.y + rr(-40, 40) };
        u->hasMoveGoal = true;
        u->state = "撤离";
    } else if (id == "moveLeft" || id == "moveRight") {
        const float base = th ? std::atan2(th->y - u->y, th->x - u->x) : u->facing;
        const float a = base + (id == "moveLeft" ? -3.141592653589793f / 2 : 3.141592653589793f / 2);
        u->moveGoal = { u->x + std::cos(a) * 120, u->y + std::sin(a) * 120 };
        u->hasMoveGoal = true;
        u->state = "移动";
    } else if (id == "spread") {
        Unit *near = nullptr; float best = 1e18f;
        for (auto &x : W.units) {
            if (x.team != Team::Ally || &x == u || x.dead) continue;
            const float d = dist2f(x.x, x.y, u->x, u->y);
            if (d < best) { best = d; near = &x; }
        }
        if (near) {
            const float a = std::atan2(u->y - near->y, u->x - near->x);
            u->moveGoal = { u->x + std::cos(a) * 80, u->y + std::sin(a) * 80 };
        } else {
            u->moveGoal = { u->x + std::cos(u->facing) * 70, u->y + std::sin(u->facing) * 70 };
        }
        u->hasMoveGoal = true;
        u->state = "移动";
    } else if (id == "flank") {
        Target t = u->target.ok() ? u->target : Target(th);
        if (t.ok()) {
            const float base = std::atan2(t.ty() - u->y, t.tx() - u->x);
            u->flankPt = { t.tx() + std::cos(base + 3.141592653589793f / 2) * 220,
                           t.ty() + std::sin(base + 3.141592653589793f / 2) * 220 };
            u->hasFlankPt = true;
            u->moveGoal = u->flankPt; u->hasMoveGoal = true; u->state = "移动";
        } else {
            u->moveGoal = { u->x + std::cos(u->facing) * 140, u->y + std::sin(u->facing) * 140 };
            u->hasMoveGoal = true; u->state = "移动";
        }
    } else if (id == "takeCover" || id == "setup") {
        const float tx = th ? th->x : u->x, ty = th ? th->y : u->y + 100;
        float cx = 0, cy = 0;
        if (find_cover(u->x, u->y, tx, ty, 90, cx, cy)) { u->coverPos = { cx, cy }; u->hasCoverPos = true; }
        else { u->coverPos = { u->x, u->y }; u->hasCoverPos = true; }
        /* 玩家单位是人在操控，不会自己走过去。早期这里一视同仁地置「找掩体」，
           结果玩家收到「全体，隐蔽」后 HUD 会永久红着「找掩体」，
           而人站在原地看着那个状态也很困惑。玩家直接进「隐蔽」，
           coverPos 保留为地图上的建议位置标记。 */
        if (u->isPlayer) { u->hasMoveGoal = false; u->state = "隐蔽"; }
        else { u->moveGoal = u->coverPos; u->hasMoveGoal = true; u->state = "找掩体"; }
    } else if (id == "hold") {
        u->holdPosition = true; u->hasMoveGoal = false; u->state = "待命";
    } else if (id == "followMe") {
        u->formationFollow = true; u->followTarget = W.player; u->hasMoveGoal = false; u->state = "移动";
    } else if (id == "fire" || id == "freeFire") {
        u->fireMode = "free"; u->holdPosition = false;
        if (u->state == "隐蔽" || u->state == "待命") u->state = "战斗";
    } else if (id == "ceasefire") {
        u->fireMode = "hold"; u->state = "隐蔽"; u->holdPosition = true; u->hasMoveGoal = false;
    } else if (id == "focusFire") {
        const Target t = nearest_enemy_to_marker(u);
        if (t.ok()) {
            u->focusTarget = t; u->fireMode = "free";
            if (u->state == "隐蔽" || u->state == "待命") u->state = "战斗";
        }
    } else if (id == "suppress") {
        u->fireMode = "suppress";
        if (th) u->target = Target(th);
        if (u->state == "隐蔽" || u->state == "待命" || u->state == "警戒") u->state = "战斗";
    } else if (id == "coverMe") {
        Unit *t = nearest_enemy_at(W.player->x, W.player->y, Team::Ally);
        if (t) {
            u->target = Target(t); u->fireMode = "suppress";
            if (u->state != "反坦克") u->state = "战斗";
        }
    } else if (id == "atTank" || id == "atAPC" || id == "atJeep" || id == "atTruck" ||
               id == "atInf" || id == "atMG" || id == "atOfficer" || id == "rocket") {
        const std::string type = (id == "rocket") ? "tank" : cmd.typeTarget;
        const Target t = pick_target_by_type(u, type, cmd.bearing, cmd.hasDir);
        u->preferType = type;
        if (t.ok()) {
            u->target = t; u->fireMode = "free";
            const std::string tt = t.ttype();
            const bool isVeh = (tt == "tank" || tt == "apc" || tt == "jeep" || tt == "truck");
            if (isVeh && (u->weaponKey == "at" || u->hasCharge)) { u->state = "反坦克"; u->atTarget = t; }
            else if (u->state == "隐蔽" || u->state == "待命" || u->state == "警戒") u->state = "战斗";
        }
    } else if (id == "grenade") {
        u->grenadeOrder = true;
    } else if (id == "smoke") {
        u->smokeOrder = true;
    } else if (id == "rescue") {
        Unit *t = nullptr;
        for (const auto &mid : cmd.mentioned) {
            Unit *d = unit_by_id(mid);
            if (d && d->downed && !d->dead) { t = d; break; }
        }
        if (!t) t = nearest_downed(u);
        if (t) { u->rescueTarget = t; u->state = "救援"; u->moveGoal = { t->x, t->y }; u->hasMoveGoal = true; }
    } else if (id == "drag") {
        Unit *t = nullptr;
        for (const auto &mid : cmd.mentioned) {
            Unit *x = unit_by_id(mid);
            if (x && x->downed) { t = x; break; }
        }
        if (!t) t = nearest_downed(u);
        if (t) {
            u->rescueTarget = t; u->rescueMode = "drag"; u->state = "救援";
            u->moveGoal = { t->x, t->y }; u->hasMoveGoal = true;
        }
    } else if (id == "heal") {
        Unit *t = nullptr;
        for (const auto &mid : cmd.mentioned) {
            Unit *x = unit_by_id(mid);
            if (x && x->team == Team::Ally && x->hp < x->maxHp) { t = x; break; }
        }
        if (!t) {
            float bd = 1e18f;
            for (auto &x : W.units) {
                if (x.team != Team::Ally || x.dead || x.hp >= x.maxHp * 0.8f) continue;
                const float d = dist2f(x.x, x.y, u->x, u->y);
                if (d < bd) { bd = d; t = &x; }
            }
        }
        if (t) { u->healTarget = t; u->state = "治疗"; u->moveGoal = { t->x, t->y }; u->hasMoveGoal = true; }
    } else if (id == "resupply") {
        Unit *t = nullptr; float best = 1e18f;
        for (auto &x : W.units) {
            if (x.team != Team::Ally || x.dead || x.downed || &x == u) continue;
            const float r = (float)x.ammo / (float)(x.wpn ? x.wpn->ammo : 1);
            if (r < best) { best = r; t = &x; }
        }
        if (t) { u->resupplyTarget = t; u->state = "移动"; u->moveGoal = { t->x, t->y }; u->hasMoveGoal = true; }
    } else if (id == "grabBox") {
        if (W.box.active && !W.box.taken && !W.box.by) {
            u->boxTask = true; u->state = "移动";
            u->moveGoal = { W.box.x, W.box.y }; u->hasMoveGoal = true;
        }
    } else if (id == "detonate") {
        detonate_mines(u);
    } else if (id == "blowBridge") {
        u->bridgeTask = true; u->state = "移动";
        u->moveGoal = { POINTS[2].x, POINTS[2].y };   // C 点
        u->hasMoveGoal = true;
    } else if (id == "cancel") {
        u->order.active = false;
        u->hasMoveGoal = false; u->hasCoverPos = false;
        u->holdPosition = false; u->fireMode = "free";
        u->preferType.clear(); u->focusTarget = Target();
        u->formationFollow = false; u->boxTask = false; u->bridgeTask = false;
        u->state = "待命";
    } else {
        u->state = "警戒";
    }
}

// ------------------------------------------------------------- 前置检查
Precheck precheck(Unit *u, const ParsedCmd &cmd) {
    Precheck r;
    const std::string &id = cmd.actId;
    const ActKind kind = action_by_id(id) ? action_by_id(id)->kind : ActKind::Misc;
    if (id == "rescue" || id == "drag") {
        if (!nearest_downed(u)) { r.ok = false; r.msg = "没有人需要救"; return r; }
    }
    if (id == "atTank" || id == "atAPC") {
        if (u->weaponKey == "at" && u->rockets <= 0) { r.ok = false; r.msg = "火箭弹打光了"; return r; }
    }
    if ((id == "atTank" || id == "atAPC" || id == "atJeep" || id == "atTruck") && u->weaponKey == "at") {
        const Target t = pick_target_by_type(u, cmd.typeTarget, cmd.bearing, cmd.hasDir);
        if (!t.ok()) { r.ok = false; r.msg = "视野里没有目标"; return r; }
        if (u->rockets <= 0) { r.ok = false; r.msg = "火箭弹打光了"; return r; }
    }
    if (id == "grenade" && u->grenades <= 0) { r.ok = false; r.msg = "手雷用完了"; return r; }
    if (id == "smoke" && u->smokes <= 0) { r.ok = false; r.msg = "没有烟雾弹"; return r; }
    if (id == "grabBox" && (!W.box.active || W.box.taken)) { r.ok = false; r.msg = "密码箱不在地上了"; return r; }
    if (u->ammo <= 0 && (kind == ActKind::Combat || kind == ActKind::Atk)) { r.ok = false; r.msg = "没弹药了"; return r; }
    return r;
}

// ------------------------------------------------------------- 状态汇报
std::string report_line(Unit *u, const std::string &id) {
    const std::string st = u->downed ? "受伤倒地"
        : u->hp > 85 ? "未受伤"
        : u->hp > 55 ? "轻伤"
        : u->hp > 30 ? "受伤" : "重伤";
    if (id == "reportAmmo") {
        std::string s = "弹药还有 " + std::to_string((int)std::lround(std::max(0.0f, (float)u->ammo))) + " 发";
        if (u->weaponKey == "at") s += "，火箭弹 " + std::to_string(u->rockets) + " 发";
        return s;
    }
    if (id == "reportPos") {
        const char *near = "公路南侧"; float bd = 1e9f;
        for (int i = 0; i < 7; ++i) {
            const float d = distf(u->x, u->y, POINTS[i].x, POINTS[i].y);
            if (d < bd) { bd = d; near = POINTS[i].name; }
        }
        return std::string(near) + "附近";
    }
    if (id == "reportCas") {
        auto d = downed_allies();
        int dead = 0;
        for (auto &x : W.units) if (x.team == Team::Ally && x.dead) dead++;
        if (d.empty() && dead == 0) return "无人伤亡";
        std::string s;
        if (dead) s += std::to_string(dead) + " 人阵亡，";
        if (!d.empty()) {
            for (size_t i = 0; i < d.size(); ++i) { if (i) s += "、"; s += d[i]->name; }
            s += "受伤";
        } else {
            s += "无人需要救援";
        }
        return s;
    }
    if (id == "reportContact") {
        std::vector<std::string> list;
        for (auto *v : enemy_vehicles()) if (can_see(*u, v->x, v->y))
            list.push_back(v->name + " 在 " + bearing_name(u->x, u->y, v->x, v->y));
        std::vector<Unit *> ei;
        for (auto *e : enemies()) if (!e->mount && can_see(*u, e->x, e->y)) ei.push_back(e);
        if (ei.size() > 4) ei.resize(4);
        if (!ei.empty()) {
            std::string s = "步兵 ";
            for (size_t i = 0; i < ei.size(); ++i) { if (i) s += "、"; s += bearing_name(u->x, u->y, ei[i]->x, ei[i]->y); }
            list.push_back(s);
        }
        if (list.empty()) return "视野内没有敌人";
        if (list.size() > 3) list.resize(3);
        std::string s;
        for (size_t i = 0; i < list.size(); ++i) { if (i) s += "；"; s += list[i]; }
        return s;
    }
    if (id == "reportRemain") {
        int inf = 0, veh = 0;
        for (auto *e : enemies()) if (!e->mount) inf++;
        for (auto *v : enemy_vehicles()) if (v->type != "truck") veh++;
        return "还剩 " + std::to_string(inf) + " 个步兵，" + std::to_string(veh) + " 辆装甲目标";
    }
    if (id == "ready") return "已就位";
    return st + "，弹药 " + std::to_string((int)std::lround(u->ammo));
}

// ------------------------------------------------------------- 指令入口
void issue_command(const ParsedCmd &cmd, bool silent) {
    if (W.over || cmd.actId.empty()) return;
    W.lastCmd = cmd; W.hasLastCmd = true;
    W.stats.cmdIssued++;
    auto targets = resolve_targets(cmd);
    const std::string &id = cmd.actId;

    /* 全局指令：起爆 / 炸桥 */
    if (id == "detonate" || id == "blowBridge") {
        Unit *holder = ally_by_id("laobai");
        if (!holder || holder->dead || holder->downed) {
            holder = nullptr;
            for (auto &x : W.units) if (x.id == "shitou" && !x.dead && !x.downed) { holder = &x; break; }
        }
        if (!holder) { say("全体", "没人能操作炸药了", "no"); return; }
        const float ob = calc_obey(*holder, id);
        if (ob < 0.3f) {
            say(holder->name, pick_cstr(REPLIES.refuse), "no");
            W.stats.cmdRefused++;
            return;
        }
        apply_order(holder, cmd);
        W.stats.cmdExec++;
        say(holder->name, id == "detonate" ? "收到，准备起爆" : "收到，我去炸桥", "ok");
        return;
    }
    if (id == "repeat") {
        if (!W.hasLastCmd || W.lastCmd.raw == cmd.raw) { say("全体", "……请把指令再说一遍", "no"); return; }
        for (auto &u : W.units) if (u.team == Team::Ally) u.obeyBoost = 0.2f;
        std::string t = "重复：" + W.lastCmd.actLabel;
        if (!W.lastCmd.csLabel.empty()) t += "（" + W.lastCmd.csLabel + "）";
        say("你", t, "cmd");
        ParsedCmd c2 = W.lastCmd;
        issue_command(c2, true);
        return;
    }
    if (id == "cancel") {
        for (auto *u : targets) { if (u->dead) continue; apply_order(u, cmd); }
        say("全体", "取消指令，返回自动状态", "sys");
        return;
    }
    if (id == "ack") { say("你", "收到", "cmd"); return; }

    /* 状态报告：即时语音回复，不改状态 */
    const ActionDef *ad = action_by_id(id);
    if (ad && ad->kind == ActKind::Report) {
        for (auto *u : targets) { if (u->dead) continue; say(u->name, report_line(u, id), "ok"); }
        W.stats.cmdExec++;
        return;
    }

    /* applyOrder 前置检查：需要满足前提条件 */
    for (auto *u : targets) {
        if (u->dead || u->downed) continue;
        const Precheck need = precheck(u, cmd);
        if (!need.ok) { say(u->name, need.msg, "no"); W.stats.cmdRefused++; continue; }
        const float ob = calc_obey(*u, id);
        u->obeyRoll = ob;
        if (ob > 0.6f) {
            apply_order(u, cmd);
            W.stats.cmdExec++;
            if (!silent) say(u->name, pick_cstr(REPLIES.ok), "ok");
        } else if (ob > 0.3f) {
            u->pendingCmd = cmd; u->hasPendingCmd = true; u->pendingT = rr(1, 3);
            say(u->name, pick_cstr(REPLIES.delay), "warn");
            W.stats.cmdRefused++;
        } else {
            say(u->name, u->state == "恐慌" ? pick_cstr(REPLIES.panic) : pick_cstr(REPLIES.refuse), "no");
            W.stats.cmdRefused++;
        }
    }
    if (targets.empty()) say("全体", "没有人响应（呼号未匹配）", "no");
}

// 解析文本 + 下发（HUD / 语音层统一入口）
ParsedCmd run_command_text(const std::string &text, const std::string &source, bool typed) {
    ParsedCmd cmd = parse_command(text, W.noise, -1, typed, source);
    if (cmd.valid()) issue_command(cmd, false);
    return cmd;
}

} // namespace va
