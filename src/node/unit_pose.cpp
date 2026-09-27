// VolunteerArmyPC —— 单位"姿态"按状态换模型（见 unit_pose.h 的完整设计说明）
//
// 一句话：走路画行军模型，正在开火时改画 <键>_fire 那套据枪模型。
// 只读逻辑层的 Unit::fireCd，src/sim/ 一行不改。
#include "node/unit_pose.h"

#include <cstdlib>
#include <cstring>

#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "node/scene_builder.h"

using namespace godot;

namespace volunteer_army {
namespace {

// 据枪模型的键 = 行军键 + 这个后缀（见 unit_pose.h「键名怎么派生」）。
const char *kFireSuffix = "_fire";

float env_f(const char *p_key, float p_def) {
    const char *v = std::getenv(p_key);
    if (v == nullptr || *v == '\0') return p_def;
    return (float)std::strtod(v, nullptr);
}

bool env_flag(const char *p_key, bool p_def) {
    const char *v = std::getenv(p_key);
    if (v == nullptr || *v == '\0') return p_def;
    return !(std::strcmp(v, "0") == 0 || std::strcmp(v, "false") == 0);
}

} // namespace

// ============================================================ setup
void UnitPose::setup() {
    enabled_ = env_flag("VA_POSE", true);
    if (!enabled_) {
        UtilityFunctions::print(String::utf8(
            "[pose] VA_POSE=0 —— 姿态切换关闭（全队只有行军姿，画面与未加本层时逐像素相同）"));
        return;
    }

    dbg_ = env_flag("VA_DBG_POSE", false);
    dy_ = env_f("VA_POSE_DY", 0.0f);

    /* 只在**缺据枪模型**时才打印（[unit] 缺模型文件 …，由 load_unit_proto 打），
       这里不逐键探测 —— 11 个键里今天只有 1 个做了，开局就刷 10 行"缺文件"
       会把真正的信息淹掉。谁第一个开枪、谁就先触发那一行。 */
    UtilityFunctions::print(
        String::utf8("[pose] 姿态切换就绪：开火时换 <键>_fire 模型（判据 fireCd>0，"
                     "缺该模型则安静回退行军姿）垂直微调 "),
        String::num((double)dy_, 3), String::utf8(" m"));
}

// ============================================================ 每帧每单位
Node3D *UnitPose::resolve(std::size_t p_index, Node3D *p_walk, const std::string &p_walk_key,
                          const va::Unit &p_u, Node *p_parent, std::string &r_key) {
    r_key = p_walk_key;
    /* 本层关闭 / 没有行军节点 / 没有挂载点 —— 一律原样返回。
       这里是 VA_POSE=0 的"零代价"落点：连一个 Godot setter 都不调。 */
    if (!enabled_ || p_walk == nullptr || p_parent == nullptr) return p_walk;

    /* 藏着（不信）的单位不建第二个模型。这一条同时管住两件事：
       ① 玩家自己 —— 第一人称不画身体，但它是全队开枪最勤的那个，
          不判可见性就会给它单独建一个永远不显示的据枪模型；
       ② 检阅台期（VA_UNIT_SHOW）—— refs_.units 整组是 set_visible(false) 的，
          陈列排与关卡逻辑同时在跑，放行的话陈列排会按战局里的开火状态乱换模型。 */
    if (!p_walk->is_visible()) {
        release(p_index);
        return p_walk;
    }

    if (p_u.fireCd <= 0.0f) {
        std::map<std::size_t, Entry>::iterator it = fire_.find(p_index);
        if (it != fire_.end()) {
            if (it->second.node != nullptr && it->second.node->is_visible()) {
                it->second.node->set_visible(false);
            }
            it->second.firing = false;
        }
        return p_walk;
    }

    // 想要据枪态。缺模型时这里会留下一块 failed 墓碑，免得每帧重试一遍。
    Entry &e = fire_[p_index];
    if (e.failed) {
        ++no_asset_;
        return p_walk;
    }

    if (e.node == nullptr) {
        const std::string fkey = p_walk_key + kFireSuffix;
        Node3D *n = make_unit_node_by_key(fkey, p_parent);
        if (n == nullptr) {
            /* 缺文件是**可预期**的（其余 10 个键还没做），不是错误 ——
               make_unit_node_by_key 内部已经打过一行"[unit] 缺模型文件 …"。
               这里只记墓碑、静默回退，绝不 push_error：日志里有没有 ERROR
               是本工程的回归判据。 */
            e.failed = true;
            ++no_asset_;
            return p_walk;
        }
        // 与行军节点同一个父节点 —— 变换才可比（见 unit_pose.h）。
        p_parent->add_child(n);
        e.node = n;
        e.key = fkey;
        ++created_;
        if (dbg_) {
            UtilityFunctions::print(String::utf8("[pose] 新建据枪节点 #"), (int)p_index,
                                    String::utf8(" 键 "), String::utf8(fkey.c_str()));
        }
    }

    /* 变换**整份抄**行军节点这一帧的：地形抬升 / 朝向平滑 / 倒地侧翻 /
       跑动起伏这四层都已经在里面了，自己再算一遍就是第二次实现的机会。 */
    Transform3D xf = p_walk->get_transform();
    if (dy_ != 0.0f) {
        // 右乘 = 在模型**局部**空间里上移（本工程单位的局部 +Y 恒为"上"，
        // unit_transform 的基只有绕 Y 的偏航，倒地时才有绕 Z 的侧翻）。
        xf = xf * Transform3D(Basis(), Vector3(0.0f, dy_, 0.0f));
    }
    e.node->set_transform(xf);
    if (!e.node->is_visible()) e.node->set_visible(true);
    if (p_walk->is_visible()) p_walk->set_visible(false);

    if (!e.firing) {
        e.firing = true;
        ++switched_;
        if (dbg_) {
            UtilityFunctions::print(String::utf8("[pose] #"), (int)p_index, " ",
                                    String::utf8(p_walk_key.c_str()),
                                    String::utf8(" → 据枪（fireCd="), String::num((double)p_u.fireCd, 3),
                                    String::utf8("s）"));
        }
    }

    // ⚠️ 键必须跟着换 —— 双腿层的材质缓存是**按 key** 索引的（见 unit_pose.h）。
    r_key = e.key;
    return e.node;
}

bool UnitPose::firing(std::size_t p_index) const {
    if (!enabled_) return false;
    std::map<std::size_t, Entry>::const_iterator it = fire_.find(p_index);
    if (it == fire_.end()) return false;
    return it->second.node != nullptr && it->second.node->is_visible();
}

// ============================================================ 收 / 清
void UnitPose::release(std::size_t p_index) {
    std::map<std::size_t, Entry>::iterator it = fire_.find(p_index);
    if (it == fire_.end()) return;
    it->second.firing = false;
    if (it->second.node != nullptr && it->second.node->is_visible()) {
        it->second.node->set_visible(false);
    }
}

void UnitPose::reset() {
    for (std::map<std::size_t, Entry>::iterator it = fire_.begin(); it != fire_.end(); ++it) {
        if (it->second.node != nullptr) {
            // 先藏再 queue_free：queue_free 是延迟的，本帧剩下的部分它还会画一次。
            it->second.node->set_visible(false);
            it->second.node->queue_free();
        }
    }
    fire_.clear();
    switched_ = 0;
    no_asset_ = 0;
    created_ = 0;
}

// ============================================================ 诊断
void UnitPose::tick_diag(double p_wall_delta, double p_sim_t) {
    if (!dbg_ || !enabled_) return;
    dbg_acc_ += p_wall_delta;
    if (dbg_acc_ < 0.5) return;
    dbg_acc_ = 0.0;

    int live = 0;
    for (std::map<std::size_t, Entry>::const_iterator it = fire_.begin(); it != fire_.end(); ++it) {
        if (it->second.node != nullptr && it->second.node->is_visible()) ++live;
    }
    UtilityFunctions::print(String::utf8("[pose] 战局 "), String::num(p_sim_t, 1),
                            String::utf8("s 当前据枪 "), live,
                            String::utf8(" 个 · 本区间切换 "), switched_ - dbg_switched_,
                            String::utf8(" 次（累计 "), switched_,
                            String::utf8("）· 缺模型放弃累计 "), no_asset_,
                            String::utf8(" 单位帧"));
    dbg_switched_ = switched_;
}

String UnitPose::dump() const {
    String s = String::utf8("姿态切换层：");
    if (!enabled_) return s + String::utf8("VA_POSE=0 关闭");

    int nodes = 0, live = 0, failed = 0;
    for (std::map<std::size_t, Entry>::const_iterator it = fire_.begin(); it != fire_.end(); ++it) {
        if (it->second.node != nullptr) {
            ++nodes;
            if (it->second.node->is_visible()) ++live;
        }
        if (it->second.failed) ++failed;
    }
    s += String::utf8("据枪节点 ");
    s += String::num(nodes);
    s += String::utf8(" 个（累计建过 ");
    s += String::num(created_);
    s += String::utf8("）· 当前据枪 ");
    s += String::num(live);
    s += String::utf8(" 个 · 切换 ");
    s += String::num(switched_);
    s += String::utf8(" 次 · 缺模型放弃 ");
    s += String::num(no_asset_);
    s += String::utf8(" 单位帧（缺模型的");
    /* ⚠️ 这里数的是**单位槽位**（fire_ 里 failed=true 的条目），不是"缺了几把模型"。
       同一把缺模型会被多个单位各占一条 —— 比如三个敌步枪手各自触发一次。 */
    s += String::utf8("单位 ");
    s += String::num(failed);
    s += String::utf8(" 个）");
    if (dy_ != 0.0f) {
        s += String::utf8(" · 垂直微调 ");
        s += String::num((double)dy_, 3);
        s += String::utf8(" m");
    }
    return s;
}

} // namespace volunteer_army
