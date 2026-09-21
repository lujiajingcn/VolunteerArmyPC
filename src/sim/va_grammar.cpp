// VolunteerArmyPC —— 语音指令文法表 + 中文归一 / 字符相似度
// 对应网页版 index.html 615~913 行（第一章：语音指令解析）
#include "sim/va_config.h"

#include <algorithm>
#include <cmath>

#include "sim/va_utf8.h"

namespace va {

// --------------------------------------------------------------- 动作表
// 顺序即优先级排序依据之一（打分时用 keyLen*100 + pri），务必与网页版一致。
const std::vector<ActionDef> ACTIONS = {
    { "ceasefire",     "停火",         { "停火", "别开火", "停止射击", "不要开火", "先别打", "别打了" }, ActKind::Combat,  95 },
    { "cancel",        "取消当前指令", { "取消", "解除命令", "撤销指令", "取消命令" }, ActKind::Misc,  92 },
    { "repeat",        "重复上一指令", { "重复", "再说一遍", "重新下令", "重复命令" }, ActKind::Misc,  91 },
    { "ack",           "收到",         { "收到", "了解", "知道了", "明白" }, ActKind::Misc,  90 },
    { "detonate",      "起爆地雷",     { "起爆", "引爆", "炸雷", "按雷", "起爆炸药" }, ActKind::Special, 88 },
    { "blowBridge",    "炸桥",         { "炸桥", "炸掉桥", "把桥炸了", "爆破桥梁" }, ActKind::Special, 87 },
    { "blowDam",       "炸水库",       { "炸水库", "炸大坝", "炸水坝", "炸坝", "爆破水库", "把水库炸了", "炸开水库" }, ActKind::Special, 86 },
    { "retreatTo",     "撤退到指定点", { "撤退到", "撤到", "退到", "向后撤到", "撤退至" }, ActKind::Move, 86, true },
    { "retreat",       "撤退",         { "撤退", "后撤", "提前撤", "撤回去" }, ActKind::Move, 85 },
    { "evac",          "撤离",         { "撤离", "撤出", "脱离战斗", "快跑", "撤出去", "离开这里" }, ActKind::Move, 84 },
    { "hold",          "原地待命",     { "等待", "待命", "原地", "别动", "停下", "停住" }, ActKind::Move, 54 },
    { "spread",        "散开",         { "散开", "拉开距离", "分散", "拉开" }, ActKind::Move, 53 },
    { "takeCover",     "隐蔽",         { "隐蔽", "躲起来", "找掩体", "趴下", "躲好", "藏起来", "进入隐蔽" }, ActKind::Move, 52 },
    { "followMe",      "跟我来",       { "跟我来", "跟随我", "跟上我", "跟着我", "跟我走", "跟随" }, ActKind::Move, 51 },
    { "fallback",      "后退",         { "后退", "退后", "往回走", "退回去", "往后撤" }, ActKind::Move, 50 },
    { "flank",         "包围/包抄",    { "包围", "包抄", "绕过去", "迂回", "绕到侧面", "走侧翼" }, ActKind::Move, 49 },
    { "advance",       "前进",         { "前进", "推进", "往前走", "压上去", "向前推进", "上前" }, ActKind::Move, 48 },
    { "gotoPoint",     "前往地点",     { "前往", "移动到", "转移到", "去到", "走到", "去往", "去" }, ActKind::Move, 47, true },
    { "moveLeft",      "左移",         { "左移", "向左移动", "往左边移", "向左" }, ActKind::Move, 46 },
    { "moveRight",     "右移",         { "右移", "向右移动", "往右边移", "向右" }, ActKind::Move, 46 },
    { "enterBuilding", "进屋/进掩体",  { "进屋", "进房子", "进建筑", "进屋警戒" }, ActKind::Move, 45 },
    { "focusFire",     "集火",         { "集中火力", "集火", "打那个", "打这个", "一起打", "打准星那个", "都打", "打它" }, ActKind::Combat, 82 },
    { "atTank",        "打坦克",       { "打坦克", "攻击坦克", "反坦克", "炸坦克", "干掉坦克", "打掉坦克" }, ActKind::Atk, 80, false, "tank" },
    { "atAPC",         "打装甲车",     { "打装甲车", "攻击装甲车", "打运兵车", "炸装甲车", "打装甲" }, ActKind::Atk, 79, false, "apc" },
    { "atJeep",        "打吉普",       { "打吉普车", "打吉普", "打小车", "攻击吉普" }, ActKind::Atk, 78, false, "jeep" },
    { "atTruck",       "打卡车",       { "打卡车", "打货车", "打运兵卡车", "攻击卡车", "打那辆卡车", "打补给车", "打军卡", "打运输车", "炸卡车" }, ActKind::Atk, 77, false, "truck" },
    { "atMG",          "打机枪手",     { "打机枪手", "干掉机枪手", "打机枪" }, ActKind::Atk, 76, false, "mg" },
    { "atOfficer",     "打军官",       { "打指挥官", "打军官", "干掉军官", "打当官的" }, ActKind::Atk, 76, false, "officer" },
    { "atInf",         "打步兵",       { "打步兵群", "打步兵", "打敌人", "打人", "清理步兵" }, ActKind::Atk, 74, false, "inf" },
    { "coverMe",       "掩护我",       { "掩护我前进", "掩护我", "帮我压制", "掩护", "压制他们" }, ActKind::Combat, 73 },
    { "suppress",      "火力压制",     { "火力压制", "压制", "压住他们", "压住", "别让他们抬头" }, ActKind::Combat, 72 },
    { "grenade",       "投手雷",       { "投手雷", "扔手榴弹", "手榴弹", "扔雷", "投雷" }, ActKind::Combat, 66 },
    { "smoke",         "放烟雾",       { "放烟雾", "烟雾弹", "打烟雾", "烟幕", "放烟" }, ActKind::Combat, 65 },
    { "rocket",        "用火箭筒",     { "用火箭筒", "火箭筒", "打火箭" }, ActKind::Combat, 64 },
    { "freeFire",      "自由射击",     { "自由射击", "随意开火", "自由开火" }, ActKind::Combat, 60 },
    { "fire",          "开火",         { "开火", "射击", "还击", "打" }, ActKind::Combat, 55 },
    { "rescue",        "救援伤员",     { "救伤员", "救人", "去救", "抢救", "救援", "救" }, ActKind::Support, 68 },
    { "drag",          "拖回伤员",     { "拖回来", "拖到掩体", "拉回来", "拖走", "拉回" }, ActKind::Support, 67 },
    /* grabBox 的物件名**逐关不同**（密码本 / 布防图 / 团部密码本 / 师部电台 /
       连队花名册 / 观察哨日志），但这张表是静态的 —— 于是原先只有"拿密码箱 /
       搬箱子"几个说法能匹配上。后果不是"少一种说法"，而是**一类目标永远达不成**：
       机械剧本那句"铁头，搬密码箱"实测一直是 ok=0（"搬密码箱"三个字里没有任何
       一个键是它的子串），第五关"带出连队花名册"因此从来没打勾过。
       所以这里把六关的物件名都收进来 —— 玩家按简报里那个词说就能识别。 */
    { "grabBox",       "搬运物件",     { "拿密码箱", "抢密码箱", "搬密码箱", "搬箱子", "搬运",
                                         "拿物资", "拿箱子", "拿文件", "带出文件",
                                         "密码箱", "密码本", "布防图", "电台", "花名册", "名册",
                                         "记录本", "观察哨日志", "日志", "文件" }, ActKind::Support, 66 },
    { "heal",          "治疗",         { "治疗", "包扎", "医疗", "止血" }, ActKind::Support, 63 },
    { "resupply",      "给弹药",       { "给弹药", "送弹药", "弹药补给", "补弹", "补给" }, ActKind::Support, 62 },
    { "setup",         "架设武器",     { "架设", "架机枪", "设置火力点", "布置" }, ActKind::Support, 44 },
    { "reportRemain",  "剩余敌人",     { "剩余敌人", "还剩多少敌人", "还有多少敌人", "战果" }, ActKind::Report, 75 },
    { "reportContact", "发现敌人",     { "发现敌人", "报告敌情", "有没有敌人", "敌人在哪" }, ActKind::Report, 74 },
    { "reportCas",     "报告伤亡",     { "报告伤亡", "伤亡情况", "谁受伤了", "伤情" }, ActKind::Report, 73 },
    { "reportAmmo",    "报告弹药",     { "报告弹药", "还有多少子弹", "弹药情况" }, ActKind::Report, 72 },
    { "reportPos",     "报告位置",     { "报告位置", "你的位置", "你在哪", "位置" }, ActKind::Report, 71 },
    { "reportAll",     "报告状态",     { "报告状态", "汇报", "报告" }, ActKind::Report, 70 },
    { "ready",         "就位",         { "已就位", "就位", "到位" }, ActKind::Report, 69 },
};

const ActionDef *action_by_id(const std::string &id) {
    for (const auto &a : ACTIONS) if (id == a.id) return &a;
    return nullptr;
}

// --------------------------------------------------------------- 呼号词表
const std::vector<const char *> CS_ALL = {
    "全体人员", "所有队员", "所有单位", "全体", "所有人", "全员", "全队", "大家", "全部"
};

const std::vector<CsGroup> CS_GROUP = {
    { "反坦克小组", "反坦克组" }, { "火力小组", "火力组" }, { "支援小组", "支援组" },
    { "第一组", "1组" }, { "第二组", "2组" }, { "一组", "1组" }, { "二组", "2组" },
    { "1组", "1组" }, { "2组", "2组" },
    { "火力组", "火力组" }, { "反坦克组", "反坦克组" }, { "支援组", "支援组" },
};

const std::vector<LocKey> LOC_KEYS = {
    { "南侧高地", "A" }, { "高地", "A" }, { "A点", "A" }, { "a点", "A" }, { "A区", "A" },
    { "北侧岩石", "B" }, { "岩石", "B" }, { "壕沟", "B" }, { "B点", "B" }, { "b点", "B" },
    { "水库", "B" }, { "大坝", "B" }, { "水坝", "B" },
    { "桥梁", "C" }, { "桥", "C" }, { "隘口", "C" }, { "C点", "C" }, { "c点", "C" }, { "撤离点", "C" },
    { "油桶", "D" }, { "D点", "D" }, { "d点", "D" },
    { "南侧树林", "E" }, { "树林", "E" }, { "密林", "E" }, { "E点", "E" }, { "e点", "E" },
    { "东侧入口", "F" }, { "东口", "F" }, { "F点", "F" },
    { "西侧出口", "G" }, { "西口", "G" }, { "出口", "G" }, { "G点", "G" },
};

// bearing：0/90/180/270 为绝对方位；负值为相对方位哨兵（DIR_L / DIR_R / DIR_FL / DIR_FR）
const std::vector<DirKey> DIR_KEYS = {
    { "北边", 0 }, { "北面", 0 }, { "南边", 180 }, { "南面", 180 },
    { "东边", 90 }, { "东面", 90 }, { "西边", 270 }, { "西面", 270 },
    { "左翼", DIR_L }, { "右翼", DIR_R }, { "左边", DIR_L }, { "左面", DIR_L }, { "左侧", DIR_L },
    { "右边", DIR_R }, { "右面", DIR_R }, { "右侧", DIR_R },
    { "左前方", DIR_FL }, { "右前方", DIR_FR },
};

const std::vector<TargetNoun> TARGET_NOUNS = {
    { "装甲车", "apc" }, { "运兵车", "apc" }, { "装甲", "apc" }, { "坦克", "tank" },
    { "吉普", "jeep" }, { "小车", "jeep" },
    { "卡车", "truck" }, { "货车", "truck" }, { "军卡", "truck" }, { "运输车", "truck" }, { "补给车", "truck" },
    { "机枪手", "mg" }, { "机枪", "mg" },
    { "指挥官", "officer" }, { "军官", "officer" }, { "当官的", "officer" },
    { "步兵", "inf" },
};

const char *target_act_of(const std::string &type) {
    if (type == "tank")    return "atTank";
    if (type == "apc")     return "atAPC";
    if (type == "jeep")    return "atJeep";
    if (type == "truck")   return "atTruck";
    if (type == "mg")      return "atMG";
    if (type == "officer") return "atOfficer";
    if (type == "inf")     return "atInf";
    return nullptr;
}

const Replies REPLIES = {
    { "收到", "明白", "执行", "收到，执行" },
    { "被压制，等一下", "等一下，我在换弹", "稍等，压得抬不起头" },
    { "不行，太危险", "没弹药了", "不行，我这儿被压住了" },
    { "我不行了！", "顶不住了，跑吧！", "别打了，太多了！" },
    { "我中弹了！", "需要医疗！", "我倒了……" },
};

// --------------------------------------------------- 中文归一 / 字符相似度

// 需要剔除的标点（对应 /[\s,，。、；;!！?？.．"“”'‘’`~·\-—_（）()\[\]【】]/g）
static bool is_stripped(uint32_t c) {
    switch (c) {
        case ' ': case '\t': case '\n': case '\r':
        case ',': case ';': case '!': case '?': case '.': case '"':
        case '\'': case '`': case '~': case '-': case '_':
        case 0x3000:  // 全角空格
        case 0xFF0C:  // ，
        case 0x3002:  // 。
        case 0x3001:  // 、
        case 0xFF1B:  // ；
        case 0xFF01:  // ！
        case 0xFF1F:  // ？
        case 0xFF0E:  // ．
        case 0x201C: case 0x201D:  // “ ”
        case 0x2018: case 0x2019:  // ‘ ’
        case 0x00B7:               // ·
        case 0x2014: case 0x2013:  // — –
        case 0xFF08: case 0xFF09:  // （ ）
        case 0x0028: case 0x0029:  // ( )
        case 0x005B: case 0x005D:  // [ ]
        case 0x3010: case 0x3011:  // 【 】
            return true;
        default:
            return false;
    }
}

std::string normalize_order(const std::string &t) {
    std::vector<uint32_t> out;
    out.reserve(t.size());
    for (uint32_t c : utf8_codes(t)) {
        if (c >= 'A' && c <= 'Z') c += 32;      // toLowerCase（仅 ASCII，与 JS 行为一致）
        if (is_stripped(c)) continue;
        if (c == 0x4E00) { out.push_back('1'); continue; }          // 一 → 1
        if (c == 0x4E8C || c == 0x4E24) { out.push_back('2'); continue; }  // 二 / 两 → 2
        if (c == 0x4E09) { out.push_back('3'); continue; }          // 三 → 3
        out.push_back(c);
    }
    return utf8_from_codes(out);
}

float char_sim(const std::string &a, const std::string &b) {
    if (a.empty() || b.empty()) return 0.0f;
    if (a == b) return 1.0f;
    const std::vector<uint32_t> A = utf8_codes(a);
    const std::vector<uint32_t> B = utf8_codes(b);
    const int n = (int)A.size(), m = (int)B.size();
    std::vector<int> dp(m + 1, 0);
    for (int i = 1; i <= n; ++i) {
        int prev = 0;
        for (int j = 1; j <= m; ++j) {
            const int tmp = dp[j];
            dp[j] = std::max(std::max(dp[j], dp[j - 1]), prev + (A[i - 1] == B[j - 1] ? 1 : 0));
            prev = tmp;
        }
    }
    return (float)dp[m] * 2.0f / (float)(n + m);
}

} // namespace va
