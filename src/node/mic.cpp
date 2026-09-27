#include "node/mic.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

#include <godot_cpp/variant/utility_functions.hpp>

/* ⚠️ 两条后端的头只允许出现在本文件（见 mic.h 顶部）。

   ---- 后端①：C++/WinRT（OneCore DNN） ----
   本工程用的是 Windows SDK 自带的投影头（Include/<ver>/cppwinrt/winrt/），
   不需要装 VS 的 C++/WinRT 扩展，也不需要跑 cppwinrt.exe 生成。
   注意 SDK 里还有一套同名但命名空间为 ABI:: 的 MIDL 头（Include/<ver>/winrt/），
   <winrt/xxx.h> 这条路径会落到哪一套由 INCLUDE 顺序决定 —— 实测落到 cppwinrt，
   所以这里统一用 winrt:: 命名空间，不要去碰 ABI::。

   ---- 后端②：SAPI5（老 Speech 8.0 桌面识别器） ----
   ⚠️ 刻意**不** include sphelper.h：它第 51 行 `#include <atlbase.h>`，
      会把这一层拖进 ATL。本层只用到它三个 helper
      （SpFindBestToken / SpGetDefaultTokenFromCategoryId / CSpEvent），
      每个都能用两三个裸 COM 调用等价替代 —— 代码多几行，依赖少一整个 ATL。
      sphelper 里那些结构体都在 sapi.h 里：SPPHRASE / SPPHRASERULE /
      SPPHRASEPROPERTY 都是 sapi.h 的完整定义（SPPHRASEPROPERTY 在 sapi.h:5776，
      属性链是 **pNextSibling** 不是 pNext）。别被 sapiddk.h 里那个
      SPPHRASEPROPERTYHANDLE 迷惑 —— 那是另一个东西。
*/
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Media.SpeechRecognition.h>

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>      // VARIANT（SPPHRASEPROPERTY::vValue 用到）
#include <sapi.h>
#include <wrl/client.h>   // Microsoft::WRL::ComPtr —— 只为了让几个临时 COM 对象不漏引用

