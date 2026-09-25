// VolunteerArmyPC —— 伤害 / 死亡 / 载具损毁 / 爆炸（对应网页版 logic_ref.js 856~1011 行）
#include "sim/va_world.h"
#include "sim/va_campaign.h"
#include "sim/va_utf8.h"

#include <algorithm>
#include <cmath>

namespace va {

// RGBA(0..255, a) → decal_push 的分量形式，避免在逻辑层里解析 CSS 颜色串
static void decal_rgba(float x, float y, float r, int cr, int cg, int cb, float ca) {
    decal_push(x, y, r, cr / 255.0f, cg / 255.0f, cb / 255.0f, ca);
}

// ------------------------------------------------------------------ 步兵伤害
void damage_unit(Unit *u, float dmg, Unit *src, const std::string &kind,
                 const VehicleSpec *srcVeh) {
    if (!u || u->dead || u->downed) return;
    /* 记录来源供平衡扫描归因。**每次伤害都整个赋值**（不写成"非空才覆盖"）——
       步兵子弹与爆炸传的是 nullptr，正好把上一次残留下来的载具来源清掉，
       否则挨过一枪机枪之后，后面所有阵亡都会被算成机枪打死的。 */
    u->lastHurtVehSpec = srcVeh;
    /* 依托掩体最多减伤 70%：伏击方在工事里、进攻方在开阔地推进，
       这个差距是文档「掩体评分」体系成立的前提（否则 AI 抢掩体毫无意义）。 */
    dmg *= 1.0f - cover_protect(*u) * 0.70f;
    u->hp -= dmg;
    u->hitFlash = 0.25f;
    /* 受伤音：玩家自己是一声贴耳的闷响（local），队友/敌人按方位放 ——
       「子弹打进土里」和「打在人身上」音色不同，混战中能听出来自己有没有打中 */
    if (u->isPlayer) sfx("hurt", 0, 0, -1.0f, true);
    else sfx("flesh", u->x, u->y, kind == "splash" ? 0.62f : -1.0f, false);
    u->lastHurtBy = src;
    if (src && src->team != u->team) u->lastHurtT = W.t;
    if (u->team == Team::Ally) {
        u->morale = clampf(u->morale - dmg * 0.12f, 0, 100);
        u->suppression = clampf(u->suppression + dmg * 0.9f, 0, 100);
    }
    decal_rgba(u->x + rr(-3, 3), u->y + rr(-3, 3), rr(2, 3.5f), 140, 20, 20, 0.5f);
    if (u->hp <= 0) {
        if (u->team == Team::Ally) {
            if (u->isPlayer) {
                down_player(u);
            } else if (RNG.next() < 0.72f) {
                u->downed = true; u->downTimer = 45; u->state = "失能"; u->hp = 0;
                say(u->name, pick_cstr(REPLIES.down), "no");
            } else {
                kill_unit(u, src);
            }
        } else {
            kill_unit(u, src);
        }
    }
}

// 玩家阵亡的处理（定义在 kill_unit 之后，这里先声明）
static void on_player_lost();

void kill_unit(Unit *u, Unit *src) {
    if (!u || u->dead) return;
    u->dead = true; u->state = "阵亡";
    sfx(u->team == Team::Ally ? "allyDown" : "body", u->x, u->y);
    if (u->carryBox) {
        u->carryBox = false;
        if (W.box.active) {
            W.box.taken = false; W.box.by = nullptr;
            W.box.x = u->x; W.box.y = u->y; W.box.onGround = true;
            say("全体", "密码箱掉了！谁去捡！", "no");
        }
    }
    if (u->team == Team::Enemy) {
        W.stats.enemyDead++;
        if (src && src->team == Team::Ally) {
            src->kills++;
            if (!src->isPlayer) src->morale = clampf(src->morale + 4, 0, 100);
        }
        if (u->officer) {
            W.stats.officerKilled = true;
            say("观察员", "敌军指挥官被击毙！", "sys");
            /* 文档：密码箱有 38% 概率由指挥官随身携带 —— 击毙他才会掉箱。
               早期这里只记了一条战果，箱子在本局压根不存在，
               于是「密码箱在军官身上」的那 38% 局面里主目标永远无法完成（实测该局面 0 胜）。 */
            if (W.boxWhere == "officer" && !W.box.active) drop_box(u->x, u->y);
        }
        decal_rgba(u->x, u->y, 6, 120, 16, 16, 0.45f);
    } else {
        W.stats.allyDead = (W.stats.allyDead || 0) + 1;
        for (auto *a : allies()) if (a != u) a->morale = clampf(a->morale - 6, 0, 100);
        say("全体", u->name + "阵亡！", "no");
        if (u->isPlayer) on_player_lost();
        on_ally_lost();
    }
}

/* 玩家阵亡 → **直接转进下一个伏击阵地**，由该阵地的队长接任。
   为什么不是"就地判失败"：本作的视角是**一个人跟着部队换阵地**，不是"一条命
   打完一整场战役"。阵亡的代价是**交出指挥权、换成一个陌生的名字**（下一关
   队伍里那个你不认识的人就是你），而不是整场战役从头再来 —— 后者只会让人
   不敢往前站，与"逐次抵抗"的立意相反。
   为什么在这里结算而不是等 check_end：check_end 的前提是"这一关还在打"，
   而玩家一死这一关就已经结束了，拖到那里会多跑几十秒的空战场。 */
static void on_player_lost() {
    if (W.playerLost) return;
    W.playerLost = true;
    if (!CAM.active) return;                 // 单关模式保持旧行为（交给 on_ally_lost）
    if (has_next_level()) {
        capture_carry();                     // 此刻还活着的人跟着转进
        commit_level_result(false);
        const LevelDef &N = level_at(next_level_index());
        end_game("转进", std::string("你阵亡了 —— ") + cur_level().place
                 + " 只拖住 " + std::to_string((int)W.t) + " 秒。剩下的交给 "
                 + N.place + " 的 " + N.ourUnit + "。");
    } else {
        commit_level_result(false);
        end_game("失败", "你在最后一个阵地上阵亡了，阻击任务失败。");
    }
}

void down_player(Unit *u) {
    u->downed = true; u->hp = 0; u->state = "失能"; u->downTimer = 40;
    sfx("down", 0, 0, -1.0f, true);       // 重重倒下 + 耳鸣
    say("你", "我中弹了——需要医疗！", "no");
    toast("你被击倒！副队长接管指挥");
    handover();
}

void handover() {
    if (W.handoverUnit) return;
    Unit *dep = nullptr, *lead = nullptr;
    for (auto &x : W.units) {
        if (x.team != Team::Ally || x.dead || x.downed) continue;
        const RosterDef *r = roster_of(x.id);
        if (!r) continue;
        if (r->deputy && !dep) dep = &x;
        if (r->leader && !lead) lead = &x;
    }
    Unit *pickU = dep ? dep : lead;
    if (pickU) {
        W.handoverUnit = pickU;
        say("全体", pickU->name + "：我接管指挥，全体听我口令！", "sys");
    }
}

void on_ally_lost() {
    int n = 0;
    for (auto &u : W.units) if (u.team == Team::Ally && !u.dead && !u.downed) n++;
    if (n == 0) end_game("全灭", "小队全灭，伏击失败。");
}

// ------------------------------------------------------------------ 载具伤害
void damage_vehicle(Vehicle *v, float dmg, const std::string &kind, Unit *byUnit) {
    if (!v || v->destroyed) return;
    const float pen = kind == "bullet" ? v->armor
                    : kind == "rocket" ? 0.95f
                    : kind == "shell"  ? 1.1f
                    : kind == "mine"   ? 1.3f
                    : 1.0f;
    v->hp -= dmg * pen;
    v->hitFlash = 0.25f;
    /* 子弹啃装甲＝金属跳弹声（火箭/炮弹的爆炸音在 explosion 里出） */
    if (kind == "bullet") sfx("clang", v->x, v->y, 0.85f);
    if (kind == "rocket" || kind == "mine") say("全体", "好！命中" + v->name + "！", "ok");
    if (v->hp <= 0) destroy_vehicle(v, byUnit);
}

void destroy_vehicle(Vehicle *v, Unit *byUnit) {
    if (!v || v->destroyed) return;
    v->destroyed = true; v->burning = 30; v->hp = 0;
    /* 殉爆半径走 spec->blast_r（0 才回退到 len × 0.9，见 va_types.h）。
       不再直接用 len：车长改实车尺寸时，写死 len × 0.9 会让杀伤半径跟着放大
       1.5~2.1 倍 —— 而伏击是贴身打，等于凭空提高难度（实测掉 1 胜 / 10 种子）。 */
    const float blastR = (v->spec != nullptr && v->spec->blast_r > 0.0f)
                       ? v->spec->blast_r : v->len * 0.9f;
    explosion(v->x, v->y, blastR, v->type == "tank" ? 160.0f : 90.0f, Team::Enemy, "vehicle");
    sfx("metal", v->x, v->y, 0.8f);          // 车体撕裂的金属余音
    decal_rgba(v->x, v->y, v->len * 0.6f, 30, 26, 22, 0.55f);
    if (byUnit && byUnit->team == Team::Ally) byUnit->kills++;
    if (v->type == "tank") {
        W.stats.tankKilled = true;
        say("全体", "坦克被摧毁！", "ok"); toast("坦克已摧毁");
        for (auto *a : allies()) a->morale = clampf(a->morale + 12, 0, 100);
        for (auto *e : enemies()) e->morale -= 12;
    } else if (v->type == "apc") {
        W.stats.apcKilled++;
        say("全体", "装甲车瘫痪！", "ok");
    }
    if (v->hasBox) drop_box(v->x, v->y);
    // 车上未下车的步兵随之阵亡
    for (auto *t : v->troops) {
        if (!t->dead && t->mount == v) { t->mount = nullptr; damage_unit(t, 90, byUnit, "splash"); }
    }
    for (auto *a : allies()) a->morale = clampf(a->morale + 3, 0, 100);
}

void drop_box(float x, float y) {
    W.box.x = x; W.box.y = y;
    W.box.taken = false; W.box.by = nullptr; W.box.active = true; W.box.onGround = true; W.box.t = 0;
    W.hasBox = true;
    sfx("boxDrop", x, y);
    say("全体", "密码箱掉出来了，去拿！", "sys");
    /* 自动指派一个人去取。早期这里只把箱子放到地上就算完事，
       而「搬密码箱」这条命令只有当箱子已经掉出来时才会真正生效（见 applyOrder）——
       玩家通常是在车还没打爆时就下令，于是箱子一落地就没人管，主目标直接判死。 */
    std::vector<Unit *> pool;
    for (auto &u : W.units) if (u.team == Team::Ally && !u.dead && !u.downed) pool.push_back(&u);
    if (pool.empty()) return;
    /* 挑人：距离越近越好，但被火力压制的人要打折（别让志愿者顶着机枪去送死）；
       支援组的铁头按文档是首选，只要他不是远得离谱。 */
    auto cost = [&](Unit *u) { return distf(u->x, u->y, x, y) + u->suppression * 4.0f; };
    std::stable_sort(pool.begin(), pool.end(), [&](Unit *p, Unit *q) { return cost(p) < cost(q); });
    Unit *support = nullptr;
    for (auto *u : pool) if (u->role == "弹药/支援") { support = u; break; }
    Unit *chosen = (support && cost(support) <= cost(pool[0]) * 1.6f) ? support : pool[0];
    chosen->boxTask = true;
    chosen->moveGoal = { x, y }; chosen->hasMoveGoal = true;
    say(chosen->name, "我去拿密码箱！", "ok");
}

/* vehMul：爆炸对车辆的伤害系数（默认 0.5，用于手雷/炮击这类通用爆炸）
   vehKind：传给 damageVehicle 的弹药种类，决定穿甲系数（火箭弹是 0.95，炮弹 1.1…）
   早期这里写死 damageVehicle(v, dmg * 0.5, 'shell')，于是：
     1) 火箭弹的 dmgVeh(300) 被再砍一半 → 单发只有 150；
     2) kind 恒为 'shell'，ROCKET 应有的 0.95 穿甲系数永远不生效。
   结果 950 HP 的坦克即使 6 发火箭全部命中（6×150×1.1=990）也几乎打不掉 —— 主目标实质不可达。 */
void explosion(float x, float y, float r, float dmg, Team team, const std::string &kind,
               float vehMul, const std::string &vehKind) {
    FxItem f; f.type = "boom"; f.x = x; f.y = y; f.r = r; f.life = 0.55f;
    fx_push(f);
    decal_rgba(x, y, r * 0.5f, 24, 20, 16, 0.5f);
    W.noise = clampf(W.noise + 0.45f, 0, 1);
    W.noiseT = 0.9f;
    /* 爆炸音效按 type 分档：半径越大的爆炸更沉、更长，并且更狠地压住其他声音
       （近距离还能听见耳鸣 ring）——这是玩家判断「炸到哪儿了」的主要线索之一 */
    {
        const std::string bid =
            kind == "mine"    ? "mine" :
            kind == "barrel"  ? "barrel" :
            kind == "grenade" ? "grenade" :
            kind == "rocket"  ? "rocketBoom" :
            kind == "shell"   ? "shellBoom" :
            kind == "vehicle" ? "vehicleBoom" :
            kind == "cannon"  ? "cannon" : "explosion";
        sfx(bid, x, y, clampf(0.6f + r / 220.0f, 0.7f, 1.35f));
    }
    for (auto &u : W.units) {
        if (u.dead || u.downed) continue;
        if (u.team == team && kind != "vehicle") continue;
        const float d = distf(x, y, u.x, u.y);
        if (d < r) damage_unit(&u, dmg * (1 - d / r * 0.6f), nullptr, "splash");
        else if (d < r * 2.1f) u.suppression = clampf(u.suppression + 34 * (1 - d / (r * 2.1f)), 0, 100);
    }
    const float vm = (vehMul < 0) ? 0.5f : vehMul;
    const std::string vk = vehKind.empty() ? kind : vehKind;
    for (auto &v : W.vehicles) {
        if (v.destroyed || v.team == team) continue;
        const float d = distf(x, y, v.x, v.y);
        if (d < r * 1.1f) damage_vehicle(&v, dmg * vm * (1 - d / (r * 1.1f)), vk, nullptr);
    }
    for (auto &p : W.props) {
        if (p.destroyed || !p.explosive) continue;
        /* 水坝（dam）**不吃流弹** —— 只有人跑到坝上安放炸药才炸得开（damTask 分支）。
           理由：坝是混凝土的，一发坦克炮弹或一个汽油桶的连锁爆炸不该把它打穿；
           更要紧的是**目标不能被偶然完成** —— 坝体 150 宽、落点就在侧射阵位
           （POINTS[1]）附近，而这里的判定是 r+8，只要肯让路过的爆炸生效，
           "跑到坝上、自断退路"那个史实两难就可能被一次意外连锁悄悄替玩家完成。
           是不是真的发生过，看 `LV.damByCharge`（工具侧打印"已炸(安放)/已炸(非安放)"）。 */
        if (p.dam) continue;
        if (distf(x, y, p.x, p.y) < r + 8) { p.destroyed = true; chain_barrel(p); }
    }
}

void chain_barrel(Prop &p) {
    /* 水坝（内外加山）。**史实的两难就在这**：水库一炸，南面平原变成泥沼，
       美军的坦克陷在那里一整个上午；同时山上那两个排也再没有退路。
       所以这里除了洪水，还要明确告诉玩家"退路也断了" ——
       只报喜不报忧的话，等他发现撤不回去时只会觉得是被坑了。 */
    if (p.dam) {
        LV.damBlown = true;
        explosion(p.x, p.y, 210, 70, Team::None, "barrel");
        say("全体", "水库炸开了！南面的平原全淹了！", "sys");
        set_alert("水淹七军 — 敌装甲陷入洪流；山上的退路也断了", 4.5f);
        toast("水坝已炸开：南面平原变成泥沼");
        return;
    }
    explosion(p.x, p.y, 78, 85, Team::None, "barrel");
}

} // namespace va
