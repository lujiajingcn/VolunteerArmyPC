// VolunteerArmyPC —— 队友 AI（感知 → 决策 → 行动 → 通讯）
// 对应网页版 logic_ref.js 1734~1989 行（第五章：队友状态机）
#include "sim/va_world.h"
#include "sim/va_campaign.h"

#include <algorithm>
#include <cmath>

namespace va {

// ------------------------------------------------------------------ 移动
void move_step(Unit *u, float dt, float speedMul) {
    if (!u->hasMoveGoal) { u->moving = false; return; }
    const Vec2 g = u->moveGoal;
    const float d = distf(u->x, u->y, g.x, g.y);
    if (d < 12) { u->moving = false; u->arrived = true; u->hasMoveGoal = false; return; }
    float sp = (u->team == Team::Ally ? 46.0f : 44.0f) * (speedMul != 0 ? speedMul : 1.0f);
    if (u->suppression > 60) sp *= 0.72f;
    if (u->reloadT > 0) sp *= 0.94f;
    if (in_river(u->x, u->y) && !on_bridge(u->x, u->y)) sp *= 0.5f;
    const float spd = d < 40 ? sp * 0.6f : sp;
    const float a = steer_angle(u, g.x, g.y);
    const float nx = u->x + std::cos(a) * spd * dt, ny = u->y + std::sin(a) * spd * dt;
    const bool stuck = prop_blocked(u->x, u->y, 6);   // 若已被卡在车体/掩体内，允许先走出来
    /* 逐帧步长只有 ~2.6px，却一直用「半径 6 的碰撞圈」判否 ——
       只要目的地仍然落在圈里，这一步就被否掉，而 2.6px 根本不够跳出圈外，
       于是队员会被永久焊在掩体外沿（实测铁头抱着「取密码箱」任务在岩石边钉了 330 秒，
       箱子就在 900px 外躺着没人去捡）。逐帧步进改用 2px 间隙：能贴着掩体滑行，
       也走不进石头里（最小掩体半径 15px，一步 2.6px 跨不过去）。 */
    auto free_xy = [&](float x, float y) { return passable(x, y) && (stuck || !prop_blocked(x, y, 2)); };
    bool moved = false;
    if (free_xy(nx, u->y)) { u->x = nx; moved = true; }
    if (free_xy(u->x, ny)) { u->y = ny; moved = true; }
    u->moving = moved;
    if (!u->aiming) u->facing = a;
    u->wobble += dt * 6;
}

float steer_angle(Unit *u, float tx, float ty) {
    const float base = std::atan2(ty - u->y, tx - u->x);
    const float offs[9] = { 0, 0.45f, -0.45f, 0.95f, -0.95f, 1.5f, -1.5f, 2.2f, -2.2f };
    for (float off : offs) {
        const float a = base + off;
        const float nx = u->x + std::cos(a) * 26, ny = u->y + std::sin(a) * 26;
        if (passable(nx, ny) && !prop_blocked(nx, ny, 7)) return a;
    }
    return base + 3.141592653589793f;
}

bool nearest_cover_spot_near(const Unit &p, const Unit &u, float &ox, float &oy) {
    float a = std::atan2(u.y - p.y, u.x - p.x);
    if (a == 0) a = RNG.next() * 6.28f;
    const float offs[10] = { 0, 0.5f, -0.5f, 1.0f, -1.0f, 1.6f, -1.6f, 2.4f, -2.4f, 3.14f };
    for (float off : offs) {
        const float ang = a + off;
        const float x = p.x + std::cos(ang) * 52, y = p.y + std::sin(ang) * 52;
        if (passable(x, y) && !prop_blocked(x, y, 6)) { ox = x; oy = y; return true; }
    }
    ox = p.x + rr(-40, 40); oy = p.y + rr(-40, 40);
    return false;   // 降级：仍然给出一个位置
}

// ------------------------------------------------------------------ 队友主循环
void update_ally(Unit *u, float dt) {
    if (u->dead) return;
    u->stateT += dt;
    u->hitFlash = std::max(0.0f, u->hitFlash - dt);
    u->aimT += dt;

    /* ---------- 失能 / 倒地 ---------- */
    if (u->downed) {
        u->downTimer -= dt;
        if (u->draggedBy && !u->draggedBy->dead && !u->draggedBy->downed) {
            Unit *d = u->draggedBy;
            const float t = std::min(1.0f, dt * 3);
            u->x = lerpf(u->x, d->x - std::cos(d->facing) * 14, t);
            u->y = lerpf(u->y, d->y - std::sin(d->facing) * 14, t);
        }
        if (u->downTimer <= 0) kill_unit(u, nullptr);
        else if (u->downTimer < 12 && u->speakCd <= 0) { u->speakCd = 8; say(u->name, "我快撑不住了……", "no"); }
        return;
    }

    /* ---------- 计时器 ---------- */
    u->fireCd -= dt; u->burstCd -= dt; u->grenadeCd -= dt;
    u->speakCd -= dt; u->reportCd -= dt; u->secReport -= dt;
    if (u->reloadT > 0) { u->reloadT -= dt; if (u->reloadT <= 0) finish_reload(u); }
    if (u->obeyBoost > 0) u->obeyBoost = std::max(0.0f, u->obeyBoost - dt * 0.025f);
    u->suppression = std::max(0.0f, u->suppression - dt * (u->holdPosition ? 13.0f : 9.5f));
    if (u->stateT > 2.5f && u->morale < 100 && u->state != "恐慌")
        u->morale = clampf(u->morale + dt * 1.4f, 0, 100);

    /* ---------- 感知 ---------- */
    u->aiT -= dt;
    if (u->aiT <= 0) { u->aiT = 0.18f + RNG.next() * 0.2f; perceive(u); }
    const Target enemy = u->seen;

    /* ---------- 延迟指令 ---------- */
    if (u->hasPendingCmd) {
        u->pendingT -= dt;
        if (u->pendingT <= 0) { apply_order(u, u->pendingCmd); u->hasPendingCmd = false; }
    }

    /* ---------- 恐慌 ---------- */
    if (u->morale < 20 && u->state != "恐慌" && RNG.next() < 1.2f * dt) {
        set_state(u, "恐慌"); say(u->name, pick_cstr(REPLIES.panic), "no");
    }
    if (u->state == "恐慌") {
        if (u->morale > 38) set_state(u, "警戒");
        else {
            if (!u->hasMoveGoal || u->arrived) {
                const float a = std::atan2(u->y - POINTS[6].y, u->x - POINTS[6].x) + rr(-0.6f, 0.6f);
                u->moveGoal = { u->x + std::cos(a) * 160, u->y + std::sin(a) * 160 };
                u->hasMoveGoal = true;
            }
            move_step(u, dt, 1.35f);
            if (enemy.ok() && RNG.next() < 0.4f) fire_at(u, enemy, dt);
            return;
        }
    }

    /* ---------- 专属任务：搬运密码箱 / 炸桥 ---------- */
    if (u->boxTask) {
        if (!W.box.active || W.box.taken) { u->boxTask = false; u->hasMoveGoal = false; }
        else {
            u->moveGoal = { W.box.x, W.box.y }; u->hasMoveGoal = true;
            move_step(u, dt, 1.15f);
            if (distf(u->x, u->y, W.box.x, W.box.y) < 26) {
                W.box.taken = true; W.box.by = u; u->carryBox = true; u->boxTask = false;
                W.stats.boxTaken = true;
                W.evacArmed = true;                       // 拿到密码箱 → 进入撤离阶段
                say(u->name, "拿到密码箱了！", "ok"); toast("密码箱已到手，全队准备撤离");
            }
            /* 这里不再 return：取箱的人必须能边走边打。
               早期一取到任务就整段跳过战斗逻辑，结果他端着步枪一言不发地走进车队的火网，
               在离箱子 58px 的地方被车顶机枪打死 —— 箱子就躺在脚边，谁也拿不到。 */
        }
    }
    if (u->bridgeTask) {
        if (!W.bridgeAlive) { u->bridgeTask = false; }
        else {
            u->moveGoal = { POINTS[2].x, POINTS[2].y + 40 }; u->hasMoveGoal = true;
            move_step(u, dt, 1.1f);
            if (distf(u->x, u->y, POINTS[2].x, POINTS[2].y) < 70) {
                u->plantT += dt;
                u->aiming = true;
                if (u->plantT > 8) {
                    W.bridgeAlive = false; W.stats.bridgeBlown = true; u->bridgeTask = false;
                    explosion(POINTS[2].x, POINTS[2].y, 120, 60, Team::None, "bridge");
                    update_evac_marker();
                    say(u->name, "桥炸了！改从南侧树林撤离", "sys");
                    toast("桥梁已摧毁 · 撤离点改为 E 南侧树林");
                } else if (std::floor(u->plantT) != std::floor(u->plantT - dt)) {
                    say(u->name, "安放炸药 " + std::to_string((int)std::floor(u->plantT)) + "/8", "ok");
                }
            }
            return;
        }
    }
    /* 炸水库（内外加山）。流程照抄炸桥：走到坝上 → 安放炸药 8 秒 → 起爆。
       为什么**必须由人跑过去安放**而不是"远程一按就炸"：史实里这一炸是 5 连
       自己跑到坝上、自断退路换来的；做成一键按钮，那个两难就消失了。 */
    if (u->damTask) {
        if (LV.damBlown) { u->damTask = false; }
        else if (!LV.damX && !LV.damY) { u->damTask = false; }
        else {
            u->moveGoal = { LV.damX, LV.damY }; u->hasMoveGoal = true;
            move_step(u, dt, 1.1f);
            /* 到达判定必须**把坝体自己的半径算进去**：坝是一堵 150 宽的墙，
               人会被它挡在外面，离中心最近也有 ~75 单位 ——
               照抄炸桥那个 70 的话，条件是永远不可能成立的（实测派工 3 人、
               安放进度恒为 0，就是这个原因）。 */
            if (distf(u->x, u->y, LV.damX, LV.damY) < LV.damR + 80.0f) {
                u->plantT += dt;
                u->aiming = true;
                if (u->plantT > 8) {
                    u->damTask = false;
                    LV.damByCharge = true;    // 记清"是人工安放的"（见 damByCharge 注释）
                    for (auto &p : W.props) {
                        if (p.dam && !p.destroyed) { p.destroyed = true; chain_barrel(p); break; }
                    }
                } else if (std::floor(u->plantT) != std::floor(u->plantT - dt)) {
                    say(u->name, "安放炸药 " + std::to_string((int)std::floor(u->plantT)) + "/8", "ok");
                }
            }
            return;
        }
    }
    /* ---------- 救援 ---------- */
    if (u->rescueTarget && (u->rescueTarget->dead || !u->rescueTarget->downed)) {
        u->rescueTarget = nullptr; u->rescueMode.clear();
        if (u->state == "救援") set_state(u, "警戒");
    }
    if (u->rescueTarget) {
        Unit *t = u->rescueTarget;
        const float d = distf(u->x, u->y, t->x, t->y);
        if (u->rescueMode == "drag") {
            if (d > 18) {
                u->moveGoal = { t->x, t->y }; u->hasMoveGoal = true;
                move_step(u, dt, 1.1f); t->draggedBy = u; return;
            }
            t->draggedBy = u;
            float cx = 0, cy = 0;
            if (find_cover(t->x, t->y, t->x + 20, t->y + 20, 70, cx, cy)) {
                if (!u->hasDragGoal) { u->dragGoal = { cx, cy }; u->hasDragGoal = true; }
                u->moveGoal = u->dragGoal; u->hasMoveGoal = true;
                t->moveGoal = u->dragGoal; t->hasMoveGoal = true;
                move_step(u, dt, 0.85f);
                const float k = std::min(1.0f, dt * 3);
                t->x = lerpf(t->x, u->x - std::cos(u->facing) * 14, k);
                t->y = lerpf(t->y, u->y - std::sin(u->facing) * 14, k);
                if (distf(u->x, u->y, cx, cy) < 20) {
                    u->hasDragGoal = false; t->draggedBy = nullptr;
                    say(u->name, t->name + "拖到掩体后了", "ok");
                    u->rescueMode.clear(); u->rescueTarget = nullptr; set_state(u, "警戒");
                }
            }
            return;
        }
        if (d > 16) {
            u->moveGoal = { t->x, t->y }; u->hasMoveGoal = true;
            move_step(u, dt, 1.2f); u->state = "救援"; return;
        }
        set_state(u, "治疗");
        u->aiming = true; t->draggedBy = nullptr;
        t->reviveT += dt / 3.2f;
        if (t->reviveT >= 1) {
            t->downed = false; t->hp = 55; t->reviveT = 0; t->suppression = 0; t->state = "警戒";
            say(u->name, t->name + "救回来了", "ok");
            for (auto *a : allies()) a->morale = clampf(a->morale + 5, 0, 100);
            u->rescueTarget = nullptr; set_state(u, "警戒");
        }
        return;
    }
    /* 治疗轻伤 */
    if (u->healTarget && u->healTarget->hp < u->healTarget->maxHp && !u->healTarget->dead) {
        Unit *t = u->healTarget;
        const float d = distf(u->x, u->y, t->x, t->y);
        if (d > 16) {
            u->moveGoal = { t->x, t->y }; u->hasMoveGoal = true;
            move_step(u, dt, 1.15f);
        } else {
            t->hp = std::min(t->maxHp, t->hp + dt * 14); u->aiming = true;
            if (t->hp >= t->maxHp) { say(u->name, t->name + "包扎好了", "ok"); u->healTarget = nullptr; set_state(u, "警戒"); }
        }
        return;
    }
    /* 补给 */
    if (u->resupplyTarget && !u->resupplyTarget->dead) {
        Unit *t = u->resupplyTarget;
        const float d = distf(u->x, u->y, t->x, t->y);
        if (d > 18) {
            u->moveGoal = { t->x, t->y }; u->hasMoveGoal = true;
            move_step(u, dt, 1.15f);
        } else {
            for (auto *a : allies()) {
                if (distf(a->x, a->y, u->x, u->y) >= 90) continue;
                a->ammo = a->wpn->ammo;
                if (a->weaponKey == "at") a->rockets = ROCKET.ammo;
                if (a->weaponKey != "at") a->grenades = std::max(a->grenades, 2);
            }
            say(u->name, "弹药补满了", "ok"); u->resupplyTarget = nullptr; set_state(u, "警戒");
        }
        return;
    }

    /* ---------- 医疗兵自动救援 ---------- */
    if (u->role == "医疗兵" && !u->rescueTarget && (!u->order.active || u->order.actId == "cancel")) {
        Unit *d = nearest_downed(u);
        if (d && distf(u->x, u->y, d->x, d->y) < 460) {
            u->rescueTarget = d; u->rescueMode.clear(); say(u->name, "我去救人！", "ok");
        }
    }
    /* ---------- 被压制 → 找掩体 ---------- */
    if (u->suppression > 62 && !u->holdPosition && u->state != "找掩体" && u->state != "撤退" && u->state != "撤离") {
        if (u->stateT > 0.8f && RNG.next() < 2.5f * dt) {
            const float tx = enemy.ok() ? enemy.tx() : u->x;
            const float ty = enemy.ok() ? enemy.ty() : u->y + 100;
            float cx = 0, cy = 0;
            if (find_cover(u->x, u->y, tx, ty, 80, cx, cy)) {
                u->coverPos = { cx, cy }; u->hasCoverPos = true;
                u->moveGoal = u->coverPos; u->hasMoveGoal = true;
                set_state(u, "找掩体");
            }
            if (u->speakCd <= 0) { u->speakCd = 6; say(u->name, "我被压住了！", "no"); }
        }
    }
    /* ---------- 自动占领掩体（无指令时） ---------- */
    if (!u->hasMoveGoal && !u->holdPosition && enemy.ok() && u->stateT > 1.2f && !u->hasCoverPos && RNG.next() < 1.6f * dt) {
        float cx = 0, cy = 0;
        if (find_cover(u->x, u->y, enemy.tx(), enemy.ty(), 78, cx, cy)) {
            u->coverPos = { cx, cy }; u->hasCoverPos = true;
            u->moveGoal = u->coverPos; u->hasMoveGoal = true;
            if (u->state != "反坦克") set_state(u, "找掩体");
        }
    }
    if (u->hasMoveGoal && u->hasCoverPos && distf(u->x, u->y, u->coverPos.x, u->coverPos.y) < 10) u->hasCoverPos = false;

    /* ---------- 通讯：发现敌人就报告 ---------- */
    if (enemy.ok() && u->reportCd <= 0 && dist_to(u->x, u->y, enemy) < 420) {
        u->reportCd = rr(9, 18);
        const std::string nm = enemy.veh() ? enemy.tname()
            : (enemy.u->officer ? "敌军军官" : (enemy.u->mg ? "机枪手" : "步兵"));
        say(u->name, nm + " 在 " + bearing_name(u->x, u->y, enemy.tx(), enemy.ty()) + "！", "ok");
    }
    if (enemy.ok()) {
        const float a = std::atan2(enemy.ty() - u->y, enemy.tx() - u->x);
        u->facing = normAng(u->facing + angDiff(a, u->facing) * std::min(1.0f, dt * 7));
        u->aiming = true;
    } else {
        u->aiming = false;
    }

    /* ---------- 开火 ---------- */
    const Target tgt = ally_target_select(u);
    u->target = tgt;
    if (tgt.ok()) {
        if (u->state == "待命" || u->state == "警戒" || u->state == "部署" || u->state == "找掩体") set_state(u, "战斗");
        if (dist_to(u->x, u->y, tgt) < u->wpn->range && !los_fire(u->x, u->y, tgt.tx(), tgt.ty())) {
            if (tgt.veh() && (u->weaponKey == "at" || u->hasCharge) && u->rockets > 0) set_state(u, "反坦克");
            fire_at(u, tgt, dt);
            if (u->grenades > 0 && !tgt.veh()) throw_grenade_auto(u, tgt);
        }
    } else if (u->state == "战斗" && !enemy.ok()) {
        if (u->stateT > 3) set_state(u, u->holdPosition ? "待命" : "警戒");
    }
    if (u->grenadeOrder) {
        u->grenadeOrder = false;
        const bool hasT = enemy.ok() || W.hasMarker;
        const float tx = enemy.ok() ? enemy.tx() : (W.hasMarker ? W.marker.x : 0);
        const float ty = enemy.ok() ? enemy.ty() : (W.hasMarker ? W.marker.y : 0);
        if (hasT && u->grenades > 0 && distf(u->x, u->y, tx, ty) < 220) {
            throw_grenade(u, tx, ty, "grenade"); say(u->name, "手雷！", "ok");
        } else {
            say(u->name, "距离太远，扔不到", "no");
        }
    }
    if (u->smokeOrder) {
        u->smokeOrder = false;
        const bool hasT = enemy.ok() || W.hasMarker;
        const float tx = enemy.ok() ? enemy.tx() : (W.hasMarker ? W.marker.x : 0);
        const float ty = enemy.ok() ? enemy.ty() : (W.hasMarker ? W.marker.y : 0);
        if (hasT && u->smokes > 0) { throw_grenade(u, tx, ty, "smoke"); say(u->name, "放烟！", "ok"); }
        else say(u->name, "我没带烟雾弹", "no");
    }

    /* ---------- 移动 ---------- */
    if (u->formationFollow) {
        Unit *p = W.player;
        float wx = 0, wy = 0;
        nearest_cover_spot_near(*p, *u, wx, wy);
        const float d = distf(u->x, u->y, p->x, p->y);
        if (d > 62 || (!u->moving && d > 48)) { u->moveGoal = { wx, wy }; u->hasMoveGoal = true; }
        else u->hasMoveGoal = false;
        move_step(u, dt, 1.05f);
    } else if (u->hasMoveGoal) {
        move_step(u, dt, (u->state == "撤离" || u->state == "撤退") ? 1.25f : 1.0f);
        if (u->arrived) {
            u->arrived = false;
            if (u->state == "找掩体") { set_state(u, "隐蔽"); if (RNG.next() < 0.5f) say(u->name, "已隐蔽", "ok"); }
            else if (u->state == "移动") set_state(u, "警戒");
        }
    } else {
        // 无指令：轻微移动 / 观察
        if (!enemy.ok() && !u->holdPosition && RNG.next() < 0.4f * dt) {
            const float a = RNG.next() * 6.28f;
            u->moveGoal = { u->x + std::cos(a) * rr(20, 50), u->y + std::sin(a) * rr(20, 50) };
            u->hasMoveGoal = true;
        }
        if (u->state == "部署" && W.t > CFG.tDeploy) set_state(u, "待命");
    }

    /* ---------- 密码箱：从旁边路过的队员顺手就捡 ---------- */
    if (!u->boxTask && W.box.active && !W.box.taken && distf(u->x, u->y, W.box.x, W.box.y) < 22) {
        W.box.taken = true; W.box.by = u; u->carryBox = true;
        W.stats.boxTaken = true; W.evacArmed = true;
        say(u->name, "密码箱到手！", "ok"); toast("密码箱已到手 · 全队撤离");
    }
    /* ---------- 撤离点判定 ---------- */
    /* 只有进入「撤离阶段」（拿到密码箱 / 收到撤退、撤离命令）后才开始计数。
       早期不加这个门，而 C 点恰好也是部署区之一 —— 开局站在那儿的队员会立刻被算成「已撤离」，
       既虚增撤离人数，也能直接凑够 6 人误判为成功。 */
    /* 「撤离」必须是活着的人走到撤离点：早期没有排除阵亡/失能者，
       而倒地的人常常就倒在撤离点附近 —— 于是「小队全灭」也能凑出 evacCount>=6，
       与 checkEnd 里 allIn 的判定（只数活人）自相矛盾，还能凭空凑齐胜利条件。 */
    if (!u->dead && !u->downed && W.evacArmed && !u->evacuated &&
        distf(u->x, u->y, W.evac.x, W.evac.y) < 95) {
        u->evacuated = true; W.stats.evacCount++;
        say(u->name, "已到撤离点！", "ok");
        check_end();
    }
    /* 带密码箱撤离（同样只有活着走到撤离点才算） */
    if (u->carryBox && u->evacuated && !u->dead && !u->downed) W.stats.boxEvacuated = true;
}

} // namespace va
