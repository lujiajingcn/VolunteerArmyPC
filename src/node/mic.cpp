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
#include <combaseapi.h>   // CoInitializeEx / CoGetApartmentType / RPC_E_CHANGED_MODE
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

/* 不看 VA_DBG_MIC、**总是**打的日志。只给"玩家会看得出来但代码里不留痕"的
   事件用 —— 目前就一条：采信窗口外丢掉的定稿。
   为什么它够格无条件打：① 罕见（松开 Q 之后还回来的那句）；
   ② 它是"喊了没反应"与"识别器没识别出来"的唯一分界点，而这俩的
   处置完全相反（前者调 VA_MIC_TAIL，后者去查麦克风/识别器）。
   不加区分地静默丢掉，正是这个 bug 藏了一整轮的原因。 */
void dbg_always(const char *fmt, ...) {
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

/* 把 winrt::hresult_error 压成一行「码 + 人话」。
   ⚠️ 本身必须**不抛**：它专供识别回调的 catch 块用（那里再抛就是二次事故），
   所以连 message() 都包一层 —— message 没缓存时会去 FormatMessage。 */
std::string hr_e_text(const winrt::hresult_error &e) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%08X ", (unsigned)e.code().value);
    std::string s(b);
    try {
        s += winrt::to_string(e.message());
    } catch (...) {
        s += "(message 取不到)";
    }
    return s;
}

/* WinRT 的置信度是**三档枚举**，不是概率。
   映射到 0~1 只是为了让逻辑层的 parse_command(asr=...) 有东西可用 ——
   那里 conf = lerpf(conf, asr, 0.55)，本工程自己会对"战场噪声"再打折。

   ⚠️ Low 档原来是 **0.55**，2026-09-27 实测证明它**必然打死短口令**。
   离线探针（sweep/sim/va_sweep.exe probe）扫出来的表：
     「全体撤退」这类「全体 + 两字动作」的语法分上限只有 **0.78**
     （= 0.30 基 + 0.34 两字动作 + 0.06 全体呼号 + 0.08 四字长），
     再经 lerpf(0.78, 0.55, 0.55) = **0.65** —— 低于 0.70 门槛，
     **连零噪声都过不去**。而 OneCore 把短词、口音、环境稍吵的句子判成 Low
     是常事，于是「按住 Q 喊『全体撤退』毫无反应」就成了**必现**。
   注意失败的形态：不崩、不报错、队友不动、状态标签不变，HUD 只闪一句 ——
   「最像麦克风坏了」的那种失败，实际坏在置信度上。

   为什么抬到 0.80 是对的：Low 只表示"引擎对这一句不太确定"，而**文本对不对
   由 parse_command 判定** —— 它拿 50+ 动作词 + 呼号 + 地点做精确/模糊匹配，
   真听错字（"撤退"→"撤回"）压根解析不出动作。这与下面 sapi_confidence()
   回标称 0.80 是同一条理由：真正的闸门是**语法匹配**与**战场噪声**，
   不是引擎这个三档枚举。
   代价：Low 与 Medium 合并成一档，三档只剩两档区分度 —— 值。 */