namespace volunteer_army {

// ===========================================================================
//  小工具
// ===========================================================================
namespace {

namespace WSR = winrt::Windows::Media::SpeechRecognition;

// VA_DBG_MIC=1：把识别器状态、每句结果与置信度打到日志（与 VA_DBG_SFX 同一习惯）。
bool dbg_on() {
    static const bool on = [] {
        const char *v = std::getenv("VA_DBG_MIC");
        return v != nullptr && *v != '0' && *v != '\0';
    }();
    return on;
}

void dbg(const char *fmt, ...) {
    if (!dbg_on()) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    godot::UtilityFunctions::print(godot::String::utf8("[mic] "), godot::String::utf8(buf));
}

std::string from_wide(const wchar_t *w) {
    if (w == nullptr || *w == L'\0') return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    s.pop_back();   // 去掉结尾的 NUL
    return s;
}

std::wstring to_wide(const std::string &s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string hr_text(HRESULT hr) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%08lX", (unsigned long)hr);
    return std::string(b);
}

/* WinRT 的置信度是**三档枚举**，不是概率。
   映射到 0~1 只是为了让逻辑层的 parse_command(asr=...) 有东西可用 ——
   那里 conf = lerpf(conf, asr, 0.55)，本工程自己会对"战场噪声"再打折。 */
float conf_value(WSR::SpeechRecognitionConfidence c) {
    using C = WSR::SpeechRecognitionConfidence;
    if (c == C::High)   return 0.95f;
    if (c == C::Medium) return 0.80f;
    if (c == C::Low)    return 0.55f;
    return 0.30f;
}

/* SAPI5 的置信度：**用不了，不要假装能用。**
   实测（VA_MIC_WAV 喂 gen_mic_wav.py 烘的「全体开火 / 全体撤退 / 全体隐蔽」）：
     · SPPHRASE 的属性链是**空的**（pProperties == nullptr，一条都没有）；
       sapi.h 里也没有一个叫 Confidence 的 SPPROP_* 常量，只有
       HIGH/NORMAL/LOW_CONFIDENCE_THRESHOLD 那几个**阈值** —— 照名字类推必错。
     · 只有 Rule.SREngineConfidence 有值，量纲 0.09 ~ 0.30，**不是概率**：
       识别正确的「全体开火」0.304、「全体撤退」0.092，
       而识别成「全集隐蔽」的那条 0.089 —— 好与坏根本分不开。
   所以回一个**标称值** 0.80，与 OneCore 的 Medium 同档：老引擎（Speech 8.0）
   确实不如 DNN，但它吐出来的句子已经过了 parse_command 那套 50+ 动作词 +
   呼号 + 地点的严格匹配 —— 垃圾句子压根解析不出 cmd。
   真正的闸门是**语法匹配**与**战场噪声**（逻辑层自己按 noise 打折），
   不是这个数。引擎原值仍然打日志（VA_DBG_MIC），要复盘随时看得到。 */
float sapi_confidence(ISpRecoResult *p_res) {
    if (p_res == nullptr) return 0.80f;
    SPPHRASE *ph = nullptr;
    if (FAILED(p_res->GetPhrase(&ph)) || ph == nullptr) return 0.80f;

    if (dbg_on()) {
        int i = 0;
        for (const SPPHRASEPROPERTY *p = ph->pProperties; p != nullptr; p = p->pNextSibling, ++i) {
            char vb[96];
            if (p->vValue.vt == VT_R4) {
                std::snprintf(vb, sizeof(vb), "R4=%g", (double)p->vValue.fltVal);
            } else if (p->vValue.vt == VT_BSTR && p->vValue.bstrVal != nullptr) {
                std::snprintf(vb, sizeof(vb), "BSTR=%s", from_wide(p->vValue.bstrVal).c_str());
            } else {
                std::snprintf(vb, sizeof(vb), "vt=%d", (int)p->vValue.vt);
            }
            dbg("  prop %-28s %-24s engineConf=%.3f conf=%d",
                p->pszName != nullptr ? from_wide(p->pszName).c_str() : "(null)",
                vb, (double)p->SREngineConfidence, (int)p->Confidence);
        }
        dbg("  属性链 %d 条；rule.SREngineConfidence=%.3f rule.Confidence=%d（都不当概率用）",
            i, (double)ph->Rule.SREngineConfidence, (int)ph->Rule.Confidence);
    }
    ::CoTaskMemFree(ph);
    return 0.80f;
}

const char *env_str(const char *p_name, const char *p_dflt) {
    const char *v = std::getenv(p_name);
    return (v != nullptr && *v != '\0') ? v : p_dflt;
}

/* 找中文识别器 token。
   SpFindBestToken(SPCAT_RECOGNIZERS, L"Language=804") 的裸 COM 等价物就是
   ISpObjectTokenCategory::EnumTokens 的 ReqAttribs 参数 —— 一次调用，不需要
   helper。找不到 804 就退而求其次拿第一个（宁可识别不准，也别整个用不了）。 */
bool sapi_pick_token(ISpObjectToken **pp_tok, int *p_lcid, std::string &p_err) {
    *pp_tok = nullptr;
    Microsoft::WRL::ComPtr<ISpObjectTokenCategory> cat;
    HRESULT hr = ::CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                    IID_ISpObjectTokenCategory, (void **)cat.GetAddressOf());
    if (FAILED(hr)) { p_err = "token category " + hr_text(hr); return false; }
    hr = cat->SetId(SPCAT_RECOGNIZERS, FALSE);
    if (FAILED(hr)) { p_err = "SetId(RECOGNIZERS) " + hr_text(hr); return false; }

    Microsoft::WRL::ComPtr<IEnumSpObjectTokens> en;
    hr = cat->EnumTokens(L"Language=804", nullptr, en.GetAddressOf());
    ULONG n = 0;
    if (SUCCEEDED(hr) && en != nullptr) en->GetCount(&n);
    dbg("SAPI: Language=804 命中 %lu 个识别器 token", (unsigned long)n);
    if (n == 0) {
        en.Reset();
        hr = cat->EnumTokens(nullptr, nullptr, en.GetAddressOf());
        if (SUCCEEDED(hr) && en != nullptr) en->GetCount(&n);
        dbg("SAPI: 退到全部 token，共 %lu 个（当前系统没有中文识别器）", (unsigned long)n);
    }
    if (n == 0) { p_err = "没有可用的识别器 token"; return false; }

    hr = en->Item(0, pp_tok);
    if (FAILED(hr) || *pp_tok == nullptr) { p_err = "取 token " + hr_text(hr); return false; }

