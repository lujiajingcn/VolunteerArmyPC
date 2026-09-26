// VolunteerArmyPC —— 任务介绍语音层实现（设计说明见 voice.h 顶部）
#include "node/voice.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "node/audio.h"        // load_wav_resource（与音效层共用同一份读盘实现）
#include "node/voice_lines.h"  // 生成的 id / 文本 / 时长表

namespace volunteer_army {
namespace {

/* 语音的基础增益。素材已经归一化到 0.95 峰，但**峰值一样不等于响度一样**：
   枪声是一瞬间的尖峰，语音是持续十五秒的能量，同样峰值下语音明显更"顶"。
   先压到 0.85（约 -1.4 dB）；实机若还嫌顶，改这一处就全局生效。 */
constexpr float kVoGain = 0.85f;

bool env_flag(const char *p_key, bool p_def) {
    const char *v = std::getenv(p_key);
    if (v == nullptr || *v == '\0') return p_def;
    return !(std::strcmp(v, "0") == 0 || std::strcmp(v, "false") == 0);
}

} // namespace

// ============================================================ 建层
void Voice::setup(godot::Node *p_parent) {
    if (setup_done_) return;
    setup_done_ = true;
    enabled_ = env_flag("VA_VO", true);
    dbg_     = env_flag("VA_DBG_VO", false);

    const std::vector<VoLine> &lines = vo_lines();
    streams_.assign(lines.size(), godot::Ref<godot::AudioStream>());
    ids_.reserve(lines.size());
    texts_.reserve(lines.size());
    for (const VoLine &l : lines) {
        ids_.push_back(l.id);
        texts_.push_back(l.text);
    }

    if (!enabled_) {
        godot::UtilityFunctions::print(godot::String::utf8(
            "[vo] VA_VO=0 —— 语音层关闭（素材不载、声部不建）"));
        return;
    }
    if (p_parent == nullptr) {
        enabled_ = false;
        return;
    }

    /* 一条自己的声部。走 Master 而不是音效层的 SFX 总线，理由有两条：
       ① SFX 总线上挂着限幅器，齐射/连爆时它会把整体压下来 —— 旁白不该
          跟着枪声一起被压；② 旁白属于"界面层声音"，跟 Master 走，
          玩家在设置里调总音量时它跟着走，语义是对的。 */
    ply_ = memnew(godot::AudioStreamPlayer);
    ply_->set_name(godot::StringName("VoiceOver"));
    ply_->set_bus(godot::StringName("Master"));
    ply_->set_volume_db(godot::UtilityFunctions::linear_to_db(kVoGain));
    p_parent->add_child(ply_);

    for (size_t i = 0; i < ids_.size(); ++i) {
        ++attempted_;
        const godot::String path = godot::String::utf8("res://assets/audio/vo_") +
                                   godot::String::utf8(ids_[i].c_str()) +
                                   godot::String::utf8(".wav");
        godot::Ref<godot::AudioStream> s = load_wav_resource(path);
        if (s.is_valid()) {
            streams_[i] = s;
            ++loaded_;
        }
    }

    godot::UtilityFunctions::print(
        godot::String::utf8("[vo] 语音载入 "), loaded_, "/", attempted_,
        missing() > 0 ? godot::String::utf8("  缺：") + missing_summary()
                      : godot::String());

    /* 逐条报「引擎解析出来的时长」，并与脚本烘素材时记下的时长对照。
       单看"文件在、对象有效"不够 —— 数据段被截断时对象照样有效，只是没声。
       两边对得上，才说明这条语音真的能完整播出来。 */
    if (dbg_) {
        for (size_t i = 0; i < streams_.size(); ++i) {
            if (!streams_[i].is_valid()) continue;
            const double got = (double)streams_[i]->get_length();
            const double want = (double)lines[i].dur;
            godot::UtilityFunctions::print(
                godot::String::utf8("[vo]   "), godot::String::utf8(ids_[i].c_str()),
                godot::String::utf8(" "), godot::String::num(got, 2),
                godot::String::utf8("s（脚本记 "), godot::String::num(want, 2),
                godot::String::utf8("s）"),
                std::fabs(got - want) > 0.25 ? godot::String::utf8("  ⚠ 时长对不上")
                                             : godot::String());
        }
    }
}

// ============================================================ 播 / 停
void Voice::play(const std::string &p_id) {
    if (!enabled_ || ply_ == nullptr) return;

    int idx = -1;
    for (size_t i = 0; i < ids_.size(); ++i) {
        if (ids_[i] == p_id) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0) {
        // 静默失声不会有任何报错（与音效层"未登记 id"同一条教训），所以这里要吭声
        godot::UtilityFunctions::print(
            godot::String::utf8("[vo] 没有这条语音：") +
            godot::String::utf8(p_id.c_str()));
        return;
    }
    if (!streams_[(size_t)idx].is_valid()) {
        godot::UtilityFunctions::print(
            godot::String::utf8("[vo] 素材缺失，播不了：vo_") +
            godot::String::utf8(p_id.c_str()));
        return;
    }

    // 独占：先停再放。正在播的那条被截断是**要**的行为 ——
    // 连按重播时不该叠成两重唱。
    ply_->stop();
    ply_->set_stream(streams_[(size_t)idx]);
    ply_->play();
    cur_ = p_id;
    ++n_played_;

    if (dbg_) {
        const char *tx = text_of(p_id);
        godot::UtilityFunctions::print(
            godot::String::utf8("[vo] 播放 "), godot::String::utf8(p_id.c_str()),
            godot::String::utf8(" ("),
            godot::String::num((double)streams_[(size_t)idx]->get_length(), 2),
            godot::String::utf8("s) "),
            godot::String::utf8(tx != nullptr ? tx : ""));
    }
}

void Voice::stop() {
    if (ply_ != nullptr) ply_->stop();
    cur_.clear();
}

// ============================================================ 查询
bool Voice::playing() const {
    return ply_ != nullptr && ply_->is_playing();
}

double Voice::remaining() const {
    if (ply_ == nullptr || !ply_->is_playing()) return 0.0;
    godot::Ref<godot::AudioStream> s = ply_->get_stream();
    if (!s.is_valid()) return 0.0;
    const double len = (double)s->get_length();
    const double pos = (double)ply_->get_playback_position();
    return len > pos ? len - pos : 0.0;
}

const char *Voice::text_of(const std::string &p_id) const {
    for (size_t i = 0; i < ids_.size(); ++i) {
        if (ids_[i] == p_id) return texts_[i];
    }
    return nullptr;
}

godot::String Voice::missing_summary(int p_max) const {
    godot::String out;
    int n = 0;
    for (size_t i = 0; i < streams_.size(); ++i) {
        if (streams_[i].is_valid()) continue;
        if (n > 0) out += godot::String::utf8(" ");
        out += godot::String::utf8(ids_[i].c_str());
        if (++n >= p_max) {
            out += godot::String::utf8(" …");
            break;
        }
    }
    return out;
}

} // namespace volunteer_army
