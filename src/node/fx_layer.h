#pragma once
// VolunteerArmyPC —— 射击光效层：枪口焰 / 曳光弹 / 弹着火花
//
// ============================================================ 一句话
// 把逻辑层**早就在发**的射击事件画出来：谁在哪开的枪、朝哪打的、打到哪了。
// 分阵营着色 ⇒ 一眼分得清是哪些敌人在射击。
//
// ============================================================ 为什么需要一个新层
// 逻辑层从移植第一天起就在产生这些数据，表现层**一行都没消费**：
//     va_combat.cpp:51   每次步兵开枪 → FxItem{type="flash", x=枪口, y=枪口, a=弹道角}
//     va_combat.cpp:80/97/113/135  火箭筒 / 手雷 / 车载炮 / 车载机枪 同理
//     va_combat.cpp:189/209/221    弹着火花 FxItem{type="spark"}
//     Projectile 上带 team / owner / angle / src
// 在 src/node/ 下 grep `W.fx` 与 `W.projectiles` —— **零命中**。
// 也就是说：枪口火光、曳光、弹着点，在 3D 画面里此前一个都没有。
// 唯一相关的是**玩家自己**的枪模闪光（viewmodel.cpp:1215-1239，十字火焰 + OmniLight3D），
// 那挂在第一人称枪模上，跟场上敌人无关 —— 所以"敌人从哪打过来"在画面上没有任何线索。
//
// ============================================================ 三件事分别从哪取数
//   枪口焰  ← **W.projectiles**（不是 W.fx！理由见下）
//   曳光弹  ← W.projectiles
//   火花    ← W.fx 里 type=="spark"
//
// ⚠️ 枪口焰为什么绕开 W.fx 的 flash 条目：因为 `FxItem` **没有 team 字段**。
//    逻辑层每次开枪会 push 一条 flash，位置、寿命都对，但无法回答"这是谁打的"——
//    而本需求的核心恰恰是"分清哪些敌人"。相反，`Projectile` 每颗都带 `team`。
//    于是改用"子弹刚出膛"当枪口焰的判据：
//        子弹出生点 = u->x + cos(a)*8（枪口前方 8 逻辑单位），与 flash 的 x/y **逐位相同**；
//        子弹出生后 p.t 从 0 起累加，而 flash 的 life 是 0.055 s —— 两者天然对齐。
//    所以 `p.t <= muzzle_window(kind)` 就是"这一刻枪口在发光"，且**自带阵营**。
//    （这条路径也顺带把车载机枪/炮弹的枪口焰一起覆盖了，因为它们同样 push Projectile。）
//
// ⚠️ 弹着火花为什么只能中性色：spark 是子弹**命中那一帧**产生的
//    （va_combat.cpp:189/209/221），而那颗子弹在**同一次 update_projectiles 调用里**
//    就被 erase 了（:239）。下一帧表现层去遍历 projectiles 时它已经不在了，
//    所以"这个火花是谁打的"在纯表现层拿不到。
//    火花本来回答的是"打到哪"而不是"谁在打"，中性色足够；要分阵营只能给 FxItem 加字段。
//
// ============================================================ ⚠️ 不能缓存指针 / 索引
// W.fx 与 W.projectiles 都是 std::vector，逻辑层每帧从后往前 erase
// （va_flow.cpp:310-312 / va_combat.cpp:239）。任何跨帧保存的下标或指针都会悬空。
// 所以本层**每帧无状态地全量遍历**，只按值读。光效对象则走**对象池**复用。
//
// ============================================================ 阵营配色（照搬网页版）
// 网页版的曳光弹早就按阵营分色（index.html:4582-4583），PC 版此前根本没有曳光弹：
//     友军 rgba(190,235,255) → 冷青白
//     敌方 rgba(255,190,150) → 暖橙
// 枪口焰沿用**同一套**配色 —— 玩家只要建立一次映射，就能同时读懂枪口光与弹道线。
// 好处是敌方那侧恰好也符合"枪口焰本来就是橙的"这个物理直觉。
//
// ============================================================ 忠于时长（不放大）
// 光效寿命严格按逻辑层的值走，不做"为了看清而拉长"：
//     步枪/机枪 0.055 s、车载机枪 0.05 s、手雷 0.08 s、火箭筒 0.12 s、车载炮 0.16 s、
//     火花 0.16~0.18 s
// 大小与透明度按剩余寿命 k = 1 − t/life 变化（与网页版同式，越老越大越淡 = 扩散消散）：
//     size  = base × (1.6 − k × 0.6)
//     alpha = 0.55 + k × 0.42
//
// ============================================================ 旋钮（都不必重编译）
//   VA_FX=0              整体关闭（不建节点、不遍历，画面与未加本层时逐像素相同）
//   VA_FX_MUZ=0          只关枪口焰     ┐
//   VA_FX_TRACER=0       只关曳光弹     ├ 分层消融：确认某一类光效的独立贡献
//   VA_FX_SPARK=0        只关弹着火花   ┘
//   VA_FX_GAIN=<n>       HDR 增益（默认 **1.35**，不是更大）。
//                        ⚠️ 给大了反而毁掉本需求：加色混合把颜色推到 HDR 之后，
//                        增益 3.0 会让三通道全部远超 1.0，经 ACES 统一压成纯白 ——
//                        友军冷青与敌军暖橙的差别就没了。1.35 刚好越过场景 glow 的
//                        HDR 门槛（1.05，scene_builder.cpp:2292）而产生辉光，同时保住色调。
//   VA_FX_NEAR=<米>      近距剔除半径（默认 1.5）。玩家自己那发枪口焰就在相机前
//                        0.4 m，不剔除会糊掉半个屏幕（另有 owner->isPlayer 的精确判据，
//                        两者互补：这一条是用来兜住"贴脸的队友"的）。
//   VA_FX_PROBE=1        阵营染色探针：友军绿 / 敌军红 / 火花蓝。验"颜色确实按 team 走"。
//   VA_FX_ANG=<弧度>     枪口焰/火花的**最小角尺寸**（默认 0.020 ≈ 1.15°）。
//                        ⚠️ 这一条是**需求能否成立**的关键，不是画质微调：
//                        光效是世界空间固定尺寸，于是"近处一大团、远处看不见"
//                        是必然结果 —— 而本需求要的恰恰是"远处的敌人开枪也能认出来"。
//                        按距离给角度下限（size = max(size, dist × ANG)），
//                        屏幕上占的像素数才不会随距离塌掉。近处不受影响。
//                        实测：0.010 时远处光点只有 15 px（缩略图上约 8 px）＝"画了但认不出"。
//   VA_FX_TW=<弧度>      曳光弹**最小角宽度**（默认 0.0030 ≈ 0.17°，约 3 px 线宽）。
//                        同理由：0.045 m 的条带在 50 m 外是亚像素，等于没画。
//   VA_FX_TL=<弧度>      曳光弹**最小角长度**（默认 0.016 ≈ 0.92°）。
//   VA_FX_TGAIN=<n>      曳光弹的 HDR 增益（默认 **1.15**，**低于**枪口焰的 1.35）。
//                        亮度让位于可辨性：1.35 会把冷青与暖橙一起压成纯白。
//   VA_DBG_FX=1          每 0.5 s 报一行（各类光效数量 + 峰值 + 敌我分布 +
//                        **正面/背后累计** —— "画面上没有光"时先看这个数）