    // token id 打出来 —— 只靠"有没有选到"判断不了选的是不是中文那个，
    // 而选成 en-US 的症状是"什么都识别不出"，跟"麦克风坏了"长得一模一样。
    {
        LPWSTR id = nullptr;
        if (SUCCEEDED((*pp_tok)->GetId(&id)) && id != nullptr) {
            dbg("SAPI token = %s", from_wide(id).c_str());
            ::CoTaskMemFree(id);
        }
    }
    /* 读语言：SAPI 把 token 属性放在 <token>\Attributes 下。
       ISpObjectToken 自己的 GetStringValue 会去那儿找，但实测**返回不了**
       Language（拿到的是空 → lcid 退成 0），所以再显式开一次 Attributes 子键。
       lcid 只用于日志与 backend 名字，读不到不影响识别。 */
    LPWSTR lang = nullptr;
    if (SUCCEEDED((*pp_tok)->GetStringValue(L"Language", &lang)) && lang != nullptr) {
        *p_lcid = (int)::wcstol(lang, nullptr, 10);
        ::CoTaskMemFree(lang);
        lang = nullptr;
    }
    if (*p_lcid == 0) {
        Microsoft::WRL::ComPtr<ISpDataKey> attrs;
        if (SUCCEEDED((*pp_tok)->OpenKey(L"Attributes", attrs.GetAddressOf()))
            && attrs != nullptr
            && SUCCEEDED(attrs->GetStringValue(L"Language", &lang)) && lang != nullptr) {
            *p_lcid = (int)::wcstol(lang, nullptr, 10);
            ::CoTaskMemFree(lang);
        }
    }
    return true;
}

} // namespace

// ===========================================================================
//  Impl
//
//  ⚠️ 后端的装配/拆卸是 **Impl 的成员函数**，不是文件级自由函数 ——
//     因为 Impl 是 MicVoice 的**私有**嵌套类型，文件级函数够不着它
//     （实测报 C2248：无法访问 private struct）。做成成员函数还有个好处：
//     mic.h 一个字都不用改，winrt/sapi 的头仍然只在本 .cpp 出现。
// ===========================================================================
struct MicVoice::Impl {
    enum class Kind { None, Winrt, Sapi };
    Kind kind = Kind::None;

    // ---- 识别线程 -> 主线程（两条后端共用）----
    std::mutex mu;
    std::deque<std::pair<std::string, float>> queue;
    std::string interim;
    std::atomic<long> accepted{0};
    std::atomic<long> rejected{0};

    void push(std::string p_text, float p_conf) {
        std::lock_guard<std::mutex> lk(mu);
        queue.emplace_back(std::move(p_text), p_conf);
    }

    // ---- 后端① WinRT ----
    WSR::SpeechRecognizer rec{nullptr};
    // 类型名是 SpeechContinuousRecognitionSession（不是 SpeechRecognition...）——
    // 依据是 MIDL 头里的 ISpeechContinuousRecognitionSession，第一版照着
    // "SpeechRecognizer" 类推写错了。
    WSR::SpeechContinuousRecognitionSession session{nullptr};
    winrt::event_token tok_result{};
    winrt::event_token tok_hyp{};
    bool has_result_tok = false;
    bool has_hyp_tok = false;

    // ---- 后端② SAPI5 ----
    ISpRecognizer *s_rec = nullptr;
    ISpRecoContext *s_ctx = nullptr;
    ISpRecoGrammar *s_gram = nullptr;
    IUnknown *s_input = nullptr;      // ISpAudio（麦克风）或 ISpStream（VA_MIC_WAV）
    HANDLE s_quit = nullptr;          // 手动重置事件：通知 worker 退出
    HANDLE s_thread = nullptr;
    std::atomic<bool> s_run{false};
    int s_lcid = 0;                   // 识别器 token 声明的语言（804 = zh-CN）
    bool wav_mode = false;            // VA_MIC_WAV 给了：输入从"按键"改成"自动跑一遍"
    bool s_file_input = false;        // 当前这条后端真的把 wav 当输入了（只 SAPI 支持）
    bool s_wav_started = false;       // 取证模式：已经替玩家"按"过一次
    bool s_eof = false;               // 取证模式：wav 读完了

    bool setup_winrt(bool p_dictation, std::string &p_err, std::string &p_name);
    bool setup_sapi(std::string &p_err, std::string &p_name);
    void teardown_winrt();
    void teardown_sapi();
    bool sapi_set_active(bool p_on, std::string &p_err);
    static DWORD WINAPI sapi_thread(LPVOID p_arg);
    void sapi_handle(const SPEVENT &p_ev);
};

