#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_sfx.py —— 程序化生成 VolunteerArmyPC 的音效 WAV（P0 战斗可闻批次）

【为什么是"生成"而不是"下载"】
  网页版（../VolunteerArmy/index.html）的音效本来就是**程序化合成**的 —— 没有一个是
  采样素材，全是振荡器 + 噪声 + 滤波器搭出来的。PC 版沿同一条路：把它的声学设计
  在离线阶段渲染成 WAV，落到 assets/audio/。
  好处：素材可追溯（改一个参数重跑就有新的）、可版本管理、零外部依赖、不碰版权。

【声学设计的来源（照搬，不即兴）】
  本文所有频点/包络/滤波参数都取自 index.html：
    SND 空间参数表      index.html:2857-2906
    SND_GAP 限流表      index.html:2907-2913
    synthShot 三段式枪声 index.html:3041-3053
    synthSniper         index.html:3054-3061
    synthMetal          index.html:3062-3076
    synthBoom 爆炸       index.html:3078-3104
    BOOM 爆炸参数表      index.html:3105-3114
    synthBeeps          index.html:3116-3124
    synthClack          index.html:3140-3150
    SYN 分派表          index.html:3166-3312
  改参数请先对照上表，别凭感觉调 —— 否则 PC 版与网页版就会听出两套东西。

【与网页版的**已知差异**（诚实标注，不是遗漏）】
  1) 单发变体：网页版每次播放都用新噪声缓冲，所以连发时每一枪的噪声纹理都不同。
     这里一个 id 烘一个 WAV，纹理固定。**用播放端的 pitch_scale 抖动（±4%）补**
     （见 src/node/audio.cpp），听感上足够，但不等于真随机。
  2) `_nz(..., rate)` 的 rate：网页版靠 playbackRate 重采样 2 秒噪声缓冲来改亮度。
     离线这里**忽略 rate**，一律白噪声 —— 因为后面那级带通/高通才是决定音色的部件
     （例如 rifle band=1900 / mg band=1150 的区别全在这级），rate 只影响 2% 上下的
     高频微亮。这条是近似，不是等价。
  3) 频响扫移（爆炸的低通从 lp*2.4 扫到 lp*0.45）用**分段重算系数**实现，
     分段长度 32 采样 —— 比 WebAudio 的逐样本 a-rate 稍粗，听不出台阶。

用法：
  python tools/gen_sfx.py                 # 生成 P0 全部（默认）
  python tools/gen_sfx.py --only rifle    # 只生成某几个（调试用）
  python tools/gen_sfx.py --list          # 列 id 与时长预算
  python tools/gen_sfx.py --check         # 只校验已有 WAV（不动文件）