#include <vector>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/immediate_mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace va { struct WorldState; }

namespace volunteer_army {

class FxLayer {
public:
    /* 建节点。p_parent 是 WorldSim 自己（不是相机）—— 光效属于世界，不跟着镜头走。
       VA_FX=0 时内部直接返回，一个节点都不建。 */
    void setup(godot::Node3D *p_parent, godot::Camera3D *p_cam);

    bool enabled() const { return enabled_; }

    /* 每帧同步。在相机位置更新之后调用（近距剔除要用当帧的相机位置）。
       幂等：同一帧调两次只是重画一遍。

       p_delta      —— 墙钟帧时长（只用于诊断计时）
       p_logical_dt —— **本帧推进的逻辑秒数**。枪口焰的判据必须用它，不能用墙钟：
                       步枪枪口焰只有 0.055 s，而 VA_FF 快进或掉帧时一帧能推进
                       0.1~0.13 s —— 用"当前时刻落在窗口内"去判会**整帧跳过**闪光，
                       画面表现成"敌人开枪完全没有光"，而这恰恰是本需求要解决的东西。
                       改成"本帧的时间区间 [t−dt, t] 与窗口 [0, win] 有交集"，
                       快进时退化为"每帧至少画一次"（跳帧但不断），不掉。 */
    void step(const va::WorldState &p_w, float p_delta, float p_logical_dt);

