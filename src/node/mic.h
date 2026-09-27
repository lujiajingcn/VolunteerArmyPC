#pragma once
// VolunteerArmyPC —— 语音指令输入层
//
// 【为什么要有这一层】逻辑层 src/sim/ 从第一天起就是按"用嘴下令"写的 ——
//   parse_command(text, noise, asr, typed, source) 里那个 asr 参数就是
//   **语音识别置信度**，noise 是"战场噪声降低识别率"，run_command_text 的
//   注释写着"供 HUD / 语音层调用"。网页版靠浏览器自带的 webkitSpeechRecognition
//   收话（../VolunteerArmy/index.html:5058），PC 版没有那个 API，
//   于是这一层空了 —— 队员会听、会回话、会拒绝，但玩家没法把话说出去。
//
// 【音从哪来】两条后端，都不联网：
//   ① winrt-onecore —— Windows.Media.SpeechRecognition（OneCore DNN），走 C++/WinRT。
//      识别质量好（DNN 模型）。⚠️ 它要求系统的「在线语音识别」隐私开关是开的：
//      HKCU\Software\Microsoft\Speech_OneCore\Settings\OnlineSpeechPrivacy\HasAccepted=1。
//      这个值**缺失**时 setup 一切正常，一到 StartAsync 就报
//      "The speech privacy policy was not accepted" —— 最典型的静默失败。
//   ② sapi5 —— 老 Speech 8.0 桌面识别器（MS-2052-80-DESK），裸 COM，全本地。
//      不需要任何隐私开关；本机的中文语音配置早就跑过向导了
//      （HKCU\...\Speech\Preferences\zh-CN_CompletedSpeechConfiguration=1）。
//
//   默认 auto：先试 ①，①起来就挂（含上面那条隐私错误）→ 自动切 ②。
//   VA_MIC_BACKEND=winrt|sapi|auto 可强制；强制那条失败就不回退。
//
// 【⚠️ 本头文件必须保持"轻"】
//   唯一一份 winrt 头与 sapi.h 只在 mic.cpp 里 include，本头只用 pimpl 前置声明。
//   理由：world_sim.cpp 有 12 万行，一旦把 C++/WinRT 的 base.h 链进它的
//   包含图，那一处的编译时间会涨好几倍，还会把一批 winrt 内部警告混进
//   项目"自身 0 warning"的基线里。audio/voice/fx 三层同样是这个隔离原则。
//
// 【⚠️ 线程】两条后端的识别回调都在**各自的识别线程**上跑
//   （WinRT 是识别器线程；SAPI5 是本层自己起的 worker，等 ISpRecoContext 的
//   通知事件），而本工程所有逻辑推进都在主线程（va::step_once 的调用者）。
//   跨线程直接碰 va:: 就是数据竞争 —— 所以回调只把文本塞进队列，
//   由主线程每帧 poll() 取出来再下发。

#include <chrono>
#include <memory>
#include <string>

namespace volunteer_army {

class MicVoice {
public:
    MicVoice();
    ~MicVoice();

    MicVoice(const MicVoice &) = delete;
    MicVoice &operator=(const MicVoice &) = delete;

    /* 建识别器 + 装约束 + 编译。失败不抛，记在 last_error() 里，
       available() 会是 false（整层静默降级，不影响游戏其它部分）。
       p_dictation=true 用"自由听写 + 主题约束"，识别整句中文再交给
       parse_command 做容错匹配 —— 网页版也是这条路（自由识别 + handleUtterance）。
       ⚠️ 为什么不把指令词装成 ListConstraint：那是**整句匹配** ——
       列表里放"开火"，玩家说"全体开火"就匹配不上；而本游戏的指令是
       「呼号 + 动作 + 地点」的组合，列表会组合爆炸。 */
    void setup(bool p_dictation);

    bool available() const;          // 至少有一条后端建起来了
    bool enabled() const;            // VA_MIC=0 时为 false
    void set_enabled(bool p_on) { enabled_ = p_on; }
    bool listening() const;          // 正在听

