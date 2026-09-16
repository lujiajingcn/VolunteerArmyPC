   断头谷公路 · 伏击指挥   —— 单文件实现
   依据策划文档：语音指令表 / 队友 AI 状态机 / 第一关关卡设计
   ========================================================================= */

/* ---------------------------------------------------------------- 工具 */
const clamp = (v, a, b) => v < a ? a : v > b ? b : v;
const lerp = (a, b, t) => a + (b - a) * t;
const dist = (ax, ay, bx, by) => Math.hypot(ax - bx, ay - by);
const dist2 = (ax, ay, bx, by) => (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
const angDiff = (a, b) => { let d = (a - b) % (Math.PI * 2); if (d > Math.PI) d -= Math.PI * 2; if (d < -Math.PI) d += Math.PI * 2; return d; };
const norm = v => { let d = v % (Math.PI * 2); if (d > Math.PI) d -= Math.PI * 2; if (d < -Math.PI) d += Math.PI * 2; return d; };
function mulberry32(a) { return function () { a |= 0; a = a + 0x6D2B79F5 | 0; let t = Math.imul(a ^ a >>> 15, 1 | a); t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; }; }
let RNG = mulberry32(Date.now() & 0xffff);
const rr = (a, b) => a + RNG() * (b - a);
const ri = (a, b) => Math.floor(rr(a, b + 1));
const pick = arr => arr[Math.floor(RNG() * arr.length)];

function segCircle(x1, y1, x2, y2, cx, cy, r) {
  const dx = x2 - x1, dy = y2 - y1;
  const fx = x1 - cx, fy = y1 - cy;
  const a = dx * dx + dy * dy; if (a < 1e-6) return dist2(x1, y1, cx, cy) <= r * r;
  let t = -(fx * dx + fy * dy) / a; t = clamp(t, 0, 1);
  const px = x1 + dx * t - cx, py = y1 + dy * t - cy;
  return px * px + py * py <= r * r;
}
/* 视线判定
   - 若射手/目标本身就贴着（或站在）该掩体上，不算它遮挡，否则「躲在石头后」会变成「完全瞎」
   - losFire  ：真正挡子弹的东西（岩石/石墙/油桶 + 烟雾）—— 开枪前用它
   - losSight ：再加树冠的远距离遮蔽 —— 感知/索敌用它
   - losBlocked：保留为「含树木的完全遮挡」（供地图/调试使用） */
function losCore(x1, y1, x2, y2, opts) {
  const hard = opts.hard, soft = opts.soft, smoke = opts.smoke;
  for (let i = 0; i < World.props.length; i++) {
    const p = World.props[i];
    if (p.destroyed) continue;
    if (hard && p.blocksBullet) {
      const rr2 = p.r + 4;
      if (dist2(x1, y1, p.x, p.y) < rr2 * rr2) continue;
      if (dist2(x2, y2, p.x, p.y) < rr2 * rr2) continue;
      if (segCircle(x1, y1, x2, y2, p.x, p.y, p.r)) return true;
    } else if (soft && p.soft && p.blocksLOS) {
      if (segCircle(x1, y1, x2, y2, p.x, p.y, p.r * 0.7)) return true;
    }
  }
  if (smoke) {
    const sm = World.smokes;
    if (sm) for (let i = 0; i < sm.length; i++) {
      const s = sm[i];
      if (segCircle(x1, y1, x2, y2, s.x, s.y, s.r * 0.8)) return true;
    }
  }
  return false;
}
function losFire(x1, y1, x2, y2) { return losCore(x1, y1, x2, y2, { hard: true, soft: false, smoke: true }); }
function losSight(x1, y1, x2, y2) { return losCore(x1, y1, x2, y2, { hard: true, soft: true, smoke: true }); }
function losBlocked(x1, y1, x2, y2) { return losCore(x1, y1, x2, y2, { hard: true, soft: true, smoke: true }); }

/* ------------------------------------------------------- 世界 / 关卡配置 */
const PX_PER_M = 8;
const CFG = {
  W: 2200, H: 1300,
  roadTop: 580, roadBot: 720, roadCY: 650,
  riverX1: 306, riverX2: 414,
  bridgeY1: 566, bridgeY2: 734,
  tIntel: 30,        // 0:00-0:30 情报
  tDeploy: 120,      // 0:30-2:00 部署
  convoyIn: 175,     // 2:55 先头车进入地图
  reinforceAt: 480,  // 8:00 增援
  missionEnd: 600,   // 10:00 强制结束
  tailSpeedUp: 250,  // 车队接近西侧出口
  convoyStopX: 1020, // 伏击圈停车线（第 0 辆车停在这里，正好压过 1120/1040 两枚头车雷）
  convoyGap: 145,    // 各车之间的排队间距（按车序递推，避免重叠）
};

const POINTS = {
  A: { key: 'A', name: 'A 南侧高地', short: 'A', x: 880, y: 900, r: 135, desc: '树林+岩石，视野好，会被坦克炮击' },
  B: { key: 'B', name: 'B 北侧岩石', short: 'B', x: 1080, y: 330, r: 132, desc: '巨石+壕沟，反坦克侧射位' },
  C: { key: 'C', name: 'C 桥梁/隘口', short: 'C', x: 360, y: 650, r: 115, desc: '窄路+桥，撤离点（可炸桥）' },
  D: { key: 'D', name: 'D 油桶', short: 'D', x: 1330, y: 522, r: 62, desc: '路边油桶，可引爆杀伤步兵' },
  E: { key: 'E', name: 'E 南侧树林', short: 'E', x: 760, y: 1090, r: 150, desc: '密林，隐蔽撤退路线' },
  F: { key: 'F', name: 'F 东侧入口', short: 'F', x: 2020, y: 650, r: 120, desc: '公路拐弯，先头车进入点' },
  G: { key: 'G', name: 'G 西侧出口', short: 'G', x: 120, y: 650, r: 110, desc: '公路直道，敌人逃跑点' },
};
const PT_ALIAS = { A: 'A', B: 'B', C: 'C', D: 'D', E: 'E', F: 'F', G: 'G' };
// 撤离点（可被炸桥改变）
const EVAC_DEFAULT = { key: 'C', x: POINTS.C.x, y: POINTS.C.y, name: 'C 桥梁/隘口' };
const EVAC_ALT = { key: 'E', x: POINTS.E.x, y: POINTS.E.y, name: 'E 南侧树林' };

// 公路路径（东 → 西），带 F 点拐弯
const ROAD_PATH = [
  [2320, 700], [2090, 668], [1900, 655], [1400, 648], [900, 650], [520, 650], [414, 650], [306, 650], [150, 650], [-60, 650]
];
const CONVOY_WAY = [];
(function buildWay() {
  // 沿公路中心插值，供车辆循迹
  for (let i = 0; i < ROAD_PATH.length - 1; i++) {
    const [x1, y1] = ROAD_PATH[i], [x2, y2] = ROAD_PATH[i + 1];
    const n = Math.max(1, Math.round(dist(x1, y1, x2, y2) / 60));
    for (let k = 0; k < n; k++) CONVOY_WAY.push({ x: lerp(x1, x2, k / n), y: lerp(y1, y2, k / n) });
  }
  CONVOY_WAY.push({ x: ROAD_PATH[ROAD_PATH.length - 1][0], y: ROAD_PATH[ROAD_PATH.length - 1][1] });
})();

/* ------------------------------------------------------------- 队友花名册 */
/* 呼号必须音素差异大，避免识别混淆（文档要点） */
const ROSTER = [
  { id: 'ajie', name: '阿杰', role: '步枪手', group: '1组', deputy: true, weapon: 'rifle', aliases: ['阿杰', '阿杰尔', '阿杰哥', '杰哥', '阿洁', '阿杰儿'] },
  { id: 'laozhou', name: '老周', role: '机枪手', group: '1组', weapon: 'mg', aliases: ['老周', '周哥', '周叔'] },
  { id: 'xiaoxia', name: '小夏', role: '狙击手', group: '1组', weapon: 'sniper', aliases: ['小夏', '夏姐', '小侠'] },
  { id: 'daliu', name: '大刘', role: '步枪手', group: '1组', weapon: 'rifle', aliases: ['大刘', '大流', '刘哥'] },
  { id: 'alan', name: '阿兰', role: '步枪手', group: '2组', leader: true, weapon: 'rifle', aliases: ['阿兰', '阿蓝', '兰姐'] },
  { id: 'shitou', name: '石头', role: '反坦克手', group: '2组', weapon: 'at', aliases: ['石头', '石头哥', '石哥'] },
  { id: 'houzi', name: '猴子', role: '反坦克手', group: '2组', weapon: 'at', aliases: ['猴子', '猴哥', '猴儿'] },
  { id: 'laobai', name: '老白', role: '爆破手', group: '2组', weapon: 'demo', aliases: ['老白', '白哥', '白叔'] },
  { id: 'xiaoman', name: '小满', role: '医疗兵', group: '支援组', weapon: 'medic', aliases: ['小满', '满姐', '小曼', '医疗兵', '医生', '军医'] },
  { id: 'tietou', name: '铁头', role: '弹药/支援', group: '支援组', weapon: 'rifle', aliases: ['铁头', '铁头哥', '铁哥'] },
];
const GROUPS = {
  '1组': ['ajie', 'laozhou', 'xiaoxia', 'daliu'],
  '2组': ['alan', 'shitou', 'houzi', 'laobai'],
  '火力组': ['ajie', 'laozhou', 'daliu'],
  '反坦克组': ['shitou', 'houzi'],
  '支援组': ['xiaoman', 'tietou'],
};
const ROLE_CALL = { '机枪手': ['laozhou'], '狙击手': ['xiaoxia'], '反坦克手': ['shitou', 'houzi'], '爆破手': ['laobai'], '医疗兵': ['xiaoman'], '弹药兵': ['tietou'], '步枪手': ['ajie', 'daliu', 'alan'] };

/* --------------------------------------------------------------- 武器表 */
const WEAPONS = {
  rifle: { name: '步枪', dmg: 13, range: 720, rof: 0.12, burst: 3, burstGap: 0.34, spread: 0.045, mag: 30, ammo: 240, reload: 2.2, pen: 0.12, sup: 7, speed: 950 },
  mg: { name: '机枪', dmg: 10, range: 780, rof: 0.09, burst: 8, burstGap: 1.0, spread: 0.075, mag: 100, ammo: 400, reload: 5.0, pen: 0.15, sup: 24, speed: 950 },
  sniper: { name: '狙击枪', dmg: 72, range: 1300, rof: 1.7, burst: 1, burstGap: 0, spread: 0.006, mag: 10, ammo: 60, reload: 3.2, pen: 0.22, sup: 12, speed: 1200 },
  medic: { name: '步枪', dmg: 11, range: 660, rof: 0.14, burst: 3, burstGap: 0.36, spread: 0.052, mag: 30, ammo: 180, reload: 2.3, pen: 0.10, sup: 6, speed: 950 },
  demo: { name: '步枪+炸药', dmg: 13, range: 700, rof: 0.13, burst: 3, burstGap: 0.34, spread: 0.048, mag: 30, ammo: 210, reload: 2.2, pen: 0.12, sup: 7, speed: 950 },
  at: { name: '火箭筒/步枪', dmg: 13, range: 700, rof: 0.13, burst: 3, burstGap: 0.34, spread: 0.05, mag: 30, ammo: 180, reload: 2.4, pen: 0.12, sup: 7, speed: 950 },
  erifle: { name: '敌步枪', dmg: 8, range: 640, rof: 0.16, burst: 3, burstGap: 0.5, spread: 0.075, mag: 30, ammo: 999, reload: 2.6, pen: 0.10, sup: 6, speed: 950 },
  emg: { name: '敌机枪', dmg: 7, range: 720, rof: 0.1, burst: 10, burstGap: 1.2, spread: 0.095, mag: 120, ammo: 999, reload: 5.5, pen: 0.12, sup: 20, speed: 950 },
};
/* 火箭弹：两名反坦克手各 3 发 = 全队 6 发，必须能可靠敲掉一辆 950 血的坦克。
   早期 dmgVeh 300 / splash 62，实测 6 发全部打出去只造成约 760 点伤害 ——
   坦克会顶着 4 点血活到终场，用车顶机枪打掉我方三分之一的伤亡，主目标实际不可达。
   splash 放到 84（近失弹不再几乎不算伤害）、dmgVeh 提到 340。
   弹速 400 → 900：火箭弹本身散布只有 ±0.02 弧度，但 480px 的飞行要 1.2 秒，
   而火箭没有提前量计算 —— 坦克在这 1.2 秒里能挪 30~50px，实测平均落点偏 58px，
   等于把 300 点直击衰减成 137 点溅射。900px/s 才符合 RPG 的实际速度，也把飞行压到 0.7 秒内。 */
const ROCKET = { name: '火箭弹', speed: 900, dmgVeh: 340, dmgInf: 70, splash: 84, rof: 5.5, ammo: 3, reload: 4.0, range: 640 };
const SHELL = { name: '坦克主炮', speed: 640, dmg: 190, splash: 96, rof: 11, warn: 1.6 };

/* --------------------------------------------------------------- 车辆表 */
/* 注意 armor 字段的语义：damageVehicle 里「子弹的穿透系数 = v.armor」，
   所以数值越大＝越软（坦克 0.04 几乎免疫步枪，卡车 0.60 一梭子就打穿）。
   卡车是帆布车厢的军卡，密码箱有 62% 概率锁在它后厢，若步枪打不动它，
   一旦两名反坦克手的 6 发火箭都在坦克上打光，本局主目标就再也无法完成 ——
   因此卡车必须留给小口径火力一条可靠通路。 */
const VEHICLES = {
  jeep: { name: '吉普', hp: 150, len: 46, wid: 24, speed: 78, armor: 0.30, crew: 2, cap: 0, mg: { dmg: 7, range: 480, rof: 0.13, spread: 0.10, sup: 16 } },
  apc: { name: '装甲车', hp: 400, len: 60, wid: 30, speed: 62, armor: 0.16, crew: 3, cap: 6, mg: { dmg: 9, range: 520, rof: 0.11, spread: 0.095, sup: 24 } },
  tank: { name: '坦克', hp: 950, len: 74, wid: 38, speed: 44, armor: 0.04, crew: 3, cap: 0, mg: { dmg: 9, range: 520, rof: 0.12, spread: 0.10, sup: 20 }, cannon: true },
  truck: { name: '卡车', hp: 260, len: 66, wid: 30, speed: 66, armor: 0.60, crew: 2, cap: 6, mg: null },
};

/* --------------------------------------------------------- 地图掩体物件 */
function rock(x, y, r) { return { type: 'rock', x, y, r, cover: 0.95, blocksLos: true, blocksBullet: true, hard: true, destroyed: false }; }
function tree(x, y, r) { return { type: 'tree', x, y, r: r || 15, cover: 0.45, blocksLos: true, blocksBullet: false, hard: false, destroyed: false, soft: true }; }
function bush(x, y, r) { return { type: 'bush', x, y, r: r || 17, cover: 0.35, blocksLos: true, blocksBullet: false, hard: false, destroyed: false, soft: true }; }
function barrel(x, y) { return { type: 'barrel', x, y, r: 8, cover: 0.5, blocksLos: true, blocksBullet: true, hard: true, destroyed: false, explosive: true, hp: 30 }; }
function wall(x, y, r) { return { type: 'wall', x, y, r, cover: 1.0, blocksLos: true, blocksBullet: true, hard: true, destroyed: false }; }
function trench(x, y, w, h) { return { type: 'trench', x, y, r: Math.max(w, h) / 2, w, h, cover: 0.85, blocksLos: false, blocksBullet: false, hard: false, destroyed: false, low: true }; }

const BASE_PROPS = [
  /* A 南侧高地：树林 + 岩石（视野好，会被炮击） */
  rock(806, 876, 22), rock(958, 946, 26), rock(742, 986, 19), rock(902, 1042, 24), rock(1010, 856, 18),
  tree(760, 838, 16), tree(842, 822, 17), tree(918, 852, 15), tree(690, 902, 16), tree(786, 1062, 17),
  tree(882, 1108, 16), tree(1002, 1050, 15), tree(1030, 928, 16), tree(650, 990, 15), tree(980, 1130, 16),
  bush(868, 968, 16), bush(940, 1000, 15),
  /* B 北侧岩石：巨石 + 壕沟 */
  rock(1024, 316, 27), rock(1128, 350, 31), rock(1078, 430, 23), rock(1180, 262, 20), rock(966, 400, 19),
  bush(1050, 370, 15), bush(1104, 302, 14), bush(1140, 420, 15), bush(1000, 258, 14),
  /* E 南侧树林：密林（隐蔽撤退路线） */
  tree(660, 1060, 17), tree(722, 1140, 16), tree(800, 1190, 17), tree(880, 1240, 16), tree(560, 1140, 16),
  tree(620, 1210, 15), tree(760, 1250, 16), tree(900, 1160, 15), bush(700, 1080, 16), bush(820, 1120, 15),
  /* C 桥梁/隘口：桥头石墙与石堆 */
  wall(300, 492, 20), wall(424, 492, 20), wall(300, 808, 20), wall(424, 808, 20),
  rock(268, 596, 16), rock(456, 596, 15), rock(360, 852, 18),
  /* D 油桶（可引爆） */
  barrel(1312, 512), barrel(1338, 526), barrel(1324, 556),
  /* 公路两侧零散掩体（供 AI 找掩体） */
  rock(1250, 780, 17), rock(1494, 800, 19), rock(1700, 812, 18), rock(1900, 786, 17),
  rock(1150, 520, 16), rock(1420, 506, 18), rock(1650, 498, 17), rock(1860, 530, 18), rock(2050, 540, 16),
  tree(1600, 880, 16), tree(1780, 900, 15), tree(1440, 900, 16), tree(1980, 880, 15),
  tree(1200, 430, 15), tree(1560, 388, 16), tree(1760, 400, 15),
  rock(700, 540, 18), rock(560, 758, 17), rock(830, 790, 16), rock(1000, 762, 17),
  bush(1620, 760, 15), bush(1520, 900, 15), bush(1880, 720, 15),
];

/* ------------------------------------------------------ 部署区（4 个） */
const DEPLOY_ZONES = [
  { name: '南侧树林 / 高地', x1: 600, y1: 790, x2: 1210, y2: 1210, desc: '适合火力组、狙击手' },
  { name: '北侧壕沟 / 岩石', x1: 900, y1: 230, x2: 1320, y2: 480, desc: '适合反坦克组' },
  { name: '东侧路边 / 伏击圈侧翼', x1: 1120, y1: 500, x2: 1660, y2: 920, desc: '适合爆破手埋雷 / 反坦克侧射' },
  { name: '后方 C 点', x1: 180, y1: 760, x2: 520, y2: 1090, desc: '医疗兵与撤退点' },
];

/* 推荐部署（文档「推荐部署」） */
const RECOMMEND = {
  player: { x: 872, y: 872 },
  laozhou: { x: 946, y: 930 }, xiaoxia: { x: 796, y: 856 }, ajie: { x: 902, y: 984 }, daliu: { x: 828, y: 950 },
  shitou: { x: 1058, y: 356 }, houzi: { x: 1116, y: 404 },
  alan: { x: 706, y: 1118 }, laobai: { x: 1234, y: 802 },   // 爆破手守在头车雷侧后（公路南肩，离路心约 155px）
  xiaoman: { x: 322, y: 900 }, tietou: { x: 420, y: 856 },
};

/* =========================================================================
   一、语音指令解析（呼号 + 动作 + 目标/地点 + 修饰）
   ========================================================================= */
const ACTIONS = [
  /* 紧急 / 元指令 */
  { id: 'ceasefire', label: '停火', keys: ['停火', '别开火', '停止射击', '不要开火', '先别打', '别打了'], kind: 'combat', pri: 95 },
  { id: 'cancel', label: '取消当前指令', keys: ['取消', '解除命令', '撤销指令', '取消命令'], kind: 'misc', pri: 92 },
  { id: 'repeat', label: '重复上一指令', keys: ['重复', '再说一遍', '重新下令', '重复命令'], kind: 'misc', pri: 91 },
  { id: 'ack', label: '收到', keys: ['收到', '了解', '知道了', '明白'], kind: 'misc', pri: 90 },
  { id: 'detonate', label: '起爆地雷', keys: ['起爆', '引爆', '炸雷', '按雷', '起爆炸药'], kind: 'special', pri: 88 },
  { id: 'blowBridge', label: '炸桥', keys: ['炸桥', '炸掉桥', '把桥炸了', '爆破桥梁'], kind: 'special', pri: 87 },
  /* 移动 */
  { id: 'retreatTo', label: '撤退到指定点', keys: ['撤退到', '撤到', '退到', '向后撤到', '撤退至'], kind: 'move', pri: 86, needLoc: true },
  { id: 'retreat', label: '撤退', keys: ['撤退', '后撤', '提前撤', '撤回去'], kind: 'move', pri: 85 },
  { id: 'evac', label: '撤离', keys: ['撤离', '撤出', '脱离战斗', '快跑', '撤出去', '离开这里'], kind: 'move', pri: 84 },
  { id: 'hold', label: '原地待命', keys: ['等待', '待命', '原地', '别动', '停下', '停住'], kind: 'move', pri: 54 },
  { id: 'spread', label: '散开', keys: ['散开', '拉开距离', '分散', '拉开'], kind: 'move', pri: 53 },
  { id: 'takeCover', label: '隐蔽', keys: ['隐蔽', '躲起来', '找掩体', '趴下', '躲好', '藏起来', '进入隐蔽'], kind: 'move', pri: 52 },
  { id: 'followMe', label: '跟我来', keys: ['跟我来', '跟随我', '跟上我', '跟着我', '跟我走', '跟随'], kind: 'move', pri: 51 },
  { id: 'fallback', label: '后退', keys: ['后退', '退后', '往回走', '退回去', '往后撤'], kind: 'move', pri: 50 },
  { id: 'flank', label: '包围/包抄', keys: ['包围', '包抄', '绕过去', '迂回', '绕到侧面', '走侧翼'], kind: 'move', pri: 49 },
  { id: 'advance', label: '前进', keys: ['前进', '推进', '往前走', '压上去', '向前推进', '上前'], kind: 'move', pri: 48 },
  { id: 'gotoPoint', label: '前往地点', keys: ['前往', '移动到', '转移到', '去到', '走到', '去往', '去'], kind: 'move', pri: 47, needLoc: true },
  { id: 'moveLeft', label: '左移', keys: ['左移', '向左移动', '往左边移', '向左'], kind: 'move', pri: 46 },
  { id: 'moveRight', label: '右移', keys: ['右移', '向右移动', '往右边移', '向右'], kind: 'move', pri: 46 },
  { id: 'enterBuilding', label: '进屋/进掩体', keys: ['进屋', '进房子', '进建筑', '进屋警戒'], kind: 'move', pri: 45 },
  /* 反装甲 / 战斗 */
  { id: 'focusFire', label: '集火', keys: ['集中火力', '集火', '打那个', '打这个', '一起打', '打准星那个', '都打', '打它'], kind: 'combat', pri: 82 },
  { id: 'atTank', label: '打坦克', keys: ['打坦克', '攻击坦克', '反坦克', '炸坦克', '干掉坦克', '打掉坦克'], kind: 'atk', pri: 80, target: 'tank' },
  { id: 'atAPC', label: '打装甲车', keys: ['打装甲车', '攻击装甲车', '打运兵车', '炸装甲车', '打装甲'], kind: 'atk', pri: 79, target: 'apc' },
  { id: 'atJeep', label: '打吉普', keys: ['打吉普车', '打吉普', '打小车', '攻击吉普'], kind: 'atk', pri: 78, target: 'jeep' },
  /* 密码箱有 62% 概率锁在卡车后厢，必须有办法指名打它，否则该局面下箱子拿不到 */
  { id: 'atTruck', label: '打卡车', keys: ['打卡车', '打货车', '打运兵卡车', '攻击卡车', '打那辆卡车', '打补给车', '打军卡', '打运输车', '炸卡车'], kind: 'atk', pri: 77, target: 'truck' },
  { id: 'atMG', label: '打机枪手', keys: ['打机枪手', '干掉机枪手', '打机枪'], kind: 'atk', pri: 76, target: 'mg' },
  { id: 'atOfficer', label: '打军官', keys: ['打指挥官', '打军官', '干掉军官', '打当官的'], kind: 'atk', pri: 76, target: 'officer' },
  { id: 'atInf', label: '打步兵', keys: ['打步兵群', '打步兵', '打敌人', '打人', '清理步兵'], kind: 'atk', pri: 74, target: 'inf' },
  { id: 'coverMe', label: '掩护我', keys: ['掩护我前进', '掩护我', '帮我压制', '掩护', '压制他们'], kind: 'combat', pri: 73 },
  { id: 'suppress', label: '火力压制', keys: ['火力压制', '压制', '压住他们', '压住', '别让他们抬头'], kind: 'combat', pri: 72 },
  { id: 'grenade', label: '投手雷', keys: ['投手雷', '扔手榴弹', '手榴弹', '扔雷', '投雷'], kind: 'combat', pri: 66 },
  { id: 'smoke', label: '放烟雾', keys: ['放烟雾', '烟雾弹', '打烟雾', '烟幕', '放烟'], kind: 'combat', pri: 65 },
  { id: 'rocket', label: '用火箭筒', keys: ['用火箭筒', '火箭筒', '打火箭'], kind: 'combat', pri: 64 },
  { id: 'freeFire', label: '自由射击', keys: ['自由射击', '随意开火', '自由开火'], kind: 'combat', pri: 60 },
  { id: 'fire', label: '开火', keys: ['开火', '射击', '还击', '打'], kind: 'combat', pri: 55 },
  /* 支援 */
  { id: 'rescue', label: '救援伤员', keys: ['救伤员', '救人', '去救', '抢救', '救援', '救'], kind: 'support', pri: 68 },
  { id: 'drag', label: '拖回伤员', keys: ['拖回来', '拖到掩体', '拉回来', '拖走', '拉回'], kind: 'support', pri: 67 },
  { id: 'grabBox', label: '搬运密码箱', keys: ['拿密码箱', '抢密码箱', '搬箱子', '搬运', '拿物资', '拿箱子'], kind: 'support', pri: 66 },
  { id: 'heal', label: '治疗', keys: ['治疗', '包扎', '医疗', '止血'], kind: 'support', pri: 63 },
  { id: 'resupply', label: '给弹药', keys: ['给弹药', '送弹药', '弹药补给', '补弹', '补给'], kind: 'support', pri: 62 },
  { id: 'setup', label: '架设武器', keys: ['架设', '架机枪', '设置火力点', '布置'], kind: 'support', pri: 44 },
  /* 状态报告 */
  { id: 'reportRemain', label: '剩余敌人', keys: ['剩余敌人', '还剩多少敌人', '还有多少敌人', '战果'], kind: 'report', pri: 75 },
  { id: 'reportContact', label: '发现敌人', keys: ['发现敌人', '报告敌情', '有没有敌人', '敌人在哪'], kind: 'report', pri: 74 },
  { id: 'reportCas', label: '报告伤亡', keys: ['报告伤亡', '伤亡情况', '谁受伤了', '伤情'], kind: 'report', pri: 73 },
  { id: 'reportAmmo', label: '报告弹药', keys: ['报告弹药', '还有多少子弹', '弹药情况'], kind: 'report', pri: 72 },
  { id: 'reportPos', label: '报告位置', keys: ['报告位置', '你的位置', '你在哪', '位置'], kind: 'report', pri: 71 },
  { id: 'reportAll', label: '报告状态', keys: ['报告状态', '汇报', '报告'], kind: 'report', pri: 70 },
  { id: 'ready', label: '就位', keys: ['已就位', '就位', '到位'], kind: 'report', pri: 69 },
];

const CS_ALL = ['全体人员', '所有队员', '所有单位', '全体', '所有人', '全员', '全队', '大家', '全部'];
const CS_GROUP = [
  ['反坦克小组', '反坦克组'], ['火力小组', '火力组'], ['支援小组', '支援组'],
  ['第一组', '1组'], ['第二组', '2组'], ['一组', '1组'], ['二组', '2组'], ['1组', '1组'], ['2组', '2组'],
  ['火力组', '火力组'], ['反坦克组', '反坦克组'], ['支援组', '支援组'],
];
const LOC_KEYS = [
  ['南侧高地', 'A'], ['高地', 'A'], ['A点', 'A'], ['a点', 'A'], ['A区', 'A'],
  ['北侧岩石', 'B'], ['岩石', 'B'], ['壕沟', 'B'], ['B点', 'B'], ['b点', 'B'],
  ['桥梁', 'C'], ['桥', 'C'], ['隘口', 'C'], ['C点', 'C'], ['c点', 'C'], ['撤离点', 'C'],
  ['油桶', 'D'], ['D点', 'D'], ['d点', 'D'],
  ['南侧树林', 'E'], ['树林', 'E'], ['密林', 'E'], ['E点', 'E'], ['e点', 'E'],
  ['东侧入口', 'F'], ['东口', 'F'], ['F点', 'F'],
  ['西侧出口', 'G'], ['西口', 'G'], ['出口', 'G'], ['G点', 'G'],
];
const DIR_KEYS = [['北边', 0], ['北面', 0], ['南边', 180], ['南面', 180], ['东边', 90], ['东面', 90], ['西边', 270], ['西面', 270], ['左翼', 'L'], ['右翼', 'R'], ['左边', 'L'], ['左面', 'L'], ['左侧', 'L'], ['右边', 'R'], ['右面', 'R'], ['右侧', 'R'], ['左前方', 'FL'], ['右前方', 'FR']];
/* 目标名词 → 目标类型（用于「打 11 点方向机枪手」这类动词与名词被隔开的情况） */
const TARGET_NOUNS = [
  ['装甲车', 'apc'], ['运兵车', 'apc'], ['装甲', 'apc'], ['坦克', 'tank'],
  ['吉普', 'jeep'], ['小车', 'jeep'], ['卡车', 'truck'], ['货车', 'truck'], ['军卡', 'truck'], ['运输车', 'truck'], ['补给车', 'truck'],
  ['机枪手', 'mg'], ['机枪', 'mg'], ['指挥官', 'officer'], ['军官', 'officer'], ['当官的', 'officer'], ['步兵', 'inf'],
];
const TARGET_ACT = { tank: 'atTank', apc: 'atAPC', jeep: 'atJeep', truck: 'atTruck', mg: 'atMG', officer: 'atOfficer', inf: 'atInf' };

/* 中文数字归一 + 去标点 */
function normalizeOrder(t) {
  let s = String(t || '').toLowerCase();
  s = s.replace(/[\s,，。、；;!！?？.．"“”'‘’`~·\-—_（）()\[\]【】]/g, '');
  s = s.replace(/一/g, '1').replace(/[二两]/g, '2').replace(/三/g, '3');
  return s;
}
const CV_NUM = '0123456789';

/* 字符相似度（最长公共子序列比率） */
function charSim(a, b) {
  if (!a || !b) return 0;
  if (a === b) return 1;
  const n = a.length, m = b.length;
  const dp = new Array(m + 1).fill(0);
  for (let i = 1; i <= n; i++) {
    let prev = 0;
    for (let j = 1; j <= m; j++) {
      const tmp = dp[j];
      dp[j] = Math.max(dp[j], dp[j - 1], prev + (a[i - 1] === b[j - 1] ? 1 : 0));
      prev = tmp;
    }
  }
  return dp[m] * 2 / (n + m);
}

const Parser = {
  normalize: normalizeOrder,

  /* 呼号识别：返回最早出现的匹配 */
  findCallsign(s) {
    const hits = [];
    const push = (pos, len, kind, id, label, score) => { if (pos >= 0) hits.push({ pos, len, kind, id, label, score }); };
    for (const [k, v] of CS_GROUP) { const p = s.indexOf(k); if (p >= 0) push(p, k.length, 'group', v, v + '', 1); }
    for (const k of CS_ALL) { const p = s.indexOf(k); if (p >= 0) push(p, k.length, 'all', '*', '全体', 1); }
    for (const role in ROLE_CALL) { const p = s.indexOf(role); if (p >= 0) push(p, role.length, 'role', role, role, 1); }
    const used = [];
    for (const m of ROSTER) {
      let best = null;
      for (const a of m.aliases) { const p = s.indexOf(a); if (p >= 0 && (!best || a.length > best.len)) best = { pos: p, len: a.length, key: a }; }
      if (best) { push(best.pos, best.len, 'member', m.id, m.name, 1); used.push(best.pos); }
    }
    if (!hits.length) { // 模糊匹配（阿杰尔/阿杰哥 → 阿杰）
      for (const m of ROSTER) {
        for (const a of m.aliases) {
          if (a.length < 2) continue;
          let bp = -1, bs = 0;
          for (let i = 0; i + a.length <= s.length; i++) {
            const seg = s.slice(i, i + a.length);
            const sc = charSim(a, seg);
            if (sc > bs) { bs = sc; bp = i; }
          }
          if (bs >= 0.72) { push(bp, a.length, 'member', m.id, m.name, bs); break; }
        }
      }
    }
    if (!hits.length) return null;
    hits.sort((a, b) => (a.pos - b.pos) || (b.len - a.len) || (b.score - a.score));
    return hits[0];
  },

  /* 动作识别：全局最长关键词命中 */
  findAction(s) {
    let best = null;
    for (const a of ACTIONS) {
      for (const k of a.keys) {
        const p = s.indexOf(k);
        if (p < 0) continue;
        const score = k.length * 100 + a.pri;
        if (!best || score > best.score) best = { act: a, pos: p, key: k, len: k.length, score };
      }
    }
    return best;
  },

  findLoc(s) {
    let best = null;
    for (const [k, key] of LOC_KEYS) {
      const p = s.indexOf(k);
      if (p >= 0 && (!best || k.length > best.len)) best = { key, pos: p, len: k.length, name: POINTS[key].name };
    }
    return best;
  },

  findDir(s) {
    const m = s.match(/(\d{1,2})点(方向|钟方向|钟)?/);
    if (m) { const h = parseInt(m[1], 10); if (h >= 1 && h <= 12) return { bearing: (h % 12) * 30, raw: m[0], hour: h }; }
    for (const [k, v] of DIR_KEYS) { const p = s.indexOf(k); if (p >= 0) return { bearing: v, raw: k }; }
    return null;
  },

  /* 目标名词识别（最长匹配） */
  findTargetNoun(s) {
    let best = null;
    for (const [k, v] of TARGET_NOUNS) {
      if (s.indexOf(k) >= 0 && (!best || k.length > best.len)) best = { type: v, len: k.length };
    }
    return best ? best.type : null;
  },

  /* 找出文中提到的其他队员（用于「小满，救阿杰」的救援目标） */
  findMentionedIds(s, excludeId) {
    const out = [];
    for (const m of ROSTER) {
      if (m.id === excludeId) continue;
      for (const a of m.aliases) { if (s.includes(a)) { out.push(m.id); break; } }
    }
    return out;
  },

  findRocket(s) { return null; },

  parse(text, opts) {
    opts = opts || {};
    const raw = String(text || '').trim();
    const s = normalizeOrder(raw);
    const notes = [];
    if (!s) return { ok: false, raw, reason: 'empty', confidence: 0 };

    let cs = this.findCallsign(s);
    let csSpan = cs ? [cs.pos, cs.pos + cs.len] : null;
    // 动作在排除呼号后的文本里找
    const sNoCs = cs ? (s.slice(0, csSpan[0]) + '　'.repeat(cs.len) + s.slice(csSpan[1])) : s;
    let act = this.findAction(s.includes('　') ? s : s);
    let actNoCs = this.findAction(sNoCs);
    let actionHit = actNoCs || this.findAction(s);
    let scan = sNoCs;

    const loc = this.findLoc(scan);
    const dir = this.findDir(scan);

    if (!actionHit) {
      return { ok: false, raw, callsign: cs, loc, dir, confidence: 0.1, reason: 'no-action', notes: ['未识别到动作关键词'] };
    }
    let A = actionHit.act;
    /* 目标名词补正：「打 11 点方向机枪手」→ 升级为「打机枪手」 */
    if (A.kind === 'combat' || A.kind === 'atk') {
      const tn = this.findTargetNoun(scan);
      if (tn && tn !== A.target) {
        const up = ACTIONS.find(a => a.id === TARGET_ACT[tn]);
        if (up) { A = up; actionHit = { act: up, key: '名词补正', len: 3, score: actionHit.score }; }
      }
    }

    /* 置信度评估 */
    let conf = 0.30;
    conf += actionHit.key.length >= 3 ? 0.38 : actionHit.key.length === 2 ? 0.34 : 0.14;
    if (cs) conf += cs.kind === 'all' ? 0.06 : 0.13; else { conf += 0.02; notes.push('未指定呼号 → 默认最近队友'); }
    if (A.needLoc) { if (loc) conf += 0.14; else { conf -= 0.30; notes.push('缺少地点（如「撤退到 C 点」）'); } }
    else if (loc) conf += 0.10;
    if (s.length >= 4) conf += 0.08;
    if (s.length >= 7) conf += 0.04;
    if (act && act.act.id !== A.id) { conf -= 0.16; notes.push('存在歧义：也可能理解为「' + act.act.label + '」'); }
    const noise = clamp(opts.noise || 0, 0, 1);
    if (noise > 0.02) { conf -= noise * 0.35; notes.push('战场噪声降低识别率 -' + Math.round(noise * 35) + '%'); }
    if (opts.asr != null && opts.asr > 0) conf = lerp(conf, opts.asr, 0.55);
    if (opts.typed) conf = Math.max(conf, 0.93);
    conf = clamp(conf, 0, 1);

    const mentioned = this.findMentionedIds(scan, cs && cs.kind === 'member' ? cs.id : null);
    const cmd = {
      ok: conf >= 0.70,
      raw, confidence: +conf.toFixed(2), notes,
      callsign: cs ? { kind: cs.kind, id: cs.id, label: cs.label } : { kind: 'member', id: 'auto', label: '默认（最近队友）' },
      action: { id: A.id, label: A.label, kind: A.kind, pri: A.pri, needLoc: !!A.needLoc, target: A.target || null },
      loc: loc ? { key: loc.key, name: loc.name, x: POINTS[loc.key].x, y: POINTS[loc.key].y } : null,
      dir, mentioned,
      source: opts.source || 'voice',
      marker: opts.marker || null,
      noise,
    };
    if (!cmd.ok) notes.push('置信度 ' + Math.round(conf * 100) + '% < 70%，不予执行');
    return cmd;
  },

  /* 低置信度候选（HUD 弹候选，按 1/2/3 确认） */
  candidates(s, cmd) {
    const out = [];
    const sig = c => (c.callsign ? c.callsign.kind + ':' + c.callsign.id : '-') + '|' + (c.action ? c.action.id : '-') + '|' + (c.loc ? c.loc.key : '-');
    const add = c => { if (c && c.action && !out.some(o => sig(o) === sig(c))) out.push(c); };

    if (cmd && cmd.action) add(cmd);
    // 变体 1：同一动作 → 全体
    if (cmd && cmd.action && (!cmd.callsign || cmd.callsign.kind !== 'all')) {
      add(Object.assign({}, cmd, { callsign: { kind: 'all', id: '*', label: '全体' }, confidence: 0.72 }));
    }
    // 变体 2：相似动作
    const norm = normalizeOrder(s);
    let alt = null, altScore = 0;
    for (const a of ACTIONS) {
      if (cmd && cmd.action && a.id === cmd.action.id) continue;
      for (const k of a.keys) {
        const sc = charSim(k, norm.slice(0, Math.max(k.length, 3))) * 0.6 + (norm.includes(k) ? 0.5 : 0);
        if (sc > altScore) { altScore = sc; alt = a; }
      }
    }
    if (alt && altScore > 0.3) {
      add(Object.assign({}, cmd || {}, {
        callsign: cmd && cmd.callsign ? cmd.callsign : { kind: 'all', id: '*', label: '全体' },
        action: { id: alt.id, label: alt.label, kind: alt.kind, pri: alt.pri, needLoc: !!alt.needLoc, target: alt.target || null },
        confidence: 0.70,
      }));
    }
    // 变体 3：同动作 → 某个具体队员（按相似度）
    if (out.length < 3) {
      let bm = null, bs = 0;
      for (const m of ROSTER) { for (const a of m.aliases) { const sc = charSim(a, norm); if (sc > bs) { bs = sc; bm = m; } } }
      if (bm && (!cmd || !cmd.callsign || cmd.callsign.id !== bm.id)) {
        add(Object.assign({}, cmd || {}, {
          callsign: { kind: 'member', id: bm.id, label: bm.name },
          action: cmd && cmd.action ? cmd.action : { id: 'advance', label: '前进', kind: 'move', pri: 48 },
          confidence: 0.70,
        }));
      }
    }
    return out.slice(0, 3);
  },
};

/* ------------------------------------------------------------------ 回复 */
const REPLIES = {
  ok: ['收到', '明白', '执行', '收到，执行'],
  delay: ['被压制，等一下', '等一下，我在换弹', '稍等，压得抬不起头'],
  refuse: ['不行，太危险', '没弹药了', '不行，我这儿被压住了'],
  panic: ['我不行了！', '顶不住了，跑吧！', '别打了，太多了！'],
  down: ['我中弹了！', '需要医疗！', '我倒了……'],
};

/* =========================================================================
   二、世界状态 / 单位 / 指令下发器
   ========================================================================= */
const World = {
  t: 0, phase: 'INTEL', phaseName: '情报', speed: 1, paused: false, started: false, over: false,
  props: [], units: [], vehicles: [], projectiles: [], fx: [], mines: [], decals: [],
  player: null, marker: null, evac: EVAC_DEFAULT, bridgeAlive: true, box: null,
  triggered: false, triggerT: 0, reinforceDone: false, reinforceT: CFG.reinforceAt,
  weather: 'sunny', noise: 0, noiseSrc: [], deployDone: false,
  convoyStarted: false, convoyEscaped: false, officerSpawned: false,
  subs: [], lastCmd: null, pending: [], logs: [],
  stats: {
    enemyDead: 0, apcKilled: 0, tankKilled: false, boxTaken: false, boxEvacuated: false, evacCount: 0,
    cmdIssued: 0, cmdExec: 0, cmdRefused: 0, reinforceTriggered: false, officerKilled: false,
    minesUsed: 0, bridgeBlown: false, civilians: 0, shotFired: false, firstShotByPlayer: false,
  },
  cam: { x: 0, y: 0 },
  timeScale: 1,
  viewPitch: 0,     // 第一人称准星俯仰角，由渲染层每帧写入（逻辑层只读，用于折算射程）
};

function allies() { return World.units.filter(u => u.team === 'ally' && !u.dead); }
function aliveAllies() { return World.units.filter(u => u.team === 'ally' && !u.dead && !u.downed); }
function combatAllies() { return World.units.filter(u => u.team === 'ally' && !u.dead && !u.downed && !u.isPlayer); }
function enemies() { return World.units.filter(u => u.team === 'enemy' && !u.dead); }
function enemyVehicles() { return World.vehicles.filter(v => v.team === 'enemy' && !v.destroyed); }
function downedAllies() { return World.units.filter(u => u.team === 'ally' && u.downed && !u.dead); }

function say(who, text, cls) {
  World.subs.push({ who, text, cls: cls || '', t: 6.5 });
  while (World.subs.length > 7) World.subs.shift();
  renderSubs();
  /* 无线电音：队友/系统播报是「收」（下行音），玩家的下令是「发」（上行音）——
     一耳朵就能分清「谁在说话」，也是这个「靠无线电指挥」的玩法的听觉骨架 */
  if (World.started) sfx(who === '你' ? 'radioTx' : 'radioRx');
}
function log(txt, cls) { World.logs.push({ t: World.t, txt, cls: cls || '' }); }
function fx(o) { o.t = 0; o.life = o.life || 0.5; World.fx.push(o); return o; }
function decal(x, y, r, c) { World.decals.push({ x, y, r, c, t: 0 }); if (World.decals.length > 260) World.decals.shift(); }

/* ------------------------------------------------------------ 单位工厂 */
function makeUnit(def, x, y, team, isPlayer) {
  const w = WEAPONS[def.weapon] || WEAPONS.rifle;
  return {
    id: def.id, name: def.name, role: def.role || '步枪手', group: def.group || '', isPlayer: !!isPlayer, team,
    x, y, vx: 0, vy: 0, facing: 0, dest: null, radius: 5.2,
    hp: 100, maxHp: 100, morale: 78, suppression: 0, armor: 0,
    weaponKey: def.weapon, wpn: w, ammo: w.ammo, magAmmo: w.mag, reloadT: 0, fireCd: 0,
    burstLeft: 0, burstCd: 0, aimT: 0, hitFlash: 0,
    rockets: def.weapon === 'at' ? ROCKET.ammo : 0, grenades: (def.weapon === 'mg' ? 1 : 2), smokes: def.weapon === 'rifle' ? 1 : 0,
    hasCharge: def.weapon === 'demo' || def.weapon === 'at',
    state: team === 'ally' ? '部署' : '警戒', stateT: 0,
    target: null, preferType: null, focusTarget: null, fireMode: 'free', holdPosition: false,
    moveGoal: null, coverPos: null, coverT: 0, order: null, orderT: 0, pendingOrder: null, obeyBoost: 0, obeyBase: 0.8,
    formationFollow: false, followTarget: null,
    downed: false, dead: false, downTimer: 0, reviveT: 0, dragging: null, draggedBy: null,
    kills: 0, shots: 0, hits: 0, speakCd: 0, reportCd: 0, aiT: 0, pickT: 0, thinkT: 0,
    flankRole: null, flankPt: null, aiState: 'idle', mount: null, officer: !!def.officer, mg: !!def.mg,
    pushT: 0, holdT: 0, pushPt: null,
    suppressFireT: 0, rescueTarget: null, carryBox: false, secReport: 0, lastHurtBy: null,
    stun: 0, wobble: RNG() * 6.28, patrol: null,
  };
}

function initWorld(seed) {
  RNG = mulberry32(seed >>> 0);
  World.props = BASE_PROPS.map(p => Object.assign({}, p));
  World.units = []; World.vehicles = []; World.projectiles = []; World.fx = []; World.decals = [];
  World.mines = []; World.subs = []; World.logs = [];
  World.t = 0; World.phase = 'INTEL'; World.over = false; World.triggered = false; World.triggerT = 0;
  World.reinforceDone = false; World.reinforceT = CFG.reinforceAt; World.convoyStarted = false;
  World.convoyEscaped = false; World.bridgeAlive = true; World.evac = Object.assign({}, EVAC_DEFAULT);
  World.evacArmed = false;      // 尚未进入撤离阶段（拿到密码箱 / 收到撤退撤离命令后才置位）
  World.marker = null; World.deployDone = false; World.noise = 0; World.noiseSrc = [];
  /* 运行态清零：这些字段都是在战局中途被写入的，initWorld 早期版本漏掉了它们，
     结果「重玩」会把上一局的残留带进新一局 —— 最典型的是 World.barrage：
     只要上一局打到过增援阶段，新局从 t=0 起就会被火箭炮持续覆盖，
     开局还没进入潜伏就先减员（表现为「双方都没开枪却有人在掉血」）。 */
  World.barrage = false; World.barrageCd = 0; World.pendingShells = [];
  World.box = null; World.officerVeh = null; World.officerSpawned = false;
  World.handover = null; World.convoyProgress = 0; World.mineWarned = false;
  World.noiseT = 0; World.alertT = 0; World.rangedSel = null;
  World.overKind = null; World.overText = null; World.objState = [];
  World.phaseName = '情报'; World.seed = seed >>> 0;
  World.lastCmd = null; World.stats = {
    enemyDead: 0, apcKilled: 0, tankKilled: false, boxTaken: false, boxEvacuated: false, evacCount: 0,
    cmdIssued: 0, cmdExec: 0, cmdRefused: 0, reinforceTriggered: false, officerKilled: false,
    minesUsed: 0, bridgeBlown: false, shotFired: false, firstShotByPlayer: false,
  };

  /* 天气变体（重玩性） */
  const wr = RNG();
  World.weather = wr < 0.45 ? 'sunny' : wr < 0.8 ? 'rain' : 'night';

  /* 玩家 + 10 名队友 */
  const pd = RECOMMEND.player;
  World.player = makeUnit({ id: 'player', name: '你（队长）', role: '队长', group: '1组', weapon: 'rifle' }, pd.x, pd.y, 'ally', true);
  World.player.facing = -Math.PI / 2;
  World.units.push(World.player);
  for (const r of ROSTER) {
    const p = RECOMMEND[r.id] || { x: 880, y: 900 };
    const u = makeUnit(r, p.x + rr(-8, 8), p.y + rr(-8, 8), 'ally');
    u.homePos = { x: p.x, y: p.y };
    World.units.push(u);
  }

  /* 部署区内的地雷（爆破手在战前埋设，触发后近炸引信生效）：头车雷在伏击圈内，尾车雷守在通往桥的方向 */
  const mineSpots = [
    { x: 1120, y: 662 }, { x: 1040, y: 672 },   // 东侧头车雷（伏击圈内）
    { x: 620, y: 655 }, { x: 540, y: 658 },     // 西侧尾车雷（堵住西逃方向）
  ];
  const east = pick([0, 1]), west = pick([2, 3]);
  World.mines.push({ x: mineSpots[east].x, y: mineSpots[east].y, armed: false, team: 'ally', used: false, kind: 'east' });
  World.mines.push({ x: mineSpots[west].x, y: mineSpots[west].y, armed: false, team: 'ally', used: false, kind: 'west' });
  World.mineEast = World.mines[0]; World.mineWest = World.mines[1];

  /* 密码箱位置随机（卡车 / 装甲车 / 军官身上） */
  World.boxWhere = RNG() < 0.62 ? 'truck' : RNG() < 0.6 ? 'apc' : 'officer';
  /* 车队顺序随机（坦克可能在前、中、后） */
  const orderRoll = RNG();
  World.convoyOrder = orderRoll < 0.62 ? ['jeep', 'apc', 'tank', 'truck', 'apc', 'jeep']
    : orderRoll < 0.82 ? ['jeep', 'tank', 'apc', 'truck', 'apc', 'jeep']
      : ['apc', 'jeep', 'apc', 'tank', 'truck', 'jeep'];
  World.infantryTotal = ri(10, 16);
  World.reinforceT = CFG.reinforceAt + Math.round(rr(-30, 30));

  buildConvoy();
  updateEvacMarker();
}

/* ------------------------------------------------------------ 车队生成 */
function buildConvoy() {
  World.vehicles.length = 0;
  const names = { jeep: '吉普', apc: '装甲车', tank: '坦克', truck: '卡车' };
  let idx = { jeep: 0, apc: 0, tank: 0, truck: 0 };
  const total = World.convoyOrder.length;
  World.convoyOrder.forEach((type, i) => {
    idx[type]++;
    const line = i === 0 ? -1 : i === total - 1 ? 1 : 0;   // -1 先头, 1 后卫
    const v = {
      id: type + idx[type], type, name: names[type] + idx[type], team: 'enemy',
      x: 2320 + i * 132, y: 700, angle: Math.PI, speed: 0, baseSpeed: VEHICLES[type].speed * 0.30,
      dist: -i * 132,
      hp: VEHICLES[type].hp, maxHp: VEHICLES[type].hp, spec: VEHICLES[type],
      len: VEHICLES[type].len, wid: VEHICLES[type].wid, armor: VEHICLES[type].armor,
      destroyed: false, burning: 0, turret: Math.PI, fireCd: rr(2, 6), wpnCd: rr(0.5, 2),
      troops: [], capacity: VEHICLES[type].cap, dismountT: -1, dismounted: false,
      state: 'drive', target: null, line, speedMul: 1, wayI: 0, hasBox: false, hitFlash: 0,
      stopT: 0, mgHeat: 0, report: 0,
    };
    if (World.boxWhere === 'truck' && type === 'truck') v.hasBox = true;
    if (World.boxWhere === 'apc' && type === 'apc' && !World.vehicles.some(x => x.hasBox)) v.hasBox = true;
    if (type === 'apc' || type === 'truck' || type === 'jeep') {
      const n = type === 'apc' ? 6 : type === 'truck' ? 6 : 2;
      v.troopPlan = Math.min(n, Math.max(0, World.infantryTotal - World.vehicles.reduce((s, x) => s + (x.troopPlan || 0), 0)));
      if (v.troopPlan < 0) v.troopPlan = 0;
    } else v.troopPlan = 0;
    World.vehicles.push(v);
  });
  // 卡车上放密码箱（优先）
  if (World.boxWhere !== 'officer') {
    const wantTruck = World.boxWhere === 'truck';
    let found = World.vehicles.find(v => wantTruck ? v.type === 'truck' : v.type === 'apc');
    for (const v of World.vehicles) v.hasBox = false;
    if (found) found.hasBox = true;
  }
  // 军官（击毙车队指挥官 = 隐藏目标）
  const cands = World.vehicles.filter(v => v.troopPlan > 0);
  if (cands.length) World.officerVeh = pick(cands); else World.officerVeh = World.vehicles[0];
}

/* 车辆在路径上的位置（沿 CONVOY_WAY 前进，前方车先走） */
function convoyPos(v, progress) {
  // progress: 车队头部沿路径已行进的距离
  let d = progress - (World.convoyOrder.length - 1 - World.vehicles.indexOf(v)) * 132;
  if (d < 0) d = 0;
  return wayPointAt(d);
}
function wayPointAt(d) {
  let acc = 0;
  for (let i = 0; i < CONVOY_WAY.length - 1; i++) {
    const a = CONVOY_WAY[i], b = CONVOY_WAY[i + 1];
    const seg = dist(a.x, a.y, b.x, b.y);
    if (acc + seg >= d) {
      const t = seg > 0 ? (d - acc) / seg : 0;
      return { x: lerp(a.x, b.x, t), y: lerp(a.y, b.y, t), a: Math.atan2(b.y - a.y, b.x - a.x) };
    }
    acc += seg;
  }
  const last = CONVOY_WAY[CONVOY_WAY.length - 1];
  return { x: last.x, y: last.y, a: Math.PI };
}

function updateEvacMarker() {
  const p = World.bridgeAlive ? EVAC_DEFAULT : EVAC_ALT;
  World.evac = { key: p.key, x: p.x, y: p.y, name: p.name };
}

/* --------------------------------------------------------------- 地形 */
function inRiver(x, y) { return x > CFG.riverX1 && x < CFG.riverX2; }
function onBridge(x, y) { return x > CFG.riverX1 - 10 && x < CFG.riverX2 + 10 && y > CFG.bridgeY1 && y < CFG.bridgeY2; }
function passable(x, y, isVehicle) {
  if (x < 6 || y < 6 || x > CFG.W - 6 || y > CFG.H - 6) return false;
  if (isVehicle) { if (inRiver(x, y) && !onBridge(x, y)) return false; if (!World.bridgeAlive && inRiver(x, y)) return false; }
  return true;
}
function vehicleBlocked(x, y, r, v) {
  const dx = x - v.x, dy = y - v.y;
  const c = Math.cos(-v.angle), s = Math.sin(-v.angle);
  const lx = dx * c - dy * s, ly = dx * s + dy * c;
  return Math.abs(lx) < v.len / 2 + r * 0.5 && Math.abs(ly) < v.wid / 2 + r * 0.5;
}
function propBlocked(x, y, r) {
  for (const p of World.props) {
    if (p.destroyed || !p.hard) continue;
    if (dist2(x, y, p.x, p.y) < (p.r + r) * (p.r + r)) return true;
  }
  for (const v of World.vehicles) {
    if (v.destroyed) continue;
    if (vehicleBlocked(x, y, r, v)) return true;
  }
  return false;
}

/* 掩体评分（文档：防护+隐蔽+射界+撤退路线-距离-暴露时间） */
function coverScore(x, y, tx, ty) {
  let s = 0;
  for (const p of World.props) {
    if (p.destroyed) continue;
    const d = dist(x, y, p.x, p.y);
    const R = p.r + 66;
    if (d > R) continue;
    let w = 1 - d / R;
    if (p.type === 'trench') { w *= 1.5; }
    else if (p.hard) {
      const vx = p.x - x, vy = p.y - y, L = Math.hypot(vx, vy) || 1;
      const txn = tx - x, tyn = ty - y, L2 = Math.hypot(txn, tyn) || 1;
      const dot = (vx / L) * (txn / L2) + (vy / L) * (tyn / L2);
      w *= dot > 0.15 ? 1.5 : 0.3;
    } else w *= 0.55;
    s += w * p.cover;
  }
  // 撤退路线：后方 60px 内没有障碍加分
  if (!propBlocked(x + (x - tx) * 0.25, y + (y - ty) * 0.25, 6)) s += 0.25;
  /* 射界（文档评分项之一，早期漏了）：候选位置朝威胁方向必须「看得见」。
     没有这一项时，AI 会挑树林深处的好掩体 —— 防护满分、射界为零，
     于是「全体，打卡车」之后大半队员 canSee() 为假、target 为空，
     站着不动一枪不放（实测卡车 142 血卡了整整 3 分钟没人打）。 */
  if (losSight(x, y, tx, ty)) s -= 1.2;
  const th = nearestEnemyAt(x, y, 'ally');
  if (th) s -= clamp((260 - dist(x, y, th.x, th.y)) / 400, 0, 0.5);
  return s;
}

function findCover(fromX, fromY, tx, ty, maxDist) {
  maxDist = maxDist || 80;   // 10 米
  let best = null, bs = -1e9;
  for (let ri2 = 1; ri2 <= 3; ri2++) {
    const rad = maxDist * ri2 / 3;
    for (let i = 0; i < 16; i++) {
      const a = i * Math.PI / 8 + (RNG() - 0.5) * 0.3;
      const x = fromX + Math.cos(a) * rad, y = fromY + Math.sin(a) * rad;
      if (!passable(x, y) || propBlocked(x, y, 6)) continue;
      const sc = coverScore(x, y, tx, ty) - rad / maxDist * 0.35;
      if (sc > bs) { bs = sc; best = { x, y }; }
    }
  }
  if (!best) return null;
  return best;
}

/* ----------------------------------------------------------- 目标检索 */
function nearestEnemyAt(x, y, team, filter) {
  let best = null, bd = 1e9;
  for (const u of World.units) {
    if (u.dead || u.downed || u.team === team) continue;
    if (filter && !filter(u)) continue;
    const d = dist2(x, y, u.x, u.y);
    if (d < bd) { bd = d; best = u; }
  }
  for (const v of World.vehicles) {
    if (v.destroyed || v.team === team) continue;
    if (filter) { if (!filter(v)) continue; } else continue;
    const d = dist2(x, y, v.x, v.y);
    if (d < bd) { bd = d; best = v; }
  }
  return best;
}
function canSee(u, tgt) {
  const r = visRange(u);
  const d = dist(u.x, u.y, tgt.x, tgt.y);
  if (d > r) return false;
  if (d > 150) {
    if (losSight(u.x, u.y, tgt.x, tgt.y)) return false;   // 远距离：树冠也能遮断视线
  } else if (losFire(u.x, u.y, tgt.x, tgt.y)) return false;
  return true;
}
/* 视野距离。早期我方固定 680，比步枪射程 720 还短 —— 「看得见」比「打得到」还近，
   于是队员经常对着看不见的敌人干站着；狙击手拿着 1300 射程的枪却只有 680 的视野。
   这里按武器给视野：主力步枪手 780（够覆盖步枪射程），机枪手 840，狙击手 1160。 */
function visRange(u) {
  let r = u.team === 'ally' ? 780 : 620;
  if (u.weaponKey === 'sniper') r = 1160;
  else if (u.weaponKey === 'mg') r = 840;
  if (World.weather === 'rain') r *= 0.82;
  if (World.weather === 'night') r *= 0.62;
  if (u.dead || u.downed) r = 0;
  return r;
}
function targetPoint(u) { return { x: u.x, y: u.y }; }

/* --------------------------------------------------------------- 伤害 */
/* 掩体防护系数 0~1：贴住掩体只露半个身子。
   数值直接取物件自己的 cover 权重（岩石 .95 / 石墙 1.0 / 壕沟 .85 / 油桶 .5 / 树 .45 / 灌木 .35），
   与 coverScore 评分用的是同一套权重 —— 早期这里只认 hard 物件，
   于是「树林/灌木」在评分里是合格掩体、到了减伤阶段却等于零，
   AI 会照着评分把阿兰派进 E 点树林，然后站在那里毫无防护地挨完整场。 */
function coverProtect(u) {
  if (u.dead || u.downed) return 0;
  let best = 0;
  for (const p of World.props) {
    if (p.destroyed) continue;
    const reach = p.hard ? 38 : 26;            // 软掩体（树/灌木）要贴得更近才有用
    const d = dist(u.x, u.y, p.x, p.y);
    if (d >= p.r + reach) continue;
    const near = 1 - Math.max(0, d - p.r) / reach;
    const w = p.cover === undefined ? (p.hard ? 0.9 : 0.4) : p.cover;
    best = Math.max(best, near * w);
  }
  if (u.state === '隐蔽' || u.state === '待命') best = Math.max(best, 0.4);
  return clamp(best, 0, 1);
}
function damageUnit(u, dmg, src, kind) {
  if (u.dead || u.downed) return;
  /* 依托掩体最多减伤 70%：伏击方在工事里、进攻方在开阔地推进，
     这个差距是文档「掩体评分」体系成立的前提（否则 AI 抢掩体毫无意义）。 */
  dmg *= 1 - coverProtect(u) * 0.70;
  u.hp -= dmg;
  u.hitFlash = 0.25;
  /* 受伤音：玩家自己是一声贴耳的闷响（local），队友/敌人按方位放 ——
     「子弹打进土里」和「打在人身上」音色不同，混战中能听出来自己有没有打中 */
  if (u.isPlayer) sfx('hurt', null, null, { local: true });
  else sfx('flesh', u.x, u.y, kind === 'splash' ? { gain: 0.62 } : null);
  u.lastHurtBy = src || null;
  if (src && src.team && src.team !== u.team) u.lastHurtT = World.t;
  if (u.team === 'ally') {
    u.morale = clamp(u.morale - dmg * 0.12, 0, 100);
    u.suppression = clamp(u.suppression + dmg * 0.9, 0, 100);
  }
  decal(u.x + rr(-3, 3), u.y + rr(-3, 3), rr(2, 3.5), 'rgba(140,20,20,.5)');
  if (u.hp <= 0) {
    if (u.team === 'ally') {
      if (u.isPlayer) { downPlayer(u); }
      else if (RNG() < 0.72) { u.downed = true; u.downTimer = 45; u.state = '失能'; u.hp = 0; say(u.name, pick(REPLIES.down), 'no'); }
      else killUnit(u, src);
    } else {
      killUnit(u, src);
    }
  }
}
function killUnit(u, src) {
  if (u.dead) return;
  u.dead = true; u.state = '阵亡';
  sfx(u.team === 'ally' ? 'allyDown' : 'body', u.x, u.y);
  if (u.carryBox) { u.carryBox = false; if (World.box) { World.box.taken = false; World.box.carrier = null; World.box.x = u.x; World.box.y = u.y; say('全体', '密码箱掉了！谁去捡！', 'no'); } }
  if (u.team === 'enemy') {
    World.stats.enemyDead++;
    if (src && src.team === 'ally') { src.kills++; if (!src.isPlayer) src.morale = clamp(src.morale + 4, 0, 100); }
    if (u.officer) {
      World.stats.officerKilled = true; say('观察员', '敌军指挥官被击毙！', 'sys');
      /* 文档：密码箱有 38% 概率由指挥官随身携带 —— 击毙他才会掉箱。
         早期这里只记了一条战果，箱子在本局压根不存在，
         于是「密码箱在军官身上」的那 38% 局面里主目标永远无法完成（实测该局面 0 胜）。 */
      if (World.boxWhere === 'officer' && !World.box) dropBox(u.x, u.y);
    }
    decal(u.x, u.y, 6, 'rgba(120,16,16,.45)');
  } else {
    World.stats.allyDead = (World.stats.allyDead || 0) + 1;
    for (const a of allies()) if (a !== u) a.morale = clamp(a.morale - 6, 0, 100);
    say('全体', u.name + '阵亡！', 'no');
    onAllyLost();
  }
}
function downPlayer(u) {
  u.downed = true; u.hp = 0; u.state = '失能'; u.downTimer = 40;
  sfx('down', null, null, { local: true });       // 重重倒下 + 耳鸣
  say('你', '我中弹了——需要医疗！', 'no');
  toast('你被击倒！副队长接管指挥');
  handover();
}
function handover() {
  if (World.handover) return;
  const dep = World.units.find(x => x.team === 'ally' && ROSTER.find(r => r.id === x.id && r.deputy) && !x.dead && !x.downed)
    || World.units.find(x => x.team === 'ally' && ROSTER.find(r => r.id === x.id && r.leader) && !x.dead && !x.downed);
  if (dep) { World.handover = dep; say('全体', dep.name + '：我接管指挥，全体听我口令！', 'sys'); }
}
function onAllyLost() {
  const al = allies().filter(u => !u.downed);
  if (al.length === 0) endGame('全灭', '小队全灭，伏击失败。');
}

function damageVehicle(v, dmg, kind, byUnit) {
  if (v.destroyed) return;
  const pen = kind === 'bullet' ? v.armor : kind === 'rocket' ? 0.95 : kind === 'shell' ? 1.1 : kind === 'mine' ? 1.3 : 1;
  v.hp -= dmg * pen;
  v.hitFlash = 0.25;
  /* 子弹啃装甲＝金属跳弹声（火箭/炮弹的爆炸音在 explosion 里出） */
  if (kind === 'bullet') sfx('clang', v.x, v.y, { gain: 0.85 });
  if (kind === 'rocket' || kind === 'mine') say('全体', '好！命中' + v.name + '！', 'ok');
  if (v.hp <= 0) destroyVehicle(v, byUnit);
}
function destroyVehicle(v, byUnit) {
  if (v.destroyed) return;
  v.destroyed = true; v.burning = 30; v.hp = 0;
  explosion(v.x, v.y, v.len * 0.9, v.type === 'tank' ? 160 : 90, 'enemy', 'vehicle');
  sfx('metal', v.x, v.y, { gain: 0.8 });          // 车体撕裂的金属余音
  decal(v.x, v.y, v.len * 0.6, 'rgba(30,26,22,.55)');
  if (byUnit && byUnit.team === 'ally') { byUnit.kills++; }
  if (v.type === 'tank') {
    World.stats.tankKilled = true;
    say('全体', '坦克被摧毁！', 'ok'); toast('坦克已摧毁');
    for (const a of allies()) a.morale = clamp(a.morale + 12, 0, 100);
    for (const e of enemies()) e.morale -= 12;
  } else if (v.type === 'apc') { World.stats.apcKilled++; say('全体', '装甲车瘫痪！', 'ok'); }
  if (v.hasBox) dropBox(v.x, v.y);
  // 车上未下车的步兵随之阵亡
  for (const t of v.troops) if (!t.dead && t.mount === v) { t.mount = null; damageUnit(t, 90, byUnit, 'splash'); }
  for (const a of allies()) a.morale = clamp(a.morale + 3, 0, 100);
}
function dropBox(x, y) {
  World.box = { x, y, taken: false, carrier: null, t: 0 };
  sfx('boxDrop', x, y);
  say('全体', '密码箱掉出来了，去拿！', 'sys');
  /* 自动指派一个人去取。早期这里只把箱子放到地上就算完事，
     而「搬密码箱」这条命令只有当箱子已经掉出来时才会真正生效（见 applyOrder）——
     玩家通常是在车还没打爆时就下令，于是箱子一落地就没人管，主目标直接判死。 */
  const pool = allies().filter(u => !u.dead && !u.downed);
  if (!pool.length) return;
  /* 挑人：距离越近越好，但被火力压制的人要打折（别让志愿者顶着机枪去送死）；
     支援组的铁头按文档是首选，只要他不是远得离谱。 */
  const cost = u => dist(u.x, u.y, x, y) + u.suppression * 4;
  pool.sort((p, q) => cost(p) - cost(q));
  const support = pool.find(u => u.role === '弹药/支援');
  const pick = (support && cost(support) <= cost(pool[0]) * 1.6) ? support : pool[0];
  pick.boxTask = true; pick.moveGoal = { x, y };
  say(pick.name, '我去拿密码箱！', 'ok');
}
/* vehMul：爆炸对车辆的伤害系数（默认 0.5，用于手雷/炮击这类通用爆炸）
   vehKind：传给 damageVehicle 的弹药种类，决定穿甲系数（火箭弹是 0.95，炮弹 1.1…）
   早期这里写死 damageVehicle(v, dmg * 0.5, 'shell')，于是：
     1) 火箭弹的 dmgVeh(300) 被再砍一半 → 单发只有 150；
     2) kind 恒为 'shell'，ROCKET 应有的 0.95 穿甲系数永远不生效。
   结果 950 HP 的坦克即使 6 发火箭全部命中（6×150×1.1=990）也几乎打不掉 —— 主目标实质不可达。 */
function explosion(x, y, r, dmg, team, kind, vehMul, vehKind) {
  fx({ type: 'boom', x, y, r, life: 0.55 });
  decal(x, y, r * 0.5, 'rgba(24,20,16,.5)');
  World.noise = clamp(World.noise + 0.45, 0, 1);
  World.noiseT = 0.9;
  /* 爆炸音效按 type 分档：半径越大的爆炸更沉、更长，并且更狠地压住其他声音
     （近距离还能听见耳鸣 ring）——这是玩家判断「炸到哪儿了」的主要线索之一 */
  {
    const bid = ({ mine: 'mine', barrel: 'barrel', grenade: 'grenade', rocket: 'rocketBoom',
      shell: 'shellBoom', vehicle: 'vehicleBoom', cannon: 'cannon' })[kind] || 'explosion';
    sfx(bid, x, y, { gain: clamp(0.6 + r / 220, 0.7, 1.35) });
  }
  for (const u of World.units) {
    if (u.dead || u.downed) continue;
    if (u.team === team && kind !== 'vehicle') continue;
    const d = dist(x, y, u.x, u.y);
    if (d < r) damageUnit(u, dmg * (1 - d / r * 0.6), null, 'splash');
    else if (d < r * 2.1) u.suppression = clamp(u.suppression + 34 * (1 - d / (r * 2.1)), 0, 100);
  }
  const vm = vehMul === undefined ? 0.5 : vehMul;
  const vk = vehKind || kind;
  for (const v of World.vehicles) {
    if (v.destroyed || v.team === team) continue;
    const d = dist(x, y, v.x, v.y);
    if (d < r * 1.1) damageVehicle(v, dmg * vm * (1 - d / (r * 1.1)), vk, null);
  }
  for (const p of World.props) {
    if (p.destroyed || !p.explosive) continue;
    if (dist(x, y, p.x, p.y) < r + 8) { p.destroyed = true; chainBarrel(p); }
  }
}
let armMines = false;
function chainBarrel(p) {
  explosion(p.x, p.y, 78, 85, null, 'barrel');
}

/* =========================================================================
   三、指令下发（呼号 → 队员；服从度 → 立即/延迟/拒绝）
   ========================================================================= */
function calcObey(u, cmd) {
  let o = u.obeyBase + (u.morale - 55) / 220;
  const th = nearestEnemyAt(u.x, u.y, 'ally', x => !x.dead);
  const d = th ? dist(u.x, u.y, th.x, th.y) : 9999;
  let danger = 0;
  if (d < 220) danger += 0.09;
  if (d < 110) danger += 0.13;
  if (u.suppression > 55) danger += 0.17;
  if (u.hp < 45) danger += 0.10;
  const id = cmd.action.id;
  if (id === 'takeCover' || id === 'ceasefire' || id === 'retreat' || id === 'retreatTo' || id === 'evac') danger *= 0.3;
  const supPen = (u.suppression / 100) * 0.34;
  o = o - danger - supPen + (u.obeyBoost || 0);
  if (u.role === '医疗兵' && (id === 'rescue' || id === 'heal' || id === 'drag')) o += 0.30;
  if (u.role === '反坦克手' && (id === 'atTank' || id === 'atAPC' || id === 'rocket')) o += 0.25;
  if (u.group === '火力组' && (id === 'fire' || id === 'suppress' || id === 'focusFire')) o += 0.18;
  if (u.group === '支援组' && id === 'resupply') o += 0.3;
  if (u.role === '爆破手' && (id === 'detonate' || id === 'blowBridge')) o += 0.35;
  if (u.state === '恐慌') o -= 0.35;
  if (u.downed || u.dead) o = 0;
  return clamp(o, 0, 1);
}

function resolveTargets(cmd) {
  const cs = cmd.callsign;
  const out = [];
  const add = id => { const u = World.units.find(x => x.id === id && x.team === 'ally'); if (u) out.push(u); };
  if (!cs) { const a = nearestAllyTo(World.player, cmd); if (a) out.push(a); return out; }
  if (cs.kind === 'all') { for (const u of World.units) if (u.team === 'ally') out.push(u); }
  else if (cs.kind === 'group') { for (const id of (GROUPS[cs.id] || [])) add(id); }
  else if (cs.kind === 'role') { for (const id of (ROLE_CALL[cs.id] || [])) add(id); }
  else if (cs.kind === 'member' && cs.id !== 'auto') add(cs.id);
  else { const a = nearestAllyTo(World.player, cmd); if (a) out.push(a); }
  return out.filter(u => u && !u.dead);
}
function nearestAllyTo(p, cmd) {
  let best = null, bd = 1e9;
  for (const u of World.units) {
    if (u.team !== 'ally' || u.dead || u.downed || u === p) continue;
    const d = dist2(u.x, u.y, p.x, p.y);
    if (d < bd) { bd = d; best = u; }
  }
  return best;
}

/* 目标检索：按「打 X」的类型与方向修饰 */
function pickTargetByType(u, type, cmd) {
  const pt = { x: u.x, y: u.y };
  let best = null, bs = -1e9;
  const consider = (o, kind) => {
    if (o.dead || o.downed) return;
    const d = dist(u.x, u.y, o.x, o.y);
    if (d > 900) return;
    let s = 1000 - d;
    if (cmd && cmd.dir) {
      const brg = (Math.atan2(o.x - pt.x, -(o.y - pt.y)) * 180 / Math.PI + 360) % 360;
      let db = Math.abs(((brg - cmd.dir.bearing + 540) % 360) - 180);
      s -= db * 3;
    } else {
      if (o.target && o.target.isPlayer) s += 160;
      const th = o.target && o.target.team === 'ally' ? 200 : 0;
      s += th;
      if (o.lastHurtT && World.t - o.lastHurtT < 6) s += 80;
    }
    if (s > bs) { bs = s; best = o; }
  };
  if (type === 'inf' || type === 'mg' || type === 'officer') {
    for (const e of World.units) {
      if (e.team !== 'enemy' || e.dead) continue;
      if (e.mount) continue;
      if (type === 'mg' && !e.mg) continue;
      if (type === 'officer' && !e.officer) continue;
      consider(e);
    }
  } else {
    for (const v of World.vehicles) {
      if (v.destroyed || v.team !== 'enemy') continue;
      const nt = v.type === 'apc' ? 'apc' : v.type;
      if (nt !== type) continue;
      consider(v);
    }
  }
  return best;
}

function applyOrder(u, cmd) {
  const id = cmd.action.id, A = cmd.action;
  u.order = cmd; u.orderT = World.t; u.pendingOrder = null;
  const th = nearestEnemyAt(u.x, u.y, 'ally');
  switch (id) {
    case 'advance': {
      const goal = cmd.loc ? { x: cmd.loc.x, y: cmd.loc.y } : World.marker ? { x: World.marker.x, y: World.marker.y } : (th ? { x: th.x, y: th.y } : { x: u.x, y: u.y - 120 });
      u.moveGoal = goal; u.holdPosition = false; u.state = '移动'; break;
    }
    case 'gotoPoint': case 'retreatTo': case 'enterBuilding': {
      const goal = cmd.loc ? { x: cmd.loc.x + rr(-40, 40), y: cmd.loc.y + rr(-40, 40) } : World.marker;
      if (goal) { u.moveGoal = goal; u.holdPosition = false; u.state = id === 'retreatTo' ? '撤退' : '移动'; }
      break;
    }
    case 'fallback': {
      const back = Math.atan2(u.y - (th ? th.y : u.y), u.x - (th ? th.x : u.x));
      u.moveGoal = { x: u.x + Math.cos(back) * 130, y: u.y + Math.sin(back) * 130 };
      u.state = '移动'; break;
    }
    case 'retreat': {
      World.evacArmed = true;
      u.moveGoal = { x: World.evac.x + rr(-50, 50), y: World.evac.y + rr(-50, 50) };
      u.state = '撤退'; u.fireMode = 'free'; break;
    }
    case 'evac': {
      World.evacArmed = true;
      u.moveGoal = { x: World.evac.x + rr(-40, 40), y: World.evac.y + rr(-40, 40) };
      u.state = '撤离'; break;
    }
    case 'moveLeft': case 'moveRight': {
      const base = th ? Math.atan2(th.y - u.y, th.x - u.x) : u.facing;
      const a = base + (id === 'moveLeft' ? -Math.PI / 2 : Math.PI / 2);
      u.moveGoal = { x: u.x + Math.cos(a) * 120, y: u.y + Math.sin(a) * 120 };
      u.state = '移动'; break;
    }
    case 'spread': {
      const other = World.units.filter(x => x.team === 'ally' && x !== u && !x.dead);
      const near = other.sort((a, b) => dist2(a.x, a.y, u.x, u.y) - dist2(b.x, b.y, u.x, u.y))[0];
      if (near) {
        const a = Math.atan2(u.y - near.y, u.x - near.x);
        u.moveGoal = { x: u.x + Math.cos(a) * 80, y: u.y + Math.sin(a) * 80 };
      } else {
        u.moveGoal = { x: u.x + Math.cos(u.facing) * 70, y: u.y + Math.sin(u.facing) * 70 };
      }
      u.state = '移动'; break;
    }
    case 'flank': {
      const t = u.target || th;
      if (t) {
        const base = Math.atan2(t.y - u.y, t.x - u.x);
        u.flankPt = { x: t.x + Math.cos(base + Math.PI / 2) * 220, y: t.y + Math.sin(base + Math.PI / 2) * 220 };
        u.moveGoal = u.flankPt; u.state = '移动';
      } else { u.moveGoal = { x: u.x + Math.cos(u.facing) * 140, y: u.y + Math.sin(u.facing) * 140 }; u.state = '移动'; }
      break;
    }
    case 'takeCover': case 'setup': {
      const t = th || { x: u.x, y: u.y + 100 };
      const c = findCover(u.x, u.y, t.x, t.y, 90) || { x: u.x, y: u.y };
      u.coverPos = c;
      /* 玩家单位是人在操控，不会自己走过去。早期这里一视同仁地置「找掩体」，
         结果玩家收到「全体，隐蔽」后 HUD 会永久红着「找掩体」，
         而人站在原地看着那个状态也很困惑。玩家直接进「隐蔽」，
         coverPos 保留为地图上的建议位置标记。 */
      if (u.isPlayer) { u.moveGoal = null; u.state = '隐蔽'; }
      else { u.moveGoal = c; u.state = '找掩体'; }
      break;
    }
    case 'hold': { u.holdPosition = true; u.moveGoal = null; u.state = '待命'; break; }
    case 'followMe': { u.formationFollow = true; u.followTarget = World.player; u.moveGoal = null; u.state = '移动'; break; }
    case 'fire': case 'freeFire': { u.fireMode = 'free'; u.holdPosition = false; if (u.state === '隐蔽' || u.state === '待命') u.state = '战斗'; break; }
    case 'ceasefire': { u.fireMode = 'hold'; u.state = '隐蔽'; u.holdPosition = true; u.moveGoal = null; break; }
    case 'focusFire': {
      const t = nearestEnemyToMarker(u);
      if (t) { u.focusTarget = t; u.fireMode = 'free'; if (u.state === '隐蔽' || u.state === '待命') u.state = '战斗'; }
      break;
    }
    case 'suppress': {
      u.fireMode = 'suppress'; const t = th; if (t) { u.target = t; }
      if (u.state === '隐蔽' || u.state === '待命' || u.state === '警戒') u.state = '战斗';
      break;
    }
    case 'coverMe': {
      const t = nearestEnemyAt(World.player.x, World.player.y, 'ally');
      if (t) { u.target = t; u.fireMode = 'suppress'; if (u.state !== '反坦克') u.state = '战斗'; }
      break;
    }
    case 'atTank': case 'atAPC': case 'atJeep': case 'atTruck': case 'atInf': case 'atMG': case 'atOfficer': case 'rocket': {
      const type = id === 'rocket' ? 'tank' : A.target;
      const t = pickTargetByType(u, type, cmd);
      u.preferType = type;
      if (t) {
        u.target = t; u.fireMode = 'free';
        if ((t.type === 'tank' || t.type === 'apc' || t.type === 'jeep' || t.type === 'truck') && (u.weaponKey === 'at' || u.hasCharge)) {
          u.state = '反坦克'; u.atTarget = t;
        } else if (u.state === '隐蔽' || u.state === '待命' || u.state === '警戒') u.state = '战斗';
      }
      break;
    }
    case 'grenade': { u.grenadeOrder = true; break; }
    case 'smoke': { u.smokeOrder = true; break; }
    case 'rescue': {
      const id2 = (cmd.mentioned || []).find(i => { const d = World.units.find(x => x.id === i); return d && d.downed && !d.dead; });
      const t = id2 ? World.units.find(x => x.id === id2) : nearestDowned(u);
      if (t) { u.rescueTarget = t; u.state = '救援'; u.moveGoal = { x: t.x, y: t.y }; }
      break;
    }
    case 'drag': {
      const t = (cmd.mentioned || []).map(i => World.units.find(x => x.id === i)).find(x => x && x.downed)
        || nearestDowned(u);
      if (t) { u.rescueTarget = t; u.rescueMode = 'drag'; u.state = '救援'; u.moveGoal = { x: t.x, y: t.y }; }
      break;
    }
    case 'heal': {
      const t = (cmd.mentioned || []).map(i => World.units.find(x => x.id === i)).find(x => x && x.team === 'ally' && x.hp < x.maxHp)
        || World.units.filter(x => x.team === 'ally' && !x.dead && x.hp < x.maxHp * 0.8).sort((a, b) => dist2(a.x, a.y, u.x, u.y) - dist2(b.x, b.y, u.x, u.y))[0];
      if (t) { u.healTarget = t; u.state = '治疗'; u.moveGoal = { x: t.x, y: t.y }; }
      break;
    }
    case 'resupply': {
      const t = World.units.filter(x => x.team === 'ally' && !x.dead && !x.downed && x !== u)
        .sort((a, b) => (a.ammo / a.wpn.ammo) - (b.ammo / b.wpn.ammo))[0];
      if (t) { u.resupplyTarget = t; u.state = '移动'; u.moveGoal = { x: t.x, y: t.y }; }
      break;
    }
    case 'grabBox': {
      if (World.box && !World.box.taken && !World.box.carrier) { u.boxTask = true; u.state = '移动'; u.moveGoal = { x: World.box.x, y: World.box.y }; }
      break;
    }
    case 'detonate': { detonateMines(u); break; }
    case 'blowBridge': { u.bridgeTask = true; u.state = '移动'; u.moveGoal = { x: POINTS.C.x, y: POINTS.C.y }; break; }
    case 'cancel': { u.order = null; u.moveGoal = null; u.coverPos = null; u.holdPosition = false; u.fireMode = 'free'; u.preferType = null; u.focusTarget = null; u.formationFollow = false; u.boxTask = false; u.bridgeTask = false; u.state = '待命'; break; }
    default: u.state = '警戒';
  }
}

function nearestDowned(u) {
  let best = null, bd = 1e9;
  for (const d of World.units) {
    if (d.team !== 'ally' || !d.downed || d.dead) continue;
    const v = dist2(u.x, u.y, d.x, d.y);
    if (v < bd) { bd = v; best = d; }
  }
  return best;
}
function nearestEnemyToMarker(u) {
  const m = World.marker;
  let best = null, bd = 1e9;
  const cand = World.units.filter(x => x.team === 'enemy' && !x.dead && !x.mount).concat(World.vehicles.filter(v => v.team === 'enemy' && !v.destroyed));
  for (const c of cand) {
    const d = m ? dist2(c.x, c.y, m.x, m.y) : dist2(c.x, c.y, u.x, u.y);
    if (d < bd) { bd = d; best = c; }
  }
  return best;
}
function detonateMines(u) {
  let any = false;
  for (const m of World.mines) {
    if (m.used) continue;
    let near = null;
    for (const v of enemyVehicles()) if (dist(v.x, v.y, m.x, m.y) < 60) near = v;
    if (near) {
      m.used = true; any = true; World.stats.minesUsed++;
      explosion(m.x, m.y, 70, 150, 'ally', 'mine');
      damageVehicle(near, 240, 'mine', u);
    }
  }
  if (any) { for (const a of World.units) if (a.team === 'ally') a.morale = clamp(a.morale + 6, 0, 100); }
  else say(u.name, '雷区里还没有车，等一下', 'ok');
}

/* --------------------------------------------------------------- 指令入口 */
function issueCommand(cmd, silent) {
  if (World.over || !cmd || !cmd.action) return;
  World.lastCmd = cmd;
  World.stats.cmdIssued++;
  const targets = resolveTargets(cmd);
  const id = cmd.action.id;

  /* 全局指令：起爆 / 炸桥 */
  if (id === 'detonate' || id === 'blowBridge') {
    const holder = World.units.find(x => x.id === 'laobai' && x.team === 'ally' && !x.dead && !x.downed)
      || World.units.find(x => x.id === 'shitou' && !x.dead && !x.downed);
    if (!holder) { say('全体', '没人能操作炸药了', 'no'); return; }
    const ob = calcObey(holder, cmd);
    if (ob < 0.3) { say(holder.name, pick(REPLIES.refuse), 'no'); World.stats.cmdRefused++; return; }
    applyOrder(holder, cmd);
    World.stats.cmdExec++;
    say(holder.name, id === 'detonate' ? '收到，准备起爆' : '收到，我去炸桥', 'ok');
    return;
  }
  if (id === 'repeat') {
    if (!World.lastCmd || World.lastCmd === cmd) { say('全体', '……请把指令再说一遍', 'no'); return; }
    for (const u of World.units) if (u.team === 'ally') u.obeyBoost = 0.2;
    say('你', '重复：' + World.lastCmd.action.label + (World.lastCmd.callsign ? '（' + World.lastCmd.callsign.label + '）' : ''), 'cmd');
    const c2 = JSON.parse(JSON.stringify(World.lastCmd));
    issueCommand(c2, true);
    return;
  }
  if (id === 'cancel') {
    for (const u of targets) { if (u.dead) continue; applyOrder(u, cmd); }
    say('全体', '取消指令，返回自动状态', 'sys'); return;
  }
  if (id === 'ack') { say('你', '收到', 'cmd'); return; }

  /* 状态报告：即时语音回复，不改状态 */
  if (cmd.action.kind === 'report') {
    for (const u of targets) { if (u.dead) continue; say(u.name, reportLine(u, id), 'ok'); }
    World.stats.cmdExec++;
    return;
  }

  /* applyOrder 前置检查：需要满足前提条件 */
  for (const u of targets) {
    if (u.dead || u.downed) continue;
    const need = precheck(u, cmd);
    if (!need.ok) { say(u.name, need.msg, 'no'); World.stats.cmdRefused++; continue; }
    const ob = calcObey(u, cmd);
    u.obeyRoll = ob;
    if (ob > 0.6) {
      applyOrder(u, cmd);
      World.stats.cmdExec++;
      if (!silent) say(u.name, pick(REPLIES.ok), 'ok');
      if (u.isPlayer) { /* 玩家指令无需回复 */ }
    } else if (ob > 0.3) {
      u.pendingOrder = cmd; u.pendingT = rr(1, 3);
      say(u.name, pick(REPLIES.delay), 'warn');
      World.stats.cmdRefused++;
    } else {
      say(u.name, u.state === '恐慌' ? pick(REPLIES.panic) : pick(REPLIES.refuse), 'no');
      World.stats.cmdRefused++;
    }
  }
  if (!targets.length) say('全体', '没有人响应（呼号未匹配）', 'no');
}
function precheck(u, cmd) {
  const id = cmd.action.id;
  if (id === 'rescue' || id === 'drag') {
    const t = nearestDowned(u);
    if (!t) return { ok: false, msg: '没有人需要救' };
  }
  if (id === 'atTank' || id === 'atAPC') {
    if (u.weaponKey === 'at' && u.rockets <= 0) return { ok: false, msg: '火箭弹打光了' };
  }
  if ((id === 'atTank' || id === 'atAPC' || id === 'atJeep' || id === 'atTruck') && u.weaponKey === 'at') {
    const t = pickTargetByType(u, cmd.action.target, cmd);
    if (!t) return { ok: false, msg: '视野里没有目标' };
    if (u.rockets <= 0) return { ok: false, msg: '火箭弹打光了' };
  }
  if (id === 'grenade' && u.grenades <= 0) return { ok: false, msg: '手雷用完了' };
  if (id === 'smoke' && u.smokes <= 0) return { ok: false, msg: '没有烟雾弹' };
  if (id === 'grabBox' && (!World.box || World.box.taken)) return { ok: false, msg: '密码箱不在地上了' };
  if (u.ammo <= 0 && (cmd.action.kind === 'combat' || cmd.action.kind === 'atk')) return { ok: false, msg: '没弹药了' };
  return { ok: true };
}
function reportLine(u, id) {
  const st = u.downed ? '受伤倒地' : u.hp > 85 ? '未受伤' : u.hp > 55 ? '轻伤' : u.hp > 30 ? '受伤' : '重伤';
  if (id === 'reportAmmo') return '弹药还有 ' + Math.max(0, Math.round(u.ammo)) + ' 发' + (u.weaponKey === 'at' ? '，火箭弹 ' + u.rockets + ' 发' : '');
  if (id === 'reportPos') {
    let near = '公路南侧', bd = 1e9;
    for (const k in POINTS) { const d = dist(u.x, u.y, POINTS[k].x, POINTS[k].y); if (d < bd) { bd = d; near = POINTS[k].name; } }
    return near + '附近';
  }
  if (id === 'reportCas') {
    const d = downedAllies();
    const dead = World.units.filter(x => x.team === 'ally' && x.dead);
    if (!d.length && !dead.length) return '无人伤亡';
    return (dead.length ? dead.length + ' 人阵亡，' : '') + (d.length ? d.map(x => x.name).join('、') + ' 受伤' : '无人需要救援');
  }
  if (id === 'reportContact') {
    const list = [];
    for (const v of enemyVehicles()) { if (canSee(u, v)) list.push(v.name + ' 在 ' + bearingName(u, v)); }
    const ei = enemies().filter(e => !e.mount && canSee(u, e)).slice(0, 4);
    if (ei.length) list.push('步兵 ' + ei.map(e => bearingName(u, e)).join('、'));
    return list.length ? list.slice(0, 3).join('；') : '视野内没有敌人';
  }
  if (id === 'reportRemain') {
    const inf = enemies().filter(e => !e.mount).length;
    const veh = enemyVehicles().filter(v => v.type !== 'truck').length;
    return '还剩 ' + inf + ' 个步兵，' + veh + ' 辆装甲目标';
  }
  if (id === 'ready') return '已就位';
  return st + '，弹药 ' + Math.round(u.ammo);
}
function bearingName(from, to) {
  let brg = (Math.atan2(to.x - from.x, -(to.y - from.y)) * 180 / Math.PI + 360) % 360;
  const h = Math.round(brg / 30) % 12 || 12;
  return h + ' 点方向';
}

/* =========================================================================
   四、战斗：弹道 / 命中 / 压制
   ========================================================================= */
function spawnBullet(u, tgt, spreadMul, opts) {
  const w = u.wpn;
  const base = Math.atan2(tgt.y - u.y, tgt.x - u.x);
  const sp = (w.spread || 0.05) * (spreadMul || 1);
  const a = base + (RNG() + RNG() + RNG() - 1.5) * sp * 2;
  World.projectiles.push({
    type: 'bullet', x: u.x + Math.cos(a) * 8, y: u.y + Math.sin(a) * 8,
    vx: Math.cos(a) * w.speed, vy: Math.sin(a) * w.speed, dmg: w.dmg, pen: w.pen,
    team: u.team, owner: u,
    /* rangeMul：第一人称下用准星俯仰角折算的有效射程系数（AI 不开火时恒为 1） */
    life: (w.range * (opts && opts.rangeMul !== undefined ? opts.rangeMul : 1)) / w.speed,
    t: 0, sup: w.sup,
    color: u.team === 'ally' ? 'rgba(190,235,255,.95)' : 'rgba(255,190,150,.95)',
  });
  u.magAmmo--; u.shots++;
  if (u.team === 'ally') World.stats.shotFired = true;
  const mx = u.x + Math.cos(a) * 8, my = u.y + Math.sin(a) * 8;
  fx({ type: 'flash', x: mx, y: my, life: 0.055, a });
  /* 枪声：按阵营与武器区分音色。玩家自己那一声走 local 居中播放 ——
     否则「自己开枪」会被当成一个贴在耳边的点声源，声像会随视角乱甩。 */
  const sid = u.isPlayer ? 'rifle' : u.team === 'ally'
    ? (u.weaponKey === 'mg' ? 'mg' : u.weaponKey === 'sniper' ? 'sniper' : 'rifle')
    : (u.weaponKey === 'emg' ? 'enemyMG' : 'enemyRifle');
  sfx(sid, mx, my, u.isPlayer ? { local: true, gain: 0.9 } : null);
  if (u.team === 'ally') { World.noise = clamp(World.noise + 0.02, 0, 1); World.noiseT = Math.max(World.noiseT || 0, 0.25); }
}
function spawnRocket(u, tgt) {
  const base = Math.atan2(tgt.y - u.y, tgt.x - u.x) + rr(-0.02, 0.02);
  World.projectiles.push({
    type: 'rocket', x: u.x + Math.cos(base) * 12, y: u.y + Math.sin(base) * 12,
    vx: Math.cos(base) * ROCKET.speed, vy: Math.sin(base) * ROCKET.speed,
    dmg: ROCKET.dmgVeh, dmgInf: ROCKET.dmgInf, splash: ROCKET.splash, team: u.team, owner: u,
    life: ROCKET.range / ROCKET.speed, t: 0, color: 'rgba(255,220,120,.95)',
  });
  u.rockets--; u.shots++;
  if (u.team === 'ally') World.stats.shotFired = true;
  fx({ type: 'flash', x: u.x + Math.cos(base) * 10, y: u.y + Math.sin(base) * 10, life: 0.12, a: base, big: true });
  sfx('rocketFire', u.x + Math.cos(base) * 10, u.y + Math.sin(base) * 10, u.isPlayer ? { local: true } : null);
  say(u.name, '火箭弹出去了！', 'ok');
}
function throwGrenade(u, tx, ty, kind) {
  World.projectiles.push({
    type: kind === 'smoke' ? 'smokeG' : 'grenade', x: u.x, y: u.y,
    tx, ty, t: 0, life: 1.5, fuse: kind === 'smoke' ? 0.9 : 1.7, team: u.team, owner: u,
    dmg: 85, splash: 72, color: kind === 'smoke' ? 'rgba(200,200,200,.9)' : 'rgba(140,170,120,.95)',
  });
  if (kind === 'smoke') u.smokes--; else u.grenades--;
  fx({ type: 'flash', x: u.x, y: u.y, life: 0.08, a: Math.atan2(ty - u.y, tx - u.x) });
  sfx('grenadeThrow', u.x, u.y, u.isPlayer ? { local: true } : null);
}
function spawnShell(v, tx, ty) {
  const base = Math.atan2(ty - v.y, tx - v.x) + rr(-0.02, 0.02);
  World.projectiles.push({
    type: 'shell', x: v.x + Math.cos(base) * 36, y: v.y + Math.sin(base) * 36,
    vx: Math.cos(base) * SHELL.speed, vy: Math.sin(base) * SHELL.speed,
    dmg: SHELL.dmg, splash: SHELL.splash, team: v.team, owner: v, life: 2.0, t: 0,
    color: 'rgba(255,240,190,.95)',
  });
  fx({ type: 'flash', x: v.x + Math.cos(base) * 34, y: v.y + Math.sin(base) * 34, life: 0.16, a: base, big: true });
  sfx('cannon', v.x, v.y);
  World.noise = clamp(World.noise + 0.5, 0, 1); World.noiseT = 1.0;
}
function spawnTankMG(v, tgt) {
  const base = Math.atan2(tgt.y - v.y, tgt.x - v.x) + (RNG() + RNG() - 1) * 0.10;
  World.projectiles.push({
    type: 'bullet', x: v.x + Math.cos(base) * 30, y: v.y + Math.sin(base) * 30,
    vx: Math.cos(base) * 950, vy: Math.sin(base) * 950, dmg: v.spec.mg.dmg, pen: 0.12,
    team: v.team, owner: v, life: 0.6, t: 0, sup: v.spec.mg.sup, color: 'rgba(255,160,120,.95)',
  });
  fx({ type: 'flash', x: v.x + Math.cos(base) * 30, y: v.y + Math.sin(base) * 30, life: 0.05, a: base });
  /* 车载机枪：走 enemyMG 音色但压低一点，免得抢过步兵步枪的方位感 */
  sfx('enemyMG', v.x + Math.cos(base) * 30, v.y + Math.sin(base) * 30, { gain: 0.85 });
}

function updateProjectiles(dt) {
  const P = World.projectiles;
  for (let i = P.length - 1; i >= 0; i--) {
    const p = P[i];
    p.t += dt;
    if (p.type === 'grenade' || p.type === 'smokeG') {
      const k = p.t / p.life;
      p.x = lerp(p.x, p.tx, Math.min(1, dt * 4.5 + (k > 0.9 ? 1 : 0.12)));
      p.y = lerp(p.y, p.ty, Math.min(1, dt * 4.5 + (k > 0.9 ? 1 : 0.12)));
      if (p.t >= p.life) {
        if (p.type === 'smokeG') { World.smokes.push({ x: p.x, y: p.y, r: 62, t: 0, life: 16 }); say('全体', '烟雾已释放', 'ok'); }
        else explosion(p.x, p.y, p.splash, p.dmg, p.team, 'grenade');
        P.splice(i, 1);
      }
      continue;
    }
    const steps = Math.max(1, Math.ceil(Math.hypot(p.vx, p.vy) * dt / 4));
    let dead = false;
    for (let s = 0; s < steps && !dead; s++) {
      const px = p.x, py = p.y;
      p.x += p.vx * dt / steps; p.y += p.vy * dt / steps;
      /* 掠弹音：子弹从耳边擦过去。只算「不是自己打的」子弹，且每发只响一次 ——
         这是第一人称下判断「有人正朝我打」最直接的一条听觉线索。 */
      if (Sound.ok && !p.cracked && !(p.owner && p.owner.isPlayer)) {
        const L = listener();
        if (dist2(p.x, p.y, L.x, L.y) < 13225) { p.cracked = true; sfx('crack', p.x, p.y, { gap: 0.02 }); }
      }
      // 掩体
      for (const pr of World.props) {
        if (pr.destroyed || !pr.blocksBullet) continue;
        const rin = pr.r + 2.5;
        if (dist2(px, py, pr.x, pr.y) <= rin * rin) continue;   // 出膛时已贴着这块掩体 → 视为擦过，否则贴身射击会被自己的掩体吃掉
        if (segCircle(px, py, p.x, p.y, pr.x, pr.y, pr.r)) {
          if (pr.explosive) { pr.destroyed = true; chainBarrel(pr); }
          fx({ type: 'spark', x: p.x, y: p.y, life: 0.18 });
          sfx('impact', p.x, p.y);
          dead = true; break;
        }
      }
      if (dead) break;
      // 步兵
      for (const u of World.units) {
        if (u.dead || u.downed || u.team === p.team) continue;
        if (u.mount) continue;
        /* 用「本子步的位移线段」判命中，而不是只判终点：
           步兵判定半径 5.2+3=8.2px 与子步长同量级，只判终点会让掠射弹整段漏检（命中率被腰斩） */
        if (segCircle(px, py, p.x, p.y, u.x, u.y, u.radius + 3)) {
          if (p.type === 'rocket') { explosion(p.x, p.y, p.splash, p.dmgInf, p.team, 'rocket'); }
          else damageUnit(u, p.dmg, p.owner, 'bullet');
          if (p.owner && p.owner.team === 'ally') p.owner.hits++;
          if (p.owner && p.owner.isPlayer) sfx('hit');       // 准星命中反馈音
          fx({ type: 'spark', x: p.x, y: p.y, life: 0.16, blood: true });
          dead = true; break;
        }
      }
      if (dead) break;
      // 车辆
      for (const v of World.vehicles) {
        if (v.destroyed || v.team === p.team) continue;
        if (vehicleHit(v, p.x, p.y)) {
          if (p.type === 'rocket') explosion(p.x, p.y, p.splash, p.dmg, p.team, 'rocket', 1.0, 'rocket');
        else if (p.type === 'shell') explosion(p.x, p.y, p.splash, p.dmg, p.team, 'shell', 0.5, 'shell');
          else damageVehicle(v, p.dmg, 'bullet', p.owner);
          fx({ type: 'spark', x: p.x, y: p.y, life: 0.16 });
          dead = true; break;
        }
      }
      if (dead) break;
      // 近失弹压制
      if (p.sup) {
        for (const u of World.units) {
          if (u.dead || u.team === p.team) continue;
          if (dist2(p.x, p.y, u.x, u.y) < 900) u.suppression = clamp(u.suppression + p.sup * dt * 6, 0, 100);
        }
      }
    }
    if (!dead && p.t > p.life) {
      if (p.type === 'rocket' || p.type === 'shell') explosion(p.x, p.y, p.splash, p.dmg, p.team, 'shell');
      dead = true;
    }
    if (dead) P.splice(i, 1);
  }
}
function vehicleHit(v, x, y) {
  const dx = x - v.x, dy = y - v.y;
  const c = Math.cos(-v.angle), s = Math.sin(-v.angle);
  const lx = dx * c - dy * s, ly = dx * s + dy * c;
  return Math.abs(lx) < v.len / 2 + 2 && Math.abs(ly) < v.wid / 2 + 2;
}

/* =========================================================================
   五、队友 AI（感知 → 决策 → 行动 → 通讯）
   ========================================================================= */
function setState(u, s) { if (u.state !== s) { u.state = s; u.stateT = 0; } }

function perceive(u) {
  let best = null, bd = 1e9;
  const r = visRange(u);
  const cand = [];
  /* 目标必须是「对面阵营」：早期这里写死 team === 'enemy'，
     导致敌人调用本函数时把同袍（甚至自己）当成目标 —— 敌人会朝自己开枪且永不推进 */
  for (const e of World.units) { if (e !== u && e.team !== u.team && !e.dead && !e.downed && !e.mount) cand.push(e); }
  for (const v of World.vehicles) { if (v.team !== u.team && !v.destroyed) cand.push(v); }
  for (const e of cand) {
    const d = dist(u.x, u.y, e.x, e.y);
    if (d > r) continue;
    if (losSight(u.x, u.y, e.x, e.y) && d > 130) continue;
    if (d < bd) { bd = d; best = e; }
  }
  u.seen = best; u.seenD = bd;
  if (best) u.lastSeen = { x: best.x, y: best.y, t: World.t };
  return best;
}

function allyTargetSelect(u) {
  // 目标优先级（文档 5.）：玩家指定 > 正在攻击队友的敌人 > 角色优先 > 最近威胁
  if (u.focusTarget && !u.focusTarget.dead && !u.focusTarget.destroyed && canSee(u, u.focusTarget)) return u.focusTarget;
  if (u.atTarget && !u.atTarget.dead && !u.atTarget.destroyed) return u.atTarget;
  if (u.preferType) {
    const t = pickTargetByType(u, u.preferType, null);
    if (t && canSee(u, t)) return t;
  }
  // 反坦克手优先装甲
  if (u.weaponKey === 'at' && u.rockets > 0) {
    const t = pickTargetByType(u, 'tank', null) || pickTargetByType(u, 'apc', null);
    if (t && canSee(u, t) && dist(u.x, u.y, t.x, t.y) < ROCKET.range) return t;
  }
  // 狙击手优先军官 / 机枪手 / 驾驶员
  if (u.weaponKey === 'sniper') {
    const o = pickTargetByType(u, 'officer', null);
    if (o && canSee(u, o)) return o;
    const m = pickTargetByType(u, 'mg', null);
    if (m && canSee(u, m)) return m;
  }
  if (u.seen && !ineffectiveTarget(u, u.seen)) return u.seen;
  /* 「看得见的」只剩打不动的装甲目标 → 改打打得动的步兵，别对着铁壳子空转整场 */
  const soft = pickTargetByType(u, 'inf', null);
  if (soft && canSee(u, soft)) return soft;
  return u.seen || null;
}

/* 手里的枪啃不动的目标：子弹对车辆的穿透系数就是车辆 armor 值（坦克 0.04 → 每发 0.52 点），
   一个弹匣打上去不如半发火箭。这里统一判定，避免步枪手把整场火力浪费在装甲车上。
   反坦克手与带炸药包的老白不算。 */
function ineffectiveTarget(u, t) {
  if (!t || !t.spec) return false;
  if (u.weaponKey === 'at' || u.hasCharge) return false;
  return t.type === 'tank' || t.type === 'apc';
}

function tryReload(u) {
  if (u.reloadT > 0) return;
  if (u.magAmmo > u.wpn.mag * 0.4) return;
  if (u.ammo <= 0) return;
  u.reloadT = u.wpn.reload * (u.state === '被压制' ? 1.35 : 1);
  sfx('reloadStart', u.x, u.y, u.isPlayer ? { local: true } : null);
}
function finishReload(u) {
  const need = Math.min(u.wpn.mag - u.magAmmo, u.ammo);
  u.magAmmo += need; u.ammo -= need;
  if (need > 0) sfx('reloadEnd', u.x, u.y, u.isPlayer ? { local: true } : null);
}

function spreadMul(u) {
  let m = 1;
  m *= 1 + u.suppression / 110;
  if (u.moving) m *= 1.45;
  if (u.aimT < 0.45) m *= 1.8;
  if (World.weather === 'rain') m *= 1.12;
  if (World.weather === 'night') m *= 1.2;
  return m;
}

function fireAt(u, tgt, dt) {
  if (u.fireMode === 'hold' || u.hideFire) return;
  if (u.reloadT > 0) return;
  if (u.magAmmo <= 0) { tryReload(u); return; }
  if (u.fireCd > 0) return;
  const w = u.wpn;
  const d = dist(u.x, u.y, tgt.x, tgt.y);
  if (d > w.range) return;
  if (d > 60 && losFire(u.x, u.y, tgt.x, tgt.y)) return;
  /* 潜伏阶段的火力纪律（文档：「沉住气，不要提前开火」）
     —— 未收到开火命令时必须按住，只有敌人贴到 55px 才允许自卫还击。
     早期用的是 130px，结果车队刚开进地图边沿就被伏击圈外的队员点着，
     头车在伏击圈以东几百米处被打瘫，后面所有车都被残骸堵在那里 —— 伏击圈形同虚设。 */
  if (u.team === 'ally' && !World.triggered && !orderedToFire(u) && d > 55) return;
  const isVeh = !!tgt.spec;
  /* 火箭弹只打坦克/装甲车；未受命时近距自卫才用，避免把伏击位置提前打乱 */
  if (isVeh && atMayUseRocket(u, tgt, d)) { spawnRocket(u, tgt); u.fireCd = ROCKET.rof; u.reloadT = ROCKET.reload * 0.5; return; }
  /* 手里的枪啃不动的装甲目标：除非玩家明确下令（如「全体，打装甲车」），否则不必浪费弹药 */
  if (isVeh && ineffectiveTarget(u, tgt) && !orderedToFire(u)) return;
  u.burstLeft = u.burstLeft > 0 ? u.burstLeft - 1 : w.burst - 1;
  u.fireCd = u.burstLeft > 0 ? w.rof : w.rof + w.burstGap;
  if (u.team === 'ally' && !World.triggered) triggerAmbush('selfdef');
  spawnBullet(u, tgt, spreadMul(u));
  if (u.state === '隐蔽' || u.state === '待命') { if (u.noAutoFire) return; setState(u, '战斗'); }
}

/* 该单位是否已收到「开火类」命令（收到后就不再受潜伏期火力纪律约束，可以主动打响第一枪） */
const FIRE_ACTIONS = ['fire', 'freeFire', 'focusFire', 'suppress', 'coverMe', 'atTank', 'atAPC',
  'atJeep', 'atTruck', 'atInf', 'atMG', 'atOfficer', 'grenade', 'smoke', 'rocket'];
function orderedToFire(u) {
  return !!(u.order && u.order.action && FIRE_ACTIONS.indexOf(u.order.action.id) >= 0);
}

/* 反坦克手是否该用火箭弹（文档：反坦克手：坦克 > 装甲车 > 吉普）
   注意：卡车默认不算装甲目标（火箭弹要留给坦克/装甲车），但玩家一旦明确下令「打卡车」
   就必须放行 —— 密码箱有 62% 概率锁在卡车后厢，不允许打就意味着该局面下主目标不可达。 */
function atMayUseRocket(u, tgt, d) {
  if (u.weaponKey !== 'at' || u.rockets <= 0) return false;
  const ordered = u.order && u.order.action &&
    (u.order.action.id === 'atTank' || u.order.action.id === 'atAPC' || u.order.action.id === 'atTruck' || u.order.action.id === 'rocket');
  const armTarget = tgt.type === 'tank' || tgt.type === 'apc' || (ordered && tgt.type === 'truck');
  if (!armTarget) return false;
  /* 这里曾经限制「未受命时 d<420」。但潜伏期的火力纪律（fireAt 里的 orderedToFire 判定）
     已经保证了伏击前不会有人开枪，这条限制只剩下副作用：反坦克手会抱着 3 发火箭弹
     被 460px 外正在开火的坦克打死，一发都不还手。触发后按火箭弹实际射程交火。 */
  return d < ROCKET.range;
}
function throwGrenadeAuto(u, tgt) {  if (u.grenades <= 0 || u.grenadeCd > 0) return;
  const d = dist(u.x, u.y, tgt.x, tgt.y);
  if (d > 150) return;
  let cluster = 0;
  for (const e of enemies()) if (!e.mount && dist(e.x, e.y, tgt.x, tgt.y) < 45) cluster++;
  if (cluster < 2 && d > 90) return;
  u.grenadeCd = 10;
  throwGrenade(u, tgt.x + rr(-8, 8), tgt.y + rr(-8, 8), 'grenade');
  say(u.name, '手雷！', 'ok');
}

function moveStep(u, dt, speedMul) {
  if (!u.moveGoal) { u.moving = false; return; }
  const g = u.moveGoal;
  const d = dist(u.x, u.y, g.x, g.y);
  if (d < 12) { u.moving = false; u.arrived = true; u.moveGoal = null; return; }
  let sp = (u.team === 'ally' ? 46 : 44) * (speedMul || 1);
  if (u.suppression > 60) sp *= 0.72;
  if (u.reloadT > 0) sp *= 0.94;
  if (inRiver(u.x, u.y) && !onBridge(u.x, u.y)) sp *= 0.5;
  const spd = d < 40 ? sp * 0.6 : sp;
  const a = steerAngle(u, g.x, g.y);
  const nx = u.x + Math.cos(a) * spd * dt, ny = u.y + Math.sin(a) * spd * dt;
  const stuck = propBlocked(u.x, u.y, 6);   // 若已被卡在车体/掩体内，允许先走出来
  /* 逐帧步长只有 ~2.6px，却一直用「半径 6 的碰撞圈」判否 ——
     只要目的地仍然落在圈里，这一步就被否掉，而 2.6px 根本不够跳出圈外，
     于是队员会被永久焊在掩体外沿（实测铁头抱着「取密码箱」任务在岩石边钉了 330 秒，
     箱子就在 900px 外躺着没人去捡）。逐帧步进改用 2px 间隙：能贴着掩体滑行，
     也走不进石头里（最小掩体半径 15px，一步 2.6px 跨不过去）。 */
  const free = (x, y) => passable(x, y) && (stuck || !propBlocked(x, y, 2));
  let moved = false;
  if (free(nx, u.y)) { u.x = nx; moved = true; }
  if (free(u.x, ny)) { u.y = ny; moved = true; }
  u.moving = moved;
  if (!u.aiming) u.facing = a;
  u.wobble += dt * 6;
}
function steerAngle(u, tx, ty) {
  const base = Math.atan2(ty - u.y, tx - u.x);
  for (const off of [0, 0.45, -0.45, 0.95, -0.95, 1.5, -1.5, 2.2, -2.2]) {
    const a = base + off;
    const nx = u.x + Math.cos(a) * 26, ny = u.y + Math.sin(a) * 26;
    if (passable(nx, ny) && !propBlocked(nx, ny, 7)) return a;
  }
  return base + Math.PI;
}

function updateAlly(u, dt) {
  if (u.dead) return;
  u.stateT += dt;
  u.hitFlash = Math.max(0, u.hitFlash - dt);
  u.aimT += dt;

  /* ---------- 失能 / 倒地 ---------- */
  if (u.downed) {
    u.downTimer -= dt;
    if (u.draggedBy && !u.draggedBy.dead && !u.draggedBy.downed) {
      const d = u.draggedBy;
      u.x = lerp(u.x, d.x - Math.cos(d.facing) * 14, Math.min(1, dt * 3));
      u.y = lerp(u.y, d.y - Math.sin(d.facing) * 14, Math.min(1, dt * 3));
    }
    if (u.downTimer <= 0) killUnit(u, null);
    else if (u.downTimer < 12 && u.speakCd <= 0) { u.speakCd = 8; say(u.name, '我快撑不住了……', 'no'); }
    return;
  }

  /* ---------- 计时器 ---------- */
  u.fireCd -= dt; u.burstCd -= dt; u.grenadeCd = (u.grenadeCd || 0) - dt;
  u.speakCd -= dt; u.reportCd -= dt; u.secReport -= dt;
  if (u.reloadT > 0) { u.reloadT -= dt; if (u.reloadT <= 0) finishReload(u); }
  if (u.obeyBoost > 0) u.obeyBoost = Math.max(0, u.obeyBoost - dt * 0.025);
  u.suppression = Math.max(0, u.suppression - dt * (u.holdPosition ? 13 : 9.5));
  if (u.stateT > 2.5 && u.morale < 100 && u.state !== '恐慌') u.morale = clamp(u.morale + dt * 1.4, 0, 100);

  /* ---------- 感知 ---------- */
  u.aiT -= dt;
  if (u.aiT <= 0) { u.aiT = 0.18 + RNG() * 0.2; perceive(u); }
  const enemy = u.seen;

  /* ---------- 延迟指令 ---------- */
  if (u.pendingOrder) {
    u.pendingT -= dt;
    if (u.pendingT <= 0) { applyOrder(u, u.pendingOrder); u.pendingOrder = null; }
  }

  /* ---------- 恐慌 ---------- */
  if (u.morale < 20 && u.state !== '恐慌' && RNG() < 1.2 * dt) { setState(u, '恐慌'); say(u.name, pick(REPLIES.panic), 'no'); }
  if (u.state === '恐慌') {
    if (u.morale > 38) setState(u, '警戒');
    else {
      if (!u.moveGoal || u.arrived) {
        const a = Math.atan2(u.y - POINTS.G.y, u.x - POINTS.G.x) + rr(-0.6, 0.6);
        u.moveGoal = { x: u.x + Math.cos(a) * 160, y: u.y + Math.sin(a) * 160 };
      }
      moveStep(u, dt, 1.35);
      if (enemy && RNG() < 0.4) fireAt(u, enemy, dt);
      return;
    }
  }

  /* ---------- 专属任务：搬运密码箱 / 炸桥 ---------- */
  if (u.boxTask) {
    if (!World.box || World.box.taken) { u.boxTask = false; u.moveGoal = null; }
    else {
      u.moveGoal = { x: World.box.x, y: World.box.y };
      moveStep(u, dt, 1.15);
      if (dist(u.x, u.y, World.box.x, World.box.y) < 26) {
        World.box.taken = true; World.box.carrier = u; u.carryBox = true; u.boxTask = false;
        World.stats.boxTaken = true;
        World.evacArmed = true;                       // 拿到密码箱 → 进入撤离阶段
        say(u.name, '拿到密码箱了！', 'ok'); toast('密码箱已到手，全队准备撤离');
      }
      /* 这里不再 return：取箱的人必须能边走边打。
         早期一取到任务就整段跳过战斗逻辑，结果他端着步枪一言不发地走进车队的火网，
         在离箱子 58px 的地方被车顶机枪打死 —— 箱子就躺在脚边，谁也拿不到。 */
    }
  }
  if (u.bridgeTask) {
    if (!World.bridgeAlive) { u.bridgeTask = false; }
    else {
      u.moveGoal = { x: POINTS.C.x, y: POINTS.C.y + 40 };
      moveStep(u, dt, 1.1);
      if (dist(u.x, u.y, POINTS.C.x, POINTS.C.y) < 70) {
        u.plantT = (u.plantT || 0) + dt;
        u.aiming = true;
        if (u.plantT > 8) {
          World.bridgeAlive = false; World.stats.bridgeBlown = true; u.bridgeTask = false;
          explosion(POINTS.C.x, POINTS.C.y, 120, 60, null, 'bridge');
          updateEvacMarker();
          say(u.name, '桥炸了！改从南侧树林撤离', 'sys');
          toast('桥梁已摧毁 · 撤离点改为 E 南侧树林');
        } else if (Math.floor(u.plantT) !== Math.floor(u.plantT - dt)) say(u.name, '安放炸药 ' + Math.floor(u.plantT) + '/8', 'ok');
      }
      return;
    }
  }
  /* ---------- 救援 ---------- */
  if (u.rescueTarget && (u.rescueTarget.dead || !u.rescueTarget.downed)) { u.rescueTarget = null; u.rescueMode = null; if (u.state === '救援') setState(u, '警戒'); }
  if (u.rescueTarget) {
    const t = u.rescueTarget;
    const d = dist(u.x, u.y, t.x, t.y);
    if (u.rescueMode === 'drag') {
      if (d > 18) { u.moveGoal = { x: t.x, y: t.y }; moveStep(u, dt, 1.1); t.draggedBy = u; return; }
      t.draggedBy = u;
      const c = findCover(t.x, t.y, t.x + 20, t.y + 20, 70);
      if (c) {
        if (!u.dragGoal) u.dragGoal = c;
        u.moveGoal = u.dragGoal; t.moveGoal = u.dragGoal;
        moveStep(u, dt, 0.85);
        t.x = lerp(t.x, u.x - Math.cos(u.facing) * 14, Math.min(1, dt * 3));
        t.y = lerp(t.y, u.y - Math.sin(u.facing) * 14, Math.min(1, dt * 3));
        if (dist(u.x, u.y, c.x, c.y) < 20) {
          u.dragGoal = null; t.draggedBy = null;
          say(u.name, t.name + '拖到掩体后了', 'ok');
          u.rescueMode = null; u.rescueTarget = null; setState(u, '警戒');
        }
      }
      return;
    }
    if (d > 16) { u.moveGoal = { x: t.x, y: t.y }; moveStep(u, dt, 1.2); u.state = '救援'; return; }
    setState(u, '治疗');
    u.aiming = true; t.draggedBy = null;
    t.reviveT += dt / 3.2;
    if (t.reviveT >= 1) {
      t.downed = false; t.hp = 55; t.reviveT = 0; t.suppression = 0; t.state = '警戒';
      say(u.name, t.name + '救回来了', 'ok');
      for (const a of allies()) a.morale = clamp(a.morale + 5, 0, 100);
      u.rescueTarget = null; setState(u, '警戒');
    }
    return;
  }
  /* 治疗轻伤 */
  if (u.healTarget && u.healTarget.hp < u.healTarget.maxHp && !u.healTarget.dead) {
    const t = u.healTarget, d = dist(u.x, u.y, t.x, t.y);
    if (d > 16) { u.moveGoal = { x: t.x, y: t.y }; moveStep(u, dt, 1.15); }
    else {
      t.hp = Math.min(t.maxHp, t.hp + dt * 14); u.aiming = true;
      if (t.hp >= t.maxHp) { say(u.name, t.name + '包扎好了', 'ok'); u.healTarget = null; setState(u, '警戒'); }
    }
    return;
  }
  /* 补给 */
  if (u.resupplyTarget && !u.resupplyTarget.dead) {
    const t = u.resupplyTarget, d = dist(u.x, u.y, t.x, t.y);
    if (d > 18) { u.moveGoal = { x: t.x, y: t.y }; moveStep(u, dt, 1.15); }
    else {
      for (const a of allies()) if (dist(a.x, a.y, u.x, u.y) < 90) {
        a.ammo = a.wpn.ammo; if (a.weaponKey === 'at') a.rockets = ROCKET.ammo; if (a.weaponKey !== 'at') a.grenades = Math.max(a.grenades, 2);
      }
      say(u.name, '弹药补满了', 'ok'); u.resupplyTarget = null; setState(u, '警戒');
    }
    return;
  }

  /* ---------- 医疗兵自动救援 ---------- */
  if (u.role === '医疗兵' && !u.rescueTarget && (!u.order || u.order.action.id === 'cancel')) {
    const d = nearestDowned(u);
    if (d && dist(u.x, u.y, d.x, d.y) < 460) { u.rescueTarget = d; u.rescueMode = null; say(u.name, '我去救人！', 'ok'); }
  }
  /* ---------- 被压制 → 找掩体 ---------- */
  if (u.suppression > 62 && !u.holdPosition && u.state !== '找掩体' && u.state !== '撤退' && u.state !== '撤离') {
    if (u.stateT > 0.8 && RNG() < 2.5 * dt) {
      const t = enemy || { x: u.x, y: u.y + 100 };
      const c = findCover(u.x, u.y, t.x, t.y, 80);
      if (c) { u.coverPos = c; u.moveGoal = c; setState(u, '找掩体'); }
      if (u.speakCd <= 0) { u.speakCd = 6; say(u.name, '我被压住了！', 'no'); }
    }
  }
  /* ---------- 自动占领掩体（无指令时） ---------- */
  if (!u.moveGoal && !u.holdPosition && enemy && u.stateT > 1.2 && !u.coverPos && RNG() < 1.6 * dt) {
    const c = findCover(u.x, u.y, enemy.x, enemy.y, 78);
    if (c) { u.coverPos = c; u.moveGoal = c; if (u.state !== '反坦克') setState(u, '找掩体'); }
  }
  if (u.moveGoal && u.coverPos && dist(u.x, u.y, u.coverPos.x, u.coverPos.y) < 10) u.coverPos = null;

  /* ---------- 通讯：发现敌人就报告 ---------- */
  if (enemy && u.reportCd <= 0 && dist(u.x, u.y, enemy.x, enemy.y) < 420) {
    u.reportCd = rr(9, 18);
    const nm = enemy.spec ? enemy.name : (enemy.officer ? '敌军军官' : enemy.mg ? '机枪手' : '步兵');
    say(u.name, nm + ' 在 ' + bearingName(u, enemy) + '！', 'ok');
  }
  if (enemy) { const a = Math.atan2(enemy.y - u.y, enemy.x - u.x); u.facing = norm(u.facing + angDiff(a, u.facing) * Math.min(1, dt * 7)); u.aiming = true; }
  else u.aiming = false;

  /* ---------- 开火 ---------- */
  const tgt = allyTargetSelect(u);
  u.target = tgt;
  if (tgt) {
    if (u.state === '待命' || u.state === '警戒' || u.state === '部署' || u.state === '找掩体') setState(u, '战斗');
    if (dist(u.x, u.y, tgt.x, tgt.y) < u.wpn.range && !losFire(u.x, u.y, tgt.x, tgt.y)) {
      if (tgt.spec && (u.weaponKey === 'at' || u.hasCharge) && u.rockets > 0) setState(u, '反坦克');
      fireAt(u, tgt, dt);
      if (u.grenades > 0 && !tgt.spec) throwGrenadeAuto(u, tgt);
    }
  } else if (u.state === '战斗' && !enemy) {
    if (u.stateT > 3) setState(u, u.holdPosition ? '待命' : '警戒');
  }
  if (u.grenadeOrder) {
    u.grenadeOrder = false;
    const t = enemy || (World.marker ? World.marker : null);
    if (t && u.grenades > 0 && dist(u.x, u.y, t.x, t.y) < 220) { throwGrenade(u, t.x, t.y, 'grenade'); say(u.name, '手雷！', 'ok'); }
    else say(u.name, '距离太远，扔不到', 'no');
  }
  if (u.smokeOrder) {
    u.smokeOrder = false;
    const t = enemy || (World.marker ? World.marker : null);
    if (t && u.smokes > 0) { throwGrenade(u, t.x, t.y, 'smoke'); say(u.name, '放烟！', 'ok'); }
    else say(u.name, '我没带烟雾弹', 'no');
  }

  /* ---------- 移动 ---------- */
  if (u.formationFollow) {
    const p = World.player;
    const want = nearestCoverSpotNear(p, u);
    if (dist(u.x, u.y, p.x, p.y) > 62 || (u.moving === false && dist(u.x, u.y, p.x, p.y) > 48)) u.moveGoal = want;
    else u.moveGoal = null;
    moveStep(u, dt, 1.05);
  } else if (u.moveGoal) {
    moveStep(u, dt, u.state === '撤离' || u.state === '撤退' ? 1.25 : 1);
    if (u.arrived) {
      u.arrived = false;
      if (u.state === '找掩体') { setState(u, '隐蔽'); if (RNG() < 0.5) say(u.name, '已隐蔽', 'ok'); }
      else if (u.state === '移动') setState(u, '警戒');
    }
  } else {
    // 无指令：轻微移动 / 观察
    if (!enemy && !u.holdPosition && RNG() < 0.4 * dt) {
      const a = RNG() * 6.28;
      u.moveGoal = { x: u.x + Math.cos(a) * rr(20, 50), y: u.y + Math.sin(a) * rr(20, 50) };
    }
    if (u.state === '部署' && World.t > CFG.tDeploy) setState(u, '待命');
  }

  /* ---------- 密码箱：从旁边路过的队员顺手就捡 ---------- */
  if (!u.boxTask && World.box && !World.box.taken && dist(u.x, u.y, World.box.x, World.box.y) < 22) {
    World.box.taken = true; World.box.carrier = u; u.carryBox = true;
    World.stats.boxTaken = true; World.evacArmed = true;
    say(u.name, '密码箱到手！', 'ok'); toast('密码箱已到手 · 全队撤离');
  }
  /* ---------- 撤离点判定 ---------- */
  /* 只有进入「撤离阶段」（拿到密码箱 / 收到撤退、撤离命令）后才开始计数。
     早期不加这个门，而 C 点恰好也是部署区之一 —— 开局站在那儿的队员会立刻被算成「已撤离」，
     既虚增撤离人数，也能直接凑够 6 人误判为成功。 */
  /* 「撤离」必须是活着的人走到撤离点：早期没有排除阵亡/失能者，
     而倒地的人常常就倒在撤离点附近 —— 于是「小队全灭」也能凑出 evacCount>=6，
     与 checkEnd 里 allIn 的判定（只数活人）自相矛盾，还能凭空凑齐胜利条件。 */
  if (!u.dead && !u.downed && dist(u.x, u.y, World.evac.x, World.evac.y) < 95 && !u.evacuated && World.evacArmed) {
    u.evacuated = true; World.stats.evacCount++;
    say(u.name, '已到撤离点！', 'ok');
    checkEnd();
  }
  /* 带密码箱撤离（同样只有活着走到撤离点才算） */
  if (u.carryBox && u.evacuated && !u.dead && !u.downed) { World.stats.boxEvacuated = true; }
}
function nearestCoverSpotNear(p, u) {
  const a = Math.atan2(u.y - p.y, u.x - p.x) || RNG() * 6.28;
  for (const off of [0, 0.5, -0.5, 1.0, -1.0, 1.6, -1.6, 2.4, -2.4, 3.14]) {
    const ang = a + off;
    const x = p.x + Math.cos(ang) * 52, y = p.y + Math.sin(ang) * 52;
    if (passable(x, y) && !propBlocked(x, y, 6)) return { x, y };
  }
  return { x: p.x + rr(-40, 40), y: p.y + rr(-40, 40) };
}

/* =========================================================================
   六、玩家
   ========================================================================= */
function updatePlayer(u, dt) {
  if (u.dead) return;
  u.hitFlash = Math.max(0, u.hitFlash - dt);
  u.suppression = Math.max(0, u.suppression - dt * 11);
  u.stateT += dt;
  if (u.fireCd > 0) u.fireCd -= dt;
  if (u.reloadT > 0) { u.reloadT -= dt; if (u.reloadT <= 0) finishReload(u); }
  if (u.downed) {
    u.downTimer -= dt;
    if (u.downTimer <= 0) { killUnit(u, null); }
    else if (u.downTimer < 12 && u.speakCd <= 0) { u.speakCd = 7; say('你', '我快不行了……', 'no'); }
    else if (World.handover) { /* 副队长会来救 */ }
    const med = World.units.find(x => x.role === '医疗兵' && !x.dead && !x.downed);
    if (med && !med.rescueTarget) { med.rescueTarget = u; med.rescueMode = null; say(med.name, '我来了队长！', 'ok'); }
    return;
  }
  if (World.handover && World.handover.rescueTarget === u) { }
  /* 移动：第一人称下 WASD 相对视角 —— W 沿视线前进、A/D 横向平移、S 后退。
     逻辑层不依赖渲染层：只有 Input 与 World.viewPitch 两个输入源，
     所以在无 DOM 的离线仿真里（Cam 不存在）也能照常跑。 */
  let fwd = 0, str = 0;
  if (Input.key['w'] || Input.key['arrowup']) fwd += 1;
  if (Input.key['s'] || Input.key['arrowdown']) fwd -= 1;
  if (Input.key['a'] || Input.key['arrowleft']) str -= 1;
  if (Input.key['d'] || Input.key['arrowright']) str += 1;
  if (fwd || str) {
    const cy = Math.cos(u.facing), sy = Math.sin(u.facing);
    let mx = cy * fwd - sy * str, my = sy * fwd + cy * str;
    const L = Math.hypot(mx, my) || 1; mx /= L; my /= L;
    let sp = 88 * (Input.key['shift'] ? 1.35 : 1);
    if (Input.key['control'] || Input.key['c']) sp *= 0.52;      // 蹲下
    if (u.reloadT > 0) sp *= 0.85;
    if (inRiver(u.x, u.y) && !onBridge(u.x, u.y)) sp *= 0.55;
    if (u.suppression > 60) sp *= 0.85;
    const nx = u.x + mx * sp * dt, ny = u.y + my * sp * dt;
    if (passable(nx, u.y) && !propBlocked(nx, u.y, 6)) u.x = nx;
    if (passable(u.x, ny) && !propBlocked(u.x, ny, 6)) u.y = ny;
    u.moving = true;
  } else u.moving = false;
  /* 朝向：第一人称下由视角决定（渲染层每帧把 yaw 写进 u.facing）；
     俯视/离线仿真时回退到鼠标世界坐标。 */
  const m = Input.mouseWorld;
  if (m && !Input.fpsAim) u.facing = Math.atan2(m.y - u.y, m.x - u.x);
  /* 射击 */
  if (Input.mouseDown && u.reloadT <= 0 && u.fireCd <= 0) {
    if (u.magAmmo <= 0) { tryReload(u); }
    else {
      const w = u.wpn;
      u.fireCd = w.rof;
      const tgt = { x: u.x + Math.cos(u.facing) * 400, y: u.y + Math.sin(u.facing) * 400 };
      /* 第一人称：准星抬离水平面越多，子弹越打不到地面目标（射击高处/天空基本落空）。
         逻辑层的弹道始终躺在水平面上，所以这里用「有效射程」表达俯仰带来的偏差。 */
      const pm = World.viewPitch || 0;
      const rangeMul = clamp(1 - Math.abs(pm) / 0.24, 0.04, 1);
      spawnBullet(u, tgt, spreadMul(u) * 0.85, { rangeMul: rangeMul });
      if (!World.triggered) triggerAmbush('player');
    }
  }
  if (u.magAmmo <= 0 && u.reloadT <= 0) tryReload(u);
  if (Input.grenade) { Input.grenade = false; if (u.grenades > 0) { throwGrenade(u, u.x + Math.cos(u.facing) * 120, u.y + Math.sin(u.facing) * 120, 'grenade'); } }
  /* 拾取密码箱 */
  if (World.box && !World.box.taken && dist(u.x, u.y, World.box.x, World.box.y) < 22) {
    World.box.taken = true; World.box.carrier = u; u.carryBox = true;
    World.stats.boxTaken = true;
    World.evacArmed = true;                       // 拿到密码箱 → 进入撤离阶段
    sfx('pickup', null, null, { local: true });
    say('你', '密码箱到手！', 'ok'); toast('密码箱已到手 · 全队撤离');
  }
  /* 撤离（同样要等撤离阶段开始才计数；倒地/阵亡不算撤离人数） */
  if (!u.dead && !u.downed && dist(u.x, u.y, World.evac.x, World.evac.y) < 95 && !u.evacuated && World.evacArmed) {
    u.evacuated = true; World.stats.evacCount++;
    sfx('evac', null, null, { local: true });
    say('你', '我到撤离点了', 'ok'); checkEnd();
  }
}

/* =========================================================================
   七、敌方部队
   ========================================================================= */
function allyCentroid() {
  let x = 0, y = 0, n = 0;
  for (const u of World.units) { if (u.team === 'ally' && !u.dead && !u.downed) { x += u.x; y += u.y; n++; } }
  return n ? { x: x / n, y: y / n } : { x: POINTS.A.x, y: POINTS.A.y };
}
function dismount(v) {
  if (v.dismounted) return;
  v.dismounted = true;
  const n = v.troopPlan || 0;
  for (let i = 0; i < n; i++) {
    const side = RNG() < 0.5 ? 1 : -1;
    const a = Math.PI / 2 * side + rr(-0.5, 0.5);
    const isOfficer = (World.officerVeh === v) && !World.officerSpawned && i === 0;
    if (isOfficer) World.officerSpawned = true;
    const def = {
      id: 'e' + v.id + '_' + i, name: isOfficer ? '敌军军官' : (i % 5 === 0 ? '敌机枪手' : '敌步兵'),
      role: '步兵', group: '', weapon: (i % 5 === 0) ? 'emg' : 'erifle', officer: isOfficer, mg: (i % 5 === 0),
    };
    const e = makeUnit(def, v.x + Math.cos(a) * rr(16, 42), v.y + Math.sin(a) * rr(16, 42), 'enemy');
    e.hp = isOfficer ? 60 : (e.mg ? 42 : 34); e.maxHp = e.hp;
    e.morale = 72;
    e.flankRole = RNG() < 0.38 ? 'flank' : 'front';
    e.flankSide = RNG() < 0.5 ? 1 : -1;
    e.mount = null;
    World.units.push(e);
  }
  say('全体', v.name + ' 上步兵下车了！', 'no');
}
function updateVehicles(dt) {
  World.convoyProgress = World.convoyProgress || 0;
  for (let i = 0; i < World.vehicles.length; i++) {
    const v = World.vehicles[i];
    v.hitFlash = Math.max(0, v.hitFlash - dt * 3);
    if (v.destroyed) { v.burning = Math.max(0, v.burning - dt); if (v.fireFxT === undefined) v.fireFxT = 0; v.fireFxT -= dt; if (v.fireFxT <= 0) { v.fireFxT = 0.3; fx({ type: 'fire', x: v.x + rr(-12, 12), y: v.y + rr(-8, 8), life: 0.7, r: rr(8, 16) }); } continue; }
    /* 前方被毁车辆：向它的车尾靠拢后再停。
       早期写法是「前方有残骸就立即 speed=0」，结果整列车队被冻结在各自的初始位置、
       在 1300px 的路面上拉成一条长蛇，伏击圈外的车根本进不了交战距离。 */
    let wreck = null;
    for (let j = i - 1; j >= 0; j--) {
      const o = World.vehicles[j];
      if (o.destroyed && (!!o.isReinforcement) === (!!v.isReinforcement)) { wreck = o; break; }
    }
    if (wreck) v.blockedT = (v.blockedT || 0) + dt;
    let speed = 0;
    if (wreck && dist(v.x, v.y, wreck.x, wreck.y) <= 52) {
      speed = 0;
      if (!v.dismounted && v.type !== 'tank' && v.blockedT > 1.5) dismount(v);
    }
    else if (v.isReinforcement) {
      /* 增援车队：一路开到伏击圈再下车 */
      v.stopT += dt;
      speed = 26;
      if (!v.dismounted && (v.x < 1500 || v.stopT > 30)) dismount(v);
    } else if (!World.convoyStarted) {
      speed = 0;
    } else if (!World.triggered) {
      speed = 26;
    } else if (v.x > CFG.convoyStopX + i * CFG.convoyGap && !v.atZone) {
      /* 按车序排队停车：第 i 辆车停在自己的位置上，避免全部挤到同一个 x 重叠成一堆 */
      speed = 26;
    } else {
      v.atZone = true;
      v.stopT += dt;
      speed = v.stopT > 110 ? v.spec.speed * 0.85 : 0;   // 久攻不下 → 试图冲出西侧
      if (!v.dismounted && v.type !== 'tank' && speed === 0) {
        if (v.blockedT > 2.5 || v.stopT > 5.5) dismount(v);
      }
    }
    if (speed > 0) {
      const nd = v.dist + speed * dt;
      const p = wayPointAt(nd);
      const crossBlocked = !World.bridgeAlive && v.x >= CFG.riverX2 && p.x < CFG.riverX2;
      if (!crossBlocked) v.dist = nd;
      else v.atBridge = true;
      v.speed = speed;
    } else v.speed = 0;
    const p = wayPointAt(v.dist);
    v.px = v.x; v.py = v.y; v.x = p.x; v.y = p.y;
    v.angle = p.a;

    /* 触发遇袭：停车、混乱 */
    if (World.triggered && !v.isReinforcement && v.stopT > 0 && v.stopT < 0.2 && !v.chaosSaid) { v.chaosSaid = true; }
    /* 车载武器 */
    const mg = v.spec.mg;
    if (mg && World.triggered) {
      v.wpnCd -= dt;
      if (v.wpnCd <= 0) {
        const t = nearestEnemyAt(v.x, v.y, 'enemy', u => !u.dead && !u.downed && !u.spec);
        if (t && dist(v.x, v.y, t.x, t.y) < mg.range && !losFire(v.x, v.y, t.x, t.y)) {
          spawnTankMG(v, t); v.wpnCd = mg.rof * ri(2, 5);
        } else v.wpnCd = 0.6;
      }
    }
    /* 坦克主炮 */
    if (v.spec.cannon && !v.destroyed) {
      v.fireCd -= dt;
      if (v.aimT > 0) {
        v.aimT -= dt;
        v.turret = norm(v.turret + angDiff(Math.atan2(v.aimPt.y - v.y, v.aimPt.x - v.x), v.turret) * Math.min(1, dt * 2.2));
        if (v.aimT <= 0) {
          spawnShell(v, v.aimPt.x, v.aimPt.y);
          v.fireCd = SHELL.rof * rr(0.85, 1.2);
          v.aimPt = null;
        }
      } else if (v.fireCd <= 0 && World.triggered) {
        // 找我方最密集位置
        let best = null, bn = 1;
        for (const u of World.units) {
          if (u.team !== 'ally' || u.dead || u.downed) continue;
          if (dist(v.x, v.y, u.x, u.y) > 900) continue;
          if (losFire(v.x, v.y, u.x, u.y)) continue;
          let n = 0;
          for (const o of World.units) if (o.team === 'ally' && !o.dead && dist(o.x, o.y, u.x, u.y) < 70) n++;
          if (n > bn) { bn = n; best = u; }
        }
        if (best) {
          v.aimPt = { x: best.x + rr(-24, 24), y: best.y + rr(-24, 24) };
          v.aimT = SHELL.warn;
          setAlert('炮击预警！' + (best.isPlayer ? '目标是你' : '目标：' + best.name) + ' — 全体隐蔽/转移', SHELL.warn);
          say('全体', '坦克炮口瞄准了我们的阵地！', 'no');
          World.stats.tankShells = (World.stats.tankShells || 0) + 1;
        } else v.fireCd = 1.5;
      }
    }
    /* 增援/逃跑判定 */
    if (!v.destroyed && v.x < 130) {
      const boxVeh = v.hasBox;
      if (v.type === 'tank' || (boxVeh && !World.stats.boxTaken)) {
        World.convoyEscaped = true;
        endGame('失败', '车队冲过了西侧出口，伏击失败。');
      }
    }
  }
}
function updateEnemy(e, dt) {
  if (e.dead) return;
  e.stateT += dt;
  e.hitFlash = Math.max(0, e.hitFlash - dt * 3);
  if (e.downed) { e.downTimer -= dt; if (e.downTimer <= 0) killUnit(e, null); return; }
  if (e.mount) return;
  e.fireCd -= dt; e.aiT -= dt; e.reportCd -= dt;
  if (e.reloadT > 0) { e.reloadT -= dt; if (e.reloadT <= 0) finishReload(e); }
  e.suppression = Math.max(0, e.suppression - dt * 9);
  if (e.aiT <= 0) { e.aiT = 0.22 + RNG() * 0.25; perceive(e); }

  /* 士气崩溃 → 后撤 */
  if (e.morale < 18 && e.state !== '撤退') { setState(e, '撤退'); e.moraleCheck = true; }
  if (e.state === '撤退') {
    if (!e.moveGoal || e.arrived) {
      const a = Math.atan2(650 - e.y, 100 - e.x) + rr(-0.5, 0.5);
      e.moveGoal = { x: e.x + Math.cos(a) * 170, y: e.y + Math.sin(a) * 170 };
    }
    moveStep(e, dt, 1.25);
    if (e.seen && RNG() < 0.5) fireAt(e, e.seen, dt);
    return;
  }
  const tgt = e.seen;
  e.target = tgt;
  if (tgt) { const a = Math.atan2(tgt.y - e.y, tgt.x - e.x); e.facing = norm(e.facing + angDiff(a, e.facing) * Math.min(1, dt * 6)); }
  const cen = allyCentroid();
  e.pushT = (e.pushT || 0) - dt;
  e.holdT = Math.max(0, (e.holdT || 0) - dt);   // 推进节拍：跃进 → 依托掩体停火 → 再跃进
  const nearby = tgt && dist(e.x, e.y, tgt.x, tgt.y) < 230;
  if (e.pushT <= 0) {
    e.pushT = rr(4, 8);
    if (e.flankRole === 'flank') {
      const ang = Math.atan2(cen.y - e.y, cen.x - e.x);
      e.pushPt = { x: cen.x + Math.cos(ang + e.flankSide * 0.95) * 240, y: cen.y + Math.sin(ang + e.flankSide * 0.95) * 240 };
    } else {
      e.pushPt = { x: cen.x + rr(-70, 70), y: cen.y + rr(-70, 70) };
    }
  }
  if (e.suppression > 65 && !e.coverPos && RNG() < 1.5 * dt) {
    const c = findCover(e.x, e.y, tgt ? tgt.x : e.x, tgt ? tgt.y : e.y + 60, 80);
    if (c) { e.coverPos = c; e.moveGoal = c; }
  }
  if (e.moveGoal) {
    moveStep(e, dt, 1);
    if (e.arrived) { e.arrived = false; e.holdT = rr(1.2, 3.2); }
  } else if (e.holdT > 0) {
    /* 依托掩体停火：本拍不选新路线，保持火力输出 */
  } else if (nearby) {
    e.holdT = rr(1.5, 4);                       // 已在交火距离内 → 停下来对射
  } else if (e.pushPt && dist(e.x, e.y, e.pushPt.x, e.pushPt.y) > 45) {
    e.moveGoal = e.pushPt;
  } else if (tgt && dist(e.x, e.y, tgt.x, tgt.y) > 250) {
    /* 已到集结点但目标仍远 → 直接朝目标做一次跃进（否则会在集结点无限期停住） */
    e.moveGoal = { x: tgt.x + rr(-40, 40), y: tgt.y + rr(-40, 40) };
  } else {
    e.holdT = rr(2, 5);
  }
  if (tgt && !e.mount) {
    if (e.state === '警戒') setState(e, '战斗');
    fireAt(e, tgt, dt);
  } else if (e.state === '战斗') setState(e, '警戒');
}
function spawnReinforcement() {
  World.reinforceDone = true; World.stats.reinforceTriggered = true;
  sfx('reinforce', null, null, { local: true });   // 远处引擎轰鸣 + 电台告警
  say('全体', '东侧出现敌方增援！两卡车步兵！', 'no');
  setAlert('敌军增援到达 — 全体立即撤退！', 6);
  toast('敌方增援到达：2 卡车步兵');
  for (let k = 0; k < 2; k++) {
    const v = {
      id: 'rein' + k, type: 'truck', name: '增援卡车' + (k + 1), team: 'enemy',
      x: 2320 + k * 140, y: 700, angle: Math.PI, speed: 26, baseSpeed: 26,
      hp: VEHICLES.truck.hp, maxHp: VEHICLES.truck.hp, spec: VEHICLES.truck,
      len: 66, wid: 30, armor: 0.34, destroyed: false, burning: 0,
      turret: Math.PI, fireCd: 99, wpnCd: 2, troops: [], capacity: 6, dismountT: -1, dismounted: false,
      state: 'drive', target: null, line: 1, speedMul: 1, dist: -k * 140, hasBox: false, hitFlash: 0, stopT: 0,
      troopPlan: 6, isReinforcement: true, blockedT: 0,
    };
    World.vehicles.push(v);
  }
  World.barrage = true; World.barrageCd = 8;
  for (const a of allies()) a.morale = clamp(a.morale - 10, 0, 100);
}
function updateBarrage(dt) {
  if (!World.barrage || World.over) return;
  World.barrageCd -= dt;
  if (World.barrageCd > 0) return;
  World.barrageCd = rr(14, 22);
  const alive = allies().filter(u => !u.downed);
  if (!alive.length) return;
  let cx = 0, cy = 0;
  for (const u of alive) { cx += u.x; cy += u.y; }
  cx /= alive.length; cy /= alive.length;
  const tx = cx + rr(-90, 90), ty = cy + rr(-90, 90);
  World.pendingShells = World.pendingShells || [];
  World.pendingShells.push({ x: tx, y: ty, t: 2.2 });
  /* 呼啸声与 2.2s 的落弹倒计时同步：先听见炮弹从头顶压下来，再听见爆炸 */
  sfx('incoming', tx, ty, { gap: 0 });
  setAlert('远程炮击来袭 — 全体隐蔽/散开！', 2.2);
  say('全体', '听！是炮击，要落我们头上！', 'no');
}
function updatePendingShells(dt) {
  if (!World.pendingShells) return;
  for (let i = World.pendingShells.length - 1; i >= 0; i--) {
    const s = World.pendingShells[i];
    s.t -= dt;
    if (s.t <= 0) { explosion(s.x, s.y, 96, 120, 'enemy', 'shell'); World.pendingShells.splice(i, 1); }
  }
}

/* =========================================================================
   八、关卡流程 / 目标 / 结算
   ========================================================================= */
function triggerAmbush(src) {
  if (World.triggered || World.over) return;
  World.triggered = true; World.triggerT = World.t;
  for (const m of World.mines) m.armed = true;
  World.stats.triggerSrc = src;
  sfx('ambush', null, null, { local: true });     // 伏击开始的号角 sting
  setAlert('伏击开始！', 2.5);
  say('全体', src === 'player' ? '队长开火了，打！' : '起爆！', 'sys');
  World.reinforceT = Math.max(World.reinforceT, World.t + 150);
}
function updateMines(dt) {
  for (const m of World.mines) {
    if (m.used || !m.armed) continue;
    for (const v of World.vehicles) {
      if (v.destroyed || v.team !== 'enemy') continue;
      if (dist(v.x, v.y, m.x, m.y) < 34) {
        m.used = true; World.stats.minesUsed++;
        explosion(m.x, m.y, 74, 60, 'ally', 'mine');
        damageVehicle(v, 260, 'mine', World.units.find(x => x.id === 'laobai') || World.player);
        if (World.box && !World.box.taken && v.hasBox) dropBox(v.x, v.y);
        break;
      }
    }
  }
}
function checkObjectives() {
  const s = World.stats;
  const o = [
    { t: '摧毁坦克', done: s.tankKilled, main: true },
    { t: '获取密码箱', done: s.boxTaken, main: true },
    { t: '至少 6 人撤离到 ' + World.evac.name, done: s.evacCount >= 6, main: true, extra: '(' + s.evacCount + '/6)' },
    { t: '摧毁 2 辆装甲车', done: s.apcKilled >= 2, main: false, extra: '(' + s.apcKilled + '/2)' },
    { t: '救回所有伤员（无阵亡/无遗留）', done: !s.allyDead && downedAllies().length === 0 && World.t > 60, main: false },
    { t: '不触发敌方增援', done: !s.reinforceTriggered, main: false },
    { t: '击毙车队指挥官', done: s.officerKilled, main: false },
  ];
  World.objState = o;
  return o;
}
function checkEnd() {
  if (World.over) return;
  const s = World.stats;
  if (s.tankKilled && s.boxTaken && s.evacCount >= 6 && !World.convoyEscaped) {
    endGame('成功', '伏击成功：坦克被摧毁，密码箱到手，' + s.evacCount + ' 人从 ' + World.evac.name + ' 撤离。');
    return;
  }
  const up = World.units.filter(u => u.team === 'ally' && !u.dead && !u.downed);
  const allIn = up.length > 0 && up.every(u => u.evacuated);
  if (World.triggered && World.evacArmed && allIn && s.evacCount < 6) {
    endGame('失败', '撤离人数不足 6 人（实际 ' + s.evacCount + ' 人），任务失败。');
  }
}
function updatePhaseName() {
  const t = World.t;
  let n;
  if (!World.started) n = '情报';
  else if (t < CFG.tIntel) n = '情报';
  else if (t < CFG.tDeploy && !World.deployDone) n = '部署';
  else if (!World.triggered) n = '潜伏';
  else if (t - World.triggerT < 40) n = '爆发';
  else if (t - World.triggerT < 130) n = '混战';
  else n = '清剿';
  if (World.stats.boxTaken && World.evacCount < 6) n = '撤离';
  if (t >= World.reinforceT && World.reinforceDone) n = '紧急撤离';
  World.phaseName = n;
}
function updateFlow(dt) {
  World.t += dt;
  if (!World.convoyStarted && World.t >= CFG.convoyIn) {
    World.convoyStarted = true;
    say('全体', '车队来了，沉住气，等我的口令！', 'sys');
  }
  if (World.triggered && World.t - World.triggerT > 3 && !World.mineWarned) {
    World.mineWarned = true;
  }
  if (!World.reinforceDone && World.t >= World.reinforceT && World.triggered) spawnReinforcement();
  if (World.t >= CFG.missionEnd && !World.over) {
    endGame('失败', '超过 10 分钟时限，任务失败。');
  }
  checkObjectives();
  if (World.t > 20) checkEnd();
  updateBarrage(dt);
  updatePendingShells(dt);
}
function endGame(kind, text) {
  if (World.over) return;
  World.over = true; World.overKind = kind; World.overText = text;
  showEndScreen(kind, text);
}
function scoreMission() {
  const s = World.stats;
  const win = World.overKind === '成功';
  const dead = s.allyDead || 0;
  const cas = dead + downedAllies().length;
  const finishT = World.t;
  let g = '失败';
  if (win) {
    if (cas <= 2 && finishT <= 480 && !s.reinforceTriggered && s.apcKilled >= 2) g = 'S';
    else if (cas <= 4) g = 'A';
    else if (cas <= 6) g = 'B';
    else g = 'C';
  }
  return { g, cas, dead, win };
}

/* =========================================================================
   八·五、音效层 —— 程序化音频引擎（零外部资源）

   为什么是「现场合成」而不是加载音频文件：
     本项目的硬约束是「单文件、零依赖、离线可用」，所以音效系统里没有任何
     wav/mp3/ogg —— 枪声、爆炸、无线电全部由振荡器 + 噪声 buffer 现场合成。

   三条设计线：

   1) 逻辑层与音频层解耦
      战斗代码只调用 sfx(id, x, y, opt)，不关心音频是否可用，也绝不等音频返回。
      AudioContext 在逻辑层是「允许缺席」的（Node 离线仿真 / 浏览器不支持 WebAudio /
      玩家还没点过页面 / 音频被禁用）—— 这些情况下 sfx 自动退化为空操作。
      这与本项目「渲染层删掉，战斗逻辑照样能在 Node 里跑完整局」是同一条原则。

   2) 空间化就是玩法
      这一关的乐趣之一是「听见车队从东边压过来、靠枪声听出队友在哪个方向开火」。
      所以每个非 local 音效都按听者（玩家）实时算三件事：
        · 距离衰减     gain = 1 / (1 + (d/ref)^1.25)
        · 空气吸收     低通截止频率随距离下降（远处枪声发「闷」）
        · 左右声像     以玩家朝向为基准的立体声 pan
      玩家自己发出的声音（开枪 / 换弹）走 local 通道居中播放，符合射击游戏直觉。

   3) 必须限流
      一场遭遇战里十几把枪同时响（机枪 0.09s 一发）。每种音效有最短触发间隔，
      另加全局「100ms 内最多 26 个新声源」的预算 —— 否则既糊成一片，又要创建
      上千个 WebAudio 节点。

   注意：音频层一律使用 Math.random()，绝不触碰 RNG()/rr() —— 那是有种子的战斗随机流，
   借它一次就会让「浏览器实机」与「Node 离线仿真」的随机序列错开。
   ========================================================================= */

/* ------------------------------------------------------- 音效空间参数表 */
/* ref：衰减参考距离（约衰减到一半）· max：最远可闻距离 · g：基础增益
   local：不参与空间化（玩家自己的声音 / UI / 无线电，居中播放）
   duck：爆炸类音效的压限强度（0~1.4）· ring：是否附带耳鸣 */
const SND = {
  rifle: { ref: 240, max: 2000, g: 0.50 },
  mg: { ref: 300, max: 2200, g: 0.52 },
  sniper: { ref: 440, max: 3200, g: 0.62 },
  enemyRifle: { ref: 250, max: 1800, g: 0.38 },
  enemyMG: { ref: 320, max: 2000, g: 0.40 },
  crack: { ref: 110, max: 460, g: 0.42 },          // 子弹从耳边掠过
  impact: { ref: 170, max: 950, g: 0.30 },          // 子弹啃到岩石/泥土
  clang: { ref: 240, max: 1300, g: 0.34 },          // 打在车体装甲上
  flesh: { ref: 130, max: 700, g: 0.26 },
  body: { ref: 150, max: 800, g: 0.34 },            // 人倒地
  allyDown: { ref: 210, max: 1300, g: 0.40 },
  hit: { local: true, g: 0.30 },                    // 命中反馈（准星）
  metal: { ref: 280, max: 1500, g: 0.34 },          // 车辆残骸金属声
  explosion: { ref: 620, max: 3400, g: 0.95, duck: 1.0, ring: 1 },
  mine: { ref: 560, max: 3000, g: 0.90, duck: 0.9, ring: 1 },
  barrel: { ref: 430, max: 2400, g: 0.78, duck: 0.7, ring: 1 },
  grenade: { ref: 400, max: 2200, g: 0.72, duck: 0.75, ring: 1 },
  rocketBoom: { ref: 520, max: 2800, g: 0.85, duck: 0.9, ring: 1 },
  shellBoom: { ref: 640, max: 3600, g: 1.00, duck: 1.0, ring: 1 },
  vehicleBoom: { ref: 700, max: 3800, g: 1.00, duck: 1.1, ring: 1 },
  cannon: { ref: 720, max: 4200, g: 1.00, duck: 1.1, ring: 1 },
  rocketFire: { ref: 300, max: 1900, g: 0.60 },
  grenadeThrow: { ref: 200, max: 1000, g: 0.34 },
  incoming: { ref: 900, max: 6000, g: 0.52 },       // 炮弹下落呼啸
  reloadStart: { ref: 90, max: 400, g: 0.30 },
  reloadEnd: { ref: 90, max: 400, g: 0.32 },
  boxDrop: { ref: 230, max: 1300, g: 0.34 },
  pickup: { local: true, g: 0.42 },
  evac: { local: true, g: 0.38 },
  radioTx: { local: true, g: 0.15 },
  radioRx: { local: true, g: 0.13 },
  alert: { local: true, g: 0.26 },
  orderSend: { local: true, g: 0.30 },
  orderFail: { local: true, g: 0.30 },
  ui: { local: true, g: 0.15 },
  micOn: { local: true, g: 0.24 },
  micOff: { local: true, g: 0.20 },
  ambush: { local: true, g: 0.55 },
  hurt: { local: true, g: 0.34 },
  down: { local: true, g: 0.45 },
  reinforce: { local: true, g: 0.40 },
  begin: { local: true, g: 0.40 },
  win: { local: true, g: 0.50 },
  lose: { local: true, g: 0.50 },
};
/* 每种音效的最短触发间隔（秒）：同一瞬间大量同类事件（全队机枪齐射）会被合并 */
const SND_GAP = {
  rifle: 0.024, mg: 0.026, enemyRifle: 0.030, enemyMG: 0.032, sniper: 0.05,
  crack: 0.05, impact: 0.03, clang: 0.03, flesh: 0.045, body: 0.06, allyDown: 0.10,
  hit: 0.04, radioRx: 0.26, radioTx: 0.22, ui: 0.05, hurt: 0.18, alert: 0.9,
  reloadStart: 0.06, reloadEnd: 0.06, pickup: 0.3, evac: 0.3,
};

const Sound = {
  ctx: null, ok: false, failed: false, pan: false,
  vol: 0.62, muted: false,
  bus: null, comp: null, duck: null, master: null, amb: null, ringBus: null,
  noise: null,
  last: {}, budget: [],
  engines: [],
  ambNodes: null, cricketT: 0, needAmb: false,
};

/* ------------------------------------------------- 惰性初始化（必须由用户手势触发） */
function noiseBuffer() {
  if (Sound.noise) return Sound.noise;
  const ctx = Sound.ctx;
  const n = Math.floor(ctx.sampleRate * 2);
  const b = ctx.createBuffer(1, n, ctx.sampleRate);
  const d = b.getChannelData(0);
  for (let i = 0; i < n; i++) d[i] = Math.random() * 2 - 1;
  Sound.noise = b;
  return b;
}
function soundPrefLoad() {
  try {
    const raw = window.localStorage.getItem('va.sound');
    if (!raw) return;
    const j = JSON.parse(raw);
    if (typeof j.v === 'number') Sound.vol = clamp(j.v, 0, 1);
    Sound.muted = !!j.m;
  } catch (e) { /* 隐私模式下 localStorage 会抛异常，忽略 */ }
}
function soundPrefSave() {
  try { window.localStorage.setItem('va.sound', JSON.stringify({ v: Sound.vol, m: Sound.muted })); } catch (e) { }
}
/* 顶栏音量控件与 Sound 状态双向同步（DOM 可能尚未就绪，全部做空值保护） */
function syncSoundUI() {
  const r = (typeof $ === 'function') && $('volRange');
  if (r) { r.value = Math.round(Sound.vol * 100); r.disabled = false; }
  const b = (typeof $ === 'function') && $('btnMute');
  if (b) b.textContent = Sound.muted ? '静音' : '音效 ON';
  const w = (typeof $ === 'function') && $('mSoundWrap');
  if (w) w.style.opacity = Sound.muted ? '0.55' : '1';
}
/* 直接作用于 master 增益：改音量/静音不必等下一帧 */
function soundApplyGain() {
  if (!Sound.ok || !Sound.master) return;
  try { Sound.master.gain.setTargetAtTime(Sound.muted ? 0 : Sound.vol, _now(), 0.03); } catch (e) { }
}
function initAudio() {
  if (Sound.ctx || Sound.failed) return Sound.ctx;
  try {
    const AC = (typeof window !== 'undefined') && (window.AudioContext || window.webkitAudioContext);
    if (!AC) { Sound.failed = true; return null; }
    const ctx = new AC();
    Sound.ctx = ctx;
    Sound.pan = typeof ctx.createStereoPanner === 'function';
    Sound.bus = ctx.createGain(); Sound.bus.gain.value = 1;
    Sound.duck = ctx.createGain(); Sound.duck.gain.value = 1;
    Sound.comp = ctx.createDynamicsCompressor();
    Sound.comp.threshold.value = -10; Sound.comp.knee.value = 22;
    Sound.comp.ratio.value = 12; Sound.comp.attack.value = 0.003; Sound.comp.release.value = 0.25;
    Sound.master = ctx.createGain();
    Sound.ringBus = ctx.createGain(); Sound.ringBus.gain.value = 1;
    Sound.amb = ctx.createGain(); Sound.amb.gain.value = 0.0001;
    soundPrefLoad();
    Sound.master.gain.value = Sound.muted ? 0 : Sound.vol;
    Sound.bus.connect(Sound.duck); Sound.duck.connect(Sound.comp); Sound.comp.connect(Sound.master);
    Sound.master.connect(ctx.destination);
    Sound.amb.connect(Sound.duck);
    Sound.ringBus.connect(Sound.master);              // 耳鸣不被爆炸压限吃掉
    Sound.ok = true;
    if (ctx.state === 'suspended') { try { ctx.resume(); } catch (e) { } }
    return ctx;
  } catch (e) {
    Sound.failed = true; Sound.ok = false; Sound.ctx = null;
    return null;
  }
}
/* 由用户手势（点「开始任务」）调用一次：建 AudioContext、铺环境音底噪。
   浏览器的自动播放策略要求音频上下文必须诞生在用户手势里，所以不能放页面 onload。 */
function bootAudio() {
  if (!initAudio()) return false;
  startAmbient();
  setAmbientWeather();
  syncSoundUI();
  return true;
}

/* ------------------------------------------------------------ 合成构件 */
function _now() { return Sound.ctx.currentTime; }
function _osc(type, t0, f0, f1, dur) {
  const o = Sound.ctx.createOscillator();
  o.type = type;
  o.frequency.setValueAtTime(Math.max(1, f0), t0);
  if (f1 && f1 !== f0) o.frequency.exponentialRampToValueAtTime(Math.max(1, f1), t0 + dur);
  o.start(t0); o.stop(t0 + dur + 0.03);
  return o;
}
function _nz(t0, dur, rate) {
  const s = Sound.ctx.createBufferSource();
  s.buffer = noiseBuffer(); s.loop = true;
  s.playbackRate.value = rate || 1;
  s.start(t0, Math.random() * 1.5, dur + 0.03);
  return s;
}
function _loop(rate) {
  const s = Sound.ctx.createBufferSource();
  s.buffer = noiseBuffer(); s.loop = true;
  s.playbackRate.value = rate || 1;
  s.start(0, Math.random() * 1.5);
  return s;
}
function _env(peak, t0, atk, dec) {
  const g = Sound.ctx.createGain();
  g.gain.setValueAtTime(0.0001, t0);
  g.gain.linearRampToValueAtTime(Math.max(0.0001, peak), t0 + atk);
  g.gain.exponentialRampToValueAtTime(0.0001, t0 + atk + dec);
  return g;
}
function _bq(type, f, q) {
  const b = Sound.ctx.createBiquadFilter();
  b.type = type; b.frequency.value = Math.max(20, f); b.Q.value = q == null ? 1 : q;
  return b;
}

/* --------------------------------------------------------------- 音色 */
/* 枪声：三段式 —— 枪口爆音（带通噪声）+ 高频锐度（高通噪声）+ 胸腔低频（三角波） */
function synthShot(t0, out, k, p) {
  const g1 = _env(k * p.a, t0, 0.002, p.d1);
  const n1 = _nz(t0, p.d1 + 0.06, p.rate);
  const f1 = _bq('bandpass', p.band, 1.05);
  n1.connect(f1); f1.connect(g1); g1.connect(out);
  const g2 = _env(k * p.a * 0.6, t0, 0.001, p.d2);
  const n2 = _nz(t0, p.d2 + 0.03, 1.0);
  const f2 = _bq('highpass', p.hf, 0.7);
  n2.connect(f2); f2.connect(g2); g2.connect(out);
  const g3 = _env(k * p.b, t0, 0.003, p.tail);
  const o3 = _osc('triangle', t0, p.lo, p.lo * 0.42, p.tail + 0.02);
  o3.connect(g3); g3.connect(out);
}
function synthSniper(t0, out, k) {
  synthShot(t0, out, k, { band: 2600, hf: 5400, a: 0.72, b: 0.50, lo: 210, d1: 0.062, d2: 0.022, tail: 0.10, rate: 1.0 });
  /* 山谷回声：远距离射击的「啪——」拖尾 */
  const g = _env(k * 0.24, t0 + 0.16, 0.03, 0.9);
  const n = _nz(t0 + 0.16, 0.95, 0.55);
  const f = _bq('bandpass', 900, 0.5);
  n.connect(f); f.connect(g); g.connect(out);
}
function synthMetal(t0, out, k, len) {
  const parts = [1480, 2370, 3560, 5210];
  len = len || 1;
  for (let i = 0; i < parts.length; i++) {
    const f = parts[i] * (0.98 + Math.random() * 0.04);
    const d = (0.2 + 0.45 / (i + 1)) * len;
    const g = _env(k * (0.42 / (i + 1)), t0, 0.002, d);
    const o = _osc('sine', t0, f, f * 0.985, d + 0.05);
    o.connect(g); g.connect(out);
  }
  const g0 = _env(k * 0.45, t0, 0.001, 0.05);
  const n0 = _nz(t0, 0.06, 1.2);
  const b0 = _bq('bandpass', 3000, 0.9);
  n0.connect(b0); b0.connect(g0); g0.connect(out);
}
/* 爆炸：低频冲击波 + 低通噪声 + 破片高频 + 山谷余响 */
function synthBoom(t0, out, k, p) {
  const d = p.dur;
  const g1 = _env(k * 0.85 * p.a, t0, 0.004, d);
  const n1 = _nz(t0, d + 0.12, 0.85);
  const lp1 = _bq('lowpass', p.lp * 2.4, 0.75);
  lp1.frequency.setValueAtTime(p.lp * 2.4, t0);
  lp1.frequency.exponentialRampToValueAtTime(Math.max(60, p.lp * 0.45), t0 + d);
  n1.connect(lp1); lp1.connect(g1); g1.connect(out);
  const g2 = _env(k * p.b, t0, 0.006, d * 1.6);
  const o2 = _osc('sine', t0, p.f0, p.f1, d * 1.7);
  o2.connect(g2); g2.connect(out);
  const g3 = _env(k * 0.34, t0, 0.001, 0.13);
  const n3 = _nz(t0, 0.16, 1.3);
  const hp3 = _bq('highpass', 1600, 0.7);
  n3.connect(hp3); hp3.connect(g3); g3.connect(out);
  const g4 = _env(k * p.echo, t0 + 0.1, 0.03, d * 2.4);
  const n4 = _nz(t0 + 0.1, d * 2.5, 0.6);
  const lp4 = _bq('lowpass', 260, 0.9);
  n4.connect(lp4); lp4.connect(g4); g4.connect(out);
  if (p.metal) synthMetal(t0 + 0.05, out, k * 0.45, 1.3);
  if (p.click) {
    const g5 = _env(k * 0.5, t0, 0.001, 0.05);
    const n5 = _nz(t0, 0.06, 1.5);
    const b5 = _bq('bandpass', 1800, 0.8);
    n5.connect(b5); b5.connect(g5); g5.connect(out);
  }
}
const BOOM = {
  explosion: { dur: 0.85, lp: 420, f0: 96, f1: 32, a: 0.95, b: 0.85, echo: 0.30 },
  mine: { dur: 0.70, lp: 520, f0: 110, f1: 36, a: 1.00, b: 0.90, echo: 0.26, click: 1 },
  barrel: { dur: 0.60, lp: 560, f0: 120, f1: 42, a: 0.85, b: 0.70, echo: 0.22, metal: 1 },
  grenade: { dur: 0.62, lp: 480, f0: 104, f1: 38, a: 0.90, b: 0.75, echo: 0.24, metal: 1 },
  rocketBoom: { dur: 0.80, lp: 440, f0: 100, f1: 34, a: 0.95, b: 0.85, echo: 0.30 },
  shellBoom: { dur: 1.00, lp: 380, f0: 84, f1: 28, a: 1.00, b: 0.95, echo: 0.36 },
  vehicleBoom: { dur: 1.20, lp: 360, f0: 78, f1: 26, a: 1.00, b: 1.00, echo: 0.40, metal: 1 },
  cannon: { dur: 1.10, lp: 340, f0: 72, f1: 24, a: 1.00, b: 1.00, echo: 0.40, click: 1 },
};
/* 一串提示音（UI / 无线电 / 结算） */
function synthBeeps(t0, out, k, freqs, dur, gap, type) {
  for (let i = 0; i < freqs.length; i++) {
    const t = t0 + i * gap;
    const g = _env(k * (i ? 0.85 : 1), t, 0.005, dur);
    const o = _osc(type || 'square', t, freqs[i], freqs[i], dur + 0.02);
    const b = _bq('bandpass', freqs[i], 0.8);
    o.connect(b); b.connect(g); g.connect(out);
  }
}
/* 无线电：静噪咔哒 + 提示音 + 收尾咔哒。上行音＝我方发射，下行音＝电台接收 */
function synthRadio(t0, out, k, up) {
  const g1 = _env(k * 0.55, t0, 0.001, 0.02);
  const n1 = _nz(t0, 0.03, 1.6); const f1 = _bq('bandpass', 2600, 1.4);
  n1.connect(f1); f1.connect(g1); g1.connect(out);
  const fr = up ? 1250 : 980;
  const g2 = _env(k, t0 + 0.012, 0.004, 0.07);
  const o2 = _osc('square', t0 + 0.012, fr, fr, 0.09);
  const f2 = _bq('bandpass', fr, 0.9);
  o2.connect(f2); f2.connect(g2); g2.connect(out);
  const g3 = _env(k * 0.4, t0 + 0.085, 0.001, 0.02);
  const n3 = _nz(t0 + 0.085, 0.03, 1.4); const f3 = _bq('bandpass', 2000, 1.2);
  n3.connect(f3); f3.connect(g3); g3.connect(out);
}
/* 机械咔哒（换弹 / 拉栓 / 落地） */
function synthClack(t0, out, k, band, dur, thud) {
  const g = _env(k, t0, 0.001, dur);
  const n = _nz(t0, dur + 0.02, 1.1);
  const f = _bq('bandpass', band, 1.1);
  n.connect(f); f.connect(g); g.connect(out);
  if (thud) {
    const g2 = _env(k * 0.5, t0, 0.002, dur * 1.4);
    const o2 = _osc('triangle', t0, 190, 120, dur * 1.5);
    o2.connect(g2); g2.connect(out);
  }
}
/* 呼啸（火箭弹发射 / 炮弹下落 / 投弹） */
function synthWhoosh(t0, out, k, p) {
  const g = _env(k * p.a, t0, p.atk, p.dur);
  const n = _nz(t0, p.dur + 0.1, p.rate);
  const f = _bq('lowpass', p.f0, 0.7);
  f.frequency.setValueAtTime(p.f0, t0);
  f.frequency.exponentialRampToValueAtTime(Math.max(60, p.f1), t0 + p.dur);
  n.connect(f); f.connect(g); g.connect(out);
  if (p.tone) {
    const g2 = _env(k * p.a * 0.45, t0, p.atk, p.dur);
    const o = _osc('sine', t0, p.tone, p.tone * 0.4, p.dur + 0.05);
    o.connect(g2); g2.connect(out);
  }
}

const SYN = {
  /* 爆炸族：共用 synthBoom，参数取自 BOOM 表（半径越大越沉、拖尾越长）。
     务必与 BOOM 的键一一对应 —— explosion() 与坦克炮都是按这些 id 调 sfx() 的，
     漏掉任何一个，那条链路就会静默失声（这也是最容易漏掉的一类 bug）。 */
  explosion: (t0, o, k) => synthBoom(t0, o, k, BOOM.explosion),
  mine: (t0, o, k) => synthBoom(t0, o, k, BOOM.mine),
  barrel: (t0, o, k) => synthBoom(t0, o, k, BOOM.barrel),
  grenade: (t0, o, k) => synthBoom(t0, o, k, BOOM.grenade),
  rocketBoom: (t0, o, k) => synthBoom(t0, o, k, BOOM.rocketBoom),
  shellBoom: (t0, o, k) => synthBoom(t0, o, k, BOOM.shellBoom),
  vehicleBoom: (t0, o, k) => synthBoom(t0, o, k, BOOM.vehicleBoom),
  cannon: (t0, o, k) => synthBoom(t0, o, k, BOOM.cannon),
  rifle: (t0, o, k) => synthShot(t0, o, k, { band: 1900, hf: 3400, a: 0.55, b: 0.40, lo: 190, d1: 0.085, d2: 0.028, tail: 0.10, rate: 1.05 }),
  mg: (t0, o, k) => synthShot(t0, o, k, { band: 1150, hf: 2500, a: 0.62, b: 0.55, lo: 150, d1: 0.115, d2: 0.032, tail: 0.15, rate: 0.92 }),
  sniper: (t0, o, k) => synthSniper(t0, o, k),
  enemyRifle: (t0, o, k) => synthShot(t0, o, k, { band: 1350, hf: 2800, a: 0.58, b: 0.44, lo: 170, d1: 0.09, d2: 0.03, tail: 0.11, rate: 0.95 }),
  enemyMG: (t0, o, k) => synthShot(t0, o, k, { band: 950, hf: 2100, a: 0.66, b: 0.58, lo: 140, d1: 0.12, d2: 0.034, tail: 0.16, rate: 0.88 }),
  crack: (t0, o, k) => {
    const g = _env(k, t0, 0.001, 0.035);
    const n = _nz(t0, 0.045, 1.4);
    const b = _bq('bandpass', 4200, 1.6);
    n.connect(b); b.connect(g); g.connect(o);
    const g2 = _env(k * 0.5, t0, 0.001, 0.05);
    const s = _osc('sine', t0, 3000, 900, 0.06);
    s.connect(g2); g2.connect(o);
  },
  impact: (t0, o, k) => {
    synthClack(t0, o, k, 800, 0.045, false);
    const g = _env(k * 0.4, t0, 0.002, 0.05);
    const s = _osc('triangle', t0, 220, 110, 0.06);
    s.connect(g); g.connect(o);
  },
  clang: (t0, o, k) => synthMetal(t0, o, k, 0.55),
  metal: (t0, o, k) => synthMetal(t0, o, k, 1.6),
  flesh: (t0, o, k) => {
    const g = _env(k, t0, 0.002, 0.07);
    const n = _nz(t0, 0.09, 0.7);
    const f = _bq('lowpass', 700, 0.8);
    n.connect(f); f.connect(g); g.connect(o);
    const g2 = _env(k * 0.6, t0, 0.002, 0.09);
    const s = _osc('triangle', t0, 150, 80, 0.1);
    s.connect(g2); g2.connect(o);
  },
  body: (t0, o, k) => {
    const g = _env(k, t0, 0.003, 0.18);
    const s = _osc('sine', t0, 165, 55, 0.2);
    s.connect(g); g.connect(o);
    const g2 = _env(k * 0.5, t0, 0.002, 0.09);
    const n = _nz(t0, 0.11, 0.6);
    const f = _bq('lowpass', 500, 0.8);
    n.connect(f); f.connect(g2); g2.connect(o);
  },
  allyDown: (t0, o, k) => {
    SYN.body(t0, o, k, null);
    const g = _env(k * 0.5, t0 + 0.03, 0.01, 0.5);
    const s = _osc('sawtooth', t0 + 0.03, 220, 130, 0.55);
    const f = _bq('lowpass', 900, 0.8);
    s.connect(f); f.connect(g); g.connect(o);
  },
  hit: (t0, o, k) => synthBeeps(t0, o, k, [1050, 1580], 0.032, 0.035, 'square'),
  boxDrop: (t0, o, k) => { synthClack(t0, o, k * 0.8, 1400, 0.08, true); synthMetal(t0 + 0.02, o, k * 0.5, 0.6); },
  pickup: (t0, o, k) => synthBeeps(t0, o, k, [880, 1320], 0.09, 0.09, 'sine'),
  evac: (t0, o, k) => synthBeeps(t0, o, k, [660, 990, 1320], 0.1, 0.09, 'sine'),
  radioTx: (t0, o, k) => synthRadio(t0, o, k, true),
  radioRx: (t0, o, k) => synthRadio(t0, o, k, false),
  orderSend: (t0, o, k) => synthBeeps(t0, o, k, [1400, 1900], 0.045, 0.06, 'square'),
  orderFail: (t0, o, k) => synthBeeps(t0, o, k, [420, 300], 0.075, 0.1, 'square'),
  alert: (t0, o, k) => synthBeeps(t0, o, k, [760, 760], 0.07, 0.11, 'square'),
  ui: (t0, o, k) => synthBeeps(t0, o, k, [1500], 0.03, 0, 'square'),
  micOn: (t0, o, k) => { synthRadio(t0, o, k, true); },
  micOff: (t0, o, k) => { synthRadio(t0, o, k, false); },
  win: (t0, o, k) => synthBeeps(t0, o, k, [523, 659, 784, 1046], 0.18, 0.14, 'sine'),
  lose: (t0, o, k) => synthBeeps(t0, o, k, [392, 330, 262], 0.22, 0.2, 'sine'),
  begin: (t0, o, k) => {
    synthBeeps(t0, o, k * 0.8, [196, 294], 0.22, 0.16, 'sawtooth');
    synthRadio(t0 + 0.3, o, k * 0.6, false);
  },
  ambush: (t0, o, k) => {
    /* 号角式 sting：两个失谐锯齿上滑 + 噪声涌起 */
    for (const f of [220, 330, 221.7, 332.2]) {
      const g = _env(k * 0.22, t0, 0.08, 0.5);
      const s = _osc('sawtooth', t0, f, f * 2, 0.6);
      const b = _bq('lowpass', 700, 0.9);
      b.frequency.setValueAtTime(700, t0);
      b.frequency.linearRampToValueAtTime(2400, t0 + 0.45);
      s.connect(b); b.connect(g); g.connect(o);
    }
    const g = _env(k * 0.3, t0, 0.25, 0.7);
    const n = _nz(t0, 1.0, 0.8);
    const f2 = _bq('bandpass', 1200, 0.5);
    n.connect(f2); f2.connect(g); g.connect(o);
  },
  hurt: (t0, o, k) => {
    const g = _env(k, t0, 0.002, 0.13);
    const n = _nz(t0, 0.16, 0.6);
    const f = _bq('lowpass', 620, 0.7);
    n.connect(f); f.connect(g); g.connect(o);
    const g2 = _env(k * 0.5, t0, 0.004, 0.16);
    const s = _osc('sine', t0, 110, 62, 0.2);
    s.connect(g2); g2.connect(o);
  },
  down: (t0, o, k) => {
    const g = _env(k, t0, 0.004, 0.5);
    const s = _osc('sine', t0, 120, 44, 0.6);
    s.connect(g); g.connect(o);
    const g2 = _env(k * 0.6, t0, 0.003, 0.4);
    const n = _nz(t0, 0.45, 0.5);
    const f = _bq('lowpass', 420, 0.7);
    n.connect(f); f.connect(g2); g2.connect(o);
  },
  reinforce: (t0, o, k) => {
    const g = _env(k * 0.5, t0, 0.5, 1.6);
    const n = _nz(t0, 2.2, 0.45);
    const f = _bq('lowpass', 320, 0.7);
    n.connect(f); f.connect(g); g.connect(o);
    synthRadio(t0 + 0.9, o, k * 0.7, false);
  },
  incoming: (t0, o, k) => {
    const g = _env(k, t0 + 0.15, 0.7, 1.6);
    const s = _osc('sine', t0 + 0.15, 1500, 260, 1.9);
    const g2 = _env(k * 0.35, t0 + 0.15, 0.7, 1.6);
    const n = _nz(t0 + 0.15, 2.0, 1.0);
    const f = _bq('bandpass', 1800, 0.6);
    f.frequency.setValueAtTime(1800, t0 + 0.15);
    f.frequency.exponentialRampToValueAtTime(600, t0 + 2.0);
    s.connect(g); g.connect(o);
    n.connect(f); f.connect(g2); g2.connect(o);
  },
  rocketFire: (t0, o, k) => {
    synthClack(t0, o, k * 0.7, 2200, 0.05, false);
    synthWhoosh(t0, o, k, { dur: 0.75, f0: 1500, f1: 240, rate: 0.9, atk: 0.02, a: 0.8, tone: 300 });
  },
  grenadeThrow: (t0, o, k) => {
    synthWhoosh(t0, o, k, { dur: 0.32, f0: 1100, f1: 380, rate: 1.2, atk: 0.03, a: 1.0 });
    synthClack(t0, o, k * 0.5, 1800, 0.03, false);
  },
  reloadStart: (t0, o, k) => {
    synthClack(t0, o, k, 2600, 0.015, false);
    synthClack(t0 + 0.09, o, k * 0.95, 1300, 0.03, true);
    synthClack(t0 + 0.2, o, k * 0.8, 900, 0.055, false);
  },
  reloadEnd: (t0, o, k) => {
    synthClack(t0, o, k, 2200, 0.02, false);
    synthClack(t0 + 0.05, o, k * 0.9, 3000, 0.015, false);
    synthMetal(t0 + 0.09, o, k * 0.35, 0.4);
  },
};

/* -------------------------------------------------- 空间化 / 压限 / 耳鸣 */
function listener() {
  const c = World.cam;
  const p = World.player;
  const ok = c && isFinite(c.x) && isFinite(c.y);
  return {
    x: ok ? c.x : (p ? p.x : 0),
    y: ok ? c.y : (p ? p.y : 0),
    yaw: p ? p.facing : 0,
  };
}
/* 空气吸收：距离越远，高频越先掉 */
function airCutoff(d) { return clamp(800 + 14000 / (1 + Math.pow(d / 300, 1.1)), 700, 16000); }
function makeChain(g, pan, cut) {
  const ctx = Sound.ctx;
  const gn = ctx.createGain(); gn.gain.value = Math.max(0.0001, g);
  let tail = gn;
  if (cut && cut < 15000) { const f = _bq('lowpass', cut, 0.6); tail.connect(f); tail = f; }
  if (Sound.pan && pan && Math.abs(pan) > 0.02) {
    const p = ctx.createStereoPanner(); p.pan.value = clamp(pan, -1, 1);
    tail.connect(p); tail = p;
  }
  tail.connect(Sound.bus);
  return gn;
}
function duckFor(amount, dur) {
  if (!Sound.duck) return;
  const t = _now();
  const target = clamp(1 - amount, 0.25, 1);
  try {
    Sound.duck.gain.cancelScheduledValues(t);
    Sound.duck.gain.setValueAtTime(Math.max(0.0001, Sound.duck.gain.value), t);
    Sound.duck.gain.linearRampToValueAtTime(target, t + 0.02);
    Sound.duck.gain.setTargetAtTime(1, t + 0.06, dur * 0.5);
  } catch (e) { }
}
function ringFor(freq, dur, g) {
  if (!Sound.ringBus || g <= 0.001) return;
  const ctx = Sound.ctx, t = _now();
  const gn = ctx.createGain();
  gn.gain.setValueAtTime(0.0001, t);
  gn.gain.linearRampToValueAtTime(g, t + 0.03);
  gn.gain.exponentialRampToValueAtTime(0.0001, t + dur);
  const o = ctx.createOscillator(); o.type = 'sine'; o.frequency.value = freq;
  const lfo = ctx.createOscillator(); lfo.type = 'sine'; lfo.frequency.value = 5 + Math.random() * 3;
  const lg = ctx.createGain(); lg.gain.value = freq * 0.012;
  lfo.connect(lg); lg.connect(o.frequency);
  o.connect(gn); gn.connect(Sound.ringBus);
  o.start(t); o.stop(t + dur + 0.05); lfo.start(t); lfo.stop(t + dur + 0.05);
}

/* ------------------------------------------------------------ 播放入口 */
/* id：音效名 · x,y：世界坐标（省略或 local 则不空间化）· opt：{local,gain,delay,gap,force} */
function sfx(id, x, y, opt) {
  if (!Sound.ok) return;
  const fn = SYN[id];
  if (!fn) return;
  const def = SND[id] || { g: 0.4 };
  const o = opt || {};
  const local = o.local || def.local || !isFinite(x);
  const now = _now();
  const gap = o.gap !== undefined ? o.gap : (SND_GAP[id] || 0.012);
  if (Sound.last[id] !== undefined && now - Sound.last[id] < gap) return;
  Sound.last[id] = now;
  if (!o.force) {
    while (Sound.budget.length && now - Sound.budget[0] > 0.1) Sound.budget.shift();
    if (Sound.budget.length >= 26) return;
    Sound.budget.push(now);
  }
  let g = def.g, pan = 0, cut = 0;
  if (!local) {
    const L = listener();
    const d = dist(x, y, L.x, L.y);
    if (d > def.max) return;
    g *= 1 / (1 + Math.pow(d / def.ref, 1.25));
    cut = airCutoff(d);
    const near = clamp(d / 70, 0, 1);
    pan = clamp(Math.sin(angDiff(Math.atan2(y - L.y, x - L.x), L.yaw)) * near, -1, 1);
    if (def.duck && d < 900) duckFor(def.duck * (1 - d / 900) * 0.55, 0.5);
    if (def.ring && d < 300) ringFor(1700 + Math.random() * 1000, 2.2, def.duck * 0.09);
  } else if (def.duck) {
    duckFor(def.duck * 0.4, 0.4);                 // 贴脸爆炸（自己在爆炸点）
  }
  if (o.gain !== undefined) g *= o.gain;
  g *= Sound.vol;
  if (g < 0.0015) return;
  try { fn(now + (o.delay || 0) + 0.002, makeChain(g, pan, cut), g, o); }
  catch (e) { /* 单个音效合成失败不该影响战斗 */ }
}

/* --------------------------------------------------------- 环境音 / 引擎 */
function startAmbient() {
  if (!Sound.ok || Sound.ambNodes) return;
  const ctx = Sound.ctx;
  const windLp = _bq('lowpass', 380, 0.7);
  const windG = ctx.createGain(); windG.gain.value = 0.05;
  const wind = _loop(0.8);
  wind.connect(windLp); windLp.connect(windG); windG.connect(Sound.amb);
  /* 风力的缓慢起伏 */
  const lfo = ctx.createOscillator(); lfo.type = 'sine'; lfo.frequency.value = 0.05;
  const lg = ctx.createGain(); lg.gain.value = 150;
  lfo.connect(lg); lg.connect(windLp.frequency); lfo.start(0);
  const rainG = ctx.createGain(); rainG.gain.value = 0.0001;
  const rainHp = _bq('highpass', 1400, 0.6);
  const rain = _loop(1.25);
  rain.connect(rainHp); rainHp.connect(rainG); rainG.connect(Sound.amb);
  const hiss = _loop(1.7);
  const hissB = _bq('bandpass', 3600, 0.5);
  const hissG = ctx.createGain(); hissG.gain.value = 0.0001;
  hiss.connect(hissB); hissB.connect(hissG); hissG.connect(rainG);
  Sound.ambNodes = { wind: windG, rain: rainG, hiss: hissG, amb: Sound.amb };
}
function setAmbientWeather() {
  if (!Sound.ambNodes) return;
  const t = _now(), w = World.weather;
  const wv = w === 'night' ? 0.028 : w === 'rain' ? 0.05 : 0.045;
  const rv = w === 'rain' ? 0.075 : 0;
  const hv = w === 'rain' ? 0.3 : 0;
  try {
    Sound.ambNodes.wind.gain.setTargetAtTime(wv, t, 1.2);
    Sound.ambNodes.rain.gain.setTargetAtTime(rv, t, 1.2);
    Sound.ambNodes.hiss.gain.setTargetAtTime(hv, t, 1.2);
    Sound.ambNodes.amb.gain.setTargetAtTime(1, t, 1.5);
  } catch (e) { }
}
function fadeAmbient(v) {
  if (!Sound.ambNodes) return;
  try { Sound.ambNodes.amb.gain.setTargetAtTime(v, _now(), 0.8); } catch (e) { }
}
/* 夜战虫鸣：断续几声，勉强能听出「这是夜里」 */
function cricket() {
  if (!Sound.ok) return;
  const L = listener();
  const pan = (Math.random() * 2 - 1) * 0.8;
  const chain = makeChain(0.05 * Sound.vol, pan, 0);
  const f = 3900 + Math.random() * 700;
  for (let i = 0; i < 3; i++) {
    const t0 = _now() + i * 0.055;
    const g = _env(0.6, t0, 0.004, 0.03);
    const o = _osc('sine', t0, f, f, 0.04);
    const b = _bq('bandpass', f, 12);
    o.connect(b); b.connect(g); g.connect(chain);
  }
}
/* 车辆引擎循环声：车队还没进视野时，这是玩家唯一的线索 */
function ensureEngines() {
  if (!Sound.ok || Sound.engines.length) return;
  const ctx = Sound.ctx;
  for (let i = 0; i < 4; i++) {
    const gn = ctx.createGain(); gn.gain.value = 0.0001;
    const lp = _bq('lowpass', 300, 0.8);
    const p = Sound.pan ? ctx.createStereoPanner() : null;
    const o1 = ctx.createOscillator(); o1.type = 'sawtooth'; o1.frequency.value = 60;
    const o2 = ctx.createOscillator(); o2.type = 'sawtooth'; o2.frequency.value = 31;
    const o2g = ctx.createGain(); o2g.gain.value = 0.6;
    /* 轻微抖动，避免听起来像纯电子音 */
    const wob = ctx.createOscillator(); wob.type = 'sine'; wob.frequency.value = 7 + i;
    const wg = ctx.createGain(); wg.gain.value = 2.5;
    wob.connect(wg); wg.connect(o1.frequency);
    o1.connect(gn); o2.connect(o2g); o2g.connect(gn);
    gn.connect(lp);
    if (p) { lp.connect(p); p.connect(Sound.bus); } else lp.connect(Sound.bus);
    o1.start(0); o2.start(0); wob.start(0);
    Sound.engines.push({ gain: gn, pan: p, o1, o2, lp });
  }
}
function updateEngines() {
  if (!Sound.ok || !Sound.engines.length) return;
  const t = _now(), L = listener();
  const cands = [];
  if (World.started && !World.over && World.vehicles) {
    for (const v of World.vehicles) {
      if (v.team !== 'enemy' || v.destroyed) continue;
      const d = dist(v.x, v.y, L.x, L.y);
      if (d > 1500) continue;
      cands.push({ v, d });
    }
    cands.sort((a, b) => a.d - b.d);
  }
  for (let i = 0; i < Sound.engines.length; i++) {
    const e = Sound.engines[i], c = cands[i];
    let g = 0.0001, pan = 0, f = 60;
    if (c) {
      const base = c.v.type === 'tank' ? 42 : c.v.type === 'truck' ? 54 : c.v.type === 'apc' ? 62 : 74;
      g = 0.16 / (1 + c.d / 380) * (c.v.speed > 0 ? 1 : 0.5);
      pan = clamp(Math.sin(angDiff(Math.atan2(c.v.y - L.y, c.v.x - L.x), L.yaw)), -1, 1);
      f = base * (1 + (c.v.speed || 0) / 150);
    }
    try {
      e.gain.gain.setTargetAtTime(Math.max(0.0001, g * Sound.vol * 2), t, 0.15);
      if (e.pan) e.pan.pan.setTargetAtTime(pan, t, 0.15);
      e.o1.frequency.setTargetAtTime(f, t, 0.25);
      e.o2.frequency.setTargetAtTime(f * 0.51, t, 0.25);
    } catch (err) { }
  }
}
/* 每帧调用一次（挂在渲染循环里，不参与逻辑步进） */
function updateAudio(dt) {
  if (!Sound.ok) return;
  const want = Sound.muted ? 0 : Sound.vol;
  if (Math.abs(Sound.master.gain.value - want) > 0.002) {
    try { Sound.master.gain.setTargetAtTime(want, _now(), 0.05); } catch (e) { }
  }
  ensureEngines();
  updateEngines();
  if (World.weather === 'night' && World.started && !World.over) {
    Sound.cricketT -= dt;
    if (Sound.cricketT <= 0) { Sound.cricketT = 0.8 + Math.random() * 1.8; cricket(); }
  }
}

/* =========================================================================
   九、第一人称 3D 渲染（原生 WebGL，无外部依赖）

   逻辑层完全不知道渲染方式 —— 它只维护 World.units / vehicles / props 的 2D 坐标。
   这里把 (x, y) 直接当成 3D 世界的地面平面 (x, 0, y)：
     比例尺 1 米 ≈ 20 世界单位（公路宽 140 ≈ 7 米、人半径 5.2 ≈ 肩宽 0.52 米、
     世界 2200×1300 ≈ 110×65 米），所以坐标不需要任何换算。
   模型约定「前方 = +X」，因此 3D 绕 Y 轴旋转角 = -facing（2D 角度取反）。
   ========================================================================= */

/* ------------------------------------------------------------- 9.1 矩阵数学 */
function m4new() { return new Float32Array(16); }
function m4ident(o) { o[0] = 1; o[1] = 0; o[2] = 0; o[3] = 0; o[4] = 0; o[5] = 1; o[6] = 0; o[7] = 0;
  o[8] = 0; o[9] = 0; o[10] = 1; o[11] = 0; o[12] = 0; o[13] = 0; o[14] = 0; o[15] = 1; return o; }
/* o = a * b（列主序） */
function m4mul(o, a, b) {
  for (let i = 0; i < 4; i++) {
    const b0 = b[i * 4], b1 = b[i * 4 + 1], b2 = b[i * 4 + 2], b3 = b[i * 4 + 3];
    o[i * 4] = a[0] * b0 + a[4] * b1 + a[8] * b2 + a[12] * b3;
    o[i * 4 + 1] = a[1] * b0 + a[5] * b1 + a[9] * b2 + a[13] * b3;
    o[i * 4 + 2] = a[2] * b0 + a[6] * b1 + a[10] * b2 + a[14] * b3;
    o[i * 4 + 3] = a[3] * b0 + a[7] * b1 + a[11] * b2 + a[15] * b3;
  }
  return o;
}
function m4persp(o, fovy, aspect, near, far) {
  const f = 1 / Math.tan(fovy / 2), nf = 1 / (near - far);
  o[0] = f / aspect; o[1] = 0; o[2] = 0; o[3] = 0;
  o[4] = 0; o[5] = f; o[6] = 0; o[7] = 0;
  o[8] = 0; o[9] = 0; o[10] = (far + near) * nf; o[11] = -1;
  o[12] = 0; o[13] = 0; o[14] = 2 * far * near * nf; o[15] = 0;
  return o;
}
function m4lookAt(o, ex, ey, ez, cx, cy, cz, ux, uy, uz) {
  let zx = ex - cx, zy = ey - cy, zz = ez - cz;
  let l = Math.hypot(zx, zy, zz) || 1; zx /= l; zy /= l; zz /= l;
  let xx = uy * zz - uz * zy, xy = uz * zx - ux * zz, xz = ux * zy - uy * zx;
  l = Math.hypot(xx, xy, xz) || 1; xx /= l; xy /= l; xz /= l;
  const yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;
  o[0] = xx; o[1] = yx; o[2] = zx; o[3] = 0;
  o[4] = xy; o[5] = yy; o[6] = zy; o[7] = 0;
  o[8] = xz; o[9] = yz; o[10] = zz; o[11] = 0;
  o[12] = -(xx * ex + xy * ey + xz * ez);
  o[13] = -(yx * ex + yy * ey + yz * ez);
  o[14] = -(zx * ex + zy * ey + zz * ez);
  o[15] = 1;
  return o;
}
/* 平移 + 绕 Y 旋转 + 缩放（模型前方 = +X） */
function m4trs(o, tx, ty, tz, ry, sx, sy, sz) {
  const c = Math.cos(ry || 0), s = Math.sin(ry || 0);
  if (sx === undefined) sx = 1; if (sy === undefined) sy = sx; if (sz === undefined) sz = sx;
  o[0] = c * sx; o[1] = 0; o[2] = -s * sx; o[3] = 0;
  o[4] = 0; o[5] = sy; o[6] = 0; o[7] = 0;
  o[8] = s * sz; o[9] = 0; o[10] = c * sz; o[11] = 0;
  o[12] = tx; o[13] = ty; o[14] = tz; o[15] = 1;
  return o;
}
/* 把模型矩阵作用于点（用于把 3D 点投影到屏幕） */
function projectPoint(mvp, x, y, z, out) {
  const cx = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
  const cy = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
  const cw = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
  if (cw <= 0.001) { out.behind = true; return out; }
  out.behind = false;
  out.x = (cx / cw * 0.5 + 0.5) * view.w;
  out.y = (0.5 - cy / cw * 0.5) * view.h;
  out.w = cw;
  return out;
}

/* --------------------------------------------------------- 9.2 WebGL 初始化 */
const CV = document.getElementById('game');
const HUDC = document.getElementById('hud');
const HCTX = HUDC.getContext('2d');
const MM = document.getElementById('minimap');
const MCTX = MM.getContext('2d');
const view = { w: 1280, h: 720, dpr: 1, bw: 1280, bh: 720 };
let GL = null;

const VS_SRC = [
  'attribute vec3 aPos; attribute vec3 aNrm; attribute vec2 aUV; attribute vec3 aCol;',
  'uniform mat4 uProj; uniform mat4 uView; uniform mat4 uModel;',
  'varying vec3 vN; varying vec3 vW; varying vec2 vUV; varying vec3 vCol;',
  'void main(){',
  '  vec4 w = uModel * vec4(aPos, 1.0);',
  '  vW = w.xyz;',
  '  vN = mat3(uModel[0].xyz, uModel[1].xyz, uModel[2].xyz) * aNrm;',
  '  vUV = aUV; vCol = aCol;',
  '  gl_Position = uProj * uView * w;',
  '}'
].join('\n');

const FS_SRC = [
  'precision mediump float;',
  'uniform vec3 uTint; uniform float uUseTex; uniform sampler2D uTex;',
  'uniform vec3 uFog; uniform float uFogNear; uniform float uFogFar; uniform vec3 uCam;',
  'uniform float uAlpha; uniform float uEmis; uniform float uUnlit;',
  'varying vec3 vN; varying vec3 vW; varying vec2 vUV; varying vec3 vCol;',
  'void main(){',
  '  vec3 base = vCol * uTint;',
  '  if (uUseTex > 0.5) base *= texture2D(uTex, vUV).rgb;',
  '  float d = distance(vW, uCam);',
  '  float fog = clamp((d - uFogNear) / max(1.0, uFogFar - uFogNear), 0.0, 1.0);',
  '  fog = fog * fog;',
  '  vec3 col;',
  '  if (uUnlit > 0.5) {',
  '    col = base;',
  '  } else {',
  '    vec3 N = normalize(vN);',
  '    vec3 L = normalize(vec3(-0.40, 0.82, 0.41));',
  '    float ndl = max(dot(N, L), 0.0);',
  '    float amb = 0.42 + 0.20 * clamp(N.y, 0.0, 1.0);',
  '    col = base * (amb + ndl * 0.80);',
  '    vec3 V = normalize(uCam - vW);',
  '    vec3 H = normalize(L + V);',
  '    col += vec3(1.0) * pow(max(dot(N, H), 0.0), 26.0) * 0.12 * uEmis;',
  '    col = mix(col, base * 1.9, uEmis);',
  '  }',
  '  col = mix(col, uFog, fog);',
  '  gl_FragColor = vec4(col, uAlpha);',
  '}'
].join('\n');

const SH = { prog: null, loc: {} };
function shader(type, src) {
  const s = GL.createShader(type);
  GL.shaderSource(s, src); GL.compileShader(s);
  if (!GL.getShaderParameter(s, GL.COMPILE_STATUS)) {
    console.error('着色器编译失败: ' + GL.getShaderInfoLog(s)); return null;
  }
  return s;
}
function initGL() {
  GL = CV.getContext('webgl', { alpha: false, antialias: true, depth: true, powerPreference: 'high-performance' })
    || CV.getContext('experimental-webgl', { alpha: false, antialias: true, depth: true });
  if (!GL) return false;
  const vs = shader(GL.VERTEX_SHADER, VS_SRC), fs = shader(GL.FRAGMENT_SHADER, FS_SRC);
  if (!vs || !fs) return false;
  const p = GL.createProgram();
  GL.attachShader(p, vs); GL.attachShader(p, fs); GL.linkProgram(p);
  if (!GL.getProgramParameter(p, GL.LINK_STATUS)) { console.error('链接失败: ' + GL.getProgramInfoLog(p)); return false; }
  SH.prog = p; GL.useProgram(p);
  ['aPos', 'aNrm', 'aUV', 'aCol'].forEach(a => SH.loc[a] = GL.getAttribLocation(p, a));
  ['uProj', 'uView', 'uModel', 'uTint', 'uUseTex', 'uTex', 'uFog', 'uFogNear', 'uFogFar', 'uCam', 'uAlpha', 'uEmis', 'uUnlit']
    .forEach(u => SH.loc[u] = GL.getUniformLocation(p, u));
  GL.enable(GL.DEPTH_TEST);
  GL.depthFunc(GL.LEQUAL);
  GL.disable(GL.CULL_FACE);          // 模型多为简单盒体，关掉背面剔除以免漏面
  GL.clearColor(0.55, 0.60, 0.63, 1);
  return true;
}

/* ------------------------------------------------------------- 9.3 程序化纹理 */
/* 全部用 canvas 现场画出来，保持「单文件、零依赖、离线可用」 */
function makeTex(size, fn) {
  const c = document.createElement('canvas'); c.width = c.height = size;
  fn(c.getContext('2d'), size);
  const t = GL.createTexture();
  GL.bindTexture(GL.TEXTURE_2D, t);
  GL.texImage2D(GL.TEXTURE_2D, 0, GL.RGBA, GL.RGBA, GL.UNSIGNED_BYTE, c);
  GL.texParameteri(GL.TEXTURE_2D, GL.TEXTURE_WRAP_S, GL.REPEAT);
  GL.texParameteri(GL.TEXTURE_2D, GL.TEXTURE_WRAP_T, GL.REPEAT);
  GL.texParameteri(GL.TEXTURE_2D, GL.TEXTURE_MIN_FILTER, GL.LINEAR_MIPMAP_LINEAR);
  GL.texParameteri(GL.TEXTURE_2D, GL.TEXTURE_MAG_FILTER, GL.LINEAR);
  GL.generateMipmap(GL.TEXTURE_2D);
  return t;
}
const TEX = {};
function buildTextures() {
  const R = mulberry32(90210);
  /* 草地：军校绿底 + 噪声斑块 */
  TEX.grass = makeTex(256, (g, S) => {
    g.fillStyle = '#5c6b42'; g.fillRect(0, 0, S, S);
    for (let i = 0; i < 2600; i++) {
      const x = R() * S, y = R() * S, r = 2 + R() * 11;
      const v = R();
      g.fillStyle = 'rgba(' + (v < 0.5 ? '74,88,50' : '116,132,84') + ',' + (0.16 + R() * 0.34).toFixed(2) + ')';
      g.beginPath(); g.ellipse(x, y, r, r * (0.4 + R() * 0.8), R() * 3.14, 0, 6.29); g.fill();
    }
    for (let i = 0; i < 900; i++) {   // 草叶
      const x = R() * S, y = R() * S;
      g.strokeStyle = 'rgba(' + (R() < 0.5 ? '132,150,92' : '60,72,42') + ',' + (0.20 + R() * 0.4).toFixed(2) + ')';
      g.lineWidth = 0.8 + R();
      g.beginPath(); g.moveTo(x, y); g.lineTo(x + (R() - 0.5) * 4, y - 2 - R() * 4); g.stroke();
    }
  });
  /* 沥青：深灰 + 细骨料 */
  TEX.road = makeTex(256, (g, S) => {
    g.fillStyle = '#4a4a48'; g.fillRect(0, 0, S, S);
    for (let i = 0; i < 4200; i++) {
      const x = R() * S, y = R() * S, r = 0.6 + R() * 2.4;
      const v = R();
      g.fillStyle = 'rgba(' + (v < 0.5 ? '34,34,34' : '104,102,96') + ',' + (0.10 + R() * 0.42).toFixed(2) + ')';
      g.beginPath(); g.arc(x, y, r, 0, 6.29); g.fill();
    }
    g.strokeStyle = 'rgba(28,28,28,.30)'; g.lineWidth = 1;   // 裂纹
    for (let i = 0; i < 14; i++) {
      g.beginPath(); let x = R() * S, y = R() * S; g.moveTo(x, y);
      for (let k = 0; k < 5; k++) { x += (R() - 0.5) * 40; y += (R() - 0.5) * 40; g.lineTo(x, y); }
      g.stroke();
    }
  });
  /* 泥土（路肩、弹坑） */
  TEX.dirt = makeTex(128, (g, S) => {
    g.fillStyle = '#6b5f47'; g.fillRect(0, 0, S, S);
    for (let i = 0; i < 1600; i++) {
      const x = R() * S, y = R() * S, r = 1 + R() * 6;
      g.fillStyle = 'rgba(' + (R() < 0.5 ? '86,74,54' : '134,120,92') + ',' + (0.14 + R() * 0.34).toFixed(2) + ')';
      g.beginPath(); g.arc(x, y, r, 0, 6.29); g.fill();
    }
  });
  /* 树皮 */
  TEX.bark = makeTex(128, (g, S) => {
    g.fillStyle = '#3f372c'; g.fillRect(0, 0, S, S);
    for (let i = 0; i < 240; i++) {
      g.strokeStyle = 'rgba(' + (R() < 0.5 ? '30,26,20' : '82,72,58') + ',' + (0.2 + R() * 0.4).toFixed(2) + ')';
      g.lineWidth = 0.8 + R() * 2.2;
      const x = R() * S;
      g.beginPath(); g.moveTo(x, 0); g.lineTo(x + (R() - 0.5) * 9, S); g.stroke();
    }
  });
  /* 军绿（车辆帆布 / 装备） */
  TEX.canvasCloth = makeTex(128, (g, S) => {
    g.fillStyle = '#57603f'; g.fillRect(0, 0, S, S);
    for (let i = 0; i < 900; i++) {
      g.fillStyle = 'rgba(' + (R() < 0.5 ? '68,76,50' : '96,104,72') + ',' + (0.2 + R() * 0.3).toFixed(2) + ')';
      g.fillRect(R() * S, R() * S, 2 + R() * 8, 1 + R() * 3);
    }
  });
  /* 水面 */
  TEX.water = makeTex(128, (g, S) => {
    g.fillStyle = '#2f4757'; g.fillRect(0, 0, S, S);
    for (let i = 0; i < 220; i++) {
      g.strokeStyle = 'rgba(150,190,210,' + (0.05 + R() * 0.16).toFixed(2) + ')';
      g.lineWidth = 0.8 + R() * 2;
      const y = R() * S;
      g.beginPath(); g.moveTo(0, y); g.lineTo(S, y + (R() - 0.5) * 4); g.stroke();
    }
  });
}

/* --------------------------------------------------------- 9.4 网格构建器 */
/* 顶点色 = 部位基色；uTint 提供阵营色调（静态场景固定为白） */
function MB() { this.p = []; this.n = []; this.u = []; this.c = []; this.i = []; this.nv = 0; }
MB.prototype.quad = function (a, b, c, d, nx, ny, nz, col, uv) {
  const b0 = this.nv;
  const pts = [a, b, c, d];
  for (let k = 0; k < 4; k++) {
    const v = pts[k];
    this.p.push(v[0], v[1], v[2]);
    this.n.push(nx, ny, nz);
    this.u.push(uv ? uv[k * 2] : (k === 1 || k === 2 ? 1 : 0), uv ? uv[k * 2 + 1] : (k >= 2 ? 1 : 0));
    this.c.push(col[0], col[1], col[2]);
  }
  this.i.push(b0, b0 + 1, b0 + 2, b0, b0 + 2, b0 + 3);
  this.nv += 4;
};
const WHT = [1, 1, 1];
MB.prototype.box = function (cx, cy, cz, sx, sy, sz, col, uv) {
  col = col || WHT;
  const x0 = cx - sx / 2, x1 = cx + sx / 2, y0 = cy - sy / 2, y1 = cy + sy / 2, z0 = cz - sz / 2, z1 = cz + sz / 2;
  const q = (a, b, c, d, nx, ny, nz) => this.quad(a, b, c, d, nx, ny, nz, col, uv);
  q([x1, y0, z1], [x1, y0, z0], [x1, y1, z0], [x1, y1, z1], 1, 0, 0);
  q([x0, y0, z0], [x0, y0, z1], [x0, y1, z1], [x0, y1, z0], -1, 0, 0);
  q([x0, y1, z1], [x1, y1, z1], [x1, y1, z0], [x0, y1, z0], 0, 1, 0);
  q([x0, y0, z0], [x1, y0, z0], [x1, y0, z1], [x0, y0, z1], 0, -1, 0);
  q([x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1], 0, 0, 1);
  q([x1, y0, z0], [x0, y0, z0], [x0, y1, z0], [x1, y1, z0], 0, 0, -1);
};
MB.prototype.cyl = function (cx, cy, cz, r, h, seg, col, rTop, axis) {
  col = col || WHT; rTop = rTop === undefined ? r : rTop;
  const y0 = cy - h / 2, y1 = cy + h / 2;
  for (let i = 0; i < seg; i++) {
    const a0 = i / seg * 6.283185, a1 = (i + 1) / seg * 6.283185;
    const c0 = Math.cos(a0), s0 = Math.sin(a0), c1 = Math.cos(a1), s1 = Math.sin(a1);
    const nx = (c0 + c1) / 2, nz = (s0 + s1) / 2;
    if (axis === 'x') {   // 沿 X 轴的圆柱（车轮）
      this.quad([cx - h / 2, y0 + r * c0, cz + r * s0], [cx + h / 2, y0 + r * c0, cz + r * s0],
        [cx + h / 2, y0 + r * c1, cz + r * s1], [cx - h / 2, y0 + r * c1, cz + r * s1], 0, nx, nz, col);
    } else if (axis === 'z') {  // 沿 Z 轴
      this.quad([cx + r * c0, y0 + r * s0, cz - h / 2], [cx + r * c1, y0 + r * s1, cz - h / 2],
        [cx + r * c1, y0 + r * s1, cz + h / 2], [cx + r * c0, y0 + r * s0, cz + h / 2], nx, nz, 0, col);
    } else {
      this.quad([cx + r * c0, y0, cz + r * s0], [cx + r * c1, y0, cz + r * s1],
        [cx + rTop * c1, y1, cz + rTop * s1], [cx + rTop * c0, y1, cz + rTop * s0], nx, 0, nz, col);
      if (rTop > 0.001) this.quad([cx + rTop * c0, y1, cz + rTop * s0], [cx + rTop * c1, y1, cz + rTop * s1],
        [cx, y1, cz], [cx, y1, cz], 0, 1, 0, col);
      else this.quad([cx, y1, cz], [cx, y1, cz], [cx, y1, cz], [cx, y1, cz], 0, 1, 0, col);
    }
  }
};
MB.prototype.upload = function () {
  const idx = this.nv > 65535 ? new Uint32Array(this.i) : new Uint16Array(this.i);
  const o = {
    n: this.i.length,
    pos: GL.createBuffer(), nrm: GL.createBuffer(), uv: GL.createBuffer(), col: GL.createBuffer(), idx: GL.createBuffer(),
    type: this.nv > 65535 ? GL.UNSIGNED_INT : GL.UNSIGNED_SHORT,
    big: this.nv > 65535,
  };
  const put = (buf, data) => { GL.bindBuffer(GL.ARRAY_BUFFER, buf); GL.bufferData(GL.ARRAY_BUFFER, new Float32Array(data), GL.STATIC_DRAW); };
  put(o.pos, this.p); put(o.nrm, this.n); put(o.uv, this.u); put(o.col, this.c);
  GL.bindBuffer(GL.ELEMENT_ARRAY_BUFFER, o.idx);
  GL.bufferData(GL.ELEMENT_ARRAY_BUFFER, idx, GL.STATIC_DRAW);
  return o;
};
MB.prototype.uploadDynamic = function () {
  /* 动态网格：顶点数据每帧变化，用 DYNAMIC_DRAW。这里复用 upload 但标记 */
  const o = this.upload();
  o.dynamic = true;
  return o;
};

/* 地面/路面用世界坐标算 UV，保证纹理平铺密度一致 */
MB.prototype.quadUV = function (a, b, c, d, nx, ny, nz, col, uvScale) {
  const uv = [a[0], a[2], b[0], b[2], c[0], c[2], d[0], d[2]].map(v => v / uvScale);
  this.quad(a, b, c, d, nx, ny, nz, col, uv);
};

/* ------------------------------------------------------- 9.5 模型：士兵/车辆 */
/* 顶点色是「部位明暗」，阵营色调由 uTint 提供：我方偏军绿、敌方偏褐红 */
const SOLDIER_COL = {
  body: [0.34, 0.40, 0.27],
  gear: [0.20, 0.22, 0.17],
  skin: [0.72, 0.58, 0.46],
  gun: [0.13, 0.13, 0.14],
};
let MESH_SOLDIER = null, MESH_SOLDIER_DOWN = null;
function buildSoldierMesh(downed) {
  const m = new MB();
  const C = SOLDIER_COL;
  if (downed) {
    /* 倒地：整体躺平 + 血泊 */
    m.box(0, 3.0, 0, 26, 9, 13, C.body);           // 躯干（横躺）
    m.box(15, 4.0, 0, 9, 8, 9, C.gear);            // 头
    m.box(-16, 2.5, 0, 14, 7, 11, C.body);         // 腿
    m.quad([-11, 0.35, -16], [11, 0.35, -16], [11, 0.35, 16], [-11, 0.35, 16], 0, 1, 0, [0.32, 0.02, 0.02]);
    return m.upload();
  }
  /* 站立：腿 / 躯干 / 头 / 手臂 / 枪，约 1.7 m 高（×20 单位） */
  m.box(-3.4, 15, 0, 6.5, 30, 8, C.body);          // 左腿
  m.box(3.4, 15, 0, 6.5, 30, 8, C.body);           // 右腿
  m.box(0, 38, 0, 17, 20, 12, C.body);             // 躯干
  m.box(0, 40, 0, 18.5, 13, 13.5, C.gear);         // 战术背心
  m.box(0, 51.5, 0, 9.5, 9.5, 9.5, C.gear);        // 头盔
  m.box(1.2, 49.5, 0, 5.5, 5, 10, C.skin);         // 面部
  m.box(0, 36, -8.2, 6, 15, 5.5, C.body);          // 左臂
  m.box(0, 36, 8.2, 6, 15, 5.5, C.body);           // 右臂
  m.box(9, 37, 6.5, 20, 2.6, 2.2, C.gun);          // 枪身
  m.box(17, 37, 6.5, 6, 2.2, 2.0, C.gun);          // 枪管
  m.box(2, 34.5, 6.5, 4, 6, 2.6, C.gun);           // 弹匣
  return m.upload();
}

let MESH_VEH = {};
function buildVehicleMesh(type) {
  const m = new MB();
  const G = [0.30, 0.34, 0.24], D = [0.14, 0.15, 0.13], W = [0.075, 0.075, 0.078], GLASS = [0.16, 0.22, 0.24];
  if (type === 'jeep') {
    m.box(0, 11, 0, 46, 7, 24, G);
    m.box(11, 16, 0, 20, 5, 22, G);                 // 引擎盖
    m.box(-6, 18, 0, 20, 8, 22, G);                 // 座舱
    m.box(-6, 22.5, 0, 18, 4, 20, GLASS);           // 风挡
    m.box(-20, 19, 0, 5, 9, 23, D);
    m.cyl(0, 6, -13, 6.4, 4.5, 12, W, 6.4, 'x'); m.cyl(0, 6, 13, 6.4, 4.5, 12, W, 6.4, 'x');
    m.cyl(-15, 6, -13, 6.4, 4.5, 12, W, 6.4, 'x'); m.cyl(-15, 6, 13, 6.4, 4.5, 12, W, 6.4, 'x');
    m.box(-15, 24, 4, 14, 2.4, 2.2, D);             // 车顶机枪
  } else if (type === 'apc') {
    m.box(0, 15, 0, 60, 16, 30, G);
    m.box(14, 26, 0, 26, 8, 26, G);                 // 上层
    m.box(12, 30, 0, 22, 4, 22, GLASS);
    m.box(-2, 32, 0, 22, 10, 22, G);                // 炮塔
    m.box(9, 32.5, 0, 22, 2.6, 2.4, D);             // 机炮
    m.cyl(0, 7, -16, 7.4, 5.5, 12, W, 7.4, 'x'); m.cyl(0, 7, 16, 7.4, 5.5, 12, W, 7.4, 'x');
    m.cyl(-19, 7, -16, 7.4, 5.5, 12, W, 7.4, 'x'); m.cyl(-19, 7, 16, 7.4, 5.5, 12, W, 7.4, 'x');
    m.cyl(19, 7, -16, 7.4, 5.5, 12, W, 7.4, 'x'); m.cyl(19, 7, 16, 7.4, 5.5, 12, W, 7.4, 'x');
  } else if (type === 'tank') {
    m.box(-2, 11, 0, 70, 13, 38, G);                // 车体
    m.box(-2, 6, -17.5, 72, 12, 8, D);              // 履带
    m.box(-2, 6, 17.5, 72, 12, 8, D);
    m.box(24, 16, 0, 24, 5, 32, G);                 // 首上
    m.box(0, 24, 0, 40, 14, 30, G);                 // 炮塔
    m.box(16, 25, 0, 26, 12, 26, G);
    m.box(34, 25.5, 0, 34, 4.2, 4.2, D);            // 主炮
    m.box(-16, 32, 0, 10, 5, 12, D);                // 后舱盖
  } else {                                          // truck
    m.box(18, 14, 0, 22, 18, 28, G);                // 驾驶室
    m.box(20, 26, 0, 18, 6, 26, GLASS);
    m.box(-12, 17, 0, 44, 24, 30, [0.33, 0.36, 0.26]);  // 帆布车厢
    m.box(30, 8, 0, 10, 8, 28, D);
    m.cyl(20, 7, -14, 7, 5.5, 12, W, 7, 'x'); m.cyl(20, 7, 14, 7, 5.5, 12, W, 7, 'x');
    m.cyl(-16, 7, -14, 7, 5.5, 12, W, 7, 'x'); m.cyl(-16, 7, 14, 7, 5.5, 12, W, 7, 'x');
    m.cyl(-26, 7, -14, 7, 5.5, 12, W, 7, 'x'); m.cyl(-26, 7, 14, 7, 5.5, 12, W, 7, 'x');
    m.box(-12, 30, 0, 42, 2.5, 27, D);
  }
  return m.upload();
}

/* 第一人称武器（视图空间绘制，所以模型中心在原点、朝向 -Z 之外由变换负责） */
let MESH_WPN = null;
function buildWeaponMesh() {
  const m = new MB();
  const D = [0.10, 0.10, 0.11], M = [0.16, 0.16, 0.17], W = [0.30, 0.26, 0.20], G = [0.20, 0.21, 0.23];
  m.box(0, 0, 0, 46, 4.2, 4.6, M);                  // 机匣
  m.box(30, 0, 0, 40, 2.2, 2.6, D);                 // 枪管
  m.box(52, 0.6, 0, 8, 2.8, 3.0, D);                // 枪口制退器
  m.box(6, -7, 0, 7, 9, 4.4, D);                    // 弹匣
  m.box(6, -1, -6.5, 4, 3, 2.6, G);                 // 拉机柄
  m.box(12, 1.6, 0, 26, 2.6, 2.8, G);               // 导轨
  m.box(4, 2.6, 0, 7, 3, 5.4, M);                   // 光学瞄具
  m.box(34, 2.4, 0, 3, 2.4, 2.6, G);                // 前准星
  m.box(-14, -2, 0, 16, 7, 3.6, W);                 // 枪托
  m.box(-2, -6, 0, 6, 9, 4.2, M);                   // 握把
  return m.upload();
}

/* 辅助网格：发光体（爆炸/火光/箱子的光晕）、密码箱、地面圆盘（弹坑） */
let MESH_GLOW = null, MESH_BOX = null, MESH_DISC = null;
function buildAuxMeshes() {
  /* 发光体：沿三个轴交叉的薄板，任意角度看都有体积感，比球省顶点 */
  const gm = new MB(), WC = [1, 1, 1];
  const S = 0.5;
  gm.quad([-S, 0, 0], [0, S, 0], [S, 0, 0], [0, -S, 0], 0, 0, 1, WC);
  gm.quad([0, 0, -S], [0, S, 0], [0, 0, S], [0, -S, 0], 1, 0, 0, WC);
  gm.quad([-S, 0, 0], [0, 0, -S], [S, 0, 0], [0, 0, S], 0, 1, 0, WC);
  MESH_GLOW = gm.upload();

  const bm = new MB();
  bm.box(0, 0, 0, 14, 9, 10, [0.85, 0.78, 0.55]);
  bm.box(0, 5.5, 0, 9, 2, 11, [0.55, 0.50, 0.38]);
  bm.box(0, 0, 0, 15, 3, 11, [0.40, 0.38, 0.32]);
  MESH_BOX = bm.upload();

  const dm = new MB();
  dm.quad([-10, 0, -10], [10, 0, -10], [10, 0, 10], [-10, 0, 10], 0, 1, 0, WC);
  MESH_DISC = dm.upload();
}

/* ------------------------------------------------------------ 9.6 静态场景 */
let SCENE = { ground: null, road: null, water: null, props: null, bridge: null };
function buildTerrain() {
  /* 地面 */
  const gm = new MB();
  const W = CFG.W, H = CFG.H;
  gm.quadUV([-40, 0, -40], [W + 40, 0, -40], [W + 40, 0, H + 40], [-40, 0, H + 40], 0, 1, 0, WHT, 150);
  SCENE.ground = gm.upload();

  /* 公路：沿 ROAD_PATH 生成条带 */
  const rm = new MB();
  const hw = (CFG.roadBot - CFG.roadTop) / 2 + 12;   // 半宽（含路肩）
  for (let i = 0; i < ROAD_PATH.length - 1; i++) {
    const a = ROAD_PATH[i], b = ROAD_PATH[i + 1];
    const dx = b[0] - a[0], dy = b[1] - a[1], L = Math.hypot(dx, dy) || 1;
    const nx = -dy / L * hw, ny = dx / L * hw;
    rm.quadUV([a[0] + nx, 0.6, a[1] + ny], [b[0] + nx, 0.6, b[1] + ny],
      [b[0] - nx, 0.6, b[1] - ny], [a[0] - nx, 0.6, a[1] - ny], 0, 1, 0, [1, 1, 1], 34);
  }
  SCENE.road = rm.upload();

  /* 河流 */
  const wm = new MB();
  wm.quadUV([CFG.riverX1, -2.5, 0], [CFG.riverX2, -2.5, 0], [CFG.riverX2, -2.5, H], [CFG.riverX1, -2.5, H], 0, 1, 0, WHT, 60);
  SCENE.water = wm.upload();

  /* 桥梁 */
  const bm = new MB();
  bm.box((CFG.riverX1 + CFG.riverX2) / 2, 2, (CFG.bridgeY1 + CFG.bridgeY2) / 2,
    CFG.riverX2 - CFG.riverX1 + 20, 6, CFG.bridgeY2 - CFG.bridgeY1, [0.42, 0.40, 0.36]);
  bm.box((CFG.riverX1 + CFG.riverX2) / 2, 7, CFG.bridgeY1 - 3, CFG.riverX2 - CFG.riverX1 + 20, 8, 3, [0.36, 0.34, 0.30]);
  bm.box((CFG.riverX1 + CFG.riverX2) / 2, 7, CFG.bridgeY2 + 3, CFG.riverX2 - CFG.riverX1 + 20, 8, 3, [0.36, 0.34, 0.30]);
  SCENE.bridge = bm.upload();

  /* 道具：树 / 岩石 / 油桶，全部烘焙成一个大网格（一次 draw call） */
  const pm = new MB();
  const R = mulberry32(7771);
  for (const p of World.props) {
    if (p.type === 'tree') {
      const h = (p.r || 26) * (1.5 + R() * 0.5);
      pm.cyl(p.x, h * 0.42, p.y, p.r * 0.16, h * 0.84, 7, [0.55, 0.48, 0.40]);
      /* 树冠：两层圆锥 */
      const cr = p.r * (0.85 + R() * 0.25);
      pm.cyl(p.x, h * 0.72, p.y, cr, h * 0.62, 9, [0.30 + R() * 0.06, 0.42 + R() * 0.07, 0.20], 0);
      pm.cyl(p.x, h * 0.95, p.y, cr * 0.62, h * 0.44, 9, [0.33 + R() * 0.06, 0.46 + R() * 0.07, 0.22], 0);
    } else if (p.type === 'rock') {
      const s = p.r * 1.7;
      const g = [0.40, 0.40, 0.38];
      pm.box(p.x, s * 0.34, p.y, s, s * 0.78, s * 0.86, g);
      pm.box(p.x + s * 0.22, s * 0.60, p.y - s * 0.14, s * 0.62, s * 0.5, s * 0.55, [0.46, 0.46, 0.44]);
    } else if (p.type === 'barrel') {
      pm.cyl(p.x, 9, p.y, 6.5, 18, 10, [0.44, 0.30, 0.16]);
      pm.cyl(p.x, 18, p.y, 6.5, 1.6, 10, [0.34, 0.36, 0.32]);
    } else if (p.type === 'wall') {
      pm.box(p.x, 8, p.y, (p.r || 20) * 2, 16, 10, [0.46, 0.44, 0.40]);
    } else if (p.type === 'trench') {
      pm.box(p.x, 0.8, p.y, (p.r || 20) * 2, 1.6, 16, [0.30, 0.26, 0.20]);
    } else if (p.type === 'bush') {
      pm.cyl(p.x, 7, p.y, (p.r || 12) * 0.9, 12, 8, [0.28, 0.36, 0.20]);
    }
  }
  /* 弹坑（decal）单独一帧绘制太碎，静态的先烘焙一层 */
  SCENE.props = pm.upload();
  return true;
}
/* 重新开局时先释放旧的 GL 缓冲，避免反复 buildTerrain 造成显存泄漏 */
function disposeScene() {
  for (const k in SCENE) {
    const m = SCENE[k];
    if (!m) continue;
    try {
      GL.deleteBuffer(m.pos); GL.deleteBuffer(m.nrm); GL.deleteBuffer(m.uv);
      GL.deleteBuffer(m.col); GL.deleteBuffer(m.idx);
    } catch (e) { /* 忽略 */ }
    SCENE[k] = null;
  }
}


/* =========================================================================