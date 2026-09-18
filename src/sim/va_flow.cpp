// VolunteerArmyPC —— 关卡流程 / 目标判定 / 结算（对应网页版 logic_ref.js 2320~2423 行）
#include "sim/va_world.h"

#include <algorithm>
#include <cmath>

namespace va {

void trigger_ambush(const std::string &src) {
    if (W.triggered || W.over) return;
    W.triggered = true; W.triggerT = W.t;
    for (auto &m : W.mines) m.armed = true;
    W.stats.triggerSrc = src;
    sfx("ambush", 0, 0, -1.0f, true);     // 伏击开始的号角 sting
    set_alert("伏击开始！", 2.5f);
    say("全体", src == "player" ? "队长开火了，打！" : "起爆！", "sys");
    W.reinforceT = std::max(W.reinforceT, W.t + 150);
}

void update_mines(float dt) {
    (void)dt;
    for (auto &m : W.mines) {
        if (m.used || !m.armed) continue;
        for (auto &v : W.vehicles) {
            if (v.destroyed || v.team != Team::Enemy) continue;
            if (distf(v.x, v.y, m.x, m.y) < 34) {
                m.used = true; W.stats.minesUsed++;
                explosion(m.x, m.y, 74, 60, Team::Ally, "mine");
                Unit *laobai = ally_by_id("laobai");
                damage_vehicle(&v, 260, "mine", laobai ? laobai : W.player);
                if (W.box.active && !W.box.taken && v.hasBox) drop_box(v.x, v.y);
                break;
            }
        }
    }
}

void update_barrage(float dt) {
    if (!W.barrage || W.over) return;
    W.barrageCd -= dt;
    if (W.barrageCd > 0) return;
    W.barrageCd = rr(14, 22);
    auto alive = alive_allies();
    if (alive.empty()) return;
    float cx = 0, cy = 0;
    for (auto *u : alive) { cx += u->x; cy += u->y; }
    cx /= (float)alive.size(); cy /= (float)alive.size();
    const float tx = cx + rr(-90, 90), ty = cy + rr(-90, 90);
    WorldState::PendingShell s; s.x = tx; s.y = ty; s.t = 2.2f;
    W.pendingShells.push_back(s);
    /* 呼啸声与 2.2s 的落弹倒计时同步：先听见炮弹从头顶压下来，再听见爆炸 */
    sfx("incoming", tx, ty);
    set_alert("远程炮击来袭 — 全体隐蔽/散开！", 2.2f);
    say("全体", "听！是炮击，要落我们头上！", "no");
}

void update_pending_shells(float dt) {
    for (int i = (int)W.pendingShells.size() - 1; i >= 0; --i) {
        W.pendingShells[(size_t)i].t -= dt;
        if (W.pendingShells[(size_t)i].t <= 0) {
            const auto &s = W.pendingShells[(size_t)i];
            explosion(s.x, s.y, 96, 120, Team::Enemy, "shell");
            W.pendingShells.erase(W.pendingShells.begin() + i);
        }
    }
}

void check_objectives() {
    const MissionStats &s = W.stats;
    std::vector<WorldState::Objective> o;
    auto add = [&](const std::string &text, bool done, bool main, const std::string &extra) {
        WorldState::Objective ob; ob.text = text; ob.done = done; ob.main = main; ob.extra = extra;
        o.push_back(ob);
    };
    add("摧毁坦克", s.tankKilled, true, "");
    add("获取密码箱", s.boxTaken, true, "");
    /* 撤离门槛出自 BAL.evac_need（"降低撤离门槛"这条方案的落点）。
       文案与判据必须共用同一个数 —— 否则改了门槛之后简报仍写"至少 6 人"、
       而 5 人就过关，这种自相矛盾是玩家最直接的不信任来源。 */
    const std::string need = std::to_string(BAL.evac_need);
    add("至少 " + need + " 人撤离到 " + W.evac.name, s.evacCount >= BAL.evac_need, true,
        "(" + std::to_string(s.evacCount) + "/" + need + ")");
    add("摧毁 2 辆装甲车", s.apcKilled >= 2, false, "(" + std::to_string(s.apcKilled) + "/2)");
    add("救回所有伤员（无阵亡/无遗留）",
        (!s.allyDead && downed_allies().empty() && W.t > 60), false, "");
    add("不触发敌方增援", !s.reinforceTriggered, false, "");
    add("击毙车队指挥官", s.officerKilled, false, "");
    W.objState = o;
    EV->on_objectives_dirty();
}

void check_end() {
    if (W.over) return;
    const MissionStats &s = W.stats;
    if (s.tankKilled && s.boxTaken && s.evacCount >= BAL.evac_need && !W.convoyEscaped) {
        end_game("成功", "伏击成功：坦克被摧毁，密码箱到手，" + std::to_string(s.evacCount)
                         + " 人从 " + W.evac.name + " 撤离。");
        return;
    }
    int up = 0, allIn = 1;
    for (auto &u : W.units) {
        if (u.team != Team::Ally || u.dead || u.downed) continue;
        up++;
        if (!u.evacuated) allIn = 0;
    }
    if (up > 0 && allIn && W.triggered && W.evacArmed && s.evacCount < BAL.evac_need) {
        end_game("失败", "撤离人数不足 " + std::to_string(BAL.evac_need) + " 人（实际 "
                         + std::to_string(s.evacCount) + " 人），任务失败。");
    }
}

void update_phase_name() {
    const float t = W.t;
    std::string n;
    if (!W.started) n = "情报";
    else if (t < CFG.tIntel) n = "情报";
    else if (t < CFG.tDeploy && !W.deployDone) n = "部署";
    else if (!W.triggered) n = "潜伏";
    else if (t - W.triggerT < 40) n = "爆发";
    else if (t - W.triggerT < 130) n = "混战";
    else n = "清剿";
    if (W.stats.boxTaken && W.stats.evacCount < BAL.evac_need) n = "撤离";
    if (t >= W.reinforceT && W.reinforceDone) n = "紧急撤离";
    W.phaseName = n;
}

void update_flow(float dt) {
    W.t += dt;
    if (!W.convoyStarted && W.t >= CFG.convoyIn) {
        W.convoyStarted = true;
        say("全体", "车队来了，沉住气，等我的口令！", "sys");
    }
    if (W.triggered && W.t - W.triggerT > 3 && !W.mineWarned) W.mineWarned = true;
    if (!W.reinforceDone && W.triggered && W.t >= W.reinforceT) spawn_reinforcement();
    if (W.t >= CFG.missionEnd && !W.over) end_game("失败", "超过 10 分钟时限，任务失败。");
    check_objectives();
    if (W.t > 20) check_end();
    update_barrage(dt);
    update_pending_shells(dt);
}

void end_game(const std::string &kind, const std::string &text) {
    if (W.over) return;
    W.over = true; W.overKind = kind; W.overText = text;
    EV->on_end(kind, text);
}

std::string score_mission() {
    const MissionStats &s = W.stats;
    const bool win = (W.overKind == "成功");
    const int dead = s.allyDead;
    const int cas = dead + (int)downed_allies().size();
    const float finishT = W.t;
    std::string g = "失败";
    if (win) {
        if (cas <= 2 && finishT <= 480 && !s.reinforceTriggered && s.apcKilled >= 2) g = "S";
        else if (cas <= 4) g = "A";
        else if (cas <= 6) g = "B";
        else g = "C";
    }
    W.score.g = g; W.score.cas = cas; W.score.dead = dead; W.score.win = win;
    return g;
}

// ------------------------------------------------------------------ 主步进
// 严格照抄网页版 index.html 5367~5388 行的 stepOnce 顺序 ——
// 顺序本身是玩法的一部分（比如 updateFlow 里推进的 t 会立刻影响本帧所有 AI 的计时）。
void step_once(float dt) {
    if (W.paused) return;
    W.phase = W.t < CFG.tIntel ? "INTEL" : (W.deployDone ? "BATTLE" : "DEPLOY");
    if (!W.deployDone && W.t >= CFG.tDeploy) W.deployDone = true;
    update_flow(dt);
    update_vehicles(dt);
    /* JS 的 for...of 会在遍历中访问新追加的元素（下车 / 增援在同一 tick 内即被更新），
       所以这里用「实时增长的 size」而不是先取快照；deque 的 push_back 不会让已有元素失效，
       因此 &W.units[i] 在整个循环里始终安全。 */
    for (size_t i = 0; i < W.units.size(); ++i) {
        Unit *u = &W.units[i];
        if (u->team == Team::Ally) {
            if (u->isPlayer) update_player(dt);
            else update_ally(u, dt);
        } else {
            update_enemy(u, dt);
        }
    }
    update_projectiles(dt);
    update_mines(dt);
    for (int i = (int)W.smokes.size() - 1; i >= 0; --i) {
        W.smokes[(size_t)i].t += dt;
        if (W.smokes[(size_t)i].t > W.smokes[(size_t)i].life) W.smokes.erase(W.smokes.begin() + i);
    }
    for (int i = (int)W.fx.size() - 1; i >= 0; --i) {
        W.fx[(size_t)i].t += dt;
        if (W.fx[(size_t)i].t > W.fx[(size_t)i].life) W.fx.erase(W.fx.begin() + i);
    }
    for (auto &d : W.decals) d.t += dt;
    W.noise = std::max(0.0f, W.noise - dt * 0.30f);
    if (W.noiseT > 0) W.noiseT -= dt;
    update_phase_name();
}

} // namespace va
