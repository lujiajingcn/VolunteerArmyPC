// VolunteerArmyPC —— 玩家控制 + 车队行军 / 车载武器
// 对应网页版 logic_ref.js 1994~2068（第六章：玩家）、2101~2206（车队）行
#include "sim/va_world.h"
#include "sim/va_campaign.h"

#include <algorithm>
#include <cmath>

namespace va {

// ------------------------------------------------------------------ 玩家
void update_player(float dt) {
    Unit *u = W.player;
    if (!u || u->dead) return;
    u->hitFlash = std::max(0.0f, u->hitFlash - dt);
    u->suppression = std::max(0.0f, u->suppression - dt * 11);
    u->stateT += dt;
    if (u->fireCd > 0) u->fireCd -= dt;
    if (u->reloadT > 0) { u->reloadT -= dt; if (u->reloadT <= 0) finish_reload(u); }
    if (u->downed) {
        u->downTimer -= dt;
        if (u->downTimer <= 0) {
            kill_unit(u, nullptr);
        } else if (u->downTimer < 12 && u->speakCd <= 0) {
            u->speakCd = 7; say("你", "我快不行了……", "no");
        }
        const Unit *med = nullptr;
        for (auto &x : W.units) if (x.role == "医疗兵" && !x.dead && !x.downed) { med = &x; break; }
        Unit *medM = const_cast<Unit *>(med);
        if (medM && !medM->rescueTarget) {
            medM->rescueTarget = u; medM->rescueMode.clear();
            say(medM->name, "我来了队长！", "ok");
        }
        return;
    }
    /* 移动：第一人称下 WASD 相对视角 —— W 沿视线前进、A/D 横向平移、S 后退。
       逻辑层不依赖渲染层：只有 Input 与 World.viewPitch 两个输入源，
       所以在无 DOM 的离线仿真里（Cam 不存在）也能照常跑。 */
    float fwd = 0, str = 0;
    if (IN.w) fwd += 1;
    if (IN.s) fwd -= 1;
    if (IN.a) str -= 1;
    if (IN.d) str += 1;
    if (fwd != 0 || str != 0) {
        const float cy = std::cos(u->facing), sy = std::sin(u->facing);
        float mx = cy * fwd - sy * str, my = sy * fwd + cy * str;
        const float L = hypot2f(mx, my);
        if (L > 0) { mx /= L; my /= L; }
        float sp = 88.0f * (IN.shift ? 1.35f : 1.0f);
        if (IN.ctrl) sp *= 0.52f;      // 蹲下
        if (u->reloadT > 0) sp *= 0.85f;
        if (in_river(u->x, u->y) && !on_bridge(u->x, u->y)) sp *= 0.55f;
        if (u->suppression > 60) sp *= 0.85f;
        const float nx = u->x + mx * sp * dt, ny = u->y + my * sp * dt;
        if (passable(nx, u->y) && !prop_blocked(nx, u->y, 6)) u->x = nx;
        if (passable(u->x, ny) && !prop_blocked(u->x, ny, 6)) u->y = ny;
        u->moving = true;
    } else {
        u->moving = false;
    }
    /* 朝向：第一人称下由视角决定（渲染层每帧把 yaw 写进 u.facing）；
       俯视/离线仿真时回退到鼠标世界坐标。 */
    if (IN.hasMouseWorld && !IN.fpsAim) {
        u->facing = std::atan2(IN.mouseWorldY - u->y, IN.mouseWorldX - u->x);
    }
    /* 射击 */
    if (IN.fire && u->reloadT <= 0 && u->fireCd <= 0) {
        if (u->magAmmo <= 0) {
            try_reload(u);
        } else {
            const WeaponSpec *w = u->wpn;
            u->fireCd = w->rof;
            const float tx = u->x + std::cos(u->facing) * 400;
            const float ty = u->y + std::sin(u->facing) * 400;
            /* 第一人称：准星抬离水平面越多，子弹越打不到地面目标（射击高处/天空基本落空）。
               逻辑层的弹道始终躺在水平面上，所以这里用「有效射程」表达俯仰带来的偏差。 */
            const float pm = W.viewPitch;
            const float rangeMul = clampf(1.0f - std::fabs(pm) / 0.24f, 0.04f, 1.0f);
            spawn_bullet(u, tx, ty, spread_mul(*u) * 0.85f, rangeMul);
            if (!W.triggered) trigger_ambush("player");
        }
    }
    if (u->magAmmo <= 0 && u->reloadT <= 0) try_reload(u);
    if (IN.grenade) {
        IN.grenade = false;
        if (u->grenades > 0) throw_grenade(u, u->x + std::cos(u->facing) * 120, u->y + std::sin(u->facing) * 120, "grenade");
    }
    /* 拾取密码箱 */
    if (W.box.active && !W.box.taken && distf(u->x, u->y, W.box.x, W.box.y) < 22) {
        W.box.taken = true; W.box.by = u; u->carryBox = true;
        W.stats.boxTaken = true;
        W.evacArmed = true;                       // 拿到密码箱 → 进入撤离阶段
        sfx("pickup", 0, 0, -1.0f, true);
        say("你", "密码箱到手！", "ok"); toast("密码箱已到手 · 全队撤离");
    }
    /* 撤离（同样要等撤离阶段开始才计数；倒地/阵亡不算撤离人数） */
    if (!u->dead && !u->downed && W.evacArmed && !u->evacuated &&
        distf(u->x, u->y, W.evac.x, W.evac.y) < 95) {
        u->evacuated = true; W.stats.evacCount++;
        sfx("evac", 0, 0, -1.0f, true);
        say("你", "我到撤离点了", "ok"); check_end();
    }
}

// ------------------------------------------------------------------ 车队
void update_vehicles(float dt) {
    for (size_t i = 0; i < W.vehicles.size(); ++i) {
        Vehicle *v = &W.vehicles[i];
        v->hitFlash = std::max(0.0f, v->hitFlash - dt * 3);
        if (v->destroyed) {
            v->burning = std::max(0.0f, v->burning - dt);
            v->fireFxT -= dt;
            if (v->fireFxT <= 0) {
                v->fireFxT = 0.3f;
                FxItem f; f.type = "fire";
                f.x = v->x + rr(-12, 12); f.y = v->y + rr(-8, 8);
                f.life = 0.7f; f.r = rr(8, 16);
                fx_push(f);
            }
            continue;
        }
        /* 前方被毁车辆：向它的车尾靠拢后再停。
           早期写法是「前方有残骸就立即 speed=0」，结果整列车队被冻结在各自的初始位置、
           在 1300px 的路面上拉成一条长蛇，伏击圈外的车根本进不了交战距离。 */
        Vehicle *wreck = nullptr;
        for (int j = (int)i - 1; j >= 0; --j) {
            Vehicle *o = &W.vehicles[(size_t)j];
            if (o->destroyed && (!!o->isReinforcement) == (!!v->isReinforcement)) { wreck = o; break; }
        }
        if (wreck) v->blockedT += dt;
        float speed = 0;
        /* 跟停距离按**两车半长之和**算，不再写死 52。
           52 是"车长 3 米时代"的数（两车中心距 2.6 m）：那时就已经让后车叠进
           前车 0.7 m，只是车小看不太出来；车长改成实车尺寸后会叠进去 4 米 ——
           一辆 6.9 m 的卡车有一半插在另一辆里。改成"半长之和 + 8 单位（0.4 m）"，
           车多大就停多远，以后加车也不用再回来调。 */
        const float stopGap = wreck != nullptr ? (v->len + wreck->len) * 0.5f + 8.0f : 0.0f;
        if (wreck && distf(v->x, v->y, wreck->x, wreck->y) <= stopGap) {
            speed = 0;
            if (!v->dismounted && v->type != "tank" && v->blockedT > 1.5f) dismount(v);
        } else if (v->isReinforcement) {
            /* 增援车队：一路开到伏击圈再下车 */
            v->stopT += dt;
            speed = 26;
            if (!v->dismounted && (v->x < 1500 || v->stopT > 30)) dismount(v);
        } else if (!W.convoyStarted) {
            speed = 0;
        } else if (!W.triggered) {
            speed = 26;
        } else if (v->x > CFG.convoyStopX + (float)i * CFG.convoyGap && !v->atZone) {
            /* 按车序排队停车：第 i 辆车停在自己的位置上，避免全部挤到同一个 x 重叠成一堆 */
            speed = 26;
        } else {
            v->atZone = true;
            v->stopT += dt;
            speed = v->stopT > 110 ? v->spec->speed * 0.85f : 0;   // 久攻不下 → 试图冲出西侧
            if (!v->dismounted && v->type != "tank" && speed == 0) {
                if (v->blockedT > 2.5f || v->stopT > 5.5f) dismount(v);
            }
        }
        /* 洪水（内外加山）：陷进泥水里的车还在爬，只是爬得极慢。
           刻意不写 0 —— 史实是"整整一个上午无法前进"，不是"钉死不动"；
           留一点速度，玩家能看见它们在往前拱，压迫感才在。 */
        speed *= flood_speed_mul(v->x, v->y);
        if (speed > 0) {
            const float nd = v->dist + speed * dt;
            float px = 0, py = 0, pa = 0;
            way_point_at(nd, px, py, pa);
            const bool crossBlocked = !W.bridgeAlive && v->x >= CFG.riverX2 && px < CFG.riverX2;
            if (!crossBlocked) v->dist = nd;
            else v->atBridge = true;
            v->speed = speed;
        } else {
            v->speed = 0;
        }
        {
            float px = 0, py = 0, pa = 0;
            way_point_at(v->dist, px, py, pa);
            v->px = v->x; v->py = v->y;
            v->x = px; v->y = py; v->angle = pa;
        }

        /* 触发遇袭：停车、混乱 */
        if (W.triggered && !v->isReinforcement && v->stopT > 0 && v->stopT < 0.2f && !v->chaosSaid) v->chaosSaid = true;

        /* 车载武器 */
        const VehicleSpec *sp = v->spec;
        if (sp->has_mg && W.triggered) {
            v->wpnCd -= dt;
            if (v->wpnCd <= 0) {
                Unit *t = nearest_enemy_at(v->x, v->y, Team::Enemy,
                    [](Unit *x) { return !x->dead && !x->downed; });
                if (t && distf(v->x, v->y, t->x, t->y) < sp->mg_range && !los_fire(v->x, v->y, t->x, t->y)) {
                    spawn_tank_mg(v, t->x, t->y);
                    // veh_mg_gap 只缩放"打完之后隔多久再打"，不动一轮几发（ri 的调用次序不变）
                    v->wpnCd = sp->mg_rof * BAL.veh_mg_gap * (float)ri(2, 5);
                } else {
                    v->wpnCd = 0.6f;
                }
            }
        }
        /* 坦克主炮 */
        if (sp->cannon && !v->destroyed) {
            v->fireCd -= dt;
            if (v->aimT > 0) {
                v->aimT -= dt;
                v->turret = normAng(v->turret + angDiff(std::atan2(v->aimPt.y - v->y, v->aimPt.x - v->x), v->turret)
                                    * std::min(1.0f, dt * 2.2f));
                if (v->aimT <= 0) {
                    spawn_shell(v, v->aimPt.x, v->aimPt.y);
                    v->fireCd = SHELL.rof * rr(0.85f, 1.2f);
                    v->hasAimPt = false;
                }
            } else if (v->fireCd <= 0 && W.triggered) {
                // 找我方最密集位置
                Unit *best = nullptr; int bn = 1;
                for (auto &u : W.units) {
                    if (u.team != Team::Ally || u.dead || u.downed) continue;
                    if (distf(v->x, v->y, u.x, u.y) > 900) continue;
                    if (los_fire(v->x, v->y, u.x, u.y)) continue;
                    int n = 0;
                    for (auto &o : W.units) if (o.team == Team::Ally && !o.dead && distf(o.x, o.y, u.x, u.y) < 70) n++;
                    if (n > bn) { bn = n; best = &u; }
                }
                if (best) {
                    v->aimPt = { best->x + rr(-24, 24), best->y + rr(-24, 24) };
                    v->hasAimPt = true;
                    v->aimT = SHELL.warn;
                    set_alert(std::string("炮击预警！") + (best->isPlayer ? "目标是你" : ("目标：" + best->name))
                              + " — 全体隐蔽/转移", SHELL.warn);
                    say("全体", "坦克炮口瞄准了我们的阵地！", "no");
                    W.stats.tankShells++;
                } else {
                    v->fireCd = 1.5f;
                }
            }
        }
        /* 增援/逃跑判定 */
        if (!v->destroyed && v->x < 130) {
            const bool boxVeh = v->hasBox;
            if (v->type == "tank" || (boxVeh && !W.stats.boxTaken)) {
                W.convoyEscaped = true;
                end_game("失败", "车队冲过了西侧出口，伏击失败。");
            }
        }
    }
}

} // namespace va
