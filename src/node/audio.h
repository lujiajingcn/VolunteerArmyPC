#pragma once
// VolunteerArmyPC —— 音效层（P0：战斗可闻批次）
//
// 【为什么这一层只有一个入口】逻辑层 `src/sim/` 不认识引擎，需要出声的地方一律
// 通过 `va::SimEvents::on_sfx(id, x, y, gain, local)` 往外抛（va_world.h:27）。
// 本类就是那个回调的落点 —— 逻辑层一行不用改，将来换音频后端也只动这里。
//
// 【素材从哪来】`tools/gen_sfx.py` 程序化合成 → `assets/audio/<id>.wav`。
// 声学设计照搬网页版 ../VolunteerArmy/index.html（那里的 SND / SYN / BOOM 三张表），
// 脚本头部列了逐条出处。**没有一个是采样素材**，与工程「程序化生成」的路线一致。
//
// 【怎么读出来】音频**从没经过 Godot 编辑器的导入工序**（跑生成脚本才落盘），
// 所以 res:// 下没有 .import / .sample 缓存。走 ResourceLoader 会报
// "No loader found for resource" —— 与 hud.cpp:1955-1991 处理五张 PNG 是同一个坑，
// 那里用 Image::load_from_file 绕开，这里用 AudioStreamWAV::load_from_file。
// 两条路都试、谁成用谁（万一有人用编辑器打开过工程把 wav 导入了，走导入那条更省）。
//
// 【空间化：交给引擎，不自己算】
// 网页版自己实现了衰减/声像/空气吸收；PC 版这里**只做距离剔除与基础增益**，
// 距离衰减与左右声像交给 AudioStreamPlayer3D（`unit_size = def.ref`，
// 引擎的 INVERSE_DISTANCE 在 d = ref 处正好降到一半，与网页版 SND 表里
// 「ref：衰减参考距离（约衰减到一半）」是同一条口径）。
//   · 已知差异 1：网页版指数是 1.25，引擎是 1.0 —— 2 倍参考距离处 0.296 vs 0.333，
//     约 1 dB 之差，听不太出来，但**不是等价**。
//   · 已知差异 2：**不做空气吸收**（远处高频的额外衰减）。要补得加总线滤波器。
//
// 【限流】网页版有每 id 最短间隔 + 「100 ms 内最多 26 个声源」两条。这里两条都有：
// 前者是 kGap 表（照抄 index.html:2907-2913），后者是语音池大小（kSpatialVoices）。
//
// 【随机数纪律】抖动 pitch 用的是本地 mt19937，**绝不碰 va::Rng** ——
// 那是有种子的战斗随机流，借它一次就会让「实机」与「va_sweep 离线仿真」错开
// （网页版 index.html:2853 也专门写了这条）。

#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string_name.hpp>

namespace volunteer_army {

// 音效的空间参数。照抄网页版 SND 表（index.html:2857-2906）。
//
// 【单位：ref / max 是**逻辑层单位**，不是米 —— 这是个真踩过的坑】
// 网页版的世界本身就是 2200×1300 的逻辑单位，那里的 `ref: 240` 说的是
// 240 逻辑单位 = 12 米。本工程的映射是 `to3(x) = x * S`（scene_builder.h:29），
// S = 0.05，而 **1 Godot 单位 = 1 米**（同处注释：2200×1300 → 110×65 米的战场，
// 眼高是直接把 1.65 传进 to3 的第三个参数，不是 min(1.65/S)）。
//   ⇒ 距离剔除按逻辑单位比（与网页版逐字一致）；
//     传给引擎的 unit_size / max_distance 必须**乘 S 换成米**。
//   第一版漏了这步：把 240 当米传进去 = 衰减参考距离被放大到 240 m，
//   全图几乎等响，只有极远的车声才掉。日志探针里那句 "距离 298.6m" 同病，
//   它其实是 14.9 m。
struct SndDef {
    const char *id;
    float ref;     // 衰减参考距离（约衰减到一半），米
    float max;     // 最远可闻距离，米
    float g;       // 基础增益（线性）
    bool  local;   // 不参与空间化（居中的「自身上」/ HUD 音）
};

// 按 id 查参数；找不到返回 nullptr。
const SndDef *snd_lookup(const std::string &p_id);

class Audio {
public:
    // 建语音池 + 载入素材 + 铺一条带限幅的 SFX 总线。
    // p_parent：语音池挂哪（传 WorldSim 自己）
    // p_cam   ：听者挂哪（传相机；听者必须是相机的子节点才能跟着转头）
    void setup(godot::Node3D *p_parent, godot::Camera3D *p_cam);

    // 播一个音效。gain < 0 表示调用方没指定（按 1.0 处理）。
    // 【注意】本函数可能在 va::step_once 内部被同步调用（on_sfx 就是那时响的），
    // 所以它必须便宜、不能分配、不能抛。
    void play(const std::string &p_id, float p_x, float p_y, float p_gain, bool p_local);

    // 听者位置（逻辑层坐标）。WorldSim 每帧喂一次；音频只在帧间被触发，
    // 用上一帧的听者位置听不出错位（玩家一帧最多走几厘米）。
    void set_listener(float p_x, float p_y) { lx_ = p_x; ly_ = p_y; }

    bool enabled() const { return enabled_; }
    bool ready() const { return loaded_ > 0; }
    int  loaded() const { return loaded_; }
    int  attempted() const { return attempted_; }
    int  missing() const { return attempted_ - loaded_; }
    long played() const { return n_played_; }
    long dropped() const { return n_dropped_; }

    // 载入失败（或压根没素材）的 id 清单，给日志用（截断到前若干个）。
    godot::String missing_summary(int p_max = 6) const;
    // 逻辑层发了、但 kSnd 表里没有的 id（形如 `ambush=12`）。这类事件**不会报错**，
    // 只会静默无声 —— 名字记下来，日志里一眼能看出差哪几个、各漏了多少次。
    godot::String unknown_summary(int p_max = 8) const;
    // 触发次数按 id 汇总（VA_DBG_SFX=1 收尾时用）。
    godot::String played_summary() const;

private:
    struct Slot {
        godot::AudioStreamPlayer3D *p = nullptr;
        float pitch = 1.0f;
    };
    struct LSlot {
        godot::AudioStreamPlayer *p = nullptr;
        float pitch = 1.0f;
    };

    godot::AudioStreamPlayer3D *pick_voice();
    godot::AudioStreamPlayer   *pick_local_voice();

    std::vector<godot::Ref<godot::AudioStream>> streams_;   // 下标与 kSnd 表一一对应
    std::vector<Slot>  spatial_;
    std::vector<LSlot> local_;
    std::vector<float> last_t_;                             // 每 id 上次触发时刻（秒）
    std::vector<long>  count_;                              // 每 id 触发次数（诊断）
    std::vector<std::pair<std::string, long>> unknown_;     // 未登记 id + 次数（诊断）

    bool  enabled_ = true;
    bool  setup_done_ = false;
    int   loaded_ = 0;
    int   attempted_ = 0;
    float lx_ = 0.0f, ly_ = 0.0f;
    long  n_played_ = 0;
    long  n_dropped_ = 0;
    int   voice_steal_ = 0;
    int   rr_spatial_ = 0;
    int   rr_local_ = 0;
    double t0_ = 0.0;
    godot::StringName bus_{"Master"};
    std::mt19937 rng_{0xC0FFEEu};
};

} // namespace volunteer_army