"""

import argparse
import math
import os
import random
import struct
import sys
import wave

SR = 44100          # 采样率
PEAK = 0.95         # 每个音的归一化峰值（响度差异交给播放端的 def.g）
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "assets", "audio")
TAIL = 0.05         # 每个音末尾留的静音，避免削掉释放尾巴

# ============================================================ 音效 id → 参数
# 只列 P0。P1/P2 加进来时，本表与 src/node/audio.cpp 的 SND 表要同步扩。
P0_IDS = [
    # 枪声
    "rifle", "mg", "sniper", "enemyRifle", "enemyMG",
    # 爆炸族（id 与 va_level.cpp:205-215 的 bid 一一对应）
    "explosion", "mine", "barrel", "grenade",
    "rocketBoom", "shellBoom", "vehicleBoom", "cannon",
    # 命中与人体
    "hit", "crack", "impact", "clang", "metal",
    "flesh", "body", "allyDown", "down", "hurt",
    # 换弹
    "reloadStart", "reloadEnd",
]

# synthShot 参数表（index.html:3178-3182）
SHOT = {
    "rifle":      dict(band=1900, hf=3400, a=0.55, b=0.40, lo=190, d1=0.085, d2=0.028, tail=0.10),
    "mg":         dict(band=1150, hf=2500, a=0.62, b=0.55, lo=150, d1=0.115, d2=0.032, tail=0.15),
    "enemyRifle": dict(band=1350, hf=2800, a=0.58, b=0.44, lo=170, d1=0.090, d2=0.030, tail=0.11),
    "enemyMG":    dict(band=950,  hf=2100, a=0.66, b=0.58, lo=140, d1=0.120, d2=0.034, tail=0.16),
}

# BOOM 参数表（index.html:3105-3114）
BOOM = {
    "explosion":  dict(dur=0.85, lp=420, f0=96, f1=32, a=0.95, b=0.85, echo=0.30),
    "mine":       dict(dur=0.70, lp=520, f0=110, f1=36, a=1.00, b=0.90, echo=0.26, click=1),
    "barrel":     dict(dur=0.60, lp=560, f0=120, f1=42, a=0.85, b=0.70, echo=0.22, metal=1),
    "grenade":    dict(dur=0.62, lp=480, f0=104, f1=38, a=0.90, b=0.75, echo=0.24, metal=1),
    "rocketBoom": dict(dur=0.80, lp=440, f0=100, f1=34, a=0.95, b=0.85, echo=0.30),
    "shellBoom":  dict(dur=1.00, lp=380, f0=84, f1=28, a=1.00, b=0.95, echo=0.36),
    "vehicleBoom":dict(dur=1.20, lp=360, f0=78, f1=26, a=1.00, b=1.00, echo=0.40, metal=1),
    "cannon":     dict(dur=1.10, lp=340, f0=72, f1=24, a=1.00, b=1.00, echo=0.40, click=1),
}


# ============================================================ 基础构件
def _sine(p):
    return math.sin(2.0 * math.pi * p)


def _tri(p):
    t = p - math.floor(p)
    return 2.0 * abs(2.0 * t - 1.0) - 1.0


def _saw(p):
    t = p - math.floor(p)
    return 2.0 * t - 1.0


def _sqr(p):
    t = p - math.floor(p)
    return 1.0 if t < 0.5 else -1.0


_WAVE = {"sine": _sine, "triangle": _tri, "sawtooth": _saw, "square": _sqr}


def osc(kind, dur, f0, f1=None, sr=SR):
    """振荡器：f0→f1 指数扫（等价 WebAudio exponentialRampToValueAtTime）。"""
    fn = _WAVE[kind]
    n = max(1, int(dur * sr))
    out = [0.0] * n
    if f1 is None or f1 == f0:
        step = f0 / sr
        ph = 0.0
        for i in range(n):
            out[i] = fn(ph)
            ph += step
        return out
    # 指数扫：频率按几何级数走
    ratio = f1 / f0
    for i in range(n):
        u = i / (n - 1) if n > 1 else 0.0
        f = f0 * (ratio ** u)
        ph = 0.0  # 占位：下面用独立累积（见 ph_acc）
        out[i] = f
    # 用频率序列积分出相位
    ph_acc = 0.0
    for i in range(n):
        ph_acc += out[i] / sr
        out[i] = fn(ph_acc)
    return out


def noise(n, rng):
    return [rng.uniform(-1.0, 1.0) for _ in range(n)]


def env(n, peak, atk, dec, sr=SR):
    """包络：线性攻起 0→peak（atk），再指数衰减 peak→1e-4（dec）。
    对应 WebAudio 的 linearRampToValueAtTime + exponentialRampToValueAtTime。"""
    if n <= 0:
        return []
    out = [0.0] * n
    na = min(n, max(1, int(atk * sr)))
    for i in range(na):
        out[i] = peak * (i / na)
    lo = 1e-4
    for i in range(na, n):
        tau = (i - na + 1) / sr
        if dec <= 1e-9:
            out[i] = lo
        else:
            out[i] = peak * ((lo / peak) ** (tau / dec)) if peak > 0 else lo
    return out


class Biquad:
    """RBJ cookbook 双二阶。系数可重算（用于扫频）。"""

    def __init__(self):
        self.b0 = 1.0; self.b1 = 0.0; self.b2 = 0.0
        self.a1 = 0.0; self.a2 = 0.0
        self.x1 = 0.0; self.x2 = 0.0
        self.y1 = 0.0; self.y2 = 0.0

    def _set(self, kind, f, q, sr):
        f = max(20.0, min(f, sr * 0.49))
        w0 = 2.0 * math.pi * f / sr
        cw = math.cos(w0); sw = math.sin(w0)
        alpha = sw / (2.0 * max(0.05, q))
        a0 = 1.0 + alpha
        if kind == "lowpass":
            self.b0 = ((1.0 - cw) / 2.0) / a0
            self.b1 = (1.0 - cw) / a0
            self.b2 = self.b0
        elif kind == "highpass":
            self.b0 = ((1.0 + cw) / 2.0) / a0
            self.b1 = -(1.0 + cw) / a0
            self.b2 = self.b0
        elif kind == "bandpass":     # 峰值增益 1（constant peak gain）
            self.b0 = alpha / a0
            self.b1 = 0.0
            self.b2 = -alpha / a0
        else:
            raise ValueError(kind)
        self.a1 = (-2.0 * cw) / a0
        self.a2 = (1.0 - alpha) / a0

    def process(self, x, kind, f0, f1=None, q=1.0, sr=SR, blk=32):
        n = len(x)
        out = [0.0] * n
        if f1 is None or abs(f1 - f0) < 1e-6:
            self._set(kind, f0, q, sr)
            for i in range(n):
                v = self.b0 * x[i] + self.b1 * self.x1 + self.b2 * self.x2 \
                    - self.a1 * self.y1 - self.a2 * self.y2
                self.x2 = self.x1; self.x1 = x[i]
                self.y2 = self.y1; self.y1 = v
                out[i] = v
            return out
        # 指数扫频：每 blk 个采样重算一次系数
        ratio = f1 / f0
        i = 0
        while i < n:
            u = i / (n - 1) if n > 1 else 0.0
            self._set(kind, f0 * (ratio ** u), q, sr)
            j = min(n, i + blk)
            while i < j:
                v = self.b0 * x[i] + self.b1 * self.x1 + self.b2 * self.x2 \
                    - self.a1 * self.y1 - self.a2 * self.y2
                self.x2 = self.x1; self.x1 = x[i]
                self.y2 = self.y1; self.y1 = v
                out[i] = v
                i += 1
        return out


def bq(x, kind, f0, f1=None, q=1.0, sr=SR):
    return Biquad().process(x, kind, f0, f1, q, sr)


def add_into(dst, src, t0, sr=SR, gain=1.0):
    i0 = int(t0 * sr)
    if i0 >= len(dst):
        return
    m = min(len(src), len(dst) - i0)
    for i in range(m):
        dst[i0 + i] += src[i] * gain


def _layer(n, signal, peak, atk, dec, sr=SR):
    e = env(n, peak, atk, dec, sr)
    return [signal[i] * e[i] for i in range(n)]


# ============================================================ 各音色
def synth_shot(buf, t0, k, p, sr=SR):
    """三段式：枪口爆音（带通噪声）+ 高频锐度（高通噪声）+ 胸腔低频（三角波）"""
    # 1) 枪口爆音
    d = p["d1"]
    n1 = int((d + 0.06) * sr)
    nz1 = bq(noise(n1, RNG), "bandpass", p["band"], q=1.05, sr=sr)
    add_into(buf, _layer(n1, nz1, k * p["a"], 0.002, d, sr), t0, sr)
    # 2) 高频锐度
    d = p["d2"]
    n2 = int((d + 0.03) * sr)
    nz2 = bq(noise(n2, RNG), "highpass", p["hf"], q=0.7, sr=sr)
    add_into(buf, _layer(n2, nz2, k * p["a"] * 0.6, 0.001, d, sr), t0, sr)
    # 3) 胸腔低频
    d = p["tail"]
    n3 = int((d + 0.02) * sr)
    o3 = osc("triangle", (d + 0.02), p["lo"], p["lo"] * 0.42, sr)
    add_into(buf, _layer(n3, o3, k * p["b"], 0.003, d, sr), t0, sr)


def synth_sniper(buf, t0, k, sr=SR):
    synth_shot(buf, t0, k, dict(band=2600, hf=5400, a=0.72, b=0.50, lo=210,
                                d1=0.062, d2=0.022, tail=0.10), sr)
    # 山谷回声：「啪——」拖尾
    d = 0.95
    n = int((d + 0.1) * sr)
    nz = bq(noise(n, RNG), "bandpass", 900, q=0.5, sr=sr)
    add_into(buf, _layer(n, nz, k * 0.24, 0.03, d, sr), t0 + 0.16, sr)


def synth_metal(buf, t0, k, length=1.0, sr=SR):
    for i, f0 in enumerate((1480, 2370, 3560, 5210)):
        f = f0 * (0.98 + RNG.random() * 0.04)
        d = (0.2 + 0.45 / (i + 1)) * length
        o = osc("sine", d + 0.05, f, f * 0.985, sr)
        add_into(buf, _layer(len(o), o, k * (0.42 / (i + 1)), 0.002, d, sr), t0, sr)
    nz = bq(noise(int(0.06 * sr), RNG), "bandpass", 3000, q=0.9, sr=sr)
    add_into(buf, _layer(len(nz), nz, k * 0.45, 0.001, 0.05, sr), t0, sr)


def synth_boom(buf, t0, k, p, sr=SR):
    """低频冲击波 + 低通噪声 + 破片高频 + 山谷余响"""
    d = p["dur"]
    # 1) 低通噪声冲击，截止从 lp*2.4 扫到 lp*0.45
    n1 = int((d + 0.12) * sr)
    nz1 = bq(noise(n1, RNG), "lowpass", p["lp"] * 2.4, p["lp"] * 0.45, q=0.75, sr=sr)
    add_into(buf, _layer(n1, nz1, k * 0.85 * p["a"], 0.004, d, sr), t0, sr)
    # 2) 正弦低频下扫（冲击波本体）
    n2 = int(d * 1.7 * sr)
    o2 = osc("sine", d * 1.7, float(p["f0"]), float(p["f1"]), sr)
    add_into(buf, _layer(n2, o2, k * p["b"], 0.006, d * 1.6, sr), t0, sr)
    # 3) 破片高频
    n3 = int(0.16 * sr)
    nz3 = bq(noise(n3, RNG), "highpass", 1600, q=0.7, sr=sr)
    add_into(buf, _layer(n3, nz3, k * 0.34, 0.001, 0.13, sr), t0, sr)
    # 4) 山谷余响
    n4 = int(d * 2.5 * sr)
    nz4 = bq(noise(n4, RNG), "lowpass", 260, q=0.9, sr=sr)
    add_into(buf, _layer(n4, nz4, k * p["echo"], 0.03, d * 2.4, sr), t0 + 0.1, sr)
    if p.get("metal"):
        synth_metal(buf, t0 + 0.05, k * 0.45, 1.3, sr)
    if p.get("click"):
        n5 = int(0.06 * sr)
        nz5 = bq(noise(n5, RNG), "bandpass", 1800, q=0.8, sr=sr)
        add_into(buf, _layer(n5, nz5, k * 0.5, 0.001, 0.05, sr), t0, sr)


def synth_beeps(buf, t0, k, freqs, dur, gap, kind="square", sr=SR):
    for i, f in enumerate(freqs):
        t = t0 + i * gap
        n = int((dur + 0.02) * sr)
        o = osc(kind, dur + 0.02, float(f), None, sr)
        o = bq(o, "bandpass", float(f), q=0.8, sr=sr)
        add_into(buf, _layer(n, o, k * (0.85 if i else 1.0), 0.005, dur, sr), t, sr)


def synth_clack(buf, t0, k, band, dur, thud, sr=SR):
    n = int((dur + 0.02) * sr)
    nz = bq(noise(n, RNG), "bandpass", band, q=1.1, sr=sr)
    add_into(buf, _layer(n, nz, k, 0.001, dur, sr), t0, sr)
    if thud:
        n2 = int(dur * 1.5 * sr)
        o2 = osc("triangle", dur * 1.5, 190, 120, sr)
        add_into(buf, _layer(n2, o2, k * 0.5, 0.002, dur * 1.4, sr), t0, sr)


# ============================================================ 分派：id → 渲染函数
def alloc_secs(sid):
    """给每个 id 一个充裕的缓冲长度（秒）。"""
    if sid in BOOM:
        return BOOM[sid]["dur"] * 2.5 + 0.45
    if sid == "sniper":
        return 1.35
    if sid in SHOT:
        p = SHOT[sid]
        return 0.05 + p["d1"] + 0.1 + p["tail"] + 0.12
    return {
        "hit": 0.35, "crack": 0.3, "impact": 0.3, "clang": 1.2, "metal": 3.0,
        "flesh": 0.35, "body": 0.5, "allyDown": 0.95, "down": 0.9, "hurt": 0.45,
        "reloadStart": 0.5, "reloadEnd": 0.4,
    }[sid]


def render(sid, sr=SR):
    """渲染一个 id，返回 float 列表（已归一化）。"""
    n = int(alloc_secs(sid) * sr) + int(TAIL * sr)
    buf = [0.0] * n
    if sid in SHOT:
        synth_shot(buf, 0.0, 1.0, SHOT[sid], sr)
    elif sid == "sniper":
        synth_sniper(buf, 0.0, 1.0, sr)
    elif sid in BOOM:
        synth_boom(buf, 0.0, 1.0, BOOM[sid], sr)
    elif sid == "crack":
        nz = bq(noise(int(0.045 * sr), RNG), "bandpass", 4200, q=1.6, sr=sr)
        add_into(buf, _layer(len(nz), nz, 1.0, 0.001, 0.035, sr), 0.0, sr)
        o = osc("sine", 0.06, 3000, 900, sr)
        add_into(buf, _layer(len(o), o, 0.5, 0.001, 0.05, sr), 0.0, sr)
    elif sid == "impact":
        synth_clack(buf, 0.0, 1.0, 800, 0.045, False, sr)
        o = osc("triangle", 0.06, 220, 110, sr)
        add_into(buf, _layer(len(o), o, 0.4, 0.002, 0.05, sr), 0.0, sr)
    elif sid == "clang":
        synth_metal(buf, 0.0, 1.0, 0.55, sr)
    elif sid == "metal":
        synth_metal(buf, 0.0, 1.0, 1.6, sr)
    elif sid == "flesh":
        nz = bq(noise(int(0.09 * sr), RNG), "lowpass", 700, q=0.8, sr=sr)
        add_into(buf, _layer(len(nz), nz, 1.0, 0.002, 0.07, sr), 0.0, sr)
        o = osc("triangle", 0.1, 150, 80, sr)
        add_into(buf, _layer(len(o), o, 0.6, 0.002, 0.09, sr), 0.0, sr)
    elif sid == "body":
        o = osc("sine", 0.2, 165, 55, sr)
        add_into(buf, _layer(len(o), o, 1.0, 0.003, 0.18, sr), 0.0, sr)
        nz = bq(noise(int(0.11 * sr), RNG), "lowpass", 500, q=0.8, sr=sr)
        add_into(buf, _layer(len(nz), nz, 0.5, 0.002, 0.09, sr), 0.0, sr)
    elif sid == "allyDown":
        # SYN.allyDown = body + 上滑锯齿（低通 900）
        render_into = render("body", sr)
        add_into(buf, render_into, 0.0, sr)
        dur = 0.55
        o = osc("sawtooth", dur, 220, 130, sr)
        o = bq(o, "lowpass", 900, q=0.8, sr=sr)
        add_into(buf, _layer(len(o), o, 0.5, 0.01, 0.5, sr), 0.03, sr)
    elif sid == "down":
        o = osc("sine", 0.6, 120, 44, sr)
        add_into(buf, _layer(len(o), o, 1.0, 0.004, 0.5, sr), 0.0, sr)
        nz = bq(noise(int(0.45 * sr), RNG), "lowpass", 420, q=0.7, sr=sr)
        add_into(buf, _layer(len(nz), nz, 0.6, 0.003, 0.4, sr), 0.0, sr)
    elif sid == "hurt":
        nz = bq(noise(int(0.16 * sr), RNG), "lowpass", 620, q=0.7, sr=sr)
        add_into(buf, _layer(len(nz), nz, 1.0, 0.002, 0.13, sr), 0.0, sr)
        o = osc("sine", 0.2, 110, 62, sr)
        add_into(buf, _layer(len(o), o, 0.5, 0.004, 0.16, sr), 0.0, sr)
    elif sid == "hit":
        synth_beeps(buf, 0.0, 1.0, [1050, 1580], 0.032, 0.035, "square", sr)
    elif sid == "reloadStart":
        synth_clack(buf, 0.0, 1.0, 2600, 0.015, False, sr)
        synth_clack(buf, 0.09, 0.95, 1300, 0.03, True, sr)
        synth_clack(buf, 0.20, 0.80, 900, 0.055, False, sr)
    elif sid == "reloadEnd":
        synth_clack(buf, 0.0, 1.0, 2200, 0.02, False, sr)
        synth_clack(buf, 0.05, 0.90, 3000, 0.015, False, sr)
        synth_metal(buf, 0.09, 0.35, 0.4, sr)
    else:
        raise KeyError(sid)
    return normalize(trim_tail(buf))


def trim_tail(x, thresh=0.0015, sr=SR):
    """砍掉尾部静音（保留 TAIL 秒），只留真正有声的部分。
    P0 全是一次性音效，这么砍没有副作用；将来做循环音（引擎/环境）时不要走这里。"""
    last = 0
    for i in range(len(x) - 1, -1, -1):
        v = x[i]
        if (v if v > 0 else -v) > thresh:
            last = i
            break
    end = min(len(x), last + int(TAIL * sr))
    return x[:end] if last else x[:int(TAIL * sr)]


def normalize(x):
    m = 0.0
    for v in x:
        a = -v if v < 0.0 else v
        if a > m:
            m = a
    if m < 1e-9:
        return x
    s = PEAK / m
    return [v * s for v in x]


def write_wav(path, x, sr=SR):
    frames = bytearray()
    for v in x:
        iv = int(round(v * 32767.0))
        if iv > 32767:
            iv = 32767
        elif iv < -32768:
            iv = -32768
        frames += struct.pack("<h", iv)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(bytes(frames))


def check_wav(path):
    """返回 (时长秒, 峰值, RMS, 采样率) —— 用于校验生成结果。"""
    with wave.open(path, "rb") as w:
        ch = w.getnchannels(); sw = w.getsampwidth(); sr = w.getframerate()
        n = w.getnframes()
        raw = w.readframes(n)
    if sw != 2:
        raise ValueError("非 16bit: %d" % sw)
    cnt = n * ch
    vals = struct.unpack("<%dh" % cnt, raw[:cnt * 2])
    peak = 0; acc = 0.0
    for v in vals:
        a = -v if v < 0 else v
        if a > peak:
            peak = a
        acc += float(v) * float(v)
    rms = math.sqrt(acc / cnt) / 32768.0 if cnt else 0.0
    return n / float(sr), peak / 32768.0, rms, sr


# ============================================================ main
RNG = random.Random(0x5EED)


def _seed_of(sid):
    """由 id 导出确定性种子。
    不能用内置 hash()：CPython 对字符串的 hash 每个进程都加了随机盐
    （PYTHONHASHSEED），同一份代码两次跑会得到不同的噪声纹理 —— 那就不叫
    "可重新生成"了。这里用朴素的 FNV-1a，跨进程/跨机器都一致。"""
    h = 0x811C9DC5
    for ch in sid.encode("utf-8"):
        h ^= ch
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="逗号分隔的 id，只生成这些")
    ap.add_argument("--list", action="store_true", help="列 id 与预算时长")
    ap.add_argument("--check", action="store_true", help="只校验已有 WAV")
    ap.add_argument("--out", default=OUT_DIR, help="输出目录")
    args = ap.parse_args()

    ids = list(P0_IDS)
    if args.only:
        want = [s.strip() for s in args.only.split(",") if s.strip()]
        bad = [w for w in want if w not in P0_IDS]
        if bad:
            print("未知 id: %s" % ", ".join(bad)); return 2
        ids = want

    if args.list:
        for sid in ids:
            print("%-14s 预算 %5.2fs" % (sid, alloc_secs(sid)))
        print("共 %d 个" % len(ids))
        return 0

    out_dir = os.path.abspath(args.out)
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)

    if args.check:
        ok = 0
        for sid in ids:
            p = os.path.join(out_dir, sid + ".wav")
            if not os.path.isfile(p):
                print("[缺失] %-14s %s" % (sid, p)); continue
            d, pk, rms, sr = check_wav(p)
            flag = "" if (rms > 0.01 and pk > 0.5) else "  <-- 可疑（近静音或未归一）"
            print("[ok] %-14s %5.2fs peak=%.3f rms=%.3f sr=%d%s"
                  % (sid, d, pk, rms, sr, flag))
            ok += 1
        print("校验 %d/%d" % (ok, len(ids)))
        return 0 if ok == len(ids) else 1

    # 生成：每个 id 用固定种子 → 可重复
    print("输出目录：%s" % out_dir)
    tot = 0.0
    for sid in ids:
        global RNG
        RNG = random.Random(0x5EED ^ _seed_of(sid))
        x = render(sid)
        p = os.path.join(out_dir, sid + ".wav")
        write_wav(p, x)
        d, pk, rms, sr = check_wav(p)
        tot += d
        print("%-14s %5.2fs peak=%.3f rms=%.3f  %7.1f KB"
              % (sid, d, pk, rms, os.path.getsize(p) / 1024.0))
    print("共 %d 个，合计 %.2f s" % (len(ids), tot))
    return 0


if __name__ == "__main__":
    sys.exit(main())
