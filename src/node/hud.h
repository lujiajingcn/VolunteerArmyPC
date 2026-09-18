#pragma once
// VolunteerArmyPC —— HUD：手工自绘的使命召唤风格界面外壳
//
// 【为什么整篇手绘，而不是堆 Label】
// 使命召唤那套界面的辨识度几乎全在"形状"上：45° 切角面板、1px 冷白细描边、
// 罗盘上的扇形刻度、随散布张合的准星、雷达的旋转视野锥。这些用 Label 拼不出来；
// Label 只能表达文字，而这里一半的信息是用几何讲的。
// 所以本类继承 Control，全部走 _draw() 手绘：一个节点、一次绘制，
// 任何分辨率下都是矢量的（缩放系数 s_ = 视口高度 / 1080）。
//
// 【为什么主菜单 / 任务简报也在本类里，而不是另起一个 Control】
// 本类已经握有整套绘制原语（chamfer / poly_panel / tx / tx_c / tw / bar）、
// 已解析好的字体（含中文回退链）、以及 1080p 设计稿的缩放系数 s_。
// 主菜单与简报用的完全是同一套视觉语言（切角面板、冷白细描边、粗体大写标题、
// 琥珀色高亮）。另起一个 Control 就得把这套原语与状态复制一遍，
// 而且菜单 ↔ 简报 ↔ 战斗 ↔ 结算 之间的切换会变成跨节点握手。
// 所以按「同一个界面外壳的不同屏」来做：
//     SCREEN_MENU   主菜单        —— key art 背景 + 标题 + 菜单项
//     SCREEN_BRIEF  任务简报      —— 区域态势图 + 任务元信息 + 目标清单
//     SCREEN_PLAY   战斗中        —— 原有的全部战术 HUD
// 结算面板仍归 SCREEN_PLAY（它压在战场画面上），只是背景换成了 key art。
//
// 【数据来源：只读】
//   - va::W（逻辑层全局状态）—— 弹药、生命、小队、目标、车队、天气
//   - 「观测量跳变」推导的瞬时事件 —— 击中 / 受击 / 阵亡 / 阶段切换
// 和 viewmodel 同一原则：HUD 不改逻辑层、不反向写状态。
// 逻辑层删掉这些字段也不影响它自己跑测试（HUD 只是消费者）。
#include <deque>
#include <set>
#include <string>
#include <vector>

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/vector2.hpp>

namespace volunteer_army {

class Hud : public godot::Control {
    GDCLASS(Hud, godot::Control)

public:
    // 界面外壳的屏。顺序即流程：MENU → BRIEF → PLAY。
    enum Screen {
        SCREEN_MENU = 0,
        SCREEN_BRIEF = 1,
        SCREEN_PLAY = 2,
    };

    // 小队名册的格数 = 玩家 + ROSTER 里的 10 人。
    // 【为什么放 public】hud.cpp 的匿名命名空间里有一张同长度的 id 表
    // （kRosterIds），两边靠 static_assert 互锁。放在 private 里时
    // 那张表连长度都申请不下来（C2248），只能各写一个字面量 11 ——
    // 而"同一个数字写两遍"正是日后加人时漏改一处的来源。
    // 现在任一处不同步都是**编译期**报错，不是运行期看走眼。
    static constexpr int kRosterMax = 11;

    Hud();
    ~Hud() override;

    void _draw() override;
    // 注意：不加 override —— godot-cpp 的 _notification 不是虚函数基方法，
    // 加了 override 会报 C3668。
    void _notification(int p_what);

    // 每帧由 world_sim 调用：采样世界状态、推进动画计时、请求重绘
    void update(double p_delta);

    // ---- 界面外壳 ----
    // ---- 界面外壳 ----
    // 任务素材。走 Image::load_from_file 而不是 ResourceLoader：
    // 这些 PNG 是运行时新加进工程的，没有经过编辑器的导入工序，
    // res:// 下不存在对应的 .ctex，ResourceLoader 会直接报错。
    // （load_art 里两条路都试，谁成用谁。）
    // world_sim 在 _ready 里调一次 —— 纹理必须在首帧 _draw 之前就位，
    // 否则会先闪一帧没有背景的菜单。
    void load_art();

    Screen screen() const { return screen_; }
    void   set_screen(Screen s);
    // 是否还在菜单/简报里（world_sim 据此暂停逻辑步进、并让键鼠改走界面）
    bool   shell_active() const { return screen_ != SCREEN_PLAY; }
    // 键盘：返回 true 表示这次按键被界面吃掉，不要传给游戏逻辑
    bool   shell_key(int64_t p_keycode);
    // 鼠标：主菜单用鼠标悬停 / 点击（PC 菜单不能只认键盘）
    void   shell_hover(const godot::Vector2 &p);
    bool   shell_click(const godot::Vector2 &p);
    // 玩家在简报里确认开始（消费一次；world_sim 据此接管鼠标并同步实体）
    bool   take_start();
    // 玩家选了退出
    bool   take_quit();

    // ---- SimEvents 转发入口 ----
    void ev_toast(const std::string &text);
    void ev_alert(const std::string &text, float dur);

