# -*- coding: utf-8 -*-
"""为 Vehicle / Unit 补上关卡流程与车队行驶所需的运行态字段。"""
import io

P = r'E:\study_lujiajing\Games\WorkSpace_VolunteerArmy\VolunteerArmyPC\src\sim\va_types.h'
src = io.open(P, encoding='utf-8').read()

# --- Vehicle：补 updateVehicles 用到的运行态 ---
old_v = """    float hitFlash = 0, stopT = 0, mgHeat = 0, report = 0;
    int   troopPlan = 0;
};"""
new_v = """    float hitFlash = 0, stopT = 0, mgHeat = 0, report = 0;
    int   troopPlan = 0;

    // 运行态（对应网页版里在战局中途才被写入的那些字段）
    float px = 0, py = 0;          // 上一帧位置（渲染插值/残骸留痕用）
    float blockedT = 0;            // 被前方残骸堵住的累计时长
    bool  atZone = false;          // 已抵达伏击圈停车线
    bool  atBridge = false;        // 因桥被炸而停下
    bool  isReinforcement = false; // 增援车队
    bool  chaosSaid = false;
    float fireFxT = 0;             // 燃烧特效节流
    float aimT = 0;                // 坦克主炮瞄准倒计时
    bool  hasAimPt = false;
    Vec2  aimPt{};
    float lastFireCd = 0;
};"""
assert old_v in src, 'Vehicle 尾部未匹配'
src = src.replace(old_v, new_v)

# --- Unit：补 evacuated / boxTask / lastHurtT ---
old_u = """    Unit *lastHurtBy = nullptr;"""
new_u = """    Unit *lastHurtBy = nullptr;
    float lastHurtT = -1000.0f;
    bool  evacuated = false;       // 已从撤离点离开（checkEnd 用）
    bool  boxTask = false;         // 被指派去捡密码箱
    bool  hasSpec = false;         // 占位：区分"载具"与"步兵"的过滤（网页版用 u.spec）"""
assert old_u in src, 'Unit 锚点未匹配'
src = src.replace(old_u, new_u)

io.open(P, 'w', encoding='utf-8', newline='').write(src)
print('已写入，字节 %d' % len(io.open(P, 'rb').read()))
