// VolunteerArmyPC —— 音效层实现（见 audio.h 顶部的设计说明）
#include "node/audio.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <godot_cpp/classes/audio_effect_limiter.hpp>
#include <godot_cpp/classes/audio_listener3d.hpp>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "node/scene_builder.h"   // to3 / ground_h

namespace volunteer_army {
namespace {

// ============================================================ 参数表
// 【这张表的顺序就是 streams_ 的下标顺序】改顺序会让素材与参数错位，
// 加 id 请**追加**，同时跑一次 tools/gen_sfx.py 生成对应 wav。
//
// 数值出处：index.html:2857-2906 的 SND 表（逐个核对过，不是转述）。
static const SndDef kSnd[] = {
    // id            ref     max      g     local
    { "rifle",      240.0f, 2000.0f, 0.50f, false },
    { "mg",         300.0f, 2200.0f, 0.52f, false },
    { "sniper",     440.0f, 3200.0f, 0.62f, false },
    { "enemyRifle", 250.0f, 1800.0f, 0.38f, false },
    { "enemyMG",    320.0f, 2000.0f, 0.40f, false },
    { "explosion",  620.0f, 3400.0f, 0.95f, false },
    { "mine",       560.0f, 3000.0f, 0.90f, false },
    { "barrel",     430.0f, 2400.0f, 0.78f, false },
    { "grenade",    400.0f, 2200.0f, 0.72f, false },
    { "rocketBoom", 520.0f, 2800.0f, 0.85f, false },
    { "shellBoom",  640.0f, 3600.0f, 1.00f, false },
    { "vehicleBoom",700.0f, 3800.0f, 1.00f, false },
    { "cannon",     720.0f, 4200.0f, 1.00f, false },
    { "hit",          0.0f,    0.0f, 0.30f, true  },   // 命中反馈，居中
    { "crack",      110.0f,  460.0f, 0.42f, false },   // 子弹擦耳
    { "impact",     170.0f,  950.0f, 0.30f, false },   // 打在土石上
    { "clang",      240.0f, 1300.0f, 0.34f, false },   // 打在装甲上
    { "metal",      280.0f, 1500.0f, 0.34f, false },   // 车体金属余音
    { "flesh",      130.0f,  700.0f, 0.26f, false },
    { "body",       150.0f,  800.0f, 0.34f, false },   // 人倒地
    { "allyDown",   210.0f, 1300.0f, 0.40f, false },
    { "hurt",         0.0f,    0.0f, 0.34f, true  },   // 自己挨枪，居中
    { "down",         0.0f,    0.0f, 0.45f, true  },   // 自己倒下 + 耳鸣
    { "reloadStart", 90.0f,  400.0f, 0.30f, false },
    { "reloadEnd",   90.0f,  400.0f, 0.32f, false },
};
constexpr int kSndN = (int)(sizeof(kSnd) / sizeof(kSnd[0]));

// 每种音效的最短触发间隔（秒）。出处 index.html:2907-2913。
// 没列到的用 kGapDefault —— 网页版默认 0.012。
struct SndGap { const char *id; float gap; };
static const SndGap kGap[] = {
    { "rifle", 0.024f }, { "mg", 0.026f }, { "enemyRifle", 0.030f },
    { "enemyMG", 0.032f }, { "sniper", 0.05f },
    { "crack", 0.05f }, { "impact", 0.03f }, { "clang", 0.03f },
    { "flesh", 0.045f }, { "body", 0.06f }, { "allyDown", 0.10f },
    { "hit", 0.04f }, { "hurt", 0.18f },
    { "reloadStart", 0.06f }, { "reloadEnd", 0.06f },
};
constexpr int kGapN = (int)(sizeof(kGap) / sizeof(kGap[0]));
constexpr float kGapDefault = 0.012f;

// 语音池大小 = 网页版「100 ms 内最多 26 个新声源」那条预算的等价物。
// 空间声 24 路（一场遭遇战里十几把枪同时响够用）+ 居中声 6 路（自身/HUD 音）。
constexpr int kSpatialVoices = 24;
constexpr int kLocalVoices = 6;

// 总音量（线性）。网页版 Sound.vol 默认 0.62 —— 同样的余量，避免齐射时削顶。
constexpr float kMasterVol = 0.62f;

// 抖动：一个 id 只烘了一个 wav，靠 pitch 抖动补「连发不重样」。
// 音高类音效（命中提示音）不抖 —— 抖了像跑调的提示音。
constexpr float kPitchJitter = 0.04f;

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

double now_s(double p_t0) {
    const godot::Time *t = godot::Time::get_singleton();
    if (t == nullptr) return 0.0;
    return (double)t->get_ticks_usec() / 1e6 - p_t0;
}

// 载入一个 wav：先走编辑器导入那条（若存在），再直接读盘。
// 顺序与理由见 hud.cpp:1955-1991（那里是 PNG，这里是 wav，同一个坑）。
godot::Ref<godot::AudioStream> load_wav(const godot::String &p_res_path) {
    godot::ResourceLoader *rl = godot::ResourceLoader::get_singleton();
    if (rl != nullptr && rl->exists(p_res_path)) {
        godot::Ref<godot::AudioStream> s = rl->load(p_res_path);
        if (s.is_valid()) return s;
    }
    if (godot::FileAccess::file_exists(p_res_path)) {
        godot::Ref<godot::AudioStreamWAV> w = godot::AudioStreamWAV::load_from_file(p_res_path);
        if (w.is_valid()) return w;
    }
    return godot::Ref<godot::AudioStream>();
}

} // namespace

// ============================================================ 查表
const SndDef *snd_lookup(const std::string &p_id) {
    for (int i = 0; i < kSndN; ++i) {
        if (p_id == kSnd[i].id) return &kSnd[i];
    }
    return nullptr;
}

static int snd_index_of(const std::string &p_id) {
    for (int i = 0; i < kSndN; ++i) {
        if (p_id == kSnd[i].id) return i;
    }
    return -1;
}

static float gap_of(const std::string &p_id) {
    for (int i = 0; i < kGapN; ++i) {
        if (p_id == kGap[i].id) return kGap[i].gap;
    }
    return kGapDefault;
}

// ============================================================ setup
void Audio::setup(godot::Node3D *p_parent, godot::Camera3D *p_cam) {
    if (setup_done_) return;
    setup_done_ = true;
    enabled_ = env_flag("VA_SND", true);
    t0_ = now_s(0.0);
    streams_.assign((size_t)kSndN, godot::Ref<godot::AudioStream>());
    last_t_.assign((size_t)kSndN, -100.0f);
    count_.assign((size_t)kSndN, 0L);
    if (!enabled_) {
        godot::UtilityFunctions::print(godot::String::utf8(
            "[snd] VA_SND=0 —— 音频层关闭（素材不载、声源不建）"));
        return;
    }
    if (p_parent == nullptr) {
        enabled_ = false;
        return;
    }

    // ---- 听者：挂在相机下，跟着转头。不挂的话引擎会用 current 相机当听者，
    //      但那依赖「哪台相机是 current」这件隐式状态 —— 本工程在相机上踩过坑，
    //      这里显式建一个，省得以后加第二台相机时声像突然乱掉。
    if (p_cam != nullptr) {
        godot::AudioListener3D *lis = memnew(godot::AudioListener3D);
        p_cam->add_child(lis);
        lis->make_current();
    }

    // ---- SFX 总线 + 限幅。齐射/连爆时十几个声源叠加会削顶，引擎默认不兜这件事。
    //      挂一条自己的总线而不是动 Master，出问题摘掉就行。
    godot::AudioServer *as = godot::AudioServer::get_singleton();
    if (as != nullptr) {
        const int found = as->get_bus_index(godot::StringName("SFX"));
        if (found >= 0) {
            bus_ = godot::StringName("SFX");
        } else {
            as->add_bus();
            const int idx = as->get_bus_count() - 1;
            as->set_bus_name(idx, godot::String("SFX"));
            as->set_bus_send(idx, godot::StringName("Master"));
            godot::Ref<godot::AudioEffectLimiter> lim;
            lim.instantiate();
            as->add_bus_effect(idx, lim);
            bus_ = godot::StringName("SFX");
        }
    }

    // ---- 语音池
    spatial_.resize((size_t)kSpatialVoices);
    for (int i = 0; i < kSpatialVoices; ++i) {
        godot::AudioStreamPlayer3D *v = memnew(godot::AudioStreamPlayer3D);
        v->set_bus(bus_);
        // 衰减交给引擎（见 audio.h 里的口径说明）；声像与多普勒由引擎按位置算。
        v->set_attenuation_model(godot::AudioStreamPlayer3D::ATTENUATION_INVERSE_DISTANCE);
        v->set_doppler_tracking(godot::AudioStreamPlayer3D::DOPPLER_TRACKING_DISABLED);
        v->set_panning_strength(1.0f);
        p_parent->add_child(v);
        spatial_[(size_t)i].p = v;
    }
    local_.resize((size_t)kLocalVoices);
    for (int i = 0; i < kLocalVoices; ++i) {
        godot::AudioStreamPlayer *v = memnew(godot::AudioStreamPlayer);
        v->set_bus(bus_);
        p_parent->add_child(v);
        local_[(size_t)i].p = v;
    }

    // ---- 载入素材
    for (int i = 0; i < kSndN; ++i) {
        const godot::String path = godot::String("res://assets/audio/") +
                                   godot::String(kSnd[i].id) + godot::String(".wav");
        ++attempted_;
        godot::Ref<godot::AudioStream> s = load_wav(path);
        if (s.is_valid()) {
            streams_[(size_t)i] = s;
            ++loaded_;
        }
    }
    godot::UtilityFunctions::print(
        godot::String::utf8("[snd] 音效载入 "), loaded_, "/", attempted_,
        godot::String::utf8("（总线 "), godot::String(bus_),
        godot::String::utf8("，声源 "), kSpatialVoices, godot::String::utf8("+"),
        kLocalVoices, godot::String::utf8("）"),
        missing() > 0 ? godot::String::utf8("  缺：") + missing_summary()
                      : godot::String());

    /* 逐条报「引擎解析出来的时长」。这条判据比"文件存在"强一档：
       load_from_file 返回有效对象只说明 RIFF 头被认了，**时长对得上**才说明
       数据段真被读了进来。素材是脚本烘的、没走编辑器导入，把"文件在、内容是
       空/截断"这一类拦在这里 —— 否则表现是"有对象但没声"，最难查。 */
    if (env_flag("VA_DBG_SFX", false)) {
        for (int i = 0; i < kSndN; ++i) {
            if (!streams_[(size_t)i].is_valid()) continue;
            godot::UtilityFunctions::print(
                godot::String::utf8("[snd]   "), godot::String(kSnd[i].id),
                godot::String::utf8(" "),
                godot::String::num(streams_[(size_t)i]->get_length(), 2),
                godot::String::utf8("s"));
        }
    }
}

// ============================================================ 声部
godot::AudioStreamPlayer3D *Audio::pick_voice() {
    const int n = (int)spatial_.size();
    if (n == 0) return nullptr;
    for (int i = 0; i < n; ++i) {
        const int k = (rr_spatial_ + i) % n;
        if (spatial_[(size_t)k].p != nullptr && !spatial_[(size_t)k].p->is_playing()) {
            rr_spatial_ = (k + 1) % n;
            return spatial_[(size_t)k].p;
        }
    }
    // 全忙：抢轮转到的下一个。被抢的那路会「咔」一下断掉，
    // 但连射时这比整发丢音更不容易听出来。
    const int k = rr_spatial_;
    rr_spatial_ = (rr_spatial_ + 1) % n;
    ++voice_steal_;
    if (spatial_[(size_t)k].p != nullptr) spatial_[(size_t)k].p->stop();
    return spatial_[(size_t)k].p;
}

godot::AudioStreamPlayer *Audio::pick_local_voice() {
    const int n = (int)local_.size();
    if (n == 0) return nullptr;
    for (int i = 0; i < n; ++i) {
        const int k = (rr_local_ + i) % n;
        if (local_[(size_t)k].p != nullptr && !local_[(size_t)k].p->is_playing()) {
            rr_local_ = (k + 1) % n;
            return local_[(size_t)k].p;
        }
    }
    const int k = rr_local_;
    rr_local_ = (rr_local_ + 1) % n;
    ++voice_steal_;
    if (local_[(size_t)k].p != nullptr) local_[(size_t)k].p->stop();
    return local_[(size_t)k].p;
}

// ============================================================ 播放入口
void Audio::play(const std::string &p_id, float p_x, float p_y, float p_gain, bool p_local) {
    if (!enabled_ || loaded_ == 0) return;
    const int idx = snd_index_of(p_id);
    if (idx < 0) {
        // 未登记的 id：逻辑层加了新事件但这里没跟上。这类事件**不会报错**，
        // 只会静默无声 —— 是这条链路上最容易漏的一类 bug
        // （网页版 index.html:3168 也专门点了这条）。记下名字，别只留个数字。
        ++n_dropped_;
        for (auto &u : unknown_) {
            if (u.first == p_id) { u.second += 1; return; }
        }
        if (unknown_.size() < 64) unknown_.push_back(std::make_pair(p_id, 1L));
        return;
    }
    if (!streams_[(size_t)idx].is_valid()) return;

    const SndDef &def = kSnd[idx];
    const bool is_local = p_local || def.local || def.ref <= 0.0f;

    // 限流：同一 id 的最短间隔
    const double t = now_s(t0_);
    const float gap = gap_of(p_id);
    if (t - (double)last_t_[(size_t)idx] < (double)gap) return;
    last_t_[(size_t)idx] = (float)t;

    // 增益：基础 × 调用方 × 总音量
    const float caller = (p_gain >= 0.0f) ? p_gain : 1.0f;
    float g = def.g * caller * kMasterVol;

    float dist = 0.0f;
    if (!is_local) {
        // 【距离按逻辑单位算】与网页版逐字一致（listener() 取的也是逻辑坐标）。
        // ref / max 同样是逻辑单位，所以这条比较不需要任何换算。
        // 但**传给引擎的必须是米** —— 见下面 * S，那是本层最容易错的一处。
        const float dx = p_x - lx_, dy = p_y - ly_;
        dist = std::sqrt(dx * dx + dy * dy);
        if (dist > def.max) return;
    }
    if (g < 0.0015f) return;

    const float jitter = (p_id == "hit") ? 0.0f : kPitchJitter;
    std::uniform_real_distribution<float> d(-jitter, jitter);

    if (is_local) {
        godot::AudioStreamPlayer *v = pick_local_voice();
        if (v == nullptr) return;
        v->set_stream(streams_[(size_t)idx]);
        v->set_volume_db((float)godot::UtilityFunctions::linear_to_db((double)g));
        v->set_pitch_scale(1.0f + d(rng_));
        v->play();
    } else {
        godot::AudioStreamPlayer3D *v = pick_voice();
        if (v == nullptr) return;
        v->set_stream(streams_[(size_t)idx]);
        v->set_volume_db((float)godot::UtilityFunctions::linear_to_db((double)g));
        v->set_pitch_scale(1.0f + d(rng_));
        // ref 当 unit_size：引擎的 INVERSE_DISTANCE 在 d = ref 处降到一半，
        // 与 SND 表「ref：约衰减到一半」同一条口径。
        // **必须 * S**：def.ref 是逻辑单位，引擎要米（1 单位 = 1 米）。
        v->set_unit_size(def.ref * S);
        v->set_max_distance(def.max * S);
        v->set_global_position(to3(p_x, p_y, ground_h(p_x, p_y)));
        v->play();
        if (env_flag("VA_DBG_SFX", false)) {
            // 这里报**米**（dist * S），不是逻辑单位 —— 上一版直接印逻辑单位却写成 "m"，
            // 298.6 看着像 "298 米外开的枪"，其实是 14.9 m。数字带错单位比没数字更坏。
            godot::UtilityFunctions::print(
                godot::String::utf8("[snd] "), godot::String(p_id.c_str()),
                godot::String::utf8(" 距离 "), godot::String::num((double)(dist * S), 1),
                godot::String::utf8("m 音量 "),
                godot::String::num(godot::UtilityFunctions::linear_to_db((double)g), 1),
                godot::String::utf8("dB"));
        }
    }
    ++n_played_;
    count_[(size_t)idx] += 1;
}

// ============================================================ 诊断
godot::String Audio::missing_summary(int p_max) const {
    godot::String s;
    int shown = 0;
    for (int i = 0; i < kSndN; ++i) {
        if (streams_[(size_t)i].is_valid()) continue;
        if (shown >= p_max) { s += godot::String("…"); break; }
        if (shown > 0) s += godot::String(",");
        s += godot::String(kSnd[i].id);
        ++shown;
    }
    return s;
}

godot::String Audio::unknown_summary(int p_max) const {
    godot::String s;
    int shown = 0;
    for (const auto &u : unknown_) {
        if (shown >= p_max) { s += godot::String::utf8(" …"); break; }
        if (shown > 0) s += godot::String::utf8(" ");
        s += godot::String(u.first.c_str());
        s += godot::String("=");
        s += godot::String::num_real((double)u.second);
        ++shown;
    }
    return s;
}

godot::String Audio::played_summary() const {
    godot::String s;
    for (int i = 0; i < kSndN; ++i) {
        if (count_[(size_t)i] == 0) continue;
        if (s.length() > 0) s += godot::String("  ");
        s += godot::String(kSnd[i].id);
        s += godot::String("=");
        s += godot::String::num_real((double)count_[(size_t)i]);
    }
    return s;
}

} // namespace volunteer_army