    /* 玩家自己那发弹的**弹道起点**：第一人称枪模的枪口世界坐标（ViewModel::muzzle_world）。
       WorldSim 每帧在 step() 之前喂进来。

       【为什么必须单独给这一路】
       本层其余光效都用"逻辑坐标 + 地面高度"定位，这对场上任何单位都够 —— 他们的枪口
       本来就在自己身前一点点。但**玩家自己**不行：第一人称相机架在玩家头顶 1.65 m，
       枪模的枪口在相机前方约 0.8 m、高约 1.6 m；而逻辑层给玩家的子弹出生点是
       "单位前方 8 逻辑单位（= 0.4 m）、映射后离地 1.25 m"。第一人称下这两个方向
       差 30° 以上 ⇒ 玩家开枪时那条弹道线看起来从画面下方凭空冒出，
       跟右下角枪模的枪口对不上（这就是玩家报的症状）。

       有了它，玩家自己的曳光弹改成"从枪口沿弹道方向伸出去"，与枪口焰（枪模自带）
       同源同点；敌方/AI 的光效一律走旧口径，一行不变。

       p_valid=false（枪模未建好 / VA_VM_HIDE=1 的取证模式 / VA_FX_PMUZ=0）时
       本层自动回退旧口径 —— 所以才做成"布尔 + 坐标"而不是只给坐标：
       没有显式失效标志的话，(0,0,0) 会被当成世界原点那个合法位置用下去。 */
    void set_player_muzzle(bool p_valid, const godot::Vector3 &p_pos);

    // 重开一局 / 换关：清空在用的槽位（逻辑层已经 W.fx.clear()，表现层也要跟上）
    void reset();

    godot::String dump() const;

private:
    // 一个发光广告牌槽位。材质是**每槽独立**的 —— 加色混合下的强度要逐光效调
    // （albedo_color 的 alpha），共享材质做不到。24 个材质的开销可以忽略。
    struct Slot {
        godot::MeshInstance3D *node = nullptr;
        godot::Ref<godot::StandardMaterial3D> mat;
        bool used = false;
    };

    Slot *take_slot();
    godot::OmniLight3D *take_light();

    /* 放一个发光广告牌。x/y 是**逻辑层坐标**，h 是相对地面的高度（米）。
       k = 剩余寿命比例（1 = 刚生成）；big = 大型火光（火箭/炮弹/车载机枪）。 */
    void emit(float p_x, float p_y, float p_h, float p_size, const godot::Color &p_col,
              float p_alpha, bool p_with_light);

    void build_tracers(const va::WorldState &p_w);
    void hide_rest();

    std::vector<Slot> slots_;
    std::vector<godot::OmniLight3D *> lights_;
    std::vector<bool> light_used_;

    godot::Node3D *root_ = nullptr;
    godot::Camera3D *cam_ = nullptr;
    godot::Ref<godot::Texture2D> glow_tex_;