// ===========================================================================
//  后端①：C++/WinRT（OneCore DNN）
// ===========================================================================
bool MicVoice::Impl::setup_winrt(bool p_dictation, std::string &p_err, std::string &p_name) {
    p_name = "winrt-onecore";
    try {
        /* COM apartment。Godot 主线程可能已经进过某个 apartment（MTA），
           再 init 另一种模式会抛 RPC_E_CHANGED_MODE(0x80010106) —— 那不是故障，
           说明已经初始化过，接着用就行。用 MTA 而不是 STA：本层没有窗口消息泵。 */
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (const winrt::hresult_error &e) {
            if (e.code().value != static_cast<std::int32_t>(0x80010106L)) throw;
        }

        rec = WSR::SpeechRecognizer();
        dbg("winrt 识别器已建，语言 = %s",
            winrt::to_string(rec.CurrentLanguage().LanguageTag()).c_str());

        rec.Constraints().Clear();
        if (p_dictation) {
            /* 自由听写 + 主题约束：识别**整句**中文，再交给 parse_command 做容错匹配。
               为什么不把指令词装成 ListConstraint：那是**整句匹配** ——
               列表里放"开火"，玩家说"全体开火"就匹配不上；而本游戏的指令是
               「呼号 + 动作 + 地点」的组合，列表会组合爆炸。网页版也是自由识别
               + handleUtterance，同一个道理。 */
            rec.Constraints().Append(WSR::SpeechRecognitionTopicConstraint(
                WSR::SpeechRecognitionScenario::Dictation, L"va_commands"));
        }

        // ⚠️ 必须编译约束，否则上面那条 Append 只是个摆设。
        auto cr = rec.CompileConstraintsAsync().get();
        if (cr.Status() != WSR::SpeechRecognitionResultStatus::Success) {
            p_err = "CompileConstraints 失败，status=" + std::to_string((int)cr.Status());
            return false;
        }

        session = rec.ContinuousRecognitionSession();

        // 闭包捕获的是 Impl 本身（this）：析构里先摘事件再 reset impl_，
        // 与第一版同一个不变式（谁也不会在对象没了之后还回调）。
        tok_result = session.ResultGenerated(
            [this](const auto &, const WSR::SpeechContinuousRecognitionResultGeneratedEventArgs &a) {
                auto r = a.Result();
                if (r.Status() != WSR::SpeechRecognitionResultStatus::Success) {
                    rejected.fetch_add(1);
                    return;
                }
                std::string t = winrt::to_string(r.Text());
                if (t.empty()) return;
                // 只入队：这里在识别器线程上，绝不能碰 va:: 的任何东西。
                push(std::move(t), conf_value(r.Confidence()));
            });
        has_result_tok = true;

        /* 中间结果那条挂在**识别器**上（ISpeechRecognizer2::HypothesisGenerated），
           不在会话上 —— 这是 WinRT 的既有形状，照 MIDL 头抄，别按会话的思路类推。 */
        tok_hyp = rec.HypothesisGenerated(
            [this](const auto &, const WSR::SpeechRecognitionHypothesisGeneratedEventArgs &a) {
                std::lock_guard<std::mutex> lk(mu);
                interim = winrt::to_string(a.Hypothesis().Text());
            });
        has_hyp_tok = true;

        kind = Kind::Winrt;
        dbg("winrt 后端就绪 backend=%s", p_name.c_str());
        return true;
    } catch (const winrt::hresult_error &e) {
        p_err = "winrt: " + winrt::to_string(e.message());
    } catch (...) {
        p_err = "winrt: 未知异常";
    }
    rec = nullptr;
    session = nullptr;
    kind = Kind::None;
    return false;
}

void MicVoice::Impl::teardown_winrt() {
    /* 摘事件 + 停会话必须在销毁 winrt 对象之前做完：
       ResultGenerated 的闭包捕获了 this（= Impl），会话还在跑就析构 = 回调打到野指针。
       StopAsync().get() 在这里可以阻塞 —— 它在切换/析构路径上，慢一点没关系，
       要紧的是"保证没有回调在飞"。 */
    try {
        if (session != nullptr) {
            if (has_result_tok) session.ResultGenerated(tok_result);
            if (has_hyp_tok && rec != nullptr)
                rec.HypothesisGenerated(tok_hyp);   // 这条挂在识别器上，不在会话上
            session.StopAsync().get();
        }
    } catch (const winrt::hresult_error &) {
        // 会话本来就没起来 —— 没什么可停的
    } catch (...) {
    }
    has_result_tok = false;
    has_hyp_tok = false;
    session = nullptr;
    rec = nullptr;
    if (kind == Kind::Winrt) kind = Kind::None;
}