    /* 按住说话：按下 start()，松开 stop()。stop 之后结果可能还要几百毫秒才回来。

       ⚠️ OneCore 后端是**会话常开**：只有第一次 start() 会真的 StartAsync 起会话，
       之后 stop() 只是"不再采信结果"（会话不关，结果照样进队列、由 poll 丢掉）。
       两条实测理由（2026-09-27，见 mic.cpp 的 MicVoice::start）：
         ① StopAsync 是异步的，没落地时再 StartAsync 会**同步抛** hresult_error
            ——VS 调试器在抛出点中断，玩家看到的就是"按 Q 说话游戏崩了"；
         ② StopAsync 还要等"当前这句说完"，不等它就永远起不来第二次。
       SAPI5 后端不变，仍是每次真开真关（它没有 Stopping 这个异步态）。
       玩家侧语义完全一样：按住才生效。区别只是麦克风从第一次按 Q 起一直开着。

       ⚠️⚠️ **采信窗口 = 按住期间 + 松手后一小段（尾窗）**，不是"按住期间"。
       识别器是**滞后**的：一句「全体撤退」说完要等一小段静音才定稿
       （OneCore 的 ResultGenerated 就是这么触发的）。玩家的自然动作是
       "说完就松手" ⇒ 定稿几乎必然落在松手**之后**。
       早先按 listen_ 一刀切（松手即丢），于是"按住 Q 喊了命令没有任何反应"，
       而且**崩都不崩、一句日志都不留** —— 2026-09-27 实测踩到。
       尾窗长度 VA_MIC_TAIL（秒，默认 2.0）。 */
    void start();
    void stop();

    /* 主线程每帧调用一次，**必须在 poll() 之前**。
       它做的事：把「玩家还按着 Q 吗」（want_）与实际会话状态对账 ——
       主要是 OneCore 起不来时换成 SAPI5 并把这次会话续上（见 mic.cpp 的 pump）。
       为什么要拆成单独一步：换后端要拆 COM 对象、建新对象，只能在主线程做；
       而"起不来"这件事是在 start() 里发现的，那时还在 _input 阶段。 */
    void pump();

    /* 主线程每帧调用。返回 true 时 p_text 是一句完整的话，可直接下发。
       p_confidence 是识别置信度（0~1，低置信度由逻辑层按噪声模型打折）。
       ⚠️ 只在**采信窗口**内返回结果（见 start/stop 上面那段）；窗口外的结果
       连带清掉队列 —— 否则会话常开时，玩家没按 Q 的自言自语也会下令。 */
    bool poll(std::string &p_text, float &p_confidence);

    // 当前是否处于采信窗口（按住期间，或松手后的尾窗内）。诊断用。
    bool accepting() const;

    // HUD 用：正在识别中的**中间结果**（还没定稿那句），没有则为空串。
    std::string interim() const;

    /* 取走一条"该报给玩家"的一次性通知（换过后端 / 起不来），取完即清空。
       换后端这件事玩家看不见 —— 得在 HUD 上说一句，否则他会以为 Q 坏了。 */
    std::string take_notice();

    // 诊断
    const std::string &backend() const { return backend_; }
    const std::string &last_error() const { return last_error_; }
    long accepted() const;      // 收到几句定稿
    long rejected() const;      // 识别器报错丢掉的次数（回调线程计数，故放 .cpp）

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    void try_start();           // 真的去起会话（按当前 kind 分派）
    void do_fallback();         // OneCore 起不来 → 换 SAPI5 并续上会话

    bool enabled_ = true;
    bool listen_ = false;       // 会话**实际**在听
    bool want_ = false;         // 玩家**意图**在听（按着 Q）；两者分开是为了回退

    /* 采信窗口的截止时刻。按住期间置成 time_point::max()（"一直采信"），
       松手时置成 now + tail_secs_。steady_clock：单调，不受系统时间调整影响。 */
    std::chrono::steady_clock::time_point accept_until_{};
    float tail_secs_ = 2.5f;    // VA_MIC_TAIL
    /* 采信窗口外丢掉的定稿（只在主线程 poll 里改，不需要锁）。
       这两项存在的原因：**"喊了没反应"与"没识别出来"必须能分开看** ——
       前者是本层把结果丢了，后者是识别器没出结果，处置完全不同。 */
    long late_dropped_ = 0;
    std::string late_dropped_last_;
    bool pending_fallback_ = false;
    std::string pref_ = "auto"; // VA_MIC_BACKEND
    std::string backend_ = "none";
    std::string last_error_;
    std::string notice_;        // 一次性通知
};

} // namespace volunteer_army
