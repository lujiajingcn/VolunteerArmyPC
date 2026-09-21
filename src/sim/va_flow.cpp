// VolunteerArmyPC —— 关卡流程 / 目标判定 / 结算（对应网页版 logic_ref.js 2320~2423 行）
#include "sim/va_world.h"
#include "sim/va_campaign.h"

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
    /* 战役里目标**逐关不同**，所以读 LEVELS 而不是写死这七条。
       !CAM.active 时（tools/va_sweep 等离线入口）必须原样走旧清单 ——
       否则平衡扫描的判据变了，新扫出来的胜率跟旧基线不可比。 */
    if (CAM.active) {
        for (const auto &g : cur_level().goals)
            add(g.text, goal_done(g), g.main, goal_progress(g));
        W.objState = o;
        EV->on_objectives_dirty();
        return;
    }
    add("摧毁坦克", s.tankKilled, true, "");
    add("获取密码箱", s.boxTaken, true, "");
    /* 撤离门槛出自 BAL.evac_need（"降低撤离门槛"这条方案的落点）。
       文案与判据必须共用同一个数 —— 否则改了门槛之后简报仍写"至少 6 人"、
       而 5 人就过关，这种自相矛盾是玩家最直接的不信任来源。 */
    const std::string need = std::to_string(effective_evac_need());
    add("至少 " + need + " 人撤离到 " + W.evac.name, s.evacCount >= effective_evac_need(), true,
        "(" + std::to_string(s.evacCount) + "/" + need + ")");
    add("摧毁 2 辆装甲车", s.apcKilled >= 2, false, "(" + std::to_string(s.apcKilled) + "/2)");
    add("救回所有伤员（无阵亡/无遗留）",
        (!s.allyDead && downed_allies().empty() && W.t > 60), false, "");
    add("不触发敌方增援", !s.reinforceTriggered, false, "");
    add("击毙车队指挥官", s.officerKilled, false, "");
    W.objState = o;
    EV->on_objectives_dirty();
}

/* 收拢阶段：仗已经打完（除撤离外的主目标全部达成），并且已经开始往外带人
   （下过撤退令，或者撤离人数已经够）。
   **为什么不用 W.evacArmed**：它在"拿到密码箱"时也会置位。单关卡里那等于
   "该走了"，但战役里"守住阵地 150 秒"才是主目标、物件只是副目标 ——
   拿箱不等于要走。用 evacArmed 会让收拢窗口在箱子刚到手时就开门、
   四十秒后关掉，那时队伍还在阵地上，于是被判定成"一个人都没带出来"
   （实测第三关：9 人活着、0 人撤离）。
   **为什么不用"撤离人数已够"当唯一条件**：那样差一个人的时候反而不给时间，
   恰恰是最需要这二十秒的时候没有（实测第二关差 1 人，5/6）。
   在这个阶段里关卡**不因时限收关**：时限是用来逼"还在打"的关卡的，
   不是用来砍掉"已经赢了、正在把最后几个人带出去"的最后几十秒。
   什么时候结束由 check_end 结算（窗口走完 / 能走的都走到了）。 */
static bool gather_open() {
    if (!CAM.active || W.over) return false;
    for (const auto &g : cur_level().goals) {
        if (g.main && g.kind != GoalKind::Evac && !goal_done(g)) return false;
    }
    return W.evacOrdered || W.stats.evacCount >= effective_evac_need();
}

