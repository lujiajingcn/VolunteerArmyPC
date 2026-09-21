// VolunteerArmyPC —— 铁原战役层实现：六关数据 / 铺图 / 目标判定 / 跨关继承
//
// 关卡取自 1951 年 5 月 30 日—6 月 12 日第 63 军在涟川—铁原之间的逐次抵抗。
// 六关全部出自阵地清单里的三道防线与关键据点：
//   L1 玉女峰·沙宗洞（第一道防线前哨，5/30）
//   L2 涟川山口 162 / 167.1 高地（第一道防线核心，6/1）
//   L3 种子山·233.2 高地（第一道防线，6/2 失守 → 6/3 凌晨夜袭夺回）
//   L4 加齿项·477.2 高地（第二道防线，6/3 美 25 师加入后突破）
//   L5 207 高地·255.1 高地（第二道防线纵深，6/5—6/6 八勇士）
//   L6 内外加山 279.5 高地（铁原东南最后屏障，6/9—6/10 水淹七军）
//
// 史实口径：每关只取"这一仗是怎么打的" —— 敌我番号、地形性质、当时的战术难题，
// 具体时间与兵力按游戏节奏压缩（史实一天，游戏里 4~7 分钟）。
#include "sim/va_campaign.h"

#include "sim/va_math.h"
#include "sim/va_world.h"

#include <cmath>