// ===========================================================================
//  后端②：SAPI5（裸 COM）
// ===========================================================================
bool MicVoice::Impl::setup_sapi(std::string &p_err, std::string &p_name) {
    p_name = "sapi5";

    ISpObjectToken *tok = nullptr;
    if (!sapi_pick_token(&tok, &s_lcid, p_err)) { p_err = "SAPI: " + p_err; return false; }

    // Attach 而不是构造：tok 出来时已经 AddRef 过一次，交给 ComPtr 接管即可，
    // 这样下面任何一条 return 都不会漏引用。
    Microsoft::WRL::ComPtr<ISpObjectToken> hold;
    hold.Attach(tok);

    HRESULT hr = ::CoCreateInstance(CLSID_SpInprocRecognizer, nullptr, CLSCTX_ALL,
                                    IID_ISpRecognizer, (void **)&s_rec);
    if (FAILED(hr) || s_rec == nullptr) { p_err = "SAPI: 建 inproc 识别器 " + hr_text(hr); return false; }
    hr = s_rec->SetRecognizer(tok);
    if (FAILED(hr)) { p_err = "SAPI: SetRecognizer " + hr_text(hr); return false; }

    /* ---- 输入源 ----
       生产路径：默认录音设备（SPCAT_AUDIOIN 的默认 token）。
       取证路径：VA_MIC_WAV=<wav> 时把 wav 文件当输入 —— 这样整条链路
       （COM 装配 → 事件线程 → 队列 → parse_command 下发）能在**没有麦克风、
       没有人说话**的情况下跑通，是这一层唯一可自动化的端到端判据。
       两条路拿到的都是 IUnknown*，因为 ISpRecognizer::SetInput 收的就是它。 */
    const char *wav = env_str("VA_MIC_WAV", nullptr);
    if (wav != nullptr) {
        Microsoft::WRL::ComPtr<ISpStream> st;
        hr = ::CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL,
                                IID_ISpStream, (void **)st.GetAddressOf());
        if (FAILED(hr)) { p_err = "SAPI: 建 SpStream " + hr_text(hr); return false; }
        const std::wstring wp = to_wide(wav);
        hr = st->BindToFile(wp.c_str(), SPFM_OPEN_READONLY, &SPDFID_WaveFormatEx,
                            nullptr, SPFEI_ALL_EVENTS);
        if (FAILED(hr)) { p_err = "SAPI: BindToFile " + hr_text(hr); return false; }
        st->AddRef();                 // 让引用活过 ComPtr 的作用域
        s_input = st.Get();
        s_file_input = true;
        dbg("SAPI 输入 = wav 文件（取证模式）");
    } else {
        Microsoft::WRL::ComPtr<ISpObjectTokenCategory> acat;
        hr = ::CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                IID_ISpObjectTokenCategory, (void **)acat.GetAddressOf());
        if (FAILED(hr)) { p_err = "SAPI: audio category " + hr_text(hr); return false; }
        acat->SetId(SPCAT_AUDIOIN, FALSE);
        LPWSTR aid = nullptr;
        hr = acat->GetDefaultTokenId(&aid);
        if (FAILED(hr) || aid == nullptr) { p_err = "SAPI: 没有默认录音设备 " + hr_text(hr); return false; }

        Microsoft::WRL::ComPtr<ISpObjectToken> atok;
        hr = ::CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL,
                                IID_ISpObjectToken, (void **)atok.GetAddressOf());
        if (SUCCEEDED(hr)) hr = atok->SetId(SPCAT_AUDIOIN, aid, FALSE);
        ::CoTaskMemFree(aid);
        if (FAILED(hr)) { p_err = "SAPI: 打开录音 token " + hr_text(hr); return false; }
        IUnknown *in = nullptr;
        hr = atok->CreateInstance(nullptr, CLSCTX_ALL, IID_ISpAudio, (void **)&in);
        if (FAILED(hr) || in == nullptr) { p_err = "SAPI: 建 ISpAudio " + hr_text(hr); return false; }
        s_input = in;
    }
    hr = s_rec->SetInput(s_input, TRUE);
    if (FAILED(hr)) { p_err = "SAPI: SetInput " + hr_text(hr); return false; }

    // ---- 上下文 + 事件 ----
    hr = s_rec->CreateRecoContext(&s_ctx);
    if (FAILED(hr) || s_ctx == nullptr) { p_err = "SAPI: CreateRecoContext " + hr_text(hr); return false; }
    hr = s_ctx->SetNotifyWin32Event();
    if (FAILED(hr)) { p_err = "SAPI: SetNotifyWin32Event " + hr_text(hr); return false; }
    const ULONGLONG qi = SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_HYPOTHESIS)
                       | SPFEI(SPEI_FALSE_RECOGNITION) | SPFEI(SPEI_END_SR_STREAM);
    s_ctx->SetInterest(qi, qi);   // 两个掩码都给：不给 queued 掩码 GetEvents 取不到东西

    // ---- 听写文法 ----
    hr = s_ctx->CreateGrammar(1, &s_gram);
    if (FAILED(hr) || s_gram == nullptr) { p_err = "SAPI: CreateGrammar " + hr_text(hr); return false; }
    hr = s_gram->LoadDictation(nullptr, SPLO_STATIC);
    if (FAILED(hr)) { p_err = "SAPI: LoadDictation " + hr_text(hr); return false; }

    // ---- 事件线程 ----
    s_quit = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_run.store(true);
    s_thread = ::CreateThread(nullptr, 0, &MicVoice::Impl::sapi_thread, this, 0, nullptr);
    if (s_thread == nullptr) { p_err = "SAPI: CreateThread 失败"; return false; }

    char nm[64];
    std::snprintf(nm, sizeof(nm), "sapi5-%d%s", s_lcid, s_file_input ? "-wav" : "");
    p_name = nm;
    kind = Kind::Sapi;
    dbg("sapi 后端就绪 backend=%s", p_name.c_str());
    return true;
}