    /* 曳光弹用**一个** ImmediateMesh 画全部弹道：
       每颗弹是一个面向相机的手搓四边形条带（6 顶点 / 2 三角形）。
       为什么不用 PRIMITIVE_LINES：Godot 的线宽固定 1 像素、不可调，
       在 3D 世界里细到看不见。所以自己按"线段方向 × 指向相机"叉乘出侧向量造面片。
       好处是任意多发弹只有 1 次 draw call，不需要给曳光弹做对象池。 */
    godot::MeshInstance3D *tracer_ = nullptr;
    godot::Ref<godot::ImmediateMesh> tracer_mesh_;
    godot::Ref<godot::StandardMaterial3D> tracer_mat_;

    bool  enabled_ = false;
    bool  muz_on_ = true, tracer_on_ = true, spark_on_ = true;
    int   probe_ = 0;
    bool  dbg_ = false;
    float gain_ = 1.35f;      // 见 .h 顶部「VA_FX_GAIN」：给大了会把阵营色压成纯白
    float near_cull_ = 1.5f;  // 米
    // 最小角尺寸（屏幕空间下限）。见 .h 顶部「VA_FX_ANG」——
    // 没有它们，远处（>=30 m）的枪口焰与曳光弹在画面上是 0~2 个像素，等于没画。
    float ang_muz_ = 0.020f;      // 枪口焰/火花：约 1.15°（≈ 21 px @1080p）
    float ang_tracer_w_ = 0.0030f; // 曳光弹宽：约 0.17°（≈ 3 px 线宽）
    float ang_tracer_l_ = 0.016f;  // 曳光弹长：约 0.92°
    /* 曳光弹的**近处**宽度上限：约 0.7°（≈ 11 px）。见 .cpp 的 kTracerNearAng ——
       没有它，玩家自己那发（起点就是枪口、整段在 1~3 m 内）会摊成一块 40+ px 的白片。 */
    float ang_tracer_near_ = 0.012f;
    /* 曳光弹单独一个增益，**不跟枪口焰共用 gain_**。
       原因见 .cpp 的阵营配色段：颜色乘上 1.35 再进 ACES，三通道一起被压向 1
       ⇒ 冷青与暖橙都变成白，"分清敌我"失效。1.15 刚过场景 glow 的 HDR 门槛
       （1.05），既留得住辉光，也留得住色相 —— 亮度让位于可辨性。 */
    float tracer_gain_ = 1.15f;

    /* ---- 玩家自己的弹道起点（枪口）---- 见 set_player_muzzle 的说明。
       pmuz_on_：VA_FX_PMUZ=0 整条关掉（回退旧口径 ⇒ 与加这一层之前逐像素相同），
                 用来做"确实修的是这条"的消融对照。 */
    bool  pmuz_on_ = true;
    bool  pmuz_valid_ = false;
    godot::Vector3 pmuz_;
    /* 诊断：本帧有几发弹走了"从枪口出发"这条路。0 而曳光弹数不为 0 ⇒
       玩家自己没在开枪，或者枪口坐标没喂进来（两种故障在画面上都是
       "弹道还是不从枪口出"，这个计数把它们分开）。 */
    int   last_pmuz_ = 0;

    // 诊断计数（每帧重置）
    int last_muz_ = 0, last_spark_ = 0, last_tracer_ = 0;
    int last_muz_ally_ = 0, last_muz_enemy_ = 0;
    /* 被近距剔除掉的光效数。**「一个光效都没画」和「画了但全被近距剔除」是两种故障**
       （后者多见于"以为自己在看战场、其实光效全生成在相机跟前"），
       画面上都表现为"没看见光"，这个数把它们分开。 */
    int near_culled_ = 0;
    /* 诊断（**累计**，不每帧清零）：光效落在相机**正面还是背后**。
       「光效明明在画、画面上却什么都没有」只有两种可能 —— 在相机背后，
       或者正面但太小/被雾吞掉。这两个计数就是把它们分开的那一条；
       只看截图是分不出来的（两种情况都表现为"没看见光"）。 */
    int cum_front_ = 0, cum_behind_ = 0;
    int peak_muz_ = 0, peak_tracer_ = 0;
    float dbg_t_ = 0.0f;
    int   attached_ = 0;
};

} // namespace volunteer_army
