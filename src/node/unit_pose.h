#pragma once
// VolunteerArmyPC —— 单位"姿态"按状态换模型：走路 = 行军姿，开火 = 据枪姿
//
// ============================================================ 这一层在做什么
// 全队 11 个角色（all_art_keys）现在是**一套行军姿**：枪斜挎在胸前、手垂着，
// 走到哪都是行军的样子 —— 正在跟人对着打的时候也是。看上去就是"举着空手对峙"。
//
// 本层给每个单位配**第二套模型**（据枪姿，文件名 = 行军键 + "_fire"），
// 平时画行军姿，**正在开火时**换成据枪姿。走路/站岗/隐蔽一律不动。
//
// ⚠️ 为什么只能"换整份模型"，不能"摆关节"：
// 本工程的 11 个 char_*.glb 全是**单块融合网格**（nodes=1 meshes=1 skins=0
// joints=0 animations=0），枪和手臂已经**烤进网格**了 —— 既没有骨骼可转，
// 也没有可分离的枪件。"据枪"这个姿态在几何上就只存在于另一份网格里。
// 这是模型产出方式（图生3D 单视图重建）决定的，不是本层偷懒。
//
// ============================================================ 键名怎么派生
//     <行军键> + "_fire"      例：char_rifleman → char_rifleman_fire
// 走文件名约定而不是加一张映射表，有三个好处：
//   · 零 header 改动 —— 不碰 all_art_keys()（它是"谁是谁"的真值，不是姿态表）
//   · **缺文件自动安静回退** —— 只补了 1 个键也能上战场，其余 10 个各自回退到
//     行军姿，不会因为"还没做全"而整体不能用
//   · 与 unit_model_yaw_deg 的逐键表同一个键名口径（那张表里已经有
//     char_rifleman_fire 那一行），不需要在两处之间做转换
//
// ============================================================ 判据：fireCd > 0
// 只读逻辑层字段，**src/sim/ 一行不改**。
//
//     Unit::fireCd（sim/va_types.h:195）
//
// 语义是"距离下一发还有多久"，而它是在**每一枪打出的当帧**被赋值的：
//     sim/va_combat.cpp:376   u->fireCd = u->burstLeft > 0 ? w->rof : w->rof + w->burstGap;
//     sim/va_units.cpp:18     if (u->fireCd > 0) u->fireCd -= dt;      ← 只减不为负
// 而开火是**每 tick 都试**的（sim/va_ai_ally.cpp:321 在"有目标且在射程内"分支里
// 无限速地调 fire_at），所以只要单位还在对着人打，fireCd 就被反复顶回正值 ⇒
// **一次交火期内它是连续的**，不会在连发之间闪回行军姿。
//
// 停火之后它自然衰减到 0，于是"据枪 → 行军"的收枪时机**自带一个 dwell**，
// 而且这个 dwell 是按武器给的：
//     步枪 0.12 / +0.34 弹夹间歇   机枪 0.09 / +1.00    狙击 1.70（bolt 上膛）
//     敌步枪 0.16 / +0.50          敌机枪 0.10 / +1.20
// 也就是说狙击手打完一发会端着枪 1.7 秒（正好是拉栓的时间），机枪手打完一个
// 长点射会端 1.0 秒 —— 不需要额外再写一个"保持 N 秒"的状态机。
//
// 【为什么不看 W.fx / u.state】fx 是"爆炸与枪口焰"这种**瞬时**事件
// （FxItem 没有 owner 字段，按坐标+类型匹配就得做半径搜索，还要自己去重），
// 而 u.state 是"隐蔽 / 待命 / 战斗"这类**战术**状态 —— 战斗状态下的人有大量
// 时间在跑位和找掩体，拿它当"在开火"用会让全队一边跑一边端枪。fireCd 是唯一
// 一个**直接就是"枪刚刚响过"**的字段。
//
// ============================================================ 变换：整份拷贝，不重算
// 据枪节点是**行军节点的兄弟节点**（同一个父节点 refs_.units），每帧
//     fire->set_transform(walk->get_transform())
// 直接把行军节点这一帧的变换整个抄过来，而不是自己再算一遍
// unit_transform(…) * pose_transform(pv)。理由：那一串里有
// 地形抬升（ground_h）、朝向平滑（face_）、倒地侧翻、跑动起伏与前倾/摇摆四层，
// 抄一遍就有第二次实现的机会，而两套实现一旦漂移，现象是"开枪那一瞬间人往
// 旁边跳一下"—— 频率低、幅度小，极难在截图上抓到。整份拷贝把它变成不可能。
//
// 索引对齐也照抄 world_sim 的口径：fire_ 以**单位下标**为键（与 unit_nodes_ 同一套
// 下标），节点数量对不上时由调用方 reset()（见 sync_entity_nodes 的重建分支）。
//
// ============================================================ ⚠️ 双腿层必须收到新键
// UnitLeg::apply(node, key, …) 的材质缓存是**按 key 索引**的（unit_leg.cpp:113），
// 而键里编着髋高/过渡带/膝弯三个几何量（它们由 mesh 自己的包围盒推出来）。
// 所以换模型之后继续传旧键，就会：把行军模型的髋高套在据枪模型上 ⇒
// 腿从腰上摆起来。⇒ resolve() 把"这一帧该用的键"一并写回给调用方（r_key），
// 调用方原样转给 leg_.apply，不要自己再拼一次。
//
// 归零口径不必额外处理：据枪模型的 AABB 与行军模型不同，ensure() 会自动按
// 它自己的包围盒重算一套几何量，缓存也是分开的（键不同）。但**两套模型归一化
// 后的脚底高度可能有差**（上一轮实测约 5 cm ≈ 3% 身高），换姿那一瞬间会是
// 一次小跳 —— 用 VA_POSE_DY 微调（见下）。
//
// ============================================================ 两条纪律（与 anim_/leg_/face_/fx_ 一致）
// 1. 只**读**逻辑层状态（fireCd），一行逻辑都不改；删掉不影响任何逻辑。
// 2. **VA_POSE=0 时逐像素等同未加本层** —— resolve() 立刻返回行军节点与行军键，
//    连一个据枪节点都不建，也不调用任何 Godot setter。
//
// ============================================================ 开关
//     VA_POSE=0        整层关闭（消融用，也是"零代价"的证明）
//     VA_POSE_DY=<米>  据枪模型的垂直微调（默认 0）—— 只在换姿那一跳看得出来时用
//     VA_DBG_POSE=1    每次切换打一行 + 收尾一行（见 dump()）