void check_end() {
    if (W.over) return;
    const MissionStats &s = W.stats;
    const int need = effective_evac_need();

    /* 还在场上、且还站得起来的人：up = 人数，allIn = 是否已经全部撤到撤离点。
       提前算出来是因为下面的"集结窗口"要用它当第二个出口。 */
    int up = 0, allIn = 1;
    for (auto &u : W.units) {
        if (u.team != Team::Ally || u.dead || u.downed) continue;
        up++;
        if (!u.evacuated) allIn = 0;
    }

    /* ---- 战役：主目标全达成 + 撤离够人 → 过关。
       过关不等于结束 —— 还有下一关就 end_game("转进")，由引擎层接住继续。
       end_game 的 kind 是给外部看的：HUD 按它决定显示"任务完成"还是"继续转进"。 ---- */
    bool mainDone = true;
    if (CAM.active) {
        for (const auto &g : cur_level().goals) {
            if (g.main && !goal_done(g)) { mainDone = false; break; }
        }
    } else {
        mainDone = (s.tankKilled && s.boxTaken && s.evacCount >= need);
    }
    if (CAM.active && gather_open() && !W.convoyEscaped) {
        /* 集结/收拢窗口：进入这个阶段不等于立刻收关（见 CampaignState::evacOpenT）。
           两个出口 —— 窗口走完，或者能走的人都走到了。 */
        if (CAM.evacOpenT < 0.0f) CAM.evacOpenT = W.t;
        if (W.t - CAM.evacOpenT >= cur_level().evacWindow || allIn) {
            if (!mainDone) {
                /* 窗口关掉、人还是没带够 —— 这才是真的没带出来。 */
                commit_level_result(false);
                end_game("失败", "撤离人数不足 " + std::to_string(need) + " 人（实际 "
                         + std::to_string(s.evacCount) + " 人），" + std::string(cur_level().place)
                         + " 未能守住。");
                return;
            }
            capture_carry();          // 先数清楚谁跟着走（结算面板要用）
            commit_level_result(true);
            const LevelDef &L = cur_level();
            if (has_next_level()) {
                const LevelDef &N = level_at(next_level_index());
                end_game("转进", std::string(L.place) + " 的任务完成：" + std::to_string(s.evacCount)
                         + " 人撤出。转进" + N.place + " —— " + N.ourUnit + "。");
            } else {
                /* 史实落点：6 月 12 日 19:30，63 军奉兵团命令转向伊川地区休整。 */
                end_game("胜利", "阻击任务完成。第 63 军转向伊川地区休整 —— 铁原以北，"
                         "新的防线已经起来了。");
            }
            return;
        }
    } else if (!CAM.active && s.tankKilled && s.boxTaken && s.evacCount >= need && !W.convoyEscaped) {
        end_game("成功", "伏击成功：坦克被摧毁，密码箱到手，" + std::to_string(s.evacCount)
                         + " 人从 " + W.evac.name + " 撤离。");
        return;
    }
    if (up > 0 && allIn && W.triggered && W.evacArmed && s.evacCount < need) {
        end_game("失败", "撤离人数不足 " + std::to_string(need) + " 人（实际 "
                         + std::to_string(s.evacCount) + " 人），任务失败。");
        return;
    }
    /* 人都撤出来了、但主目标没完成 → 也得给个结束。
       不写这条的话玩家会卡在"全员已撤离、界面却什么都不发生"的状态里 ——
       既不能继续打（人都走了），也不结算。 */
    if (CAM.active && up > 0 && allIn && W.triggered && !mainDone) {
        commit_level_result(false);
        end_game("失败", "撤出来了，但 " + std::string(cur_level().place) + " 的任务没完成。");
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
    if (W.stats.boxTaken && W.stats.evacCount < effective_evac_need()) n = "撤离";
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
    if (W.t >= CFG.missionEnd && !W.over) {
        /* 战役里的"超时"是**这一关**超时（每关 4~7 分钟），不是整个 10 分钟；
           文案必须说清是哪一层，否则玩家会以为整场战役结束了。 */
        if (CAM.active) {
            /* 已经在收拢阶段 → 不在这里判死，交给 check_end 按窗口结算：
               时限是逼"还在打"的关卡的，不是砍"已经赢了、正在把人带出去"的。 */
            if (!gather_open()) {
                commit_level_result(false);
                end_game("失败", std::string(cur_level().place) + " 未能按时完成，敌人已经绕过去了。");
            }
        } else {
            end_game("失败", "超过 10 分钟时限，任务失败。");
        }
    }
    update_level_goals(dt);          // 夜袭夺回 / 阵地坚守的累计计时
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
    /* "转进" 也算赢（这一关打下来了），但它不是战役的终点 ——
       评级留空，让 HUD 显示"转进下一阵地"而不是一个 S/A/B/C 徽章。
       只有最后一关的 "胜利" 才给整个战役评级（按累计阵亡，六关打完唯一的硬指标）。 */
    const bool win = (W.overKind == "成功" || W.overKind == "胜利" || W.overKind == "转进");
    const int dead = s.allyDead;
    const int cas = dead + (int)downed_allies().size();
    const float finishT = W.t;
    std::string g = "失败";
    if (win && W.overKind == "转进") {
        g = "";
    } else if (win) {
        if (CAM.active) {
            const int td = CAM.totalDead;
            if (CAM.cleared >= level_count() && td <= 8) g = "S";
            else if (td <= 14) g = "A";
            else if (td <= 22) g = "B";
            else g = "C";
        } else if (cas <= 2 && finishT <= 480 && !s.reinforceTriggered && s.apcKilled >= 2) {
            g = "S";
        } else if (cas <= 4) {
            g = "A";
        } else if (cas <= 6) {
            g = "B";
        } else {
            g = "C";
        }
    }
    W.score.g = g; W.score.cas = cas; W.score.dead = dead;
    /* score.win 给结算面板定调（"任务达成"还是"任务失败"），所以它必须和上面那个
       `win` 一致 —— **"转进"也是达成**（阵地守住了、人也带出来了，只是还没打完）。
       原先这里卡的是 `overKind == "胜利" || "成功"`，于是过关那一屏出现两张
       自相矛盾的话：标题写"任 务 失 败"，底部提示"按 R 转进下一阵地"。
       实机取证 sweep/camp_adv2/cap_260s.png 拍到的就是这一张。 */
    W.score.win = win;
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