void MicVoice::Impl::teardown_sapi() {
    // ⚠️ 次序不能反：worker 线程还在用 s_ctx，先把它收干净再拆 COM 对象。
    s_run.store(false);
    if (s_quit != nullptr) ::SetEvent(s_quit);
    if (s_thread != nullptr) {
        ::WaitForSingleObject(s_thread, 3000);
        ::CloseHandle(s_thread);
        s_thread = nullptr;
    }
    if (s_quit != nullptr) { ::CloseHandle(s_quit); s_quit = nullptr; }
    if (s_gram != nullptr) {
        s_gram->SetDictationState(SPRS_INACTIVE);
        s_gram->Release();
        s_gram = nullptr;
    }
    if (s_ctx != nullptr) { s_ctx->Release(); s_ctx = nullptr; }
    if (s_rec != nullptr) {
        s_rec->SetRecoState(SPRST_INACTIVE);
        s_rec->Release();
        s_rec = nullptr;
    }
    if (s_input != nullptr) { s_input->Release(); s_input = nullptr; }
    s_file_input = false;
    s_eof = false;
    if (kind == Kind::Sapi) kind = Kind::None;
}

bool MicVoice::Impl::sapi_set_active(bool p_on, std::string &p_err) {
    if (s_rec == nullptr || s_gram == nullptr) { p_err = "SAPI: 没建起来"; return false; }
    if (p_on) {
        // 先让引擎转起来，再让听写文法生效 —— 反过来的话第一句常被吞掉。
        HRESULT hr = s_rec->SetRecoState(SPRST_ACTIVE);
        if (FAILED(hr)) { p_err = "SAPI: SetRecoState(ACTIVE) " + hr_text(hr); return false; }
        hr = s_gram->SetDictationState(SPRS_ACTIVE);
        if (FAILED(hr)) { p_err = "SAPI: SetDictationState(ACTIVE) " + hr_text(hr); return false; }
    } else {
        s_gram->SetDictationState(SPRS_INACTIVE);
        s_rec->SetRecoState(SPRST_INACTIVE);
    }
    return true;
}

DWORD WINAPI MicVoice::Impl::sapi_thread(LPVOID p_arg) {
    Impl &I = *(Impl *)p_arg;
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HANDLE hn = I.s_ctx != nullptr ? I.s_ctx->GetNotifyEventHandle() : nullptr;
    if (hn == nullptr) { ::CoUninitialize(); return 0; }
    HANDLE hs[2] = { hn, I.s_quit };

    while (I.s_run.load()) {
        const DWORD w = ::WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) break;   // s_quit 或异常 → 收工
        for (;;) {
            SPEVENT ev;
            ULONG got = 0;
            const HRESULT hr = I.s_ctx->GetEvents(1, &ev, &got);
            if (FAILED(hr) || got != 1) break;
            I.sapi_handle(ev);
            // GetEvents 取出来的对象事件是**借用**的，用完必须自己 Release。
            if (ev.elParamType == SPET_LPARAM_IS_OBJECT && ev.lParam != 0)
                ((IUnknown *)ev.lParam)->Release();
        }
    }
    ::CoUninitialize();
    return 0;
}

