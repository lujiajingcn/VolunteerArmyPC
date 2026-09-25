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
/* 五个伏击阵地 = 五个关卡。
   【目标只剩一条：拖延】不需要夺取敌人的密码箱，也不需要"撤出 N 人" ——
   每一关的判据都是同一句话：**把敌人钉在这里 240 秒**。
   为什么连撤离门槛一起去掉：本作要的是"逐次抵抗"的节奏感 ——
   打满 240 秒就转进下一个阵地；伤亡过半时你也可以**提前撤**（见 retreat），
   但那时候只拖了多少秒就记多少秒，战报里写得明明白白。
   "拖够时间"和"提前撤"是两条出口，都通向下一个阵地，区别只在战报。

   【missionEnd 必须 > Delay 的 need】update_flow 里"超时判死"那一段排在
   check_end **之前**，两者取同一个数的话，撑到 240 秒那一刻会先被判成超时失败。
   留 35 秒的余量，语义是"拖够 240 秒就走，不会在原地多待"。 */
static const float kDelayNeed = 240.0f;

static const LevelDef L1{
    "l1_yunvfeng", "第一关 · 玉女峰", "1951.5.30", "玉女峰 · 前出警戒阵地",
    "63军187师561团3营（前出警戒分队）", "美骑兵第1师搜索分队",
    "5 月 30 日夜，各师进入阵地。你们是 3 营派出的前出警戒分队，卡在玉女峰南面的"
    "公路拐弯上。任务不是死守，也不是夺什么物件 —— 是**把敌人钉在这里 240 秒**，"
    "打乱他的展开节奏，然后带着人转进下一个阵地。",
    "史实：187 师进入阵地当夜即派警戒分队前出二十公里，迫使美军于 6 月 1 日提前展开"
    "进攻，为军主力抢修工事抢出了一天。",
    TerrainTheme::Pass, 1900, 1250, 620, 58, 9.0f, 34.0f, 0.35f,
    { "玉女峰主峰", "北坡侧射位", "西侧隘口", "路边油桶", "南麓密林", "东路（敌来向）", "西路（撤回）" },
    30, 120, 165, 275, 480,
    false, false, false, false,
    10, 14,
    { { "jeep", "jeep", "apc", "truck" }, { "jeep", "apc", "truck", "jeep" } },
    {
        { GoalKind::Delay, "拖住敌人 240 秒", true, kDelayNeed },
    },
    0, "", 0.0f,
};

static const LevelDef L2{
    "l2_233", "第二关 · 233.2 高地", "1951.6.1", "233.2 高地 · 涟川以南第二道阻击线",
    "63军187师561团3营", "美骑兵第1师 5 个营 + 4 个炮兵营 + 11 辆坦克",
    "敌人的主攻方向选在了西线 —— 就是你们这里。正面不到三公里，对面是两个师。"
    "233.2 高地一丢，涟铁公路就通了。再拖 240 秒，能拖一秒是一秒。",
    "史实：561 团 3 营在涟川山口一线坚守四天三夜，抗击数倍于己的敌人十余次进攻，"
    "毙伤美军 1300 余人，战后被授予「守如泰山」锦旗。",
    TerrainTheme::Ridges, 2100, 1300, 640, 66, 6.0f, 24.0f, 0.55f,
    { "233.2主峰", "东南鞍部", "西侧谷口", "弹药堆", "北台树林", "东路（骑1师）", "西路（通铁原）" },
    30, 120, 160, 275, 480,
    false, false, false, false,
    12, 18,
    { { "jeep", "apc", "tank", "apc", "jeep" }, { "jeep", "tank", "apc", "apc", "jeep" },
      { "jeep", "apc", "apc", "tank", "jeep" } },
    {
        { GoalKind::Delay, "拖住敌人 240 秒", true, kDelayNeed },
    },
    0, "", 0.0f,
};

