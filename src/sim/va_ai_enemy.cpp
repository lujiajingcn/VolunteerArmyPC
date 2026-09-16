// VolunteerArmyPC —— 敌方步兵 AI + 下车 + 增援
// 对应网页版 logic_ref.js 2073~2100、2207~2289 行（第七章：敌方部队）
#include "sim/va_world.h"

#include <algorithm>
#include <cmath>

namespace va {

void ally_centroid(float &ox, float &oy) {
    float x = 0, y = 0; int n = 0;
    for (auto &u : W.units) {
        if (u.team == Team::Ally && !u.dead && !u.downed) { x += u.x; y += u.y; n++; }
    }
    if (n) { ox = x / n; oy = y / n; }
    else { ox = POINTS[0].x; oy = POINTS[0].y; }
}

// ------------------------------------------------------------------ 下车
void dismount(Vehicle *v) {
    if (v->dismounted) return;
    v->dismounted = true;
    const int n = v->troopPlan;
    for (int i = 0; i < n; ++i) {
        const int side = RNG.next() < 0.5f ? 1 : -1;
        const float a = 3.141592653589793f / 2 * side + rr(-0.5f, 0.5f);
        const bool isOfficer = (W.officerVeh == v) && !W.officerSpawned && i == 0;
        if (isOfficer) W.officerSpawned = true;
        const bool isLmg = (i % 5 == 0);

        RosterDef def;
        def.id = "e" + v->id + "_" + std::to_string(i);
        def.name = isOfficer ? "敌军军官" : (isLmg ? "敌机枪手" : "敌步兵");
        def.role = "步兵";
        def.group = "";
        def.weapon = isLmg ? "emg" : "erifle";
        def.deputy = false; def.leader = false;

        Unit *e = make_unit(def, v->x + std::cos(a) * rr(16, 42), v->y + std::sin(a) * rr(16, 42), Team::Enemy, false);
        e->officer = isOfficer;
        e->mg = isLmg;
        e->hp = isOfficer ? 60 : (e->mg ? 42 : 34);
        e->maxHp = e->hp;
        e->morale = 72;
        e->flankRole = RNG.next() < 0.38f ? "flank" : "front";
        e->flankSide = RNG.next() < 0.5f ? 1 : -1;
        e->mount = nullptr;
    }
    say("全体", v->name + " 上步兵下车了！", "no");
}

// ------------------------------------------------------------------ 敌方主循环
void update_enemy(Unit *e, float dt) {
    if (e->dead) return;
    e->stateT += dt;
    e->hitFlash = std::max(0.0f, e->hitFlash - dt * 3);
    if (e->downed) { e->downTimer -= dt; if (e->downTimer <= 0) kill_unit(e, nullptr); return; }
    if (e->mount) return;
    e->fireCd -= dt; e->aiT -= dt; e->reportCd -= dt;
    if (e->reloadT > 0) { e->reloadT -= dt; if (e->reloadT <= 0) finish_reload(e); }
    e->suppression = std::max(0.0f, e->suppression - dt * 9.0f);
    if (e->aiT <= 0) { e->aiT = 0.22f + RNG.next() * 0.25f; perceive(e); }

    /* 士气崩溃 → 后撤 */
    if (e->morale < 18 && e->state != "撤退") { set_state(e, "撤退"); e->moraleCheck = true; }
    if (e->state == "撤退") {
        if (!e->hasMoveGoal || e->arrived) {
            const float a = std::atan2(650 - e->y, 100 - e->x) + rr(-0.5f, 0.5f);
            e->moveGoal = { e->x + std::cos(a) * 170, e->y + std::sin(a) * 170 };
            e->hasMoveGoal = true;
        }
        move_step(e, dt, 1.25f);
        if (e->seen.ok() && RNG.next() < 0.5f) fire_at(e, e->seen, dt);
        return;
    }
    const Target tgt = e->seen;
    e->target = tgt;
    if (tgt.ok()) {
        const float a = std::atan2(tgt.ty() - e->y, tgt.tx() - e->x);
        e->facing = normAng(e->facing + angDiff(a, e->facing) * std::min(1.0f, dt * 6));
    }
    float cenx = 0, ceny = 0;
    ally_centroid(cenx, ceny);
    e->pushT -= dt;
    e->holdT = std::max(0.0f, e->holdT - dt);   // 推进节拍：跃进 → 依托掩体停火 → 再跃进
    const bool nearby = tgt.ok() && dist_to(e->x, e->y, tgt) < 230;
    if (e->pushT <= 0) {
        e->pushT = rr(4, 8);
        if (e->flankRole == "flank") {
            const float ang = std::atan2(ceny - e->y, cenx - e->x);
            e->pushPt = { cenx + std::cos(ang + e->flankSide * 0.95f) * 240,
                          ceny + std::sin(ang + e->flankSide * 0.95f) * 240 };
        } else {
            e->pushPt = { cenx + rr(-70, 70), ceny + rr(-70, 70) };
        }
        e->hasPushPt = true;
    }
    if (e->suppression > 65 && !e->hasCoverPos && RNG.next() < 1.5f * dt) {
        const float tx = tgt.ok() ? tgt.tx() : e->x;
        const float ty = tgt.ok() ? tgt.ty() : e->y + 60;
        float cx = 0, cy = 0;
        if (find_cover(e->x, e->y, tx, ty, 80, cx, cy)) {
            e->coverPos = { cx, cy }; e->hasCoverPos = true;
            e->moveGoal = e->coverPos; e->hasMoveGoal = true;
        }
    }
    if (e->hasMoveGoal) {
        move_step(e, dt, 1.0f);
        if (e->arrived) { e->arrived = false; e->holdT = rr(1.2f, 3.2f); }
    } else if (e->holdT > 0) {
        /* 依托掩体停火：本拍不选新路线，保持火力输出 */
    } else if (nearby) {
        e->holdT = rr(1.5f, 4);                       // 已在交火距离内 → 停下来对射
    } else if (e->hasPushPt && distf(e->x, e->y, e->pushPt.x, e->pushPt.y) > 45) {
        e->moveGoal = e->pushPt; e->hasMoveGoal = true;
    } else if (tgt.ok() && dist_to(e->x, e->y, tgt) > 250) {
        /* 已到集结点但目标仍远 → 直接朝目标做一次跃进（否则会在集结点无限期停住） */
        e->moveGoal = { tgt.tx() + rr(-40, 40), tgt.ty() + rr(-40, 40) };
        e->hasMoveGoal = true;
    } else {
        e->holdT = rr(2, 5);
    }
    if (tgt.ok() && !e->mount) {
        if (e->state == "警戒") set_state(e, "战斗");
        fire_at(e, tgt, dt);
    } else if (e->state == "战斗") {
        set_state(e, "警戒");
    }
}

// ------------------------------------------------------------------ 增援
void spawn_reinforcement() {
    W.reinforceDone = true; W.stats.reinforceTriggered = true;
    sfx("reinforce", 0, 0, -1.0f, true);   // 远处引擎轰鸣 + 电台告警
    say("全体", "东侧出现敌方增援！两卡车步兵！", "no");
    set_alert("敌军增援到达 — 全体立即撤退！", 6);
    toast("敌方增援到达：2 卡车步兵");
    const VehicleSpec *tsp = vehicle_of("truck");
    for (int k = 0; k < 2; ++k) {
        W.vehicles.emplace_back();
        Vehicle &v = W.vehicles.back();
        v.id = "rein" + std::to_string(k);
        v.type = "truck";
        v.name = "增援卡车" + std::to_string(k + 1);
        v.team = Team::Enemy;
        v.x = 2320 + k * 140; v.y = 700; v.angle = 3.141592653589793f;
        v.speed = 26; v.baseSpeed = 26;
        v.hp = tsp->hp; v.maxHp = tsp->hp; v.spec = tsp;
        v.len = 66; v.wid = 30; v.armor = 0.34f;
        v.destroyed = false; v.burning = 0;
        v.turret = 3.141592653589793f; v.fireCd = 99; v.wpnCd = 2;
        v.capacity = 6; v.dismountT = -1; v.dismounted = false;
        v.state = "drive"; v.line = 1; v.speedMul = 1;
        v.dist = -(float)k * 140;
        v.hasBox = false; v.hitFlash = 0; v.stopT = 0;
        v.troopPlan = 6; v.isReinforcement = true; v.blockedT = 0;
    }
    W.barrage = true; W.barrageCd = 8;
    for (auto *a : allies()) a->morale = clampf(a->morale - 10, 0, 100);
}

} // namespace va
