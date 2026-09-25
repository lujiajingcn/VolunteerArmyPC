// VolunteerArmyPC —— 语音指令解析器（文本 → ParsedCmd）
// 对应网页版 index.html 的 `const Parser = { ... }`（logic_ref.js 322~513 行）。
//
// 移植要点：网页版的模糊匹配建立在一个汉字 = 一个 UTF-16 字符的前提上，
// 这里全部改用码点序列（va_utf8.h）来复刻同一套语义，否则汉字会被按字节切开。
#include "sim/va_config.h"
#include "sim/va_utf8.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "sim/va_world.h"

namespace va {

using Cps = std::vector<uint32_t>;

static Cps cp(const char *s) { return utf8_codes(std::string(s)); }
static Cps cp(const std::string &s) { return utf8_codes(s); }
static bool has(const Cps &hay, const Cps &needle) { return utf8_find(hay, needle) >= 0; }
static bool has(const Cps &hay, const char *needle) { return has(hay, cp(needle)); }

// 字符相似度复用文法表里的 char_sim（按 UTF-8 字符串入参，内部同样走码点）
static float sim_cps(const Cps &a, const Cps &b) {
    return char_sim(utf8_from_codes(a), utf8_from_codes(b));
}

// ------------------------------------------------------------------ 命中结构
struct CsHit { int pos = -1; int len = 0; std::string kind, id, label; float score = 1; };
struct ActHit { const ActionDef *act = nullptr; int pos = -1; int len = 0; float score = 0; std::string key; };
struct LocHit { std::string key, point, name; int pos = -1; int len = 0; };
struct DirHit { float bearing = 0; std::string raw; int hour = 0; bool ok = false; };

// ------------------------------------------------------------- 呼号识别
// 对应 Parser.findCallsign：返回最早出现的匹配
static bool find_callsign(const Cps &s, CsHit &out) {
    std::vector<CsHit> hits;
    auto push = [&](int pos, int len, const std::string &kind, const std::string &id, const std::string &label, float sc) {
        if (pos < 0) return;
        CsHit h; h.pos = pos; h.len = len; h.kind = kind; h.id = id; h.label = label; h.score = sc;
        hits.push_back(h);
    };
    for (const auto &g : CS_GROUP) {
        const int p = utf8_find(s, cp(g.key));
        push(p, (int)cp(g.key).size(), "group", g.value, g.value, 1);
    }
    for (const auto *k : CS_ALL) {
        const int p = utf8_find(s, cp(k));
        push(p, (int)cp(k).size(), "all", "*", "全体", 1);
    }
    for (const auto &rc : ROLE_CALL) {
        const int p = utf8_find(s, cp(rc.role));
        push(p, (int)cp(rc.role).size(), "role", rc.role, rc.role, 1);
    }
    for (const auto &m : ROSTER) {
        bool found = false;
        int bp = -1, blen = 0;
        for (const std::string &a : m.aliases) {
            const int p = utf8_find(s, cp(a));
            const int L = (int)cp(a).size();
            if (p >= 0 && (!found || L > blen)) { found = true; bp = p; blen = L; }
        }
        if (found) push(bp, blen, "member", m.id, m.name, 1);
    }
    if (hits.empty()) {
        /* 模糊匹配（阿杰尔 / 阿杰哥 → 阿杰）：逐窗口算字符相似度，阈值 0.72 */
        for (const auto &m : ROSTER) {
            for (const std::string &a : m.aliases) {
                const Cps av = cp(a);
                if (av.size() < 2) continue;
                int bp = -1; float bs = 0;
                for (size_t i = 0; i + av.size() <= s.size(); ++i) {
                    const Cps seg(s.begin() + i, s.begin() + i + av.size());
                    const float sc = sim_cps(av, seg);
                    if (sc > bs) { bs = sc; bp = (int)i; }
                }
                if (bs >= 0.72f) { push(bp, (int)av.size(), "member", m.id, m.name, bs); break; }
            }
        }
    }
    if (hits.empty()) return false;
    std::stable_sort(hits.begin(), hits.end(), [](const CsHit &a, const CsHit &b) {
        if (a.pos != b.pos) return a.pos < b.pos;
        if (a.len != b.len) return a.len > b.len;
        return a.score > b.score;
    });
    out = hits.front();
    return true;
}

// ------------------------------------------------------------- 动作识别
// 对应 Parser.findAction：全局最长关键词命中（打分 keyLen*100 + pri）
static ActHit find_action(const Cps &s) {
    ActHit best;
    for (const auto &a : ACTIONS) {
        for (const char *k : a.keys) {
            const Cps kv = cp(k);
            const int p = utf8_find(s, kv);
            if (p < 0) continue;
            const float score = (float)kv.size() * 100.0f + (float)a.pri;
            if (!best.act || score > best.score) {
                best.act = &a; best.pos = p; best.key = k; best.len = (int)kv.size(); best.score = score;
            }
        }
    }
    return best;
}

// ------------------------------------------------------------- 地点识别
// 对应 Parser.findLoc：命中即可，多个命中时取「关键词更长」的那个
static LocHit find_loc(const Cps &s) {
    LocHit best;
    for (const auto &lk : LOC_KEYS) {
        const Cps kv = cp(lk.key);
        const int p = utf8_find(s, kv);
        if (p < 0) continue;
        if (best.pos < 0 || (int)kv.size() > best.len) {
            best.key = lk.key; best.point = lk.point;
            best.pos = p; best.len = (int)kv.size();
        }
    }
    return best;
}

// ------------------------------------------------------------- 方向识别
static bool is_digit_cp(uint32_t c) { return c >= '0' && c <= '9'; }

static DirHit find_dir(const Cps &s) {
    /* 等价 JS 正则 /(\d{1,2})点(方向|钟方向|钟)?/ —— 从左到右第一个匹配，
       数字部分贪婪（优先两位，匹配不上再退回一位）。 */
    const Cps dian = cp("点");
    const Cps fangxiang = cp("方向");
    const Cps zhongfangxiang = cp("钟方向");
    const Cps zhong = cp("钟");
    for (size_t i = 0; i < s.size(); ++i) {
        if (!is_digit_cp(s[i])) continue;
        for (int take = 2; take >= 1; --take) {
            if (i + (size_t)take > s.size()) continue;
            bool all_digit = true;
            for (int k = 0; k < take; ++k) if (!is_digit_cp(s[i + k])) all_digit = false;
            if (!all_digit) continue;
            size_t j = i + take;
            if (j + dian.size() > s.size()) continue;
            bool m = true;
            for (size_t k = 0; k < dian.size(); ++k) if (s[j + k] != dian[k]) { m = false; break; }
            if (!m) continue;
            int h = 0;
            for (int k = 0; k < take; ++k) h = h * 10 + (int)(s[i + k] - '0');
            if (h >= 1 && h <= 12) {
                std::string raw = utf8_from_codes(Cps(s.begin() + i, s.begin() + j + dian.size()));
                // 可选后缀：钟方向（更长，优先）/ 方向 / 钟
                size_t tail_end = j + dian.size();
                auto try_tail = [&](const Cps &t) {
                    if (tail_end + t.size() <= s.size()) {
                        bool mm = true;
                        for (size_t k = 0; k < t.size(); ++k) if (s[tail_end + k] != t[k]) { mm = false; break; }
                        if (mm) { tail_end += t.size(); return true; }
                    }
                    return false;
                };
                if (!try_tail(zhongfangxiang)) { if (!try_tail(fangxiang)) try_tail(zhong); }
                raw = utf8_from_codes(Cps(s.begin() + i, s.begin() + tail_end));
                DirHit d; d.bearing = (float)((h % 12) * 30); d.raw = raw; d.hour = h; d.ok = true;
                return d;
            }
        }
    }
    for (const auto &dk : DIR_KEYS) {
        const Cps kv = cp(dk.key);
        if (utf8_find(s, kv) >= 0) {
            DirHit d; d.bearing = dk.bearing; d.raw = dk.key; d.ok = true;
            return d;
        }
    }
    return DirHit{};
}

// ------------------------------------------------------------- 目标名词识别
static std::string find_target_noun(const Cps &s) {
    std::string best;
    int blen = -1;
    for (const auto &tn : TARGET_NOUNS) {
        const Cps kv = cp(tn.noun);
        if (utf8_find(s, kv) >= 0 && (int)kv.size() > blen) { blen = (int)kv.size(); best = tn.type; }
    }
    return best;
}

// --------------------------------------------------- 文中提到的其他队员
static std::vector<std::string> find_mentioned_ids(const Cps &s, const std::string &excludeId) {
    std::vector<std::string> out;
    for (const auto &m : ROSTER) {
        if (!excludeId.empty() && m.id == excludeId) continue;
        for (const std::string &a : m.aliases) {
            if (utf8_find(s, cp(a)) >= 0) { out.push_back(m.id); break; }
        }
    }
    return out;
}

// ------------------------------------------------------------------ 主解析
ParsedCmd parse_command(const std::string &text, float noiseIn, float asr, bool typed, const std::string &source) {
    ParsedCmd cmd;
    // 对应 JS 的 String(text||'').trim()
    std::string raw = text;
    {
        size_t b = 0, e = raw.size();
        auto is_ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        while (b < e && is_ws((unsigned char)raw[b])) ++b;
        while (e > b && is_ws((unsigned char)raw[e - 1])) --e;
        raw = raw.substr(b, e - b);
    }
    cmd.raw = raw;
    cmd.source = source;
    const std::string s = normalize_order(raw);
    const Cps sc = cp(s);
    if (s.empty()) { cmd.confidence = 0; cmd.notes.push_back("empty"); return cmd; }

    CsHit cs;
    const bool hasCs = find_callsign(sc, cs);
    Cps sNoCs = sc;
    if (hasCs) {
        for (int k = 0; k < cs.len && cs.pos + k < (int)sNoCs.size(); ++k) sNoCs[cs.pos + k] = 0x3000;  // '　'
    }
    const ActHit actFull = find_action(sc);
    const ActHit actNoCs = find_action(sNoCs);
    ActHit actionHit = actNoCs.act ? actNoCs : actFull;
    const Cps &scan = sNoCs;

    const LocHit loc = find_loc(scan);
    const DirHit dir = find_dir(scan);

    if (!actionHit.act) {
        cmd.confidence = 0.1f;
        cmd.notes.push_back("no-action");
        if (hasCs) { cmd.csKind = cs.kind; cmd.csId = cs.id; cmd.csLabel = cs.label; }
        return cmd;
    }

    const ActionDef *A = actionHit.act;
    /* 目标名词补正：「打 11 点方向机枪手」→ 升级为「打机枪手」 */
    if (A->kind == ActKind::Combat || A->kind == ActKind::Atk) {
        const std::string tn = find_target_noun(scan);
        const std::string curTarget = A->target ? A->target : "";
        if (!tn.empty() && tn != curTarget) {
            const ActionDef *up = action_by_id(target_act_of(tn));
            if (up) A = up;
        }
    }

    /* 置信度评估 */
    float conf = 0.30f;
    conf += actionHit.len >= 3 ? 0.38f : (actionHit.len == 2 ? 0.34f : 0.14f);
    if (hasCs) conf += (cs.kind == "all") ? 0.06f : 0.13f;
    else { conf += 0.02f; cmd.notes.push_back("未指定呼号 → 默认最近队友"); }
    if (A->needLoc) {
        if (loc.pos >= 0) conf += 0.14f;
        else { conf -= 0.30f; cmd.notes.push_back("缺少地点（如「撤退到 C 点」）"); }
    } else if (loc.pos >= 0) conf += 0.10f;
    if (sc.size() >= 4) conf += 0.08f;
    if (sc.size() >= 7) conf += 0.04f;
    if (actFull.act && actFull.act->id != A->id) {
        conf -= 0.16f;
        cmd.notes.push_back(std::string("存在歧义：也可能理解为「") + actFull.act->label + "」");
    }
    const float noise = clampf(noiseIn, 0, 1);
    if (noise > 0.02f) {
        conf -= noise * 0.35f;
        cmd.notes.push_back("战场噪声降低识别率 -" + std::to_string((int)std::round(noise * 35)) + "%");
    }
    if (asr > 0) conf = lerpf(conf, asr, 0.55f);
    if (typed) conf = std::max(conf, 0.93f);
    conf = clampf(conf, 0, 1);

    cmd.ok = conf >= 0.70f;
    cmd.confidence = conf;
    cmd.noise = noise;

    if (hasCs) { cmd.csKind = cs.kind; cmd.csId = cs.id; cmd.csLabel = cs.label; }
    else { cmd.csKind = "member"; cmd.csId = "auto"; cmd.csLabel = "默认（最近队友）"; }

    cmd.actId = A->id; cmd.actLabel = A->label;
    cmd.typeTarget = A->target ? A->target : "";
    cmd.needLoc = A->needLoc;

    if (loc.pos >= 0) {
        const TacPoint *pt = point_of(loc.key);
        cmd.hasLoc = true; cmd.locKey = loc.key; cmd.locName = pt ? pt->name : loc.key;
        if (pt) { cmd.locX = pt->x; cmd.locY = pt->y; }
    }
    if (dir.ok) { cmd.hasDir = true; cmd.bearing = dir.bearing; cmd.dirRaw = dir.raw; }

    cmd.mentioned = find_mentioned_ids(scan, (hasCs && cs.kind == "member") ? cs.id : std::string());
    if (!cmd.ok) cmd.notes.push_back("置信度 " + std::to_string((int)std::round(conf * 100)) + "% < 70%，不予执行");
    return cmd;
}

// --------------------------------------------------- 低置信度候选（HUD 按 1/2/3 确认）
std::vector<ParsedCmd> parse_candidates(const std::string &text, const ParsedCmd &cmd) {
    std::vector<ParsedCmd> out;
    auto sig = [](const ParsedCmd &c) {
        return c.csKind + ":" + c.csId + "|" + c.actId + "|" + (c.hasLoc ? c.locKey : std::string("-"));
    };
    auto add = [&](const ParsedCmd &c) {
        if (c.actId.empty()) return;
        const std::string sg = sig(c);
        for (const auto &o : out) if (sig(o) == sg) return;
        out.push_back(c);
    };

    if (!cmd.actId.empty()) add(cmd);
    // 变体 1：同一动作 → 全体
    if (!cmd.actId.empty() && cmd.csKind != "all") {
        ParsedCmd v = cmd;
        v.csKind = "all"; v.csId = "*"; v.csLabel = "全体"; v.confidence = 0.72f; v.ok = true;
        add(v);
    }
    // 变体 2：相似动作
    const std::string norm = normalize_order(text);
    const Cps nc = cp(norm);
    const ActionDef *alt = nullptr;
    float altScore = 0;
    for (const auto &a : ACTIONS) {
        if (!cmd.actId.empty() && a.id == cmd.actId) continue;
        for (const char *k : a.keys) {
            const Cps kv = cp(k);
            const size_t take = std::max<size_t>(kv.size(), 3);
            const std::string head = utf8_from_codes(utf8_slice(nc, 0, take));
            const float s1 = char_sim(k, head) * 0.6f + (has(nc, kv) ? 0.5f : 0.0f);
            if (s1 > altScore) { altScore = s1; alt = &a; }
        }
    }
    if (alt && altScore > 0.3f) {
        ParsedCmd v = cmd;
        if (v.csId.empty()) { v.csKind = "all"; v.csId = "*"; v.csLabel = "全体"; }
        v.actId = alt->id; v.actLabel = alt->label;
        v.typeTarget = alt->target ? alt->target : "";
        v.needLoc = alt->needLoc;
        v.confidence = 0.70f; v.ok = true;
        add(v);
    }
    // 变体 3：同动作 → 某个具体队员（按相似度）
    if (out.size() < 3) {
        const RosterDef *bm = nullptr;
        float bs = 0;
        for (const auto &m : ROSTER) {
            for (const std::string &a : m.aliases) {
                const float sco = char_sim(a, norm);
                if (sco > bs) { bs = sco; bm = &m; }
            }
        }
        if (bm && bm->id != cmd.csId) {
            ParsedCmd v = cmd;
            v.csKind = "member"; v.csId = bm->id; v.csLabel = bm->name;
            if (v.actId.empty()) { v.actId = "advance"; v.actLabel = "前进"; }
            v.confidence = 0.70f; v.ok = true;
            add(v);
        }
    }
    if (out.size() > 3) out.resize(3);
    return out;
}

} // namespace va
