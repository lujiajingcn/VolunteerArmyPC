#pragma once
// VolunteerArmyPC —— HUD：手工自绘的使命召唤风格战术界面
//
// 【为什么整篇手绘，而不是堆 Label】
// 使命召唤那套界面的辨识度几乎全在"形状"上：45° 切角面板、1px 冷白细描边、
// 罗盘上的扇形刻度、随散布张合的准星、雷达的旋转视野锥。这些用 Label 拼不出来；
// Label 只能表达文字，而这里一半的信息是用几何讲的。
// 所以本类继承 Control，全部走 _draw() 手绘：一个节点、一次绘制，
// 任何分辨率下都是矢量的（缩放系数 s_ = 视口高度 / 1080）。
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
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/vector2.hpp>

namespace volunteer_army {

class Hud : public godot::Control {
    GDCLASS(Hud, godot::Control)

public:
    Hud();
    ~Hud() override;

    void _draw() override;
    // 注意：不加 override —— godot-cpp 的 _notification 不是虚函数基方法，
    // 加了 override 会报 C3668。
    void _notification(int p_what);

    // 每帧由 world_sim 调用：采样世界状态、推进动画计时、请求重绘
    void update(double p_delta);

    // ---- SimEvents 转发入口 ----
    void ev_toast(const std::string &text);
    void ev_alert(const std::string &text, float dur);

    // 结算面板是否正在显示（world_sim 据此允许 R 重开）
    bool mission_over() const;

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
    void draw_end_panel();        // 结算面板
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
};

} // namespace volunteer_army
