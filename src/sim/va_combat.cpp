// VolunteerArmyPC —— 战斗：弹道 / 命中 / 压制 / 感知 / 索敌
// 对应网页版 logic_ref.js 1393~1696 行（第四章「战斗」+ 第五章开头）
#include "sim/va_world.h"

#include <algorithm>
#include <cmath>

namespace va {

// ------------------------------------------------------------ 开火指令集合
// 该单位是否已收到「开火类」命令（收到后就不再受潜伏期火力纪律约束，可以主动打响第一枪）
static const char *FIRE_ACTIONS[] = {
    "fire", "freeFire", "focusFire", "suppress", "coverMe", "atTank", "atAPC",
    "atJeep", "atTruck", "atInf", "atMG", "atOfficer", "grenade", "smoke", "rocket"
};

bool ordered_to_fire(const Unit &u) {
    if (!u.order.active) return false;
    for (const char *f : FIRE_ACTIONS) if (u.order.actId == f) return true;
    return false;
}

// ------------------------------------------------------------------ 状态
void set_state(Unit *u, const std::string &s) {
    if (!u) return;
    if (u->state != s) { u->state = s; u->stateT = 0; }
}

// ------------------------------------------------------------------ 弹道
void spawn_bullet(Unit *u, float tx, float ty, float spreadMul, float rangeMul) {
    const WeaponSpec *w = u->wpn;
    const float base = std::atan2(ty - u->y, tx - u->x);
    const float sp = (w->spread != 0 ? w->spread : 0.05f) * (spreadMul != 0 ? spreadMul : 1.0f);
    const float a = base + (RNG.next() + RNG.next() + RNG.next() - 1.5f) * sp * 2;

    Projectile p;
    p.kind = ProjKind::Bullet;
    p.x = u->x + std::cos(a) * 8; p.y = u->y + std::sin(a) * 8;
    p.vx = std::cos(a) * w->speed; p.vy = std::sin(a) * w->speed;
    p.dmg = w->dmg; p.pen = w->pen;
    p.team = u->team; p.owner = u; p.ownerT = Target(u);
    /* rangeMul：第一人称下用准星俯仰角折算的有效射程系数（AI 不开火时恒为 1） */
    p.life = (w->range * (rangeMul)) / w->speed;
    p.t = 0; p.sup = w->sup;
    p.src = u->team == Team::Ally ? "ally" : "enemy";
    W.projectiles.push_back(p);

    u->magAmmo--; u->shots++;
    if (u->team == Team::Ally) W.stats.shotFired = true;
    const float mx = u->x + std::cos(a) * 8, my = u->y + std::sin(a) * 8;
    { FxItem f; f.type = "flash"; f.x = mx; f.y = my; f.life = 0.055f; f.a = a; fx_push(f); }
    /* 枪声：按阵营与武器区分音色。玩家自己那一声走 local 居中播放 ——
       否则「自己开枪」会被当成一个贴在耳边的点声源，声像会随视角乱甩。 */
    const std::string sid = u->isPlayer ? "rifle"
        : (u->team == Team::Ally
            ? (u->weaponKey == "mg" ? "mg" : (u->weaponKey == "sniper" ? "sniper" : "rifle"))
            : (u->weaponKey == "emg" ? "enemyMG" : "enemyRifle"));
    if (u->isPlayer) sfx(sid, mx, my, 0.9f, true);
    else sfx(sid, mx, my);
    if (u->team == Team::Ally) {
        W.noise = clampf(W.noise + 0.02f, 0, 1);
        W.noiseT = std::max(W.noiseT, 0.25f);
    }
}

void spawn_rocket(Unit *u, float tx, float ty) {
    const float base = std::atan2(ty - u->y, tx - u->x) + rr(-0.02f, 0.02f);
    Projectile p;
    p.kind = ProjKind::Rocket;
    p.x = u->x + std::cos(base) * 12; p.y = u->y + std::sin(base) * 12;
    p.vx = std::cos(base) * ROCKET.speed; p.vy = std::sin(base) * ROCKET.speed;
    p.dmg = ROCKET.dmgVeh; p.dmgInf = ROCKET.dmgInf; p.splash = ROCKET.splash;
    p.team = u->team; p.owner = u; p.ownerT = Target(u);
    p.life = ROCKET.range / ROCKET.speed; p.t = 0;
    W.projectiles.push_back(p);

    u->rockets--; u->shots++;
    if (u->team == Team::Ally) W.stats.shotFired = true;
    { FxItem f; f.type = "flash"; f.x = u->x + std::cos(base) * 10; f.y = u->y + std::sin(base) * 10;
      f.life = 0.12f; f.a = base; f.b = 1; fx_push(f); }
    if (u->isPlayer) sfx("rocketFire", u->x + std::cos(base) * 10, u->y + std::sin(base) * 10, -1.0f, true);
    else sfx("rocketFire", u->x + std::cos(base) * 10, u->y + std::sin(base) * 10);
    say(u->name, "火箭弹出去了！", "ok");
}

void throw_grenade(Unit *u, float tx, float ty, const std::string &kind) {
    Projectile p;
    p.kind = (kind == "smoke") ? ProjKind::SmokeG : ProjKind::Grenade;
    p.x = u->x; p.y = u->y;
    p.tx = tx; p.ty = ty; p.t = 0; p.life = 1.5f;
    p.team = u->team; p.owner = u; p.ownerT = Target(u);
    p.dmg = 85; p.splash = 72;
    W.projectiles.push_back(p);

    if (kind == "smoke") u->smokes--; else u->grenades--;
    { FxItem f; f.type = "flash"; f.x = u->x; f.y = u->y; f.life = 0.08f;
      f.a = std::atan2(ty - u->y, tx - u->x); fx_push(f); }
    if (u->isPlayer) sfx("grenadeThrow", u->x, u->y, -1.0f, true);
    else sfx("grenadeThrow", u->x, u->y);
}

void spawn_shell(Vehicle *v, float tx, float ty) {
    const float base = std::atan2(ty - v->y, tx - v->x) + rr(-0.02f, 0.02f);
    Projectile p;
    p.kind = ProjKind::Shell;
    p.x = v->x + std::cos(base) * 36; p.y = v->y + std::sin(base) * 36;
    p.vx = std::cos(base) * SHELL.speed; p.vy = std::sin(base) * SHELL.speed;
    p.dmg = SHELL.dmg; p.splash = SHELL.splash; p.team = v->team;
    p.ownerVeh = v; p.ownerT = Target(v);
    p.life = 2.0f; p.t = 0;
    W.projectiles.push_back(p);
    { FxItem f; f.type = "flash"; f.x = v->x + std::cos(base) * 34; f.y = v->y + std::sin(base) * 34;
      f.life = 0.16f; f.a = base; f.b = 1; fx_push(f); }
    sfx("cannon", v->x, v->y);
    W.noise = clampf(W.noise + 0.5f, 0, 1); W.noiseT = 1.0f;
}

void spawn_tank_mg(Vehicle *v, float tx, float ty) {
    const float base = std::atan2(ty - v->y, tx - v->x) + (RNG.next() + RNG.next() - 1.0f) * 0.10f;
    Projectile p;
    p.kind = ProjKind::TankMG;
    p.x = v->x + std::cos(base) * 30; p.y = v->y + std::sin(base) * 30;
    p.vx = std::cos(base) * 950; p.vy = std::sin(base) * 950;
    p.dmg = v->spec->mg_dmg; p.pen = 0.12f;
    p.team = v->team; p.ownerVeh = v; p.ownerT = Target(v);
    p.life = 0.6f; p.t = 0; p.sup = v->spec->mg_sup;
    W.projectiles.push_back(p);
    { FxItem f; f.type = "flash"; f.x = v->x + std::cos(base) * 30; f.y = v->y + std::sin(base) * 30;
      f.life = 0.05f; f.a = base; fx_push(f); }
    /* 车载机枪：走 enemyMG 音色但压低一点，免得抢过步兵步枪的方位感 */
    sfx("enemyMG", v->x + std::cos(base) * 30, v->y + std::sin(base) * 30, 0.85f);
}

bool vehicle_hit(const Vehicle &v, float x, float y) {
    const float dx = x - v.x, dy = y - v.y;
    const float c = std::cos(-v.angle), s = std::sin(-v.angle);
    const float lx = dx * c - dy * s, ly = dx * s + dy * c;
    return std::fabs(lx) < v.len / 2 + 2 && std::fabs(ly) < v.wid / 2 + 2;
}

// ------------------------------------------------------------------ 弹道推进
void update_projectiles(float dt) {
    auto &P = W.projectiles;
    for (int i = (int)P.size() - 1; i >= 0; --i) {
        Projectile &p = P[(size_t)i];
        p.t += dt;
        if (p.kind == ProjKind::Grenade || p.kind == ProjKind::SmokeG) {
            const float k = p.t / p.life;
            const float tt = std::min(1.0f, dt * 4.5f + (k > 0.9f ? 1.0f : 0.12f));
            p.x = lerpf(p.x, p.tx, tt);
            p.y = lerpf(p.y, p.ty, tt);
            if (p.t >= p.life) {
                if (p.kind == ProjKind::SmokeG) {
                    Smoke s; s.x = p.x; s.y = p.y; s.r = 62; s.t = 0; s.life = 16;
                    W.smokes.push_back(s);
                    say("全体", "烟雾已释放", "ok");
                } else {
                    explosion(p.x, p.y, p.splash, p.dmg, p.team, "grenade");
                }
                P.erase(P.begin() + i);
            }
            continue;
        }
        const int steps = std::max(1, (int)std::ceil(hypot2f(p.vx, p.vy) * dt / 4.0f));
        bool dead = false;
        for (int s = 0; s < steps && !dead; ++s) {
            const float px = p.x, py = p.y;
            p.x += p.vx * dt / steps; p.y += p.vy * dt / steps;
            /* 掠弹音：子弹从耳边擦过去。只算「不是自己打的」子弹，且每发只响一次 ——
               这是第一人称下判断「有人正朝我打」最直接的一条听觉线索。 */
            if (!p.cracked && !(p.owner && p.owner->isPlayer)) {
                if (dist2f(p.x, p.y, W.cam.x, W.cam.y) < 13225.0f) {
                    p.cracked = true; sfx("crack", p.x, p.y);
                }
            }
            // 掩体
            for (auto &pr : W.props) {
                if (pr.destroyed || !pr.blocksBullet) continue;
                const float rin = pr.r + 2.5f;
                if (dist2f(px, py, pr.x, pr.y) <= rin * rin) continue;   // 出膛时已贴着这块掩体 → 视为擦过
                if (segCircle(px, py, p.x, p.y, pr.x, pr.y, pr.r)) {
                    if (pr.explosive) { pr.destroyed = true; chain_barrel(pr); }
                    { FxItem f; f.type = "spark"; f.x = p.x; f.y = p.y; f.life = 0.18f; fx_push(f); }
                    sfx("impact", p.x, p.y);
                    dead = true; break;
                }
            }
            if (dead) break;
            // 步兵
            for (auto &u : W.units) {
                if (u.dead || u.downed || u.team == p.team) continue;
                if (u.mount) continue;
                /* 用「本子步的位移线段」判命中，而不是只判终点：
                   步兵判定半径 5.2+3=8.2px 与子步长同量级，只判终点会让掠射弹整段漏检（命中率被腰斩） */
                if (segCircle(px, py, p.x, p.y, u.x, u.y, u.radius + 3)) {
                    if (p.kind == ProjKind::Rocket) explosion(p.x, p.y, p.splash, p.dmgInf, p.team, "rocket");
                    else damage_unit(&u, p.dmg, p.owner, "bullet");
                    if (p.owner && p.owner->team == Team::Ally) p.owner->hits++;
                    if (p.owner && p.owner->isPlayer) sfx("hit", 0, 0, -1.0f, true);
                    { FxItem f; f.type = "spark"; f.x = p.x; f.y = p.y; f.life = 0.16f; f.b = 1; fx_push(f); }
                    dead = true; break;
                }
            }
            if (dead) break;
            // 车辆
            for (auto &v : W.vehicles) {
                if (v.destroyed || v.team == p.team) continue;
                if (vehicle_hit(v, p.x, p.y)) {
                    if (p.kind == ProjKind::Rocket)      explosion(p.x, p.y, p.splash, p.dmg, p.team, "rocket", 1.0f, "rocket");
                    else if (p.kind == ProjKind::Shell)  explosion(p.x, p.y, p.splash, p.dmg, p.team, "shell", 0.5f, "shell");
                    else damage_vehicle(&v, p.dmg, "bullet", p.owner);
                    { FxItem f; f.type = "spark"; f.x = p.x; f.y = p.y; f.life = 0.16f; fx_push(f); }
                    dead = true; break;
                }
            }
            if (dead) break;
            // 近失弹压制
            if (p.sup > 0) {
                for (auto &u : W.units) {
                    if (u.dead || u.team == p.team) continue;
                    if (dist2f(p.x, p.y, u.x, u.y) < 900) u.suppression = clampf(u.suppression + p.sup * dt * 6, 0, 100);
                }
            }
        }
        if (!dead && p.t > p.life) {
            if (p.kind == ProjKind::Rocket || p.kind == ProjKind::Shell)
                explosion(p.x, p.y, p.splash, p.dmg, p.team, "shell");
            dead = true;
        }
        if (dead) P.erase(P.begin() + i);
    }
}

// ------------------------------------------------------------------ 感知
void perceive(Unit *u) {
    Target best;
    float bd = 1e9f;
    const float r = vis_range(*u);
    std::vector<Target> cand;
    /* 目标必须是「对面阵营」：早期这里写死 team === 'enemy'，
       导致敌人调用本函数时把同袍（甚至自己）当成目标 —— 敌人会朝自己开枪且永不推进 */
    for (auto &e : W.units) {
        if (&e == u || e.team == u->team || e.dead || e.downed || e.mount) continue;
        cand.push_back(Target(&e));
    }
    for (auto &v : W.vehicles) {
        if (v.team == u->team || v.destroyed) continue;
        cand.push_back(Target(&v));
    }
    for (const auto &e : cand) {
        const float d = distf(u->x, u->y, e.tx(), e.ty());
        if (d > r) continue;
        if (los_sight(u->x, u->y, e.tx(), e.ty()) && d > 130) continue;
        if (d < bd) { bd = d; best = e; }
    }
    u->seen = best; u->seenD = bd;
    if (best.ok()) { u->lastSeenX = best.tx(); u->lastSeenY = best.ty(); u->lastSeenT = W.t; }
}

/* 手里的枪啃不动的目标：子弹对车辆的穿透系数就是车辆 armor 值（坦克 0.04 → 每发 0.52 点），
   一个弹匣打上去不如半发火箭。这里统一判定，避免步枪手把整场火力浪费在装甲车上。
   反坦克手与带炸药包的老白不算。 */
bool ineffective_target(const Unit &u, const Target &t) {
    if (!t.ok() || !t.veh()) return false;
    if (u.weaponKey == "at" || u.hasCharge) return false;
    return t.ttype() == "tank" || t.ttype() == "apc";
}

Target ally_target_select(Unit *u) {
    // 目标优先级（文档 5.）：玩家指定 > 正在攻击队友的敌人 > 角色优先 > 最近威胁
    if (u->focusTarget.ok() && !u->focusTarget.gone() && can_see_t(*u, u->focusTarget)) return u->focusTarget;
    if (u->atTarget.ok() && !u->atTarget.gone()) return u->atTarget;
    if (!u->preferType.empty()) {
        const Target t = pick_target_by_type(u, u->preferType, 0, false);
        if (t.ok() && can_see_t(*u, t)) return t;
    }
    // 反坦克手优先装甲
    if (u->weaponKey == "at" && u->rockets > 0) {
        Target t = pick_target_by_type(u, "tank", 0, false);
        if (!t.ok()) t = pick_target_by_type(u, "apc", 0, false);
        if (t.ok() && can_see_t(*u, t) && dist_to(u->x, u->y, t) < ROCKET.range) return t;
    }
    // 狙击手优先军官 / 机枪手 / 驾驶员
    if (u->weaponKey == "sniper") {
        const Target o = pick_target_by_type(u, "officer", 0, false);
        if (o.ok() && can_see_t(*u, o)) return o;
        const Target m = pick_target_by_type(u, "mg", 0, false);
        if (m.ok() && can_see_t(*u, m)) return m;
    }
    if (u->seen.ok() && !ineffective_target(*u, u->seen)) return u->seen;
    /* 「看得见的」只剩打不动的装甲目标 → 改打打得动的步兵，别对着铁壳子空转整场 */
    const Target soft = pick_target_by_type(u, "inf", 0, false);
    if (soft.ok() && can_see_t(*u, soft)) return soft;
    return u->seen;
}

// ------------------------------------------------------------------ 换弹
bool try_reload(Unit *u) {
    if (u->reloadT > 0) return false;
    if ((float)u->magAmmo > (float)u->wpn->mag * 0.4f) return false;
    if (u->ammo <= 0) return false;
    u->reloadT = u->wpn->reload * (u->state == "被压制" ? 1.35f : 1.0f);
    if (u->isPlayer) sfx("reloadStart", u->x, u->y, -1.0f, true);
    else sfx("reloadStart", u->x, u->y);
    return true;
}
void finish_reload(Unit *u) {
    const int need = std::min(u->wpn->mag - u->magAmmo, u->ammo);
    u->magAmmo += need; u->ammo -= need;
    if (need > 0) {
        if (u->isPlayer) sfx("reloadEnd", u->x, u->y, -1.0f, true);
        else sfx("reloadEnd", u->x, u->y);
    }
}

float spread_mul(const Unit &u) {
    float m = 1;
    m *= 1 + u.suppression / 110.0f;
    if (u.moving) m *= 1.45f;
    if (u.aimT < 0.45f) m *= 1.8f;
    if (W.weather == "rain") m *= 1.12f;
    if (W.weather == "night") m *= 1.2f;
    return m;
}

/* 反坦克手是否该用火箭弹（文档：反坦克手：坦克 > 装甲车 > 吉普）
   注意：卡车默认不算装甲目标（火箭弹要留给坦克/装甲车），但玩家一旦明确下令「打卡车」
   就必须放行 —— 密码箱有 62% 概率锁在卡车后厢，不允许打就意味着该局面下主目标不可达。 */
bool at_may_use_rocket(const Unit &u, const Target &tgt, float d) {
    if (u.weaponKey != "at" || u.rockets <= 0) return false;
    const bool ordered = u.order.active &&
        (u.order.actId == "atTank" || u.order.actId == "atAPC" || u.order.actId == "atTruck" || u.order.actId == "rocket");
    const std::string tt = tgt.ttype();
    const bool armTarget = (tt == "tank" || tt == "apc") || (ordered && tt == "truck");
    if (!armTarget) return false;
    /* 这里曾经限制「未受命时 d<420」。但潜伏期的火力纪律（fireAt 里的 orderedToFire 判定）
       已经保证了伏击前不会有人开枪，这条限制只剩下副作用：反坦克手会抱着 3 发火箭弹
       被 460px 外正在开火的坦克打死，一发都不还手。触发后按火箭弹实际射程交火。 */
    return d < ROCKET.range;
}

void fire_at(Unit *u, const Target &tgt, float dt) {
    (void)dt;
    if (!tgt.ok()) return;
    if (u->fireMode == "hold" || u->hideFire) return;
    if (u->reloadT > 0) return;
    if (u->magAmmo <= 0) { try_reload(u); return; }
    if (u->fireCd > 0) return;
    const WeaponSpec *w = u->wpn;
    const float d = dist_to(u->x, u->y, tgt);
    if (d > w->range) return;
    if (d > 60 && los_fire(u->x, u->y, tgt.tx(), tgt.ty())) return;
    /* 潜伏阶段的火力纪律（文档：「沉住气，不要提前开火」）
       —— 未收到开火命令时必须按住，只有敌人贴到 55px 才允许自卫还击。 */
    if (u->team == Team::Ally && !W.triggered && !ordered_to_fire(*u) && d > 55) return;
    const bool isVeh = tgt.veh();
    /* 火箭弹只打坦克/装甲车；未受命时近距自卫才用，避免把伏击位置提前打乱 */
    if (isVeh && at_may_use_rocket(*u, tgt, d)) {
        spawn_rocket(u, tgt.tx(), tgt.ty());
        u->fireCd = ROCKET.rof;
        u->reloadT = ROCKET.reload * 0.5f;
        return;
    }
    /* 手里的枪啃不动的装甲目标：除非玩家明确下令（如「全体，打装甲车」），否则不必浪费弹药 */
    if (isVeh && ineffective_target(*u, tgt) && !ordered_to_fire(*u)) return;
    u->burstLeft = u->burstLeft > 0 ? u->burstLeft - 1 : w->burst - 1;
    u->fireCd = u->burstLeft > 0 ? w->rof : w->rof + w->burstGap;
    if (u->team == Team::Ally && !W.triggered) trigger_ambush("selfdef");
    spawn_bullet(u, tgt.tx(), tgt.ty(), spread_mul(*u));
    if (u->state == "隐蔽" || u->state == "待命") {
        if (u->noAutoFire) return;
        set_state(u, "战斗");
    }
}

void throw_grenade_auto(Unit *u, const Target &tgt) {
    if (u->grenades <= 0 || u->grenadeCd > 0) return;
    const float d = dist_to(u->x, u->y, tgt);
    if (d > 150) return;
    int cluster = 0;
    for (auto *e : enemies()) {
        if (e->mount) continue;
        if (distf(e->x, e->y, tgt.tx(), tgt.ty()) < 45) cluster++;
    }
    if (cluster < 2 && d > 90) return;
    u->grenadeCd = 10;
    throw_grenade(u, tgt.tx() + rr(-8, 8), tgt.ty() + rr(-8, 8), "grenade");
    say(u->name, "手雷！", "ok");
}

} // namespace va
