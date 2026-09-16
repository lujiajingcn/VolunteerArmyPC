// VolunteerArmyPC —— 常量表实现（逐项对应网页版 index.html 458~913 行）
#include "sim/va_config.h"

#include "sim/va_utf8.h"

namespace va {

// ------------------------------------------------------------------ 关卡
const LevelCfg CFG{};

const TacPoint POINTS[7] = {
    { "A", "A 南侧高地",  "A", 880,  900,  135, "树林+岩石，视野好，会被坦克炮击" },
    { "B", "B 北侧岩石",  "B", 1080, 330,  132, "巨石+壕沟，反坦克侧射位" },
    { "C", "C 桥梁/隘口", "C", 360,  650,  115, "窄路+桥，撤离点（可炸桥）" },
    { "D", "D 油桶",      "D", 1330, 522,  62,  "路边油桶，可引爆杀伤步兵" },
    { "E", "E 南侧树林",  "E", 760,  1090, 150, "密林，隐蔽撤退路线" },
    { "F", "F 东侧入口",  "F", 2020, 650,  120, "公路拐弯，先头车进入点" },
    { "G", "G 西侧出口",  "G", 120,  650,  110, "公路直道，敌人逃跑点" },
};
const TacPoint *point_of(const std::string &key) {
    for (int i = 0; i < 7; ++i) if (key == POINTS[i].key) return &POINTS[i];
    return nullptr;
}

const EvacPoint EVAC_DEFAULT{ "C", "C 桥梁/隘口", 360, 650 };
const EvacPoint EVAC_ALT{ "E", "E 南侧树林", 760, 1090 };

const float ROAD_PATH[][2] = {
    { 2320, 700 }, { 2090, 668 }, { 1900, 655 }, { 1400, 648 }, { 900, 650 },
    { 520, 650 },  { 414, 650 },  { 306, 650 },  { 150, 650 },  { -60, 650 },
};
const int ROAD_PATH_N = 10;
std::vector<WayPt> CONVOY_WAY;

void build_convoy_way() {
    CONVOY_WAY.clear();
    for (int i = 0; i < ROAD_PATH_N - 1; ++i) {
        const float x1 = ROAD_PATH[i][0], y1 = ROAD_PATH[i][1];
        const float x2 = ROAD_PATH[i + 1][0], y2 = ROAD_PATH[i + 1][1];
        int n = (int)std::lround(distf(x1, y1, x2, y2) / 60.0f);
        if (n < 1) n = 1;
        for (int k = 0; k < n; ++k) {
            CONVOY_WAY.push_back({ lerpf(x1, x2, (float)k / (float)n), lerpf(y1, y2, (float)k / (float)n) });
        }
    }
    CONVOY_WAY.push_back({ ROAD_PATH[ROAD_PATH_N - 1][0], ROAD_PATH[ROAD_PATH_N - 1][1] });
}

// ------------------------------------------------------------------ 花名册
const std::vector<RosterDef> ROSTER = {
    { "ajie",    "阿杰",  "步枪手",   "1组",   "rifle",  true,  false, { "阿杰", "阿杰尔", "阿杰哥", "杰哥", "阿洁", "阿杰儿" } },
    { "laozhou", "老周",  "机枪手",   "1组",   "mg",     false, false, { "老周", "周哥", "周叔" } },
    { "xiaoxia", "小夏",  "狙击手",   "1组",   "sniper", false, false, { "小夏", "夏姐", "小侠" } },
    { "daliu",   "大刘",  "步枪手",   "1组",   "rifle",  false, false, { "大刘", "大流", "刘哥" } },
    { "alan",    "阿兰",  "步枪手",   "2组",   "rifle",  false, true,  { "阿兰", "阿蓝", "兰姐" } },
    { "shitou",  "石头",  "反坦克手", "2组",   "at",     false, false, { "石头", "石头哥", "石哥" } },
    { "houzi",   "猴子",  "反坦克手", "2组",   "at",     false, false, { "猴子", "猴哥", "猴儿" } },
    { "laobai",  "老白",  "爆破手",   "2组",   "demo",   false, false, { "老白", "白哥", "白叔" } },
    { "xiaoman", "小满",  "医疗兵",   "支援组", "medic",  false, false, { "小满", "满姐", "小曼", "医疗兵", "医生", "军医" } },
    { "tietou",  "铁头",  "弹药/支援", "支援组", "rifle", false, false, { "铁头", "铁头哥", "铁哥" } },
};

const RosterDef *roster_of(const std::string &id) {
    for (const auto &r : ROSTER) if (id == r.id) return &r;
    return nullptr;
}

const std::vector<GroupDef> GROUPS = {
    { "1组",     { "ajie", "laozhou", "xiaoxia", "daliu" } },
    { "2组",     { "alan", "shitou", "houzi", "laobai" } },
    { "火力组",   { "ajie", "laozhou", "daliu" } },
    { "反坦克组", { "shitou", "houzi" } },
    { "支援组",   { "xiaoman", "tietou" } },
};

const std::vector<RoleCall> ROLE_CALL = {
    { "机枪手",   { "laozhou" } },
    { "狙击手",   { "xiaoxia" } },
    { "反坦克手", { "shitou", "houzi" } },
    { "爆破手",   { "laobai" } },
    { "医疗兵",   { "xiaoman" } },
    { "弹药兵",   { "tietou" } },
    { "步枪手",   { "ajie", "daliu", "alan" } },
};

// ------------------------------------------------------------------ 武器表
namespace {
const WeaponSpec W_rifle  { "步枪",      13, 720, 0.12f, 3, 0.34f, 0.045f, 30, 240, 2.2f, 0.12f, 7,  950 };
const WeaponSpec W_mg     { "机枪",      10, 780, 0.09f, 8, 1.00f, 0.075f, 100, 400, 5.0f, 0.15f, 24, 950 };
const WeaponSpec W_sniper { "狙击枪",    72, 1300, 1.70f, 1, 0.00f, 0.006f, 10, 60, 3.2f, 0.22f, 12, 1200 };
const WeaponSpec W_medic  { "步枪",      11, 660, 0.14f, 3, 0.36f, 0.052f, 30, 180, 2.3f, 0.10f, 6,  950 };
const WeaponSpec W_demo   { "步枪+炸药", 13, 700, 0.13f, 3, 0.34f, 0.048f, 30, 210, 2.2f, 0.12f, 7,  950 };
const WeaponSpec W_at     { "火箭筒/步枪", 13, 700, 0.13f, 3, 0.34f, 0.050f, 30, 180, 2.4f, 0.12f, 7, 950 };
const WeaponSpec W_erifle { "敌步枪",    8,  640, 0.16f, 3, 0.50f, 0.075f, 30, 999, 2.6f, 0.10f, 6,  950 };
const WeaponSpec W_emg    { "敌机枪",    7,  720, 0.10f, 10, 1.20f, 0.095f, 120, 999, 5.5f, 0.12f, 20, 950 };
} // namespace

const WeaponSpec *weapon_of(const std::string &key) {
    if (key == "rifle")  return &W_rifle;
    if (key == "mg")     return &W_mg;
    if (key == "sniper") return &W_sniper;
    if (key == "medic")  return &W_medic;
    if (key == "demo")   return &W_demo;
    if (key == "at")     return &W_at;
    if (key == "erifle") return &W_erifle;
    if (key == "emg")    return &W_emg;
    return &W_rifle;
}

const RocketSpec ROCKET{ "火箭弹", 900, 340, 70, 84, 5.5f, 3, 4.0f, 640 };
const ShellSpec  SHELL { "坦克主炮", 640, 190, 96, 11.0f, 1.6f };

// 注意 armor 语义：子弹穿透系数 = v.armor，数值越大越软。
namespace {
const VehicleSpec V_jeep  { "吉普",   150, 46, 24, 78, 0.30f, 2, 0, true,  7, 480, 0.13f, 0.100f, 16, false };
const VehicleSpec V_apc   { "装甲车", 400, 60, 30, 62, 0.16f, 3, 6, true,  9, 520, 0.11f, 0.095f, 24, false };
const VehicleSpec V_tank  { "坦克",   950, 74, 38, 44, 0.04f, 3, 0, true,  9, 520, 0.12f, 0.100f, 20, true  };
const VehicleSpec V_truck { "卡车",   260, 66, 30, 66, 0.60f, 2, 6, false, 0, 0,   0.0f,   0.0f,   0,  false };
} // namespace

const VehicleSpec *vehicle_of(const std::string &type) {
    if (type == "jeep")  return &V_jeep;
    if (type == "apc")   return &V_apc;
    if (type == "tank")  return &V_tank;
    if (type == "truck") return &V_truck;
    return &V_jeep;
}

// ------------------------------------------------------------- 地图物件
Prop make_rock(float x, float y, float r) {
    Prop p; p.type = PropType::Rock; p.x = x; p.y = y; p.r = r;
    p.cover = 0.95f; p.blocksLos = true; p.blocksBullet = true; p.hard = true; return p;
}
Prop make_tree(float x, float y, float r) {
    Prop p; p.type = PropType::Tree; p.x = x; p.y = y; p.r = r;
    p.cover = 0.45f; p.blocksLos = true; p.blocksBullet = false; p.hard = false; p.soft = true; return p;
}
Prop make_bush(float x, float y, float r) {
    Prop p; p.type = PropType::Bush; p.x = x; p.y = y; p.r = r;
    p.cover = 0.35f; p.blocksLos = true; p.blocksBullet = false; p.hard = false; p.soft = true; return p;
}
Prop make_barrel(float x, float y) {
    Prop p; p.type = PropType::Barrel; p.x = x; p.y = y; p.r = 8;
    p.cover = 0.5f; p.blocksLos = true; p.blocksBullet = true; p.hard = true;
    p.explosive = true; p.hp = 30; return p;
}
Prop make_wall(float x, float y, float r) {
    Prop p; p.type = PropType::Wall; p.x = x; p.y = y; p.r = r;
    p.cover = 1.0f; p.blocksLos = true; p.blocksBullet = true; p.hard = true; return p;
}
Prop make_trench(float x, float y, float w, float h) {
    Prop p; p.type = PropType::Trench; p.x = x; p.y = y;
    p.r = std::max(w, h) / 2.0f; p.w = w; p.h = h;
    p.cover = 0.85f; p.blocksLos = false; p.blocksBullet = false; p.hard = false; p.low = true; return p;
}

std::vector<Prop> BASE_PROPS = {
    // A 南侧高地：树林 + 岩石
    make_rock(806, 876, 22), make_rock(958, 946, 26), make_rock(742, 986, 19), make_rock(902, 1042, 24), make_rock(1010, 856, 18),
    make_tree(760, 838, 16), make_tree(842, 822, 17), make_tree(918, 852, 15), make_tree(690, 902, 16), make_tree(786, 1062, 17),
    make_tree(882, 1108, 16), make_tree(1002, 1050, 15), make_tree(1030, 928, 16), make_tree(650, 990, 15), make_tree(980, 1130, 16),
    make_bush(868, 968, 16), make_bush(940, 1000, 15),
    // B 北侧岩石：巨石 + 壕沟
    make_rock(1024, 316, 27), make_rock(1128, 350, 31), make_rock(1078, 430, 23), make_rock(1180, 262, 20), make_rock(966, 400, 19),
    make_bush(1050, 370, 15), make_bush(1104, 302, 14), make_bush(1140, 420, 15), make_bush(1000, 258, 14),
    // E 南侧树林：密林
    make_tree(660, 1060, 17), make_tree(722, 1140, 16), make_tree(800, 1190, 17), make_tree(880, 1240, 16), make_tree(560, 1140, 16),
    make_tree(620, 1210, 15), make_tree(760, 1250, 16), make_tree(900, 1160, 15), make_bush(700, 1080, 16), make_bush(820, 1120, 15),
    // C 桥梁/隘口
    make_wall(300, 492, 20), make_wall(424, 492, 20), make_wall(300, 808, 20), make_wall(424, 808, 20),
    make_rock(268, 596, 16), make_rock(456, 596, 15), make_rock(360, 852, 18),
    // D 油桶（可引爆）
    make_barrel(1312, 512), make_barrel(1338, 526), make_barrel(1324, 556),
    // 公路两侧零散掩体
    make_rock(1250, 780, 17), make_rock(1494, 800, 19), make_rock(1700, 812, 18), make_rock(1900, 786, 17),
    make_rock(1150, 520, 16), make_rock(1420, 506, 18), make_rock(1650, 498, 17), make_rock(1860, 530, 18), make_rock(2050, 540, 16),
    make_tree(1600, 880, 16), make_tree(1780, 900, 15), make_tree(1440, 900, 16), make_tree(1980, 880, 15),
    make_tree(1200, 430, 15), make_tree(1560, 388, 16), make_tree(1760, 400, 15),
    make_rock(700, 540, 18), make_rock(560, 758, 17), make_rock(830, 790, 16), make_rock(1000, 762, 17),
    make_bush(1620, 760, 15), make_bush(1520, 900, 15), make_bush(1880, 720, 15),
};

const DeployZone DEPLOY_ZONES[4] = {
    { "南侧树林 / 高地",     600,  790, 1210, 1210, "适合火力组、狙击手" },
    { "北侧壕沟 / 岩石",     900,  230, 1320, 480,  "适合反坦克组" },
    { "东侧路边 / 伏击圈侧翼", 1120, 500, 1660, 920,  "适合爆破手埋雷 / 反坦克侧射" },
    { "后方 C 点",           180,  760, 520,  1090, "医疗兵与撤退点" },
};

const std::vector<RecommendPos> RECOMMEND = {
    { "player",  872,  872  },
    { "laozhou", 946,  930  }, { "xiaoxia", 796, 856 },
    { "ajie",    902,  984  }, { "daliu",   828, 950 },
    { "shitou",  1058, 356  }, { "houzi",   1116, 404 },
    { "alan",    706,  1118 }, { "laobai",  1234, 802 },
    { "xiaoman", 322,  900  }, { "tietou",  420, 856 },
};

bool recommend_of(const std::string &id, float &ox, float &oy) {
    for (const auto &r : RECOMMEND) {
        if (id == r.id) { ox = r.x; oy = r.y; return true; }
    }
    return false;
}

} // namespace va