namespace va {

// ------------------------------------------------------------------ 全局状态
CampaignState CAM;
CarryOver     CARRY;
LevelRuntime  LV;

void reset_level_runtime() {
    LV = LevelRuntime{};
}

// ------------------------------------------------------------------ 关卡表
/* missionEnd 既是本关时限，也是 Delay 类目标的判据 —— 两个必须是同一个数，
   否则会出现"撑到了时限但目标还没打勾"这种自相矛盾的画面（简报写 5 分钟，
   目标写 300 秒，玩家按哪个算？）。Delay 关的 missionEnd 比 need 多留 30 秒撤离。 */
static const LevelDef L1{
    "l1_yunv", "第一关 · 玉女峰前哨", "1951.5.30", "涟川以南 · 第一道防线前哨",
    "63军187师561团3营（前出警戒分队）", "美骑兵第1师搜索分队",
    "5 月 30 日夜，各师进入阵地。你们是 3 营派出的前出警戒分队，在玉女峰、沙宗洞"
    "一带前出二十公里。任务不是死守 —— 是**让敌人提前展开**，打掉他的搜索队就走。",
    "史实：187 师进入阵地当夜即派警戒分队前出 20 公里骚扰，迫使美军于 6 月 1 日"
    "提前展开进攻，为军主力抢修工事抢出了一天。",
    TerrainTheme::Gorge, 2200, 1300, 650, 70, 4.5f, 18.0f, 0.5f,
    { "玉女峰主峰", "沙宗洞北坡", "下浦渡口", "前哨弹药堆", "沙宗洞密林", "东路（敌来向）", "西路（撤回）" },
    30, 120, 120, 300, 9999,
    false, false, false, true,
    8, 12,
    { { "jeep", "jeep", "apc", "truck" }, { "jeep", "apc", "truck", "jeep" } },
    {
        { GoalKind::DestroyKind, "打掉敌搜索队的装甲车", true, 1, "apc" },
        { GoalKind::Evac, "撤回下浦渡口", true, 7 },
    },
    7, "警戒哨记录本",
    /* 第一关的集结窗口给得最宽：对面只是搜索分队，火力压不住人，
       从容收拢的代价小。**这一关带走多少人，直接决定第二关的底子** ——
       实测窗口 0（旧行为）时只带走 8 人，第二关对着美骑 1 师崩掉；给 30 秒后带走 11 人。 */
    42.0f,
};

static const LevelDef L2{
    "l2_lianchuan", "第二关 · 涟川山口", "1951.6.1", "涟川山口 · 第一道防线核心",
    "63军187师561团3营", "美骑兵第1师 5 个营 + 4 个炮兵营 + 11 辆坦克",
    "敌人的主攻方向选在了西线 —— 就是你们这里。正面不到三公里，对面是两个师。"
    "卡住山口：162 高地和 167.1 高地一丢，涟铁公路就通了，铁原背后再无险可守。",
    "史实：561 团 3 营在涟川山口坚守四天三夜，抗击数倍于己的敌人十余次进攻，"
    "毙伤美军 1300 余人，战后被授予「守如泰山」锦旗。",
    TerrainTheme::Pass, 1900, 1250, 620, 58, 9.0f, 34.0f, 0.35f,
    { "162高地", "167.1高地", "榛田里北山", "新村北山", "山口南林", "东路（骑1师）", "西路（通铁原）" },
    30, 120, 165, 400, 480,
    false, false, false, true,
    12, 18,
    { { "jeep", "apc", "tank", "apc", "jeep" }, { "jeep", "tank", "apc", "apc", "jeep" },
      { "jeep", "apc", "apc", "tank", "jeep" } },
    {
        { GoalKind::Hold, "守住山口", true, 150 },
        /* 为什么是"打瘫两辆车"而不是"必须敲掉坦克"：950 HP、装甲 0.04 的坦克
           只有火箭筒啃得动，而反坦克组一共 6 发、还要穿过车顶机枪的火网 ——
           实测（离线扫描、默认平衡）真人剧本打掉它的概率约 8/10，
           也就是说**每五局就有一局因为运气而整关判死**，这不是难度，是赌博。
           改成"打瘫两辆车（坦克最好）"：打掉坦克仍然是最优解，
           但打不掉时还能靠打瘫装甲车/卡车完成任务。 */
        { GoalKind::DestroyAny, "打瘫两辆车（坦克最好）", true, 2 },
        { GoalKind::Evac, "撤出战斗", true, 6 },
    },
    6, "山口布防图",
    40.0f,     // 正对着美军主攻方向，窗口太长就是白挨炮 —— 靠"能走的都到了"提前关
};

static const LevelDef L3{
    "l3_zhongzishan", "第三关 · 种子山", "1951.6.2 — 6.3 凌晨", "种子山 · 233.2 高地",
    "63军189师566团", "伪陆战第1团 / 伪9师 / 加拿大25旅 → 美第25师",
    "白天丢了就夜里夺回来。566 团的敢死队会在凌晨发起反击 —— 在那之前，"
    "你们得把阵地**拖住**，别让敌人站稳。入夜后跟着反击队冲上主峰。",
    "史实：6 月 2 日激战竟日，种子山、五峰寺及以南阵地相继失守；当夜 566 团以"
    "一连、三连各一个排组成敢死队，于 3 日凌晨夜袭种子山，全歼守敌夺回阵地。",
    TerrainTheme::Ridges, 2300, 1350, 700, 66, 6.0f, 24.0f, 0.55f,
    { "种子山主峰", "233.2高地", "五峰寺", "弹药堆", "南麓密林", "东路（美25师）", "西路" },
    30, 120, 160, 480, 420,
    true, false, false, true,
    10, 14,
    { { "jeep", "apc", "tank", "truck", "apc", "jeep" }, { "jeep", "tank", "apc", "truck", "apc", "jeep" } },
    {
        { GoalKind::Hold, "白天拖住敌人", true, 90 },
        { GoalKind::Retake, "夜袭夺回种子山主峰", true, 0 },
        { GoalKind::Evac, "撤出战斗", true, 5 },
    },
    5, "团部密码本",
    40.0f,     // 夜战：看得见的敌人少，收拢队伍的时间可以给长一点
};

static const LevelDef L4{
    "l4_jiachixiang", "第四关 · 加齿项", "1951.6.3", "加齿项 · 477.2 高地（第二道防线）",
    "63军189师（打光前最后一天）", "美第25师 3 个团 + 坦克集群",
    "美 25 师加入了。189 师的所有营连都已经不成建制，师团机关的勤务人员都上了阵地。"
    "你们的任务不再是歼灭 —— 是**拖到天黑**，把阵地交给接防的 188 师。",
    "史实：6 月 3 日拂晓美 25 师 3 个团加入战斗，以坦克为先导突入纵深；189 师"
    "战斗减员严重，坚守到天黑才奉命将阵地移交给军预备队 188 师。",
    TerrainTheme::Ridges, 2100, 1300, 640, 72, 5.5f, 22.0f, 0.5f,
    { "加齿项主阵地", "477.2高地", "细柳洞", "油桶堆", "北台树林", "东路", "西路" },
    30, 120, 150, 380, 240,
    false, false, false, true,
    14, 20,
    { { "jeep", "tank", "apc", "tank", "truck", "jeep" }, { "tank", "jeep", "tank", "apc", "truck", "jeep" } },
    {
        { GoalKind::Delay, "拖到天黑（交给 188 师）", true, 260 },
        { GoalKind::ExtractItem, "带出师部电台", true, 0 },
        { GoalKind::Evac, "撤向二线阵地", true, 5 },
    },
    4, "师部电台",
    40.0f,     // 迟滞关：本来就是"且战且退"，收拢正好借着交替掩护做
};

static const LevelDef L5{
    "l5_207", "第五关 · 207 高地", "1951.6.5 — 6.6", "207 高地 · 255.1 高地",
    "63军188师563团", "美骑兵第1师加强团",
    "志司已经下令改坚守防御为机动防御。207 高地三面受敌，背后是悬崖绝壁 —— "
    "敌人会分两路从侧翼迂回。撑住，能带几个走就带几个走。",
    "史实：6 月 6 日敌以一个营分两路迂回 207 高地，563 团 1 连 2 排三面受敌、"
    "背后是悬崖，战至午夜只剩 8 人，弹药耗尽后高呼「胜利属于我们」跳下悬崖，"
    "5 人牺牲、3 人被树枝托住生还，后被授予「宁死不屈的八勇士」。",
    TerrainTheme::LoneHill, 2000, 1400, 720, 80, 2.5f, 10.0f, 0.7f,
    { "207高地", "255.1高地", "北台", "弹药堆", "崖下密林", "东路", "西路（悬崖）" },
    30, 120, 155, 420, 300,
    false, false, true, true,
    12, 18,
    { { "jeep", "apc", "tank", "truck", "apc", "jeep" }, { "jeep", "tank", "apc", "truck", "apc", "jeep" } },
    {
        { GoalKind::Hold, "守住 207 高地", true, 180 },
        { GoalKind::ExtractItem, "带出连队花名册", true, 0 },
        { GoalKind::Evac, "撤向二线阵地", true, 4 },
    },
    4, "连队花名册",
    36.0f,     // 三面受敌，侧翼的火力不会给你慢慢等人的时间
};

static const LevelDef L6{
    "l6_neiwaijia", "第六关 · 内外加山", "1951.6.9 — 6.10", "内外加山 279.5 高地 · 铁原东南最后屏障",
    "63军188师564团5连（两个排 70 余人）", "美第3师15团 + 骑1师5团 · 上百辆坦克装甲车",
    "再往北就是平原，无险可守。山北一百米有一座水库 —— 炸开它，洪水能淹没南面的"
    "平原，把美军装甲集群陷在泥里一整天。代价是：山上的人**再也退不回来**。"
    "炸不炸，你们自己决定。",
    "史实：6 月 10 日晨 5 连自断退路炸开水库，「水淹七军」令敌坦克陷入洪流；"
    "美军出动飞机 707 架次倾泻凝固汽油弹，山体被打得像融化的冰淇淋（当地人称"
    "「冰激凌山」）。5 连以全部牺牲的代价，把一个 200 多米高的小山包守了一整天。",
    TerrainTheme::Reservoir, 2100, 1350, 680, 76, 3.0f, 12.0f, 0.45f,
    { "内外加山279.5", "水库大坝", "铁原北道", "油桶堆", "峡谷口树林", "东路（装甲集群）", "西路" },
    30, 120, 150, 420, 9999,
    false, true, false, true,
    12, 16,
    { { "jeep", "tank", "apc", "tank", "truck", "jeep" },
      { "tank", "jeep", "tank", "apc", "truck", "jeep" } },
    {
        { GoalKind::BlowDam, "炸开水库大坝", true, 0 },
        { GoalKind::Delay, "把装甲集群钉在原地", true, 200 },
        { GoalKind::Evac, "撤往铁原以北", true, 3 },
    },
    3, "观察哨日志",
    38.0f,     // 洪水已经把装甲集群钉住了，最后这段路是六关里最好走的
};

const std::vector<LevelDef> LEVELS = { L1, L2, L3, L4, L5, L6 };

int level_count() { return (int)LEVELS.size(); }
/* 撤离门槛 = min(关卡设定的上限, 开局人数 - 3)，下限 2。
   为什么不能写死一个绝对值：严格继承之下，进第二关的人数是**上一关撤出来的人**，
   第一关撤 7 个，第二关就只有 8 个人；门槛若仍是 6，等于只允许损失 2 人 ——
   涟川山口对着美骑 1 师五个营，这要求没有任何余地（实测五个种子全部卡死在
   "守住山口 √、打瘫两辆车 √、撤离 4~5 < 6"）。
   改成"允许损失 3 人"之后：**打得好的人带得走更多，下一关人手就更宽裕**，
   这个正反馈才是"逐次抵抗"该有的形状 —— 而不是每一关都逼到同一条线上。 */
int effective_evac_need() {
    if (!CAM.active) return BAL.evac_need;
    const int cap = cur_level().evacNeed > 0 ? cur_level().evacNeed : BAL.evac_need;
    int n = LV.startCount - 3;
    if (n < 2) n = 2;
    return n < cap ? n : cap;
}

const LevelDef &level_at(int idx) {
    return LEVELS[(size_t)clampf((float)idx, 0.0f, (float)LEVELS.size() - 1)];
}
const LevelDef &cur_level() { return level_at(CAM.levelIndex); }
bool has_next_level() { return CAM.levelIndex + 1 < (int)LEVELS.size(); }
int  next_level_index() { return CAM.levelIndex + 1; }

// ------------------------------------------------------------------ 铺图
/* 战术点位置按"相对地图"给。**恒 7 个点、顺序固定**：A 主阵地 / B 侧射阵位 /
   C 撤离点 / D 路侧目标 / E 撤退路线 / F 敌来向 / G 敌逃向。
   为什么不下逐关手填坐标：六个地形尺寸不同，手填会出现"撤离点跑到地图外"这类
   只在这一关才暴露的错；按比例生成则六关天然自洽，改地图尺寸也不用回头修坐标。
   系数是按原来那张断头谷地图反推的（A=0.40/roadCY+250 …），所以第一关与
   改动前逐项对齐。 */
struct PtTpl { float fx, fy; float r; const char *desc; };
static const PtTpl kPtTpl[7] = {
    { 0.40f,  250.0f, 135.0f, "主阵地：视野好，会被坦克炮击" },
    { 0.49f, -270.0f, 132.0f, "侧射阵位：反坦克组优先" },
    { 0.17f,    0.0f, 115.0f, "撤离点（可炸桥）" },
    { 0.60f, -128.0f,  62.0f, "路边油桶/弹药，可引爆杀伤步兵" },
    { 0.35f,  380.0f, 150.0f, "密林：隐蔽撤退路线" },
    { 0.92f,    0.0f, 120.0f, "敌人进入点" },
    { 0.06f,    0.0f, 110.0f, "敌人逃跑点" },
};

// 掩体配方：三个阵地各撒多少，公路两侧撒多少
struct PropMix { int rock = 0, tree = 0, bush = 0, trench = 0; };
struct ThemeMix {
    PropMix a, b, e;
    int roadRock = 0, roadTree = 0, roadBush = 0;
    int barrels = 3;
};
static ThemeMix mix_of(TerrainTheme th) {
    switch (th) {
    case TerrainTheme::Pass:      // 山口：陡岩为主，树少，视野窄
        return { { 7, 3, 2, 2 }, { 6, 1, 3, 2 }, { 2, 6, 2, 0 }, 14, 4, 4, 2 };
    case TerrainTheme::Ridges:    // 高地群：几座高地各有掩体，鞍部多壕沟
        return { { 5, 6, 2, 3 }, { 5, 3, 3, 3 }, { 2, 7, 3, 1 }, 16, 7, 6, 3 };
    case TerrainTheme::LoneHill:  // 孤山：山上有点掩体，四周近乎开阔（这才叫孤山）
        return { { 4, 4, 3, 2 }, { 3, 2, 2, 1 }, { 1, 5, 2, 0 }, 8, 3, 5, 2 };
    case TerrainTheme::Reservoir: // 水库：南山有工事，南面平原刻意空着（等着被淹）
        return { { 6, 3, 2, 3 }, { 2, 0, 0, 1 }, { 1, 4, 2, 0 }, 10, 4, 3, 2 };
    case TerrainTheme::Gorge:
    default:
        return { { 5, 8, 2, 0 }, { 5, 0, 4, 0 }, { 2, 7, 2, 0 }, 18, 7, 7, 3 };
    }
}

// 生成时躲开公路：落到路面上的掩体会让车队穿模，也会把伏击圈堵死
static void avoid_road(float &y, float roadCY, float roadHalf) {
    const float lim = roadHalf + 30.0f;
    if (std::fabs(y - roadCY) < lim) {
        y = (y >= roadCY) ? roadCY + lim + 8.0f : roadCY - lim - 8.0f;
    }
}

static void scatter(Rng &pr, std::vector<Prop> &out, float cx, float cy, float spread,
                    const PropMix &m, float roadCY, float roadHalf) {
    for (int i = 0; i < m.rock; ++i) {
        const float a = pr.next() * 6.2831853f, d = pr.range(spread * 0.22f, spread);
        float x = cx + std::cos(a) * d, y = cy + std::sin(a) * d * 0.72f;
        avoid_road(y, roadCY, roadHalf);
        out.push_back(make_rock(x, y, pr.range(15.0f, 28.0f)));
    }
    for (int i = 0; i < m.tree; ++i) {
        const float a = pr.next() * 6.2831853f, d = pr.range(spread * 0.30f, spread);
        float x = cx + std::cos(a) * d, y = cy + std::sin(a) * d * 0.72f;
        avoid_road(y, roadCY, roadHalf);
        out.push_back(make_tree(x, y, pr.range(14.0f, 18.0f)));
    }
    for (int i = 0; i < m.bush; ++i) {
        const float a = pr.next() * 6.2831853f, d = pr.range(spread * 0.25f, spread * 0.85f);
        float x = cx + std::cos(a) * d, y = cy + std::sin(a) * d * 0.72f;
        avoid_road(y, roadCY, roadHalf);
        out.push_back(make_bush(x, y, pr.range(13.0f, 17.0f)));
    }
    for (int i = 0; i < m.trench; ++i) {
        const float a = pr.next() * 6.2831853f, d = pr.range(spread * 0.20f, spread * 0.7f);
        float x = cx + std::cos(a) * d, y = cy + std::sin(a) * d * 0.72f;
        avoid_road(y, roadCY, roadHalf);
        out.push_back(make_trench(x, y, pr.range(70.0f, 130.0f), pr.range(26.0f, 40.0f)));
    }
}

void apply_level(int idx, uint32_t seed) {
    const LevelDef &L = level_at(idx);
    CAM.levelIndex = idx;
    CAM.evacOpenT = -1.0f;      // 集结窗口是"每关一次"的：换关必须关回去

    // ---- 时间轴 / 地图尺寸 ----
    /* CFG 里这些字段是逐关的。** river / bridge 跟着撤离点走**：河横在撤离点前，
       炸桥就是断自己的退路 —— 它在每一关都得成立，所以位置由 C 点反推，
       不能每关手填（手填过一次填错了，桥跑到撤离点东边，炸了等于没炸）。 */
    CFG.W = L.mapW; CFG.H = L.mapH;
    CFG.roadCY = L.roadCY;
    CFG.roadTop = L.roadCY - L.roadHalf;
    CFG.roadBot = L.roadCY + L.roadHalf;
    CFG.tIntel = L.tIntel; CFG.tDeploy = L.tDeploy;
    CFG.convoyIn = L.convoyIn;
    CFG.reinforceAt = L.reinforceAt;
    CFG.missionEnd = L.missionEnd;
    CFG.tailSpeedUp = L.mapW * 0.11f;
    CFG.convoyStopX = L.mapW * 0.46f;
    CFG.convoyGap = 145.0f;

    // ---- 战术点 ----
    POINTS.clear();
    static const char *kKeys[7] = { "A", "B", "C", "D", "E", "F", "G" };
    for (int i = 0; i < 7; ++i) {
        const PtTpl &t = kPtTpl[i];
        TacPoint p;
        p.key = kKeys[i];
        p.name = L.pointNames[i];
        p.shortName = kKeys[i];
        p.x = L.mapW * t.fx;
        p.y = L.roadCY + t.fy;
        p.r = t.r;
        p.desc = t.desc;
        POINTS.push_back(p);
    }
    // 撤离点 = C 点（桥）；备选 = E 点（密林）。炸桥后自动改走 E。
    EVAC_DEFAULT.key = "C"; EVAC_DEFAULT.name = POINTS[2].name;
    EVAC_DEFAULT.x = POINTS[2].x; EVAC_DEFAULT.y = POINTS[2].y;
    EVAC_ALT.key = "E"; EVAC_ALT.name = POINTS[4].name;
    EVAC_ALT.x = POINTS[4].x; EVAC_ALT.y = POINTS[4].y;

    const float cX = POINTS[2].x, cY = POINTS[2].y;
    CFG.riverX1 = cX - 54.0f; CFG.riverX2 = cX + 54.0f;
    CFG.bridgeY1 = cY - 84.0f; CFG.bridgeY2 = cY + 84.0f;

    // ---- 公路：由东向西。系数反推自原版那 10 个点（现已逐关缩放） ----
    ROAD_PATH.clear();
    const float wy[10] = { 50.0f, 18.0f, 5.0f, -2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    const float wx[10] = { 1.05f, 0.95f, 0.86f, 0.64f, 0.41f, 0.24f, 0.19f, 0.14f, 0.07f, -0.03f };
    for (int i = 0; i < 10; ++i) ROAD_PATH.push_back({ L.mapW * wx[i], L.roadCY + wy[i] });
    ROAD_PATH_N = (int)ROAD_PATH.size();
    build_convoy_way();

    // ---- 掩体 ----
    /* 用**独立的一条随机流**（seed 与关卡下标混合），不去动 W 的主 RNG ——
       主 RNG 的抽取顺序决定了天气 / 地雷 / 密码箱位置 / 车队顺序，
       往里插一次调用就会把旧基线全部平移，扫描出来的胜率再也对不上以前的数据。 */
    BASE_PROPS.clear();
    Rng pr; pr.reseed(seed ^ 0x9E3779B9u ^ ((uint32_t)idx * 2654435761u));
    const ThemeMix mx = mix_of(L.theme);
    const float spreadA = L.mapW * (L.theme == TerrainTheme::LoneHill ? 0.070f : 0.110f);
    const float spreadB = L.mapW * 0.095f;
    const float spreadE = L.mapW * 0.105f;
    scatter(pr, BASE_PROPS, POINTS[0].x, POINTS[0].y, spreadA, mx.a, L.roadCY, L.roadHalf);
    scatter(pr, BASE_PROPS, POINTS[1].x, POINTS[1].y, spreadB, mx.b, L.roadCY, L.roadHalf);
    scatter(pr, BASE_PROPS, POINTS[4].x, POINTS[4].y, spreadE, mx.e, L.roadCY, L.roadHalf);

    // 撤离点（桥头）：两侧桥墩墙 + 零散岩石。这是"最后一道门"，掩体必须够。
    BASE_PROPS.push_back(make_wall(cX - 60.0f, cY - 158.0f, 20.0f));
    BASE_PROPS.push_back(make_wall(cX + 60.0f, cY - 158.0f, 20.0f));
    BASE_PROPS.push_back(make_wall(cX - 60.0f, cY + 158.0f, 20.0f));
    BASE_PROPS.push_back(make_wall(cX + 60.0f, cY + 158.0f, 20.0f));
    for (int i = 0; i < 3; ++i) {
        BASE_PROPS.push_back(make_rock(cX + pr.range(-150.0f, 150.0f),
                                       cY + (pr.next() < 0.5f ? -1.0f : 1.0f) * pr.range(120.0f, 210.0f),
                                       pr.range(15.0f, 19.0f)));
    }
    // 路侧目标（油桶 / 弹药堆）
    for (int i = 0; i < mx.barrels; ++i) {
        BASE_PROPS.push_back(make_barrel(POINTS[3].x + pr.range(-26.0f, 26.0f),
                                         POINTS[3].y + pr.range(-24.0f, 24.0f)));
    }
    // 公路两侧零散掩体：沿 x 均匀撒，路南多树、路北多石
    for (int i = 0; i < mx.roadRock; ++i) {
        const float x = L.mapW * (0.10f + 0.84f * ((float)i + pr.range(0.0f, 0.7f)) / (float)mx.roadRock);
        const float side = (i % 2 == 0) ? -1.0f : 1.0f;
        BASE_PROPS.push_back(make_rock(x, L.roadCY + side * pr.range(L.roadHalf + 45.0f, L.roadHalf + 165.0f),
                                       pr.range(15.0f, 20.0f)));
    }
    for (int i = 0; i < mx.roadTree; ++i) {
        const float x = L.mapW * (0.08f + 0.86f * ((float)i + pr.range(0.0f, 0.7f)) / (float)mx.roadTree);
        const float side = (i % 3 == 0) ? -1.0f : 1.0f;
        BASE_PROPS.push_back(make_tree(x, L.roadCY + side * pr.range(L.roadHalf + 60.0f, L.roadHalf + 200.0f),
                                       pr.range(14.0f, 17.0f)));
    }
    for (int i = 0; i < mx.roadBush; ++i) {
        const float x = L.mapW * (0.12f + 0.80f * ((float)i + pr.range(0.0f, 0.7f)) / (float)mx.roadBush);
        const float side = (i % 2 == 0) ? 1.0f : -1.0f;
        BASE_PROPS.push_back(make_bush(x, L.roadCY + side * pr.range(L.roadHalf + 50.0f, L.roadHalf + 190.0f),
                                       pr.range(13.0f, 16.0f)));
    }

    // ---- 水坝（内外加山）：B 点就是那座水库大坝 ----
    reset_level_runtime();
    if (L.hasDam) {
        LV.damX = POINTS[1].x; LV.damY = POINTS[1].y; LV.damR = 75.0f;
        BASE_PROPS.push_back(make_dam(LV.damX, LV.damY, 150.0f));
        // 洪水区 = 大坝南侧那片平原（史实：水漫过田野，坦克陷进泥里）
        LV.floodX1 = L.mapW * 0.26f; LV.floodY1 = L.roadCY + 40.0f;
        LV.floodX2 = L.mapW * 0.88f; LV.floodY2 = L.mapH * 0.96f;
    }

    // ---- 部署区（4 个，包围公路）----
    DEPLOY_ZONES.clear();
    DEPLOY_ZONES.push_back({ "南侧高地 / 主阵地",    L.mapW * 0.27f, L.roadCY + 180.0f,
                             L.mapW * 0.55f, L.roadCY + 420.0f, "适合火力组、狙击手" });
    DEPLOY_ZONES.push_back({ "北侧岩石 / 侧射阵位",  L.mapW * 0.41f, L.roadCY - 360.0f,
                             L.mapW * 0.60f, L.roadCY - 140.0f, "适合反坦克组" });
    DEPLOY_ZONES.push_back({ "东侧路边 / 伏击圈侧翼", L.mapW * 0.51f, L.roadCY - 100.0f,
                             L.mapW * 0.75f, L.roadCY + 140.0f, "适合爆破手埋雷 / 反坦克侧射" });
    DEPLOY_ZONES.push_back({ "后方 / 撤离点",        L.mapW * 0.08f, L.roadCY + 110.0f,
                             L.mapW * 0.24f, L.roadCY + 440.0f, "医疗兵与撤退点" });

    // ---- 开局站位：按角色分到四个区（与原来那份手填表同构） ----
    RECOMMEND.clear();
    const float ax = L.mapW * 0.40f, bx = L.mapW * 0.49f;
    const float ay = L.roadCY + 250.0f, by = L.roadCY - 250.0f;
    const float ex = L.mapW * 0.33f, ey = L.roadCY + 420.0f;
    RECOMMEND.push_back({ "player",  ax,         ay });
    RECOMMEND.push_back({ "laozhou", ax + 74.0f,  ay + 58.0f });
    RECOMMEND.push_back({ "xiaoxia", ax - 76.0f,  ay - 16.0f });
    RECOMMEND.push_back({ "ajie",    ax + 30.0f,  ay + 112.0f });
    RECOMMEND.push_back({ "daliu",   ax - 44.0f,  ay + 78.0f });
    RECOMMEND.push_back({ "shitou",  bx - 22.0f,  by - 36.0f });
    RECOMMEND.push_back({ "houzi",   bx + 36.0f,  by + 18.0f });
    RECOMMEND.push_back({ "alan",    ex,          ey });
    RECOMMEND.push_back({ "laobai",  L.mapW * 0.56f, L.roadCY + 152.0f });
    RECOMMEND.push_back({ "xiaoman", L.mapW * 0.15f, L.roadCY + 250.0f });
    RECOMMEND.push_back({ "tietou",  L.mapW * 0.19f, L.roadCY + 206.0f });
}

// ------------------------------------------------------------------ 目标
// 阵地上敌我各有多少人（Hold / Retake 都用这一个判据）
static void count_at(float cx, float cy, float r, int &ally, int &enemy) {
    ally = 0; enemy = 0;
    for (auto &u : W.units) {
        if (u.dead || u.downed) continue;
        if (distf(u.x, u.y, cx, cy) > r) continue;
        if (u.team == Team::Ally) ++ally; else ++enemy;
    }
}

static int killed_of(const char *vtype) {
    int n = 0;
    for (auto &v : W.vehicles) {
        if (!v.destroyed || v.type != vtype) continue;
        ++n;
    }
    return n;
}
static int killed_any() {
    int n = 0;
    for (auto &v : W.vehicles) if (v.destroyed) ++n;
    return n;
}

bool goal_done(const LevelGoal &g) {
    switch (g.kind) {
    case GoalKind::Hold:        return LV.holdT >= g.need;
    case GoalKind::Delay:       return W.t >= g.need;
    case GoalKind::DestroyKind: return killed_of(g.vtype) >= (int)g.need;
    case GoalKind::DestroyAny:  return killed_any() >= (int)g.need;
    case GoalKind::BlowDam:     return LV.damBlown;
    case GoalKind::Retake:      return LV.retaken;
    case GoalKind::ExtractItem: return W.stats.boxTaken;
    /* Evac 的判据**不看 g.need**，看 effective_evac_need()：
       关卡表里那个数是上限，实际门槛随开局人数浮动。
       两个数字必须共用同一个来源 —— 否则 HUD 写"撤离 0/6"而实际只要 4 人，
       玩家会以为自己已经失败了。 */
    case GoalKind::Evac:        return W.stats.evacCount >= effective_evac_need();
    }
    return false;
}

std::string goal_progress(const LevelGoal &g) {
    char buf[64];
    switch (g.kind) {
    case GoalKind::Hold:
        std::snprintf(buf, sizeof(buf), "(%d/%d 秒)", (int)LV.holdT, (int)g.need);
        return buf;
    case GoalKind::Delay:
        std::snprintf(buf, sizeof(buf), "(%d/%d 秒)", (int)W.t, (int)g.need);
        return buf;
    case GoalKind::DestroyKind:
        std::snprintf(buf, sizeof(buf), "(%d/%d)", killed_of(g.vtype), (int)g.need);
        return buf;
    case GoalKind::DestroyAny:
        std::snprintf(buf, sizeof(buf), "(%d/%d)", killed_any(), (int)g.need);
        return buf;
    case GoalKind::Evac:
        std::snprintf(buf, sizeof(buf), "(%d/%d)", W.stats.evacCount, effective_evac_need());
        return buf;
    case GoalKind::BlowDam:
        return LV.damBlown ? "已炸开" : (cur_level().hasDam ? "未炸开" : "本关无");
    case GoalKind::Retake:
        if (LV.retaken) return "已夺回";
        if (LV.retakeArmed) return "夜袭中";
        return "待入夜";
    case GoalKind::ExtractItem:
        return W.stats.boxTaken ? "已到手" : "未获取";
    }
    return "";
}

void update_level_goals(float dt) {
    const LevelDef &L = cur_level();
    const float ax = POINTS[0].x, ay = POINTS[0].y, ar = POINTS[0].r + 70.0f;

    // 炸坝进度：人数 + 安放秒数（HUD 与排查共用同一个数）
    LV.damWorkers = 0; LV.damPlantT = 0;
    for (auto &u : W.units) {
        if (u.team != Team::Ally || !u.damTask) continue;
        LV.damWorkers++;
        if (u.plantT > LV.damPlantT) LV.damPlantT = u.plantT;
    }

    // Hold：交火之后，只要阵地还在我们手里就累计 —— 敌人压上来把主阵地占了就不算数，
    // 否则"守住 150 秒"会变成"活过 150 秒"，玩家蹲在后方也能打勾。
    bool holdGoal = false;
    for (const auto &g : L.goals) if (g.kind == GoalKind::Hold) holdGoal = true;
    count_at(ax, ay, ar, LV.holdAlly, LV.holdEnemy);
    if (holdGoal && W.triggered && !W.over) {
        if (LV.holdAlly > 0 && LV.holdAlly >= LV.holdEnemy) LV.holdT += dt;
    }

    // Retake（种子山）：入夜后敢死队反击 —— 主峰上敌人清空、我方站稳 8 秒即夺回。
    if (L.forceNight) {
        /* 0.42：夜袭要留出"冲上去 + 站稳 8 秒 + 再撤下来"的时间。
           原先是 0.55，实测等反击队上来时已经快到撤离兜底线了，
           人在半路被"全体撤离"叫回去 —— 夜袭永远差最后一口气。 */
        const float nightAt = L.missionEnd * 0.42f;
        if (!LV.retakeArmed && W.t >= nightAt) {
            LV.retakeArmed = true;
            say("全体", "反击队上来了！跟着冲上主峰！", "sys");
            set_alert("夜袭开始 — 夺回种子山主峰", 3.0f);
        }
        if (LV.retakeArmed && !LV.retaken) {
            const int al = LV.holdAlly, en = LV.holdEnemy;
            /* 判据是"我方在阵地上压倒性占优"而不是"敌人一个不剩"。
               第一版写的是 en == 0（全歼守敌，史实就是这样），实测根本达不成：
               半径 205 内总会剩一两个散兵，玩家清不完，于是这一关必输。
               史实的"全歼"是结果，不是判据 —— 判据该给"夺回"这个动作本身。 */
            if (al >= 2 && al > en) LV.retakeHold += dt;
            else LV.retakeHold = std::max(0.0f, LV.retakeHold - dt * 0.5f);
            if (LV.retakeHold >= 8.0f) {
                LV.retaken = true;
                say("全体", "主峰夺回来了！", "ok");
                toast("夜袭成功：夺回种子山主峰");
            }
        }
    }
}

float flood_speed_mul(float x, float y) {
    if (!LV.damBlown) return 1.0f;
    if (x < LV.floodX1 || x > LV.floodX2) return 1.0f;
    if (y < LV.floodY1 || y > LV.floodY2) return 1.0f;
    /* 0.18 = 陷在泥水里。**史实是"整整一个上午无法前进"，不是"走不动"** ——
       所以给一个很低但不为 0 的数：车还在爬，只是爬得极慢，
       玩家仍能感到"它们在往前拱"，而不是看到一排静止的靶子。 */
    return 0.18f;
}

// ------------------------------------------------------------------ 跨关继承
void capture_carry() {
    CARRY.valid = true;
    CARRY.units.clear();
    for (auto &u : W.units) {
        if (u.team != Team::Ally || u.dead) continue;
        /* 带走的条件：站着的必须真走到撤离点；倒地的只要被抬到撤离点附近就算救回来了
           （下一关开局仍是失能，需要医疗兵救）。
           为什么倒地的不直接算"没了"：那就把"救回所有伤员"这条加分项变成了
           一句空话 —— 救与不救结果一样，玩家没有理由冒着火力去拖人。

           **已经走出去的人（u.evacuated）优先按标志算，不重新量距离。**
           标志是"走到撤离点那一刻"打上的、用的就是下面这个 95/160 规则，
           再量一次只会引入漂移：撤离点本身会在炸桥后**改成南侧树林**
           （update_evac_marker），于是"先在旧撤离点出去的 2 个人"会被判成没带出来 ——
           结算面板写"7 人撤出"、下一关却只带来 5 人。
           实机第一关转进时见到的就是这个 7 vs 5。 */
        const bool reached = u.evacuated
                          || distf(u.x, u.y, W.evac.x, W.evac.y) < (u.downed ? 160.0f : 95.0f);
        if (!reached && !u.isPlayer) continue;
        CarryUnit c;
        c.id = u.id;
        c.hp = u.downed ? std::max(1.0f, u.maxHp * 0.30f) : clampf(u.hp, 1.0f, u.maxHp);
        c.maxHp = u.maxHp;
        c.morale = clampf(u.morale, 10.0f, 100.0f);
        c.ammo = std::max(6, u.ammo);
        c.magAmmo = u.magAmmo;
        c.rockets = u.rockets;
        c.grenades = u.grenades;
        c.smokes = u.smokes;
        c.downed = u.downed;
        c.kills = u.kills; c.shots = u.shots; c.hits = u.hits;
        CARRY.units.push_back(c);
    }
    /* 玩家必须跟着走 —— 否则"率领队友撤往下一个阵地"没有主语。
       玩家挨打只会失能不会阵亡（down_player），所以这里不需要兜底复活。 */
    bool hasPlayer = false;
    for (const auto &c : CARRY.units) if (c.id == "player") hasPlayer = true;
    if (!hasPlayer && W.player != nullptr) {
        CarryUnit c;
        c.id = "player"; c.hp = std::max(1.0f, W.player->maxHp * 0.35f); c.maxHp = W.player->maxHp;
        c.morale = 70; c.ammo = 30; c.magAmmo = 10; c.downed = true;
        CARRY.units.push_back(c);
    }
}

void commit_level_result(bool win) {
    const LevelDef &L = cur_level();
    CAM.totalDead += W.stats.allyDead;
    CAM.totalEvac += W.stats.evacCount;
    CAM.totalTime += W.t;
    if (win) CAM.cleared++;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s %s：%s · 撤离 %d 人 · 阵亡 %d 人 · 用时 %d:%02d",
                  L.date, L.place, win ? "达成" : "未完成",
                  W.stats.evacCount, W.stats.allyDead,
                  (int)(W.t / 60.0f), (int)W.t % 60);
    CAM.log.push_back(buf);
}

} // namespace va