#include <cstddef>
#include <map>
#include <string>

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string.hpp>

#include "sim/va_types.h"

namespace volunteer_army {

class UnitPose {
public:
    // 读环境旋钮（VA_POSE / VA_POSE_DY / VA_DBG_POSE）。幂等。
    void setup();

    /* 每帧每单位调一次，在**行军节点已经摆好这一帧的变换之后**（world_sim.cpp 里
       紧跟在 set_transform(...) 之后、leg_.apply 之前）。

       返回"这一帧该出现在画面上的那个节点"：
         · 没在开火 / 缺据枪模型 / 本层关闭 → 返回 p_walk（与不加本层完全一样）
         · 在开火且模型齐备            → 返回据枪节点（已拷贝好变换、已显形，
                                          并把 p_walk 藏掉）
       r_key 写出**与之配套的模型键**，直接转给 leg_.apply（上面那条 ⚠️）。

       p_index 是单位下标（与 unit_nodes_ 同一套），p_parent 是节点挂载点
       （refs_.units —— 与行军节点同一个父节点，变换才可比）。 */
    godot::Node3D *resolve(std::size_t p_index, godot::Node3D *p_walk,
                           const std::string &p_walk_key, const va::Unit &p_u,
                           godot::Node *p_parent, std::string &r_key);

    /* 阵亡 / 被隐藏（第一人称的自己）时调用：把据枪节点收掉。
       为什么必须有这个入口：world_sim 的每帧循环在 u.dead 处 `continue`，
       根本不走到 resolve() —— 不收的话，一个"正在开火时被打死"的单位会
       让据枪模型**留在原地站着**，而尸体（行军模型）已经倒地，
       画面上就是两个人。 */
    void release(std::size_t p_index);

    // 单位节点整体重建（数量变化 / 重开一局）时调用：据枪节点属于旧的索引体系。
    void reset();

    bool enabled() const { return enabled_; }

    /* 这个下标当前画的是不是据枪模型（诊断用，见 dbg_units_dump）。
       为什么不靠 unit_nodes_[i]->is_visible() 反推：那条等价关系是**间接**的
       （活着 + 不是自己 + 没被本层藏起来），读日志的人得自己推一遍；
       而"这一行是不是据枪"恰恰是看截图时最想一眼确认的事。 */
    bool firing(std::size_t p_index) const;

    /* 诊断：每 0.5 秒墙钟一行（照 anim_.tick_diag / face_.tick_diag 的范式）。
       ⚠️ 为什么不能只靠收尾的 dump()：dump() 挂在 on_end 上，而取证运行是
       "拍到指定时刻就 quit"（VA_CAPTURE 全拍完 → get_tree()->quit()），
       根本走不到 on_end —— 那样这一层的数字**只在"真打到结算"的运行里才出现**，
       而截图恰恰都是在结算之前拍的。p_sim_t 传 va::W.t，用来把这一行与截图对齐。 */
    void tick_diag(double p_wall_delta, double p_sim_t);

    godot::String dump() const;

private:
    struct Entry {
        godot::Node3D *node = nullptr;   // 据枪节点（懒建；建失败留 nullptr）
        std::string    key;              // 该节点对应的键（char_*_fire）
        bool  failed = false;            // 这个键没有模型 —— 别再每帧试一遍
        bool  firing = false;            // 上一帧是不是据枪态（只为统计"切换次数"）
    };

    std::map<std::size_t, Entry> fire_;   // 单位下标 -> 据枪节点

    bool  enabled_ = false;
    bool  dbg_ = false;
    float dy_ = 0.0f;                     // 据枪模型的垂直微调（米，局部空间）

    // 诊断（收尾 dump 用）
    int   switched_ = 0;                  // 累计"行军 → 据枪"次数
    int   no_asset_ = 0;                  // 因缺模型而放弃的**单位帧**数
    int   created_ = 0;                   // 累计建过几个据枪节点

    // 诊断（每 0.5 秒墙钟一行，见 tick_diag）
    double dbg_acc_ = 0.0;
    int    dbg_switched_ = 0;             // 上一行以来的新增切换数
};

} // namespace volunteer_army