void MicVoice::Impl::sapi_handle(const SPEVENT &p_ev) {
    if (p_ev.eEventId == SPEI_HYPOTHESIS) {
        ISpRecoResult *r = (ISpRecoResult *)p_ev.lParam;
        if (r == nullptr) return;
        LPWSTR t = nullptr;
        if (SUCCEEDED(r->GetText(SP_GETWHOLEPHRASE, SP_GETWHOLEPHRASE, TRUE, &t, nullptr))
            && t != nullptr) {
            std::lock_guard<std::mutex> lk(mu);
            interim = from_wide(t);
            ::CoTaskMemFree(t);
        }
        return;
    }
    if (p_ev.eEventId == SPEI_RECOGNITION) {
        ISpRecoResult *r = (ISpRecoResult *)p_ev.lParam;
        if (r == nullptr) { rejected.fetch_add(1); return; }
        LPWSTR t = nullptr;
        if (FAILED(r->GetText(SP_GETWHOLEPHRASE, SP_GETWHOLEPHRASE, TRUE, &t, nullptr))
            || t == nullptr) {
            rejected.fetch_add(1);
            return;
        }
        std::string s = from_wide(t);
        ::CoTaskMemFree(t);
        if (s.empty()) return;
        const float c = sapi_confidence(r);
        dbg("sapi 听清：%s（conf=%.2f）", s.c_str(), c);
        push(std::move(s), c);
        return;
    }
    if (p_ev.eEventId == SPEI_FALSE_RECOGNITION) { rejected.fetch_add(1); return; }
    // wav 取证模式：音频流到头了。主线程看到这个标志就收会话（见 pump）。
    if (p_ev.eEventId == SPEI_END_SR_STREAM) { s_eof = true; return; }
}

// ===========================================================================
//  MicVoice
// ===========================================================================
MicVoice::MicVoice() = default;

MicVoice::~MicVoice() {
    if (!impl_) return;
    impl_->teardown_winrt();
    impl_->teardown_sapi();
    impl_.reset();
}

void MicVoice::setup(bool p_dictation) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    Impl &I = *impl_;
    I.wav_mode = (env_str("VA_MIC_WAV", nullptr) != nullptr);

    pref_ = env_str("VA_MIC_BACKEND", "auto");
    const bool want_winrt = (pref_ != "sapi");
    const bool want_sapi = (pref_ != "winrt");

    std::string winrt_err;
    std::string name;
    if (want_winrt) {
        if (I.setup_winrt(p_dictation, winrt_err, name)) {
            backend_ = name;
        } else {
            dbg("winrt 后端不可用：%s", winrt_err.c_str());
        }
    } else {
        winrt_err = "（VA_MIC_BACKEND=sapi，未尝试）";
    }

    if (I.kind == Impl::Kind::None && want_sapi) {
        std::string sapi_err;
        if (I.setup_sapi(sapi_err, name)) {
            backend_ = name;
            if (!winrt_err.empty())
                notice_ = "语音后端：OneCore 不可用（" + winrt_err + "），已切离线 SAPI5";
        } else {
            last_error_ = winrt_err.empty() ? sapi_err : (winrt_err + " / " + sapi_err);
            dbg("sapi 后端不可用：%s", sapi_err.c_str());
        }
    }

    if (I.kind == Impl::Kind::None && last_error_.empty())
        last_error_ = winrt_err.empty() ? "没有可用后端" : winrt_err;
}

bool MicVoice::available() const {
    return impl_ != nullptr && impl_->kind != Impl::Kind::None;
}

bool MicVoice::enabled() const { return enabled_; }

bool MicVoice::listening() const { return listen_; }