static const LevelDef L3{
    "l3_zhongzishan", "第三关 · 种子山", "1951.6.2 — 6.3", "种子山 · 五峰寺以北",
    "63军189师566团", "伪陆战第1团 / 伪9师 / 加拿大25旅 → 美第25师",
    "白天丢了就夜里夺回来。眼下你们的任务更简单也更难：站在这里，**别让敌人**"
    "**在 240 秒内走过去**。弹药不够就省着打，人在阵地在。",
    "史实：6 月 2 日激战竟日，种子山、五峰寺及以南阵地相继失守；当夜 566 团以"
    "一连、三连各一个排组成敢死队，于 3 日凌晨夜袭种子山，全歼守敌夺回阵地。",
    TerrainTheme::Ridges, 2300, 1350, 700, 66, 6.0f, 24.0f, 0.55f,
    { "种子山主峰", "北坡侧射位", "五峰寺", "油桶堆", "南麓密林", "东路（美25师）", "西路" },
    30, 120, 160, 275, 480,
    false, false, false, false,
    12, 18,
    { { "jeep", "apc", "tank", "truck", "apc", "jeep" }, { "jeep", "tank", "apc", "truck", "apc", "jeep" } },
    {
        { GoalKind::Delay, "拖住敌人 240 秒", true, kDelayNeed },
    },
    0, "", 0.0f,
};

static const LevelDef L4{
    "l4_jiufeng", "第四关 · 鹫峰", "1951.6.3", "鹫峰 · 第二道防线左翼",
    "63军189师（减员严重的几个连）", "美第25师 3 个团 + 坦克集群",
    "美 25 师加入了。189 师的所有营连都已经不成建制，师团机关的勤务人员都上了阵地。"
    "再撑 240 秒，把阵地交给接防的部队 —— 你们的价值是**时间**，不是战果。",
    "史实：6 月 3 日拂晓美 25 师 3 个团加入战斗，以坦克为先导突入纵深；189 师"
    "战斗减员严重，坚守到天黑才奉命将阵地移交给军预备队 188 师。",
    TerrainTheme::LoneHill, 2000, 1400, 720, 80, 2.5f, 10.0f, 0.7f,
    { "鹫峰主阵地", "东岩", "细柳洞", "油桶堆", "北台树林", "东路", "西路" },
    30, 120, 150, 275, 480,
    false, false, false, false,
    14, 20,
    { { "jeep", "tank", "apc", "tank", "truck", "jeep" }, { "tank", "jeep", "tank", "apc", "truck", "jeep" } },
    {
        { GoalKind::Delay, "拖住敌人 240 秒", true, kDelayNeed },
    },
    0, "", 0.0f,
};

static const LevelDef L5{
    "l5_shazongdong", "第五关 · 沙宗洞", "1951.6.5 — 6.6", "沙宗洞 · 最后一道阻击线",
    "63军188师563团", "美骑兵第1师加强团",
    "志司已经下令改坚守防御为机动防御。沙宗洞三面受敌，背后是悬崖绝壁 —— "
    "敌人会分两路从侧翼迂回。这是最后一个阵地：再拖 240 秒，铁原以北的新防线就起来了。",
    "史实：6 月 6 日敌以一个营分两路迂回 207 高地，563 团 1 连 2 排三面受敌、"
    "背后是悬崖，战至午夜只剩 8 人，弹药耗尽后跳下悬崖，5 人牺牲、3 人生还，"
    "后被授予「宁死不屈的八勇士」。",
    TerrainTheme::Gorge, 2200, 1300, 650, 70, 4.5f, 18.0f, 0.5f,
    { "沙宗洞主峰", "北坡", "下浦渡口", "弹药堆", "洞北密林", "东路", "西路（悬崖）" },
    30, 120, 150, 275, 480,
    false, false, true, false,
    16, 22,
    { { "jeep", "tank", "apc", "tank", "truck", "apc", "jeep" },
      { "tank", "jeep", "tank", "apc", "tank", "truck", "jeep" } },
    {
        { GoalKind::Delay, "拖住敌人 240 秒", true, kDelayNeed },
    },
    0, "", 0.0f,
};

