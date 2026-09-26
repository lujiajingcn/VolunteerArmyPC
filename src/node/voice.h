#pragma once
// VolunteerArmyPC —— 任务介绍语音（VO）层
//
// 【要解决的问题】游戏从主菜单到进关，屏幕上有字、耳朵里一声没有：简报页把
// "这是哪、打谁、干什么"写在那儿，玩家得低头读。这一层把它念出来。
//
// 【为什么单独一层，不塞进 Audio】Audio 那层是**战斗音效**：按 id 查表、
// 空间化、限流、pitch 抖动、24+6 个声部互相抢占。语音的需求正好相反：
//   · 独占 —— 同一条语音不该叠着播，新的来了旧的就得停（Audio 是"随便挑个
//     空闲声部"，不保证独占，可能出现两句话叠在一起）；
//   · 不空间化、不抖动 —— 这是**旁白**，不是世界里某个东西发出的声音，
//     跟着相机转头或者抖音高都不对；
//   · 长 —— 一条 15~18 秒，塞进共享声部池会长期占着一个格子，
//     战斗最激烈的时候恰好少一路枪声。
// 所以给它一条自己的 AudioStreamPlayer + 自己的 id 表，互不干扰。
//
// 【素材怎么来】`tools/gen_voice.py` 用本机 Windows TTS 离线合成（男声 Kangkang），
// 后处理与其它素材同一口径：44100 Hz / 16-bit / mono / 峰值 0.95。
// 口播稿在那脚本顶部的 LINES 表里，同时产出 `node/voice_lines.h` ——
// 于是"念出来的"和"写下来的"永远是同一句话。
//
// 【每层一个总开关】VA_VO=0 关掉整层（素材不载、声部不建），
// 关掉之后的表现逐像素等同加这层之前 —— 与音效/光效/动作三层同一条纪律。

#include <string>
#include <vector>

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/string.hpp>

namespace volunteer_army {

class Voice {
public:
    // p_parent：语音声部挂哪（传 WorldSim 自己）
    void setup(godot::Node *p_parent);

    // 播一条。p_id 是 vo_lines() 里的 id（**不带 `vo_` 前缀**，文件名才带）。
    // 独占语义：正在播的一律先停掉再放新的 —— 见文件头"为什么不塞进 Audio"。
    void play(const std::string &p_id);

    void stop();

    bool enabled() const { return enabled_; }
    bool setup_done() const { return setup_done_; }
    bool playing() const;
    // 正在播的 id（没在播时为空串）。
    const std::string &current() const { return cur_; }
    // 还剩多少秒（没在播时 0）。
    double remaining() const;
    // 这条语音的原文（给日志 / 字幕）；找不到返回 nullptr。
    const char *text_of(const std::string &p_id) const;

    int loaded() const { return loaded_; }
    int attempted() const { return attempted_; }
    int missing() const { return attempted_ - loaded_; }
    long played_count() const { return n_played_; }
    // 载入失败的 id 清单（截断到前若干个），给日志用。
    godot::String missing_summary(int p_max = 6) const;

private:
    godot::AudioStreamPlayer *ply_ = nullptr;
    std::vector<godot::Ref<godot::AudioStream>> streams_;   // 下标与 vo_lines() 一一对应
    std::vector<std::string> ids_;
    std::vector<const char *> texts_;
    bool enabled_ = true;
    bool setup_done_ = false;
    bool dbg_ = false;
    int  loaded_ = 0;
    int  attempted_ = 0;
    std::string cur_;
    long n_played_ = 0;
};

} // namespace volunteer_army