    // 结算面板是否正在显示（world_sim 据此允许 R 重开）
    bool mission_over() const;

    // ---- 战斗事件取证（VA_CAPTURE_EV）----
    /* 命中标记只亮 0.24 秒、击杀飘字 1.5 秒，靠"按战局秒数定时截图"去撞它们
       必然要碰运气 —— 上一轮这三条路径就是因为撞不上，一直挂着"未验证"。
       这里换个方向：**由事件的产生者声明"该留一张证据了"**，由桥接层落盘。
       不让 HUD 自己截图的原因很具体：它每帧重画，拿不到"相机变换已生效"的
       时机保证，而立即读视口纹理只会拿到上一帧（没有标记的那一帧）。 */
    enum EvShot { EVSHOT_NONE = 0, EVSHOT_HIT, EVSHOT_KILL, EVSHOT_ALLY, EVSHOT_DOWN };
    EvShot take_ev_shot();          // 消费一次（同时只允许一张在途）

protected:
    static void _bind_methods() {}

private:
    // ======================================================== 布局 / 缩放
    // 所有坐标都按「1080p 设计稿」写，绘制时统一乘 s_。
    // 要调版面只需要改这里的常量，不用满文件找数字。
    float s_ = 1.0f;                    // 设计单位 → 像素
    godot::Vector2 vp_{};               // 视口尺寸（像素）
    godot::Ref<godot::Font> font_;
    bool layout();

    // ======================================================== 绘制原语
    // 切角矩形：使命召唤面板的基本形状。cut=0 时退化成普通矩形。
    godot::PackedVector2Array chamfer(const godot::Rect2 &r, float cut) const;
    void poly_panel(const godot::Rect2 &r, float cut, const godot::Color &fill,
                    const godot::Color &line, float lw);
    // 横向渐变（左 / 中 / 右三个停靠点）。逐顶点配色，一次调用完成插值 ——
    // 没有"分段"，也就不可能出现分段边界那种规则竖纹。
    // 用它替代"叠 N 条不同 alpha 的 draw_rect"：见 hud.cpp 里的实测数据。
    void grad_h(const godot::Rect2 &r, const godot::Color &cl, const godot::Color &cm,
                const godot::Color &cr);
    // 通用版：固定色，alpha 由 alpha_at(t)（t∈[0,1]）给出 —— 可以表达任意剖面
    // （罗盘底衬的 sin^1.35 就要这个；三停靠点的线性近似会把两翼压得太透）。
    // 传无捕获 lambda 即可。
    void grad_h(const godot::Rect2 &r, const godot::Color &rgb, float (*alpha_at)(float t));
    // 竖向版（t 由上沿 0 到下沿 1）。给"底部渐浓"的衬底用：上沿 alpha 取 0，
    // 于是看不出衬底从哪开始；下沿落在屏幕底边，也不会多出一条边界。
    // 如果改用"上方一条硬边的矩形"，那条边本身就是新的可见缺陷。
    void grad_v(const godot::Rect2 &r, const godot::Color &rgb, float (*alpha_at)(float t));
    void bar(const godot::Rect2 &r, float t, const godot::Color &fg, const godot::Color &bg);
    float tw(const godot::String &t, int size) const;
    // tx = 给定基线；tx_top = 给定顶边；tx_mid = 给定竖直中心
    void tx(const godot::String &t, float x, float baseline, int size, const godot::Color &c,
            godot::HorizontalAlignment al = godot::HORIZONTAL_ALIGNMENT_LEFT, float w = -1.0f);
    void tx_top(const godot::String &t, float x, float top, int size, const godot::Color &c,
                godot::HorizontalAlignment al = godot::HORIZONTAL_ALIGNMENT_LEFT, float w = -1.0f);
    void tx_mid(const godot::String &t, float x, float cy, int size, const godot::Color &c,
                godot::HorizontalAlignment al = godot::HORIZONTAL_ALIGNMENT_LEFT, float w = -1.0f);
    // 绕一个点居中 / 右对齐。
    // 【为什么需要这两个】draw_string 的对齐是"在 [pos.x, pos.x+width] 这个盒子内对齐"，
    // 传 width=0 时盒子宽度为零、对齐直接失效（文字仍从 pos.x 往右画）。
    // 想让文字"以 cx 为中心"，就得先自己量宽度。这两个函数把这件事包掉。
    void tx_c(const godot::String &t, float cx, float baseline, int size, const godot::Color &c);
    void tx_r(const godot::String &t, float rx, float baseline, int size, const godot::Color &c);

    // ======================================================== 界面外壳
    godot::Ref<godot::Texture2D> load_tex(const godot::String &p_res_path);
    // 按「cover」铺满：等比放大到刚好盖住视口，多出来的居中裁掉。
    // 不能用拉伸 —— 素材是 16:9、视口也是 16:9，但窗口尺寸可被玩家改，
    // 一旦比例变了拉伸就会把人脸拉扁。
    void draw_art_bg(const godot::Ref<godot::Texture2D> &tex, float dim_center, float dim_edge);
    void draw_menu();             // 主菜单
    void draw_brief();            // 任务简报
    // 小队名册：11 张人物胸像 + 姓名 / 职务 / 血条。
    // 胸像由 tools/prep_char.py 从全身立绘里自动裁出，数据直读 va::W.units。
    void draw_roster(float p_cx, float p_baseline);
    godot::Rect2 menu_item_rect(int i) const;
    int   menu_hit(const godot::Vector2 &p) const;
    void  menu_activate(int i);

