#pragma once
// VolunteerArmyPC —— 逻辑层基础数学（对应网页版 index.html 顶部工具函数）
// 这一层刻意**不依赖 Godot**，保证逻辑层可以脱离引擎单独跑与单测。
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace va {

inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
inline int   clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float distf(float ax, float ay, float bx, float by) { return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by)); }
inline float dist2f(float ax, float ay, float bx, float by) { return (ax - bx) * (ax - bx) + (ay - by) * (ay - by); }

// 角度差，归一化到 (-PI, PI]
inline float angDiff(float a, float b) {
    float d = std::fmod(a - b, 6.283185307179586f);
    if (d > 3.141592653589793f) d -= 6.283185307179586f;
    if (d < -3.141592653589793f) d += 6.283185307179586f;
    return d;
}
inline float normAng(float v) {
    float d = std::fmod(v, 6.283185307179586f);
    if (d > 3.141592653589793f) d -= 6.283185307179586f;
    if (d < -3.141592653589793f) d += 6.283185307179586f;
    return d;
}

// ---------------------------------------------------------------- 随机数
// 与网页版同算法（mulberry32），保证同种子下战局可复现，便于回归比对。
class Rng {
public:
    explicit Rng(uint32_t seed = 1u) { s_ = seed; }
    void reseed(uint32_t seed) { s_ = seed; }
    float next() {
        s_ += 0x6D2B79F5u;
        uint32_t t = s_;
        t = (t ^ (t >> 15)) * (t | 1u);
        t += (t ^ (t >> 7)) * (t | 61u);
        return float((t ^ (t >> 14))) / 4294967296.0f;
    }
    float range(float a, float b) { return a + next() * (b - a); }
    int   irange(int a, int b) { return int(std::floor(range(float(a), float(b) + 1.0f))); }
    uint32_t state() const { return s_; }
private:
    uint32_t s_ = 1u;
};

// 线段-圆相交（LOS 判定核心）
inline bool segCircle(float x1, float y1, float x2, float y2, float cx, float cy, float r) {
    const float dx = x2 - x1, dy = y2 - y1;
    const float fx = x1 - cx, fy = y1 - cy;
    const float a = dx * dx + dy * dy;
    if (a < 1e-6f) return dist2f(x1, y1, cx, cy) <= r * r;
    float t = -(fx * dx + fy * dy) / a;
    t = clampf(t, 0.0f, 1.0f);
    const float px = x1 + dx * t - cx, py = y1 + dy * t - cy;
    return px * px + py * py <= r * r;
}

inline float hypot2f(float x, float y) { return std::sqrt(x * x + y * y); }

} // namespace va