const std::vector<LevelDef> LEVELS = { L1, L2, L3, L4, L5 };


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

    // ---- 开局站位：按**职务**分到四个区 ----
    /* 不能再按 id 手填：五个阵地各有自己的 10 个人（共 50 个名字），
       写死一份 id 表只会让第二关之后的人全部落在默认点（recommend_of 找不到就
       原样返回调用方给的兜底值）—— 表现是"整个班挤在一个点上"，
       而日志里一行报错都没有。按职务分配则天然对任何一份花名册成立。 */
    RECOMMEND.clear();
    const float ax = L.mapW * 0.40f, bx = L.mapW * 0.49f;
    const float ay = L.roadCY + 250.0f, by = L.roadCY - 250.0f;
    RECOMMEND.push_back({ "player", ax, ay });   // 队长（玩家位）
    int nRifle = 0, nAt = 0;
    for (const auto &m : position_men(L.id)) {
        float rx = ax, ry = ay;
        if (m.role == "机枪手")          { rx = ax + 74.0f;              ry = ay + 58.0f; }
        else if (m.role == "狙击手")     { rx = ax - 76.0f;              ry = ay - 16.0f; }
        else if (m.role == "步枪手")     { rx = (nRifle++ == 0) ? ax + 30.0f : ax - 44.0f;
                                           ry = (nRifle == 1) ? ay + 112.0f : ay + 78.0f; }
        else if (m.role == "反坦克手")   { rx = (nAt++ == 0) ? bx - 22.0f : bx + 36.0f;
                                           ry = (nAt == 1) ? by - 36.0f : by + 18.0f; }
        else if (m.role == "爆破手")     { rx = L.mapW * 0.56f;          ry = L.roadCY + 152.0f; }
        else if (m.role == "医疗兵")     { rx = L.mapW * 0.15f;          ry = L.roadCY + 250.0f; }
        else                             { rx = L.mapW * 0.19f;          ry = L.roadCY + 206.0f; }
        /* id 取自 POSITION_ROSTERS（静态常量表），c_str() 的寿命够用 ——
           RecommendPos::id 就是 const char*。 */
        RECOMMEND.push_back({ m.id.c_str(), rx, ry });
    }
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
    CARRY.aliveAtCapture = 0;
    CARRY.downAtCapture = 0;
    for (auto &u : W.units) {
        if (u.team != Team::Ally || u.dead) continue;
        /* **带走的条件改成了"还活着就行"**，不再要求"走到撤离点"：
           五个阵地的任务只有拖延一条，过关的两条出口是「拖够 240 秒」和
           「伤亡过半后主动撤退」，两条都不经过撤离点。沿用旧的 95/160 距离判据
           会让第二关一开局只剩玩家一个人 —— 因为没人走到过撤离点，
           而那种失败在结算面板上只表现为"带队 1 人"。
           **阵亡的不复活**（u.dead 上面就跳过了）—— 这是"严格继承"的落点；
           倒地的照带，下一关开局仍是失能，医疗兵救起来才算恢复。 */
        CarryUnit c;
        c.id = u.id;
        c.name = u.name;
        c.hp = u.downed ? std::max(1.0f, u.maxHp * 0.30f) : clampf(u.hp, 1.0f, u.maxHp);
        c.maxHp = u.maxHp;
        c.morale = clampf(u.morale, 10.0f, 100.0f);
        c.ammo = std::max(6, u.ammo);
        c.magAmmo = u.magAmmo;
        c.rockets = u.rockets;
        c.grenades = u.grenades;
        c.smokes = u.smokes;
        c.downed = u.downed;
        if (u.downed) ++CARRY.downAtCapture; else ++CARRY.aliveAtCapture;
        c.kills = u.kills; c.shots = u.shots; c.hits = u.hits;
        CARRY.units.push_back(c);
    }
    /* **玩家阵亡了就不往 CARRY 里补人**。
       原来的兜底是"玩家没走到撤离点也强行补一个失能的 player" —— 那是撤离门槛
       时代的产物（少了它玩家就凭空消失）。现在没有撤离门槛，这条兜底只会把
       一个已经阵亡的玩家复活成失能；正确的落点是**下一阵地由该阵地的队长接任**
       （见 init_world），CARRY 里没有 player 正是"该换人了"的信号。 */
}

void commit_level_result(bool win) {
    const LevelDef &L = cur_level();
    CAM.totalDead += W.stats.allyDead;
    CAM.totalEvac += W.stats.evacCount;
    CAM.totalTime += W.t;
    if (win) CAM.cleared++;
    /* 战报改成报**拖延了多少秒**（本作的唯一指标）+ 阵亡 + 带出几人。
       不再报"撤离 N 人" —— 没有撤离门槛，那一栏恒为 0，
       看战报的人只会以为自己一个人都没带出来。 */
    int alive = 0;
    for (const auto &c : CARRY.units) if (!c.downed) ++alive;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s %s：%s · 拖延 %d/%d 秒%s · 阵亡 %d 人 · 带出 %d 人",
                  L.date, L.place, win ? "达成" : "未完成",
                  (int)W.t, (int)kDelayNeed,
                  W.retreatChoice == 1 ? "（提前撤离）" : "",
                  W.stats.allyDead, alive);
    CAM.log.push_back(buf);
}

} // namespace va