// 真的去起会话。两条后端的"起"语义不一样（WinRT 是 StartAsync，
// SAPI5 是把引擎切到 ACTIVE），所以按 kind 分派。
void MicVoice::try_start() {
    if (listen_ || !impl_) return;
    Impl &I = *impl_;

    bool ok = false;
    std::string err;
    if (I.kind == Impl::Kind::Winrt) {
        try {
            /* 不 .get()：StartAsync 要打开麦克风，阻塞等会把主线程卡住几十毫秒。
               识别结果本来就走事件回队列，不等也拿得到。 */
            I.session.StartAsync();
            ok = true;
        } catch (const winrt::hresult_error &e) {
            err = "winrt start: " + winrt::to_string(e.message());
        } catch (...) {
            err = "winrt start: 未知异常";
        }
    } else if (I.kind == Impl::Kind::Sapi) {
        ok = I.sapi_set_active(true, err);
    } else {
        return;
    }

    if (ok) {
        listen_ = true;
        { std::lock_guard<std::mutex> lk(I.mu); I.interim.clear(); }
        dbg("开始听（%s）", backend_.c_str());
    } else {
        last_error_ = err;
        dbg("start 失败：%s", err.c_str());
        /* 起不来且是 auto：交给主线程换后端。
           为什么不在这里换：本函数可能还在 _input 阶段被调用
           （_input → apply_key → mic_press → start），而拆 COM 对象、建新识别器
           是重活，掺在输入回调里会连累按键响应。 */
        if (pref_ == "auto" && I.kind == Impl::Kind::Winrt) pending_fallback_ = true;
    }
}

// OneCore 起不来 → 换 SAPI5 并把这次会话续上（玩家还按着 Q，不该让他按第二下）。
void MicVoice::do_fallback() {
    Impl &I = *impl_;
    pending_fallback_ = false;
    dbg("OneCore 起不来（%s）→ 回退 SAPI5", last_error_.c_str());
    I.teardown_winrt();

    std::string err;
    std::string name;
    if (!I.setup_sapi(err, name)) {
        last_error_ = "回退 SAPI5 也失败：" + err;
        notice_ = "语音输入不可用：OneCore 与 SAPI5 都起不来（" + err + "）";
        dbg("回退失败：%s", err.c_str());
        return;
    }
    backend_ = name;
    notice_ = "语音后端已切到离线 SAPI5（OneCore 起不来）";
    if (want_) try_start();
}

void MicVoice::start() {
    if (!enabled_) return;
    want_ = true;
    if (!impl_) return;
    try_start();
}

void MicVoice::stop() {
    want_ = false;
    if (!impl_ || !listen_) return;
    Impl &I = *impl_;
    if (I.kind == Impl::Kind::Winrt) {
        /* 同样不等：StopAsync 会等"最后一句"识别完才返回，几百毫秒 ——
           松开按键时卡那一下很难受。让它在后台收尾，结果照样走事件。 */
        try {
            if (I.session != nullptr) I.session.StopAsync();
        } catch (const winrt::hresult_error &) {
        }
    } else if (I.kind == Impl::Kind::Sapi) {
        std::string err;
        I.sapi_set_active(false, err);
    }
    listen_ = false;
    dbg("停止听");
}

void MicVoice::pump() {
    if (!impl_) return;
    Impl &I = *impl_;

    if (pending_fallback_) {
        if (!want_) pending_fallback_ = false;
        else { do_fallback(); return; }
    }

    /* wav 取证模式（VA_MIC_WAV）**与后端无关**：只要给了这个旋钮，就替玩家走一遍
       "按下 → 松开"。为什么不能只在 SAPI 后端上判：auto 会先选 winrt
       （setup 成功，隐私策略要到 StartAsync 才查），于是"起不来 → 回退 SAPI5"
       这条路径在 headless 下永远走不到 —— 实测就是这么漏掉的。 */
    if (I.wav_mode) {
        if (!I.s_wav_started) { I.s_wav_started = true; want_ = true; try_start(); }
        else if (I.s_eof && listen_) stop();
    }
}

bool MicVoice::poll(std::string &p_text, float &p_confidence) {
    if (!impl_) return false;
    Impl &I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    if (I.queue.empty()) return false;
    auto one = std::move(I.queue.front());
    I.queue.pop_front();
    I.accepted.fetch_add(1);
    p_text = std::move(one.first);
    p_confidence = one.second;
    return true;
}

std::string MicVoice::interim() const {
    if (!impl_) return {};
    Impl &I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    return I.interim;
}

std::string MicVoice::take_notice() {
    std::string s;
    s.swap(notice_);
    return s;
}

long MicVoice::accepted() const { return impl_ ? impl_->accepted.load() : 0; }
long MicVoice::rejected() const { return impl_ ? impl_->rejected.load() : 0; }

} // namespace volunteer_army