    godot::Ref<godot::Texture2D> art_menu_;     // 主菜单背景
    godot::Ref<godot::Texture2D> art_brief_;    // 简报主视觉
    godot::Ref<godot::Texture2D> art_chapter_;  // 区域态势（俯瞰公路）
    godot::Ref<godot::Texture2D> art_win_;      // 结算 · 成功
    godot::Ref<godot::Texture2D> art_lose_;     // 结算 · 失败
    // 11 张队员胸像，下标与 hud.cpp 里的 kRosterIds 一一对应
    // （长度取自 public 的 kRosterMax，见类首的说明）
    godot::Ref<godot::Texture2D> art_port_[kRosterMax];

    Screen screen_ = SCREEN_PLAY;
    int    menu_sel_ = 0;         // 当前菜单项
    float  menu_t_ = 0;           // 菜单停留时长（驱动脉冲/闪烁）
    float  brief_t_ = 0;          // 简报停留时长
    bool   start_req_ = false;    // 玩家确认开始（take_start 消费）
    bool   quit_req_ = false;     // 玩家选择退出（take_quit 消费）

    // ======================================================== 各区块
    void draw_compass();          // 顶部罗盘刻度带（含目标方位标记）
    void draw_minimap();          // 左上雷达
    void draw_info_strip();       // 雷达下方：时间 / 天气 / 阶段 / 噪声
    void draw_objectives();       // 中央目标横幅
    void draw_alert();            // 警报（伏击开始 / 炮击来袭 / 增援到达）
    void draw_killfeed();         // 右上击杀回执
    void draw_squad();            // 右侧小队状态板
    void draw_ammo();             // 右下弹药 + 装备 + 生命
    void draw_command_panel();    // 左下指挥链路
    void draw_subtitles();        // 底部无线电字幕
    void draw_crosshair();        // 动态准星
    void draw_hitmarker();        // 命中标记
    void draw_damage_arcs();      // 受击方位弧
    void draw_score_popups();     // 击杀飘字
    void draw_downed();           // 倒地倒计时
    void draw_vignette();         // 受伤全屏暗角
    void draw_end_panel();        // 结算面板（背景换成 key art）
    void draw_help();             // 操作提示（开局一段时间后淡出）

    // ======================================================== 状态采样
    float heading_deg() const;                 // 相机朝向 → 罗盘度数（0=北）
    void  reset_transient();                   // 新一局：清空所有瞬时元素

    struct FeedItem {
        std::string killer, victim;
        bool enemy = false;      // victim 是敌方 → 好事（绿）
        bool byPlayer = false;
        float t = 0;
    };
    struct DmgArc  { float wang = 0; float t = 0; };        // 世界方位角（跟随转头）
    struct Popup   { std::string text; godot::Color col; float t = 0; };
    struct Toast   { std::string text; float t = 0; bool alert = false; };

    std::deque<FeedItem> feed_;
    std::deque<DmgArc>   dmg_;
    std::deque<Popup>    popups_;
    std::deque<Toast>    toasts_;
    std::set<std::string> reported_;   // 已播报过阵亡的单位 id（防止重复）

    float hit_t_ = 0, hit_kill_t_ = 0;   // 命中标记 / 击杀命中标记
    float bloom_ = 0;                    // 连发导致的准星额外张开
    float flash_ = 0;                    // 受击瞬间红闪
    float banner_t_ = 0;                 // 目标横幅切入动画
    float end_t_ = 0;                    // 结算面板切入动画
    float help_t_ = 0;                   // 操作提示累计显示时长
    float clock_ = 0;                    // 自由计时（脉冲动效用）

    std::string banner_title_, banner_sub_;

    int   seen_hits_ = 0, seen_kills_ = 0, seen_mag_ = 0;
    float prev_hp_ = -1.0f;
    bool  prev_down_ = false, prev_over_ = false;
    std::string prev_phase_;
    std::vector<unsigned char> obj_done_;
    float last_t_ = -1.0f;

    // VA_HUD_EV：把三条「需要特定战斗事件才会出现」的路径（命中标记 / 击杀飘字 /
    // 倒地倒计时）的**触发时刻**打点到控制台。
    // 用途是把"事件什么时候发生"变成可读的数字 —— 截图探针按战局秒数触发，
    // 没有这个就只能靠密集截图碰运气，而这三条路径里最短的命中标记只亮 0.24 秒。
    bool  ev_log_ = false;

    // 事件取证的待办槽位（见 EvShot 的说明）
    EvShot pending_shot_ = EVSHOT_NONE;
    bool   ev_cap_ = false;      // VA_CAPTURE_EV：事件当帧自动落盘
};

} // namespace volunteer_army