float conf_value(WSR::SpeechRecognitionConfidence c) {
    using C = WSR::SpeechRecognitionConfidence;
    if (c == C::High)   return 0.95f;
    if (c == C::Medium) return 0.80f;
    if (c == C::Low)    return 0.80f;   // 原 0.55 —— 见上，它会打死「全体+两字动作」
    return 0.30f;                       // Rejected / 未知：真的不该执行
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

    /* 回调里**接住**的异常。为什么不就地打日志：dbg() 走的是
       godot::UtilityFunctions::print()，而回调跑在识别器的线程上，
       那里碰 Godot 的对象系统是不安全的。所以只记在这儿，
       由主线程在 pump() 里补一次日志（见 MicVoice::pump）。 */
    std::atomic<long> cb_err{0};
    std::string cb_err_where;
    long cb_err_reported = 0;

    /* 同样只在回调线程自增、由主线程打出来的"证据计数器"。
       为什么非要这个：语音这一层最坑的就是**静默失败** ——
       "开始听"打出来了、HUD 也亮了，玩家对麦喊半天什么都没发生，
       而日志里一句异常都没有（hypothesis / result 这两条事件
       此前**一个字节都不落日志**）。设了 VA_DBG_MIC 还看不出
       "到底有没有音频进来"，等于没有可观测性。 */
    std::atomic<long> hyp_seen{0};      // 中间结果（说话时每几个字一条）
    std::string hyp_last;               // 受 mu 保护
    long hyp_reported = 0;
    /* 中间结果"落在采信窗口内 / 外"的分布。
       这是**唯一能自动拿到"识别器在松手之后仍在出事件"这条证据**的东西：
       真人说话的定稿只能靠人验（放喇叭的音频失真，OneCore 定不了稿），
       但 hypothesis 是流式的，播一段音频就能看见它跨过松手时刻 ——
       有了它，"定稿也会在松手之后到达"才是推论而不是猜测。
       归类只能放在主线程：窗口状态是主线程的，回调线程不知道。 */
    long hyp_stamped = 0;               // 已归类到的 hyp_seen 水位
    long hyp_in = 0;                    // 采信窗口内
    long hyp_out = 0;                   // 采信窗口外（松手之后）
    bool hyp_out_reported = false;
    std::atomic<long> res_seen{0};      // 定稿结果
    std::atomic<long> res_bad{0};       // 定稿但 Status != Success
    long res_reported = 0;
    bool start_reported = false;        // StartAsync 的完成状态是否已报过

    void note_cb_error(const char *p_where, const std::string &p_msg) {
        rejected.fetch_add(1);
        std::lock_guard<std::mutex> lk(mu);
        cb_err.fetch_add(1);
        cb_err_where = std::string(p_where) + "（" + p_msg + "）";
    }

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
    /* StartAsync 返回的那个异步操作**必须存住**，理由是两条：
       ① 「OneCore 起不来」是**异步**报出来的：只调不查 = 会话看着起来了
          （listen_ 被置 true、HUD 也亮红点），其实一句都没听 ——
          系统的语音隐私开关关掉时就是这个症状，此前整层看不见；
       ② 连错误码都拿不到。主线程在 pump() 里查它的状态（见 MicVoice::pump）。 */
    winrt::Windows::Foundation::IAsyncAction start_op{nullptr};
    /* StopAsync 同样存住，理由和 start_op 相反：它是**阻止**我们再 Start 的那个。
       ⚠️ 会话处在 Stopping 时调 StartAsync 会**同步**抛 hresult_error
       （实测 2026-09-27：连按两次 Q 必现）。这里存下来只为查状态，
       状态查询本身不抛。 */
    winrt::Windows::Foundation::IAsyncAction stop_op{nullptr};
    bool defer_start = false;           // 上一次 Stop 没落地 → 下一帧再 Start
    /* 会话是否**已经起过**（OneCore 走"常开"：起一次，之后只切采信开关）。
       见 MicVoice::start 里那两条理由 —— Start/Stop 往返在连续识别会话上
       是个陷阱：StopAsync 既要等当前这句说完、又是异步的，等不到就只能卡住。 */
    bool sess_live = false;
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
        /* ---- COM apartment ----
           ⚠️ 【不要用 winrt::init_apartment】它内部就是 CoInitializeEx(nullptr, type)，
           失败即 throw_hresult() **抛** winrt::hresult_error（SDK 原文，base.h:6529）。
           Godot 主线程在 ole32 上已经 OleInitialize 过（拖放要用），是 **Main STA**，
           于是 MTA 版本**必然**返回 RPC_E_CHANGED_MODE(0x80010106)：
           异常虽然被接住、流程没错，但 VS 调试器会把它当"首次异常"，
           **每次开局都弹一次**（2026-09-27 实测 + 下方 dbg 行的证据）。
           所以直接调 CoInitializeEx，把三个"预期"返回码都当成功：
             S_OK                本次初始化成功
             S_FALSE              本线程已初始化过（同一模式）
             RPC_E_CHANGED_MODE   本线程已在**另一种** apartment 里（不改变现状）
           —— 三种都不该走异常。第三种就是本机实际遇到的：主线程是 Main STA。

           为什么 STA 也能用：Godot 主线程每帧 PeekMessage/DispatchMessage，
           消息泵是现成的 —— 旧注释里"本层没有窗口消息泵"只对**自建的**工作线程成立
           （SAPI5 那条就是这么干的，见 sapi_thread）。 */
        const HRESULT hr_apt = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr_apt) && hr_apt != RPC_E_CHANGED_MODE) {
            p_err = "winrt: CoInitializeEx " + hr_text(hr_apt);
            return false;
        }
        if (dbg_on()) {
            APTTYPE at = APTTYPE_CURRENT;
            APTTYPEQUALIFIER aq = APTTYPEQUALIFIER_NONE;
            const char *nm = "?";
            if (SUCCEEDED(::CoGetApartmentType(&at, &aq))) {
                switch (at) {
                    case APTTYPE_STA:     nm = "STA";       break;
                    case APTTYPE_MTA:     nm = "MTA";       break;
                    case APTTYPE_MAINSTA: nm = "MainSTA";   break;
                    case APTTYPE_NA:      nm = "NA(中立)";   break;
                    default:              nm = "其它";       break;
                }
            }
            dbg("apartment: CoInitializeEx=0x%08lX 本线程=%s（0x80010106 = 已是别的 apartment，正常）",
                (unsigned long)hr_apt, nm);
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
                /* ⚠️⚠️ 这里是 **WinRT 事件回调**，异常一个字都不许逃出去。
                   C++/WinRT 的事件委托**不做** try/catch，异常会直接穿过 COM 的
                   ABI 边界 —— 症状就是"按住 Q 说话，游戏没了"（2026-09-27 本机实证：
                   VS 报 winrt::hresult_error，取消之后进程直接终止）。
                   整个回调体裹一层是本层的铁律，不是防御性编程：
                   回调不是我们调的，是我们**被**调的，栈上没有我们的 catch。 */
                try {
                    auto r = a.Result();
                    if (r == nullptr) { rejected.fetch_add(1); return; }
                    if (r.Status() != WSR::SpeechRecognitionResultStatus::Success) {
                        rejected.fetch_add(1);
                        res_bad.fetch_add(1);
                        return;
                    }
                    std::string t = winrt::to_string(r.Text());
                    if (t.empty()) return;
                    res_seen.fetch_add(1);
                    // 只入队：这里在识别器线程上，绝不能碰 va:: 的任何东西。
                    push(std::move(t), conf_value(r.Confidence()));
                } catch (const winrt::hresult_error &e) {
                    note_cb_error("结果回调", hr_e_text(e));
                } catch (const std::exception &e) {
                    note_cb_error("结果回调", std::string("std::exception: ") + e.what());
                } catch (...) {
                    note_cb_error("结果回调", "未知异常");
                }
            });
        has_result_tok = true;

        /* 中间结果那条挂在**识别器**上（ISpeechRecognizer2::HypothesisGenerated），
           不在会话上 —— 这是 WinRT 的既有形状，照 MIDL 头抄，别按会话的思路类推。
           ⚠️ 这条比定稿结果**触发得频繁得多**（说几个字就来一条），
           所以它是"一说话就崩"的第一嫌疑人：不说话的按键测试（pair 模式）
           永远碰不到它。异常同样必须兜住。 */
        tok_hyp = rec.HypothesisGenerated(
            [this](const auto &, const WSR::SpeechRecognitionHypothesisGeneratedEventArgs &a) {
                try {
                    auto h = a.Hypothesis();
                    if (h == nullptr) return;
                    /* 先转成 std::string 再进锁：持锁期间不调 WinRT ——
                       主线程的 interim() 走的是同一把锁，回调万一被 marshal 到
                       Godot 的 Main STA 上，就是同线程二次加锁的死锁。 */
                    std::string s = winrt::to_string(h.Text());
                    std::lock_guard<std::mutex> lk(mu);
                    interim = std::move(s);
                    hyp_seen.fetch_add(1);
                    hyp_last = interim;
                } catch (const winrt::hresult_error &e) {
                    note_cb_error("中间结果回调", hr_e_text(e));
                } catch (const std::exception &e) {
                    note_cb_error("中间结果回调", std::string("std::exception: ") + e.what());
                } catch (...) {
                    note_cb_error("中间结果回调", "未知异常");
                }
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
    start_op = nullptr;
    stop_op = nullptr;
    defer_start = false;
    sess_live = false;
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

    /* 采信尾窗：松手之后还认多久的定稿（见 mic.h 的 start/stop 注释）。
       调大 → 更不容易漏命令，代价是"松手后随口说的一句"也可能被采信；
       调 0 → 退回"松手即丢"（那是本 bug 的原状）。 */
    /* 默认 2.5 秒。取值理由：OneCore 的句尾静音检测 + DNN 推理通常落在
       0.5~1.5 秒，2.5 留了一倍余量。宁可偶尔把"松手后随口一句"也采信
       （它还得过 parse_command 那套严格匹配才会真下令），也不能丢玩家的命令。 */
    tail_secs_ = (float)std::atof(env_str("VA_MIC_TAIL", "2.5"));
    if (tail_secs_ < 0.0f) tail_secs_ = 0.0f;
    dbg("采信尾窗 VA_MIC_TAIL=%.2f 秒（松手后这段时间内回来的定稿仍然作数）", tail_secs_);

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
        using winrt::Windows::Foundation::AsyncStatus;
        /* ⚠️⚠️ 上一次 StopAsync 还没落地时**绝不能** StartAsync。
           会话此刻处于 Stopping，再 Start 会**同步**抛 winrt::hresult_error
           （2026-09-27 实测：连按两次 Q 必现，抛点就在这句）。
           异常虽然被下面的 catch 接住、流程也走回退，
           但 **VS 调试器是在"抛出点"中断的**（第一次异常通知）——
           玩家看到的就是"按住 Q 说话，游戏弹异常/崩了"。
           推迟一帧就够：want_ 一直存着，pump 会替我们再来一次。 */
        if (I.stop_op != nullptr && I.stop_op.Status() == AsyncStatus::Started) {
            I.defer_start = true;
            return;
        }
        try {
            /* 不 .get()：StartAsync 要打开麦克风，阻塞等会把主线程卡住几十毫秒。
               识别结果本来就走事件回队列，不等也拿得到。
               ⚠️ 但**必须存住**这个 action：
               ① 起不来是异步报的，pump() 里要对账；
               ② 把最后一个引用放掉等于**取消**这个异步操作（实测），会话就废了。 */
            I.start_op = I.session.StartAsync();
            ok = true;
        } catch (const winrt::hresult_error &e) {
            // 一定用 hr_e_text：这个错误的 message 本机取不到（"无法找到与此错误
            // 代码关联的文本"），不打出码就完全没法定位。
            err = "winrt start: " + hr_e_text(e);
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
        /* 按住期间**一直**采信：截止时刻推到 max()，poll() 那条窗口判断就是恒真。
           松手时 stop() 会把它改成 now + tail_secs_（尾窗）。 */
        accept_until_ = std::chrono::steady_clock::time_point::max();
        I.sess_live = true;
        { std::lock_guard<std::mutex> lk(I.mu); I.interim.clear(); I.queue.clear(); }
        /* 每轮会话都让"听到声音了 / 定稿了"重新报一次：
           要确认的是**这一次**按键有没有音频进来，而不是"历史上曾经有过"。
           −1 是哨兵：表示"本轮还没有过中间结果"（见 pump 里的 first 判定）。 */
        I.hyp_reported = -1;
        I.res_reported = I.res_seen.load();
        I.start_reported = false;
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
    Impl &I = *impl_;

    /* ---- OneCore 走"会话常开" ----
       第一次按 Q 才 StartAsync；之后每次按 Q 只把 listen_ 立起来
       （即"开始采信识别结果"），**不**重启会话。两条理由都是实测来的坑：
         ① StopAsync 是**异步**的，它没落地时再 StartAsync 会**同步**抛
            hresult_error —— 而 VS 调试器在抛出点就中断（玩家看到的是"按 Q 说话
            游戏弹异常/崩了"）。连按两次 Q 就能复现。
         ② StopAsync 还会等"当前这句话说完"才返回；我们为了不卡住松键那一下
            又不能 .get() 等它 —— 于是第二下按 Q 会永远起不来（实测：日志里
            连"开始听"都不出现）。
       代价只有一个：麦克风从第一次按 Q 起一直开着（Windows 托盘会显示
       "正在使用麦克风"）。玩家侧语义不变 —— 没按住 Q 时收到的结果由
       poll() / interim() 丢掉（见那两处）。 */
    if (I.kind == Impl::Kind::Winrt && I.sess_live) {
        listen_ = true;
        accept_until_ = std::chrono::steady_clock::time_point::max();   // 见 try_start
        { std::lock_guard<std::mutex> lk(I.mu); I.interim.clear(); I.queue.clear(); }
        // 和 try_start() 成功那条一样：每一轮按键都重新开始看"有没有听到声音"
        I.hyp_reported = -1;
        I.res_reported = I.res_seen.load();
        dbg("开始听（%s，会话常开）", backend_.c_str());
        return;
    }
    try_start();
}

void MicVoice::stop() {
    want_ = false;
    if (!impl_ || !listen_) return;
    Impl &I = *impl_;
    if (I.kind == Impl::Kind::Winrt) {
        /* ⚠️ **故意不调 StopAsync**（见 MicVoice::start 顶部那两条）。
           会话留着常开，只是不再采信结果。真正停会话只发生在
           teardown_winrt()：换后端 / 析构时才停。 */
    } else if (I.kind == Impl::Kind::Sapi) {
        std::string err;
        I.sapi_set_active(false, err);
    }
    listen_ = false;
    /* ⚠️⚠️ 松手**不等于**"别再收结果"。识别器是滞后的：一句「全体撤退」
       说完要等一小段静音才定稿，而玩家的自然动作是"说完就松手" ——
       于是定稿几乎必然落在这一行之后。早先按 listen_ 一刀切（松手即丢），
       症状就是"按住 Q 喊了命令没有任何反应"，而且不崩、不留日志
       （2026-09-27 玩家实测报的正是这个）。
       现在改成开一个尾窗：这段时间内回来的定稿照样作数。
       SAPI5 也给尾窗 —— 它松手即 INACTIVE、之后不会有新结果，
       但松手**那一瞬**已经排进队列的还得取走（原来靠"只对 Winrt 判 kind"
       特判，现在统一成窗口，那处特判可以退休了）。 */
    accept_until_ = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds((long)(tail_secs_ * 1000.0f));
    dbg("停止听（采信尾窗 %.2f 秒，期间回来的定稿仍作数）", tail_secs_);
}

void MicVoice::pump() {
    if (!impl_) return;
    Impl &I = *impl_;

    /* ---- ① 回调里接住的异常：在这儿补一条日志 ----
       回调跑在识别器线程，dbg() 走 Godot 的 print（碰对象系统）不安全，
       所以回调只把"哪条回调 + 什么错"记进 Impl，由主线程一次报出来。
       报出来**不改变任何行为** —— 异常已经被兜在回调里了，会话照旧；
       这条日志的用途是：下次真出问题，能一眼看出是 WinRT 哪一步、什么码。 */
    const long ce = I.cb_err.load();
    if (ce != I.cb_err_reported) {
        I.cb_err_reported = ce;
        std::string w;
        { std::lock_guard<std::mutex> lk(I.mu); w = I.cb_err_where; }
        dbg("⚠️ 识别回调里接住 %ld 次异常，最后一次 %s（已兜住，会话不受影响）",
            ce, w.c_str());
    }

    /* ---- ①-b 证据计数器：到底有没有音频进来过 ----
       这是本层唯一能自证"会话真的在工作"的东西。没有它，
       "按 Q 说话没反应"与"麦克风没拾到"与"识别器没起会话"三者长得一模一样。 */
    const long hs = I.hyp_seen.load();
    /* 先按"落在采信窗口内还是外"归类（主线程才知道窗口状态）。
       两条都要，缺一不可：窗口内为 0 ⇒ 麦克风没拾到；窗口外不为 0 ⇒
       识别器在松手之后仍在出事件，正是采信尾窗存在的理由。 */
    if (hs > I.hyp_stamped) {
        const long d = hs - I.hyp_stamped;
        I.hyp_stamped = hs;
        if (accepting()) I.hyp_in += d; else I.hyp_out += d;
    }
    /* ⚠️ 必须带 `hs > 0`：hyp_reported 每轮被置成哨兵 −1，而 hs 从 0 起 ——
       少了这个条件，没听到任何声音也会打一条"听到声音了（「」）"的假阳性。 */
    if (hs > 0 && hs > I.hyp_reported) {
        const bool first = (I.hyp_reported < 0);   // 每轮会话的哨兵（try_start 里置 -1）
        I.hyp_reported = hs;
        if (first) {
            std::string t;
            { std::lock_guard<std::mutex> lk(I.mu); t = I.hyp_last; }
            dbg("听到声音了（中间结果第 1 条：「%s」）—— 会话确实在收话", t.c_str());
        }
    }
    if (I.hyp_out > 0 && !I.hyp_out_reported) {
        I.hyp_out_reported = true;
        dbg("松手之后仍收到中间结果 %ld 条（窗口内 %ld 条）—— 识别器不随松手停下，"
            "定稿同理，所以必须有采信尾窗", I.hyp_out, I.hyp_in);
    }
    const long rs = I.res_seen.load();
    if (rs > I.res_reported) {
        I.res_reported = rs;
        dbg("定稿结果第 %ld 条（已入队，交给 parse_command）", rs);
    }

    /* ---- ② StartAsync 的成败是**异步**报的，必须在这儿对账 ----
       只调不查 = "OneCore 起不来"完全静默：listen_=true、HUD 亮红点、
       玩家对着麦喊半天什么都没有，而且一句日志都不留。
       查出来就按 auto 的既定规矩回退（与 try_start 里同步失败那条一致）。 */
    if (I.kind == Impl::Kind::Winrt && I.start_op != nullptr) {
        using winrt::Windows::Foundation::AsyncStatus;
        const AsyncStatus st = I.start_op.Status();
        if (st == AsyncStatus::Error) {
            char hb[24];
            std::snprintf(hb, sizeof(hb), "0x%08X", (unsigned)I.start_op.ErrorCode().value);
            last_error_ = std::string("winrt start(异步) ") + hb;
            dbg("StartAsync 异步失败：%s", last_error_.c_str());
            I.start_op = nullptr;
            listen_ = false;
            if (pref_ == "auto") pending_fallback_ = true;
        } else if (st == AsyncStatus::Started) {
            /* ⚠️ 还在飞 —— 这里**绝对不能**置 nullptr。
               实测（2026-09-27）：把 IAsyncAction 的最后一个引用放掉，
               系统会把这个异步操作**取消**掉（下一帧 Status() 变 Canceled），
               会话就此再也收不到任何音频。这正是原来那句
               `I.session.StartAsync();`（临时对象出了语句就析构）埋下的坑。 */
        } else {
            if (!I.start_reported) {
                I.start_reported = true;
                dbg("StartAsync 回来了：%s",
                    st == AsyncStatus::Completed ? "成功（麦克风已开）" : "已取消");
            }
            I.start_op = nullptr;   // 完成 / 取消：引用可以放了
        }
    }

    if (pending_fallback_) {
        if (!want_) pending_fallback_ = false;
        else { do_fallback(); return; }
    }

    /* ---- ③ StopAsync 的门槛（兜底）----
       现在 stop() 不再主动停 OneCore 会话（会话常开，见 MicVoice::start），
       所以 stop_op 正常一直是空、这两段不会触发。留着是因为它们是"防止
       StartAsync 在会话 Stopping 时同步抛"的唯一护栏 —— 将来谁要把
       StopAsync 加回 stop()，这里能挡住那个必现的异常。
       ① 只放**非 Started** 的 action：放掉一个还在飞的是**取消**它，
          那会话就永远停在 Stopping 了；
       ② 被推迟的 Start 在这儿重来一次（仍然会被①挡住，不会变成每帧抛）。 */
    if (I.kind == Impl::Kind::Winrt && I.stop_op != nullptr) {
        using winrt::Windows::Foundation::AsyncStatus;
        if (I.stop_op.Status() != AsyncStatus::Started) I.stop_op = nullptr;
    }
    if (I.defer_start) {
        I.defer_start = false;
        if (want_ && !listen_) try_start();
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

bool MicVoice::accepting() const {
    return std::chrono::steady_clock::now() <= accept_until_;
}

bool MicVoice::poll(std::string &p_text, float &p_confidence) {
    if (!impl_) return false;
    Impl &I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    if (I.queue.empty()) return false;
    /* 采信窗口之外的结果一律丢掉。
       为什么需要这道闸：OneCore 是"会话常开"（见 MicVoice::start），
       松手之后识别还在继续跑 —— 不丢，玩家没按 Q 时随口说的一句也会下令。
       为什么是"窗口"而不是原来的"跟着 listen_"：见 MicVoice::stop。
       ⚠️ 这道闸一旦判错，症状是**完全静默**的（不崩、不报错、HUD 也不弹），
       所以丢掉的时候必须吭一声（下面那条 dbg_always）。 */
    if (!accepting()) {
        ++late_dropped_;
        late_dropped_last_ = I.queue.front().first;
        const size_t n = I.queue.size();
        I.queue.clear();
        /* ⚠️ 为什么要**限频**：OneCore 会话常开，玩家没按 Q 时环境里的人声
           同样会定稿、同样落在这儿。每条都喊一遍会刷屏。
           所以第一次无条件报，之后每 20 条报一次；VA_DBG_MIC=1 时每条都看得到。 */
        const bool loud = (late_dropped_ == 1) || (late_dropped_ % 20 == 0);
        if (loud) {
            dbg_always("丢弃窗口外结果 %zu 条（最后一条「%s」）—— 松手超过 %.2f 秒才回来，"
                       "不再采信。若这是你刚喊的命令，把 VA_MIC_TAIL 调大（累计丢 %ld 条）",
                       n, late_dropped_last_.c_str(), (double)tail_secs_, late_dropped_);
            /* 光写日志不够：玩家看不到控制台。"毫无反应"是最难自查的失败形态 ——
               弹一句，他至少知道**话被听见了、只是来晚了**，而不是麦克风坏了。 */
            notice_ = "语音结果来晚了未采信：「" + late_dropped_last_ + "」";
        } else {
            dbg("丢弃窗口外结果 %zu 条（「%s」，累计 %ld）",
                n, late_dropped_last_.c_str(), late_dropped_);
        }
        return false;
    }
    auto one = std::move(I.queue.front());
    I.queue.pop_front();
    I.accepted.fetch_add(1);
    p_text = std::move(one.first);
    p_confidence = one.second;
    return true;
}

std::string MicVoice::interim() const {
    // 常开会话：没按住 Q 时不往外显示（HUD 的展开态只在 listening 时为真）
    if (!impl_ || !listen_) return {};
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
