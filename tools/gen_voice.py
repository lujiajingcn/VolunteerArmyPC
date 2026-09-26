# -*- coding: utf-8 -*-
"""任务介绍语音（VO）素材生成器 —— 把每关的简报口播稿烘成 WAV。

【为什么要生成，而不是录音】与 `tools/gen_sfx.py` 同一条路线：可追溯、可重跑、
零版权、零外部依赖。音源用**本机 Windows 的 TTS**（离线，不走网络）：
优先 Windows.Media.SpeechSynthesis（OneCore，有男声 **Kangkang**，适合军事简报），
回退 SAPI5 的 SpVoice（本机只有女声 Huihui）。

【单一数据源】口播稿写在本文件顶部的 LINES 表里。这里同时产出一份
`src/node/voice_lines.h`，C++ 侧从它拿 id 与文本 —— 这样"播出去的声音"和
"HUD/日志里写的字"永远是同一句话，不会各写一份慢慢漂移。
脚本会校验表里的每个 id 在当前 `src/sim/va_campaign.cpp` 的关卡表里真实存在
（改名/删关之后忘记重跑，会在这里直接报错，而不是等实机上"没有声音"）。

【为什么稿子是人写的而不是从 LevelDef 字段拼的】字段里全是军语缩写
（`63军187师561团3营（前出警戒分队）`），机器拼出来的读法是"一百八十七师"——
错了，番号要读"一八七师"；括号读出来更怪。这类东西只有人写才自然。
脚本只负责**校验 id 对得上**，不负责造句。

用法：
    PY="C:/Users/lujiajing/.workbuddy/binaries/python/envs/default/Scripts/python.exe"
    "$PY" tools/gen_voice.py --check        # 只校验，不合成
    "$PY" tools/gen_voice.py                # 全量合成
    "$PY" tools/gen_voice.py --only l1_yunvfeng
    "$PY" tools/gen_voice.py --voice huihui # 换音色（对照试听用）
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAMPAIGN = os.path.join(ROOT, "src", "sim", "va_campaign.cpp")
OUT_DIR = os.path.join(ROOT, "assets", "audio")
HDR = os.path.join(ROOT, "src", "node", "voice_lines.h")

# ---- 目标波形格式：与 assets/audio 下其它素材一致 ----
RATE = 44100
PEAK = 0.95          # 归一化目标峰值（线性）


# ============================================================ 口播稿
# 每条 = 一个 wav（assets/audio/vo_<id>.wav）。
#
# 【写稿约定】
#   · 一句话一个意思，句号断得早一点 —— TTS 靠标点断句，逗号太密会赶。
#   · 数字一律写中文（"二百四十秒"不写 "240 秒"）：TTS 读阿拉伯数字会按
#     数量词走，而"240"在中文语音里偶尔读成"两百四"，播报腔里很跳。
#   · 军语照史实读法：番号读单 digit（"一八七师"），不读"一百八十七师"。
#   · 稿子长度 **12~20 秒**为宜：短于 12 秒显得敷衍，长于 20 秒玩家已经
#     自己看完简报点了开始，语音还在念。
LINES = [
    ("menu", "志愿军第六十三军，铁原阻击战。五个阵地，一条防线。", 0.9),
    (
        "l1_yunvfeng",   # 1951.5.30 · 玉女峰前出警戒阵地 · 美骑兵第1师搜索分队
        "全体注意。玉女峰前出警戒阵地，五月三十日。"
        "当面之敌，美骑兵第一师搜索分队。"
        "任务，把敌人钉在公路上，拖住二百四十秒。"
        "各排占领侧射位，隐蔽待机，听我口令。",
        1.0,
    ),
    (
        "l2_233",        # 1951.6.1 · 233.2 高地 · 美骑兵第1师 5 营 + 4 炮兵营 + 11 辆坦克
        "全体注意。二三三点二高地，六月一日。"
        "当面之敌，美骑兵第一师五个营，外加四个炮兵营、十一辆坦克。"
        "任务，守住高地，拖住二百四十秒。反坦克手盯住公路。",
        1.0,
    ),
    (
        "l3_zhongzishan",  # 1951.6.2—6.3 · 种子山 · 当面之敌 美第25师
        "全体注意。种子山，六月二日。"
        "当面之敌，美第二十五师。"
        "任务，夜战近战，拖住二百四十秒。"
        "天黑，看准了再打，别打着自己人。",
        1.0,
    ),
    (
        "l4_jiufeng",    # 1951.6.3 · 鹫峰 · 美第25师 3 个团 + 坦克集群
        "全体注意。鹫峰，第二道防线左翼，六月三日。"
        "当面之敌，美第二十五师三个团，坦克开道。"
        "任务，顶住正面，拖住二百四十秒。弹药省着用，打准一点。",
        1.0,
    ),
    (
        "l5_shazongdong",  # 1951.6.5—6.6 · 沙宗洞 · 美骑兵第1师加强团，两翼迂回
        "全体注意。沙宗洞，六月六日。"
        "当面之敌，美骑兵第一师加强团，会从两翼迂回。"
        "任务，最后一个阵地，拖住二百四十秒。"
        "背后是悬崖，没有退路，人打光了也得顶住。",
        1.0,
    ),
]


# ============================================================ 音色
VOICES = {
    # 优先：OneCore（Windows.Media.SpeechSynthesis），男声，播报腔
    "kangkang": ("winrt", "Microsoft Kangkang"),
    "yaoyao": ("winrt", "Microsoft Yaoyao"),
    # 回退：SAPI5（本机只有 Huihui；Zira 是英文，不能用）
    "huihui": ("sapi", "Microsoft Huihui Desktop"),
    "sapi_auto": ("sapi", None),
}


# ============================================================ 工具
def die(msg: str) -> None:
    print("!! " + msg, file=sys.stderr)
    sys.exit(1)


def level_ids() -> list[str]:
    """从关卡表里抓出全部关卡 id（只用来校验 LINES 没写错）。"""
    src = open(CAMPAIGN, encoding="utf-8").read()
    out = []
    for _tag, body in re.findall(r"static const LevelDef (L\d)\{(.*?)\n\};", src, re.S):
        m = re.search(r'"((?:[^"\\]|\\.)*)"', body)
        if m:
            out.append(m.group(1))
    return out


def wav_info(path: str) -> tuple[float, int, int]:
    """(时长秒, 采样率, 峰值 0~1)。自写解析，不依赖第三方。"""
    with wave.open(path, "rb") as w:
        n, rate, ch, sw = w.getnframes(), w.getframerate(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(n)
    if sw != 2:
        return (n / float(rate), rate, 0.0)
    count = len(raw) // 2
    vals = struct.unpack("<%dh" % count, raw[: count * 2])
    peak = max(abs(v) for v in vals) / 32768.0 if count else 0.0
    return (n / float(rate), rate, peak)


def ffmpeg_exe() -> str:
    """优先用户现成的 D:\\ffmpeg；没有则用隔离 venv 里的 imageio-ffmpeg。"""
    for p in (r"D:\ffmpeg\ffmpeg.exe", "/d/ffmpeg/ffmpeg.exe"):
        if os.path.exists(p):
            return p
    try:
        import imageio_ffmpeg  # type: ignore

        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        die("找不到 ffmpeg（D:\\ffmpeg\\ffmpeg.exe 与 imageio-ffmpeg 都没有）")
        return ""


# ============================================================ TTS
def tts_winrt(text: str, voice_name: str, out_wav: str, rate: float = 1.0) -> None:
    import asyncio

    from winrt.windows.media.speechsynthesis import SpeechSynthesizer
    from winrt.windows.storage.streams import DataReader

    async def go() -> None:
        syn = SpeechSynthesizer()
        hit = None
        for v in SpeechSynthesizer.all_voices:
            if v.display_name == voice_name:
                hit = v
                break
        if hit is None:
            die("OneCore 里没有语音 %r" % voice_name)
        syn.voice = hit
        try:
            if rate != 1.0:
                syn.options.speaking_rate = float(rate)
        except Exception:
            pass  # 个别系统上 options 不可写，退回默认语速
        stream = await syn.synthesize_text_to_stream_async(text)
        n = stream.size
        reader = DataReader(stream.get_input_stream_at(0))
        await reader.load_async(n)
        buf = bytearray(n)
        reader.read_bytes(buf)
        with open(out_wav, "wb") as f:
            f.write(bytes(buf))

    asyncio.run(go())


def tts_sapi(text: str, voice_name: str | None, out_wav: str, rate: float = 1.0) -> None:
    import comtypes.client as cc

    voice = cc.CreateObject("SAPI.SpVoice")
    toks = voice.GetVoices()
    idx = 0
    if voice_name:
        idx = -1
        for i in range(toks.Count):
            if toks.Item(i).GetDescription().startswith(voice_name):
                idx = i
                break
        if idx < 0:
            die("SAPI 里没有语音 %r" % voice_name)
    voice.Voice = toks.Item(idx)
    # SAPI 的 Rate 是 -10..10 的整数档；1.0 语速 ≈ 0 档
    voice.Rate = max(-10, min(10, int(round((rate - 1.0) * 10))))

    stream = cc.CreateObject("SAPI.SpFileStream")
    stream.Format.Type = 34          # SAFT44kHz16BitMono
    stream.Open(os.path.abspath(out_wav), 3, False)   # 3 = SSFMCreateForWrite
    voice.AudioOutputStream = stream
    voice.Speak(text)
    stream.Close()


# ============================================================ 后处理
def normalize(src: str, dst: str) -> None:
    """重采样到 44100/16/mono 并把峰值拉到 PEAK。与其它素材同一口径。

    【踩过：先测后滤 → 峰值超标】第一版是"在原始信号上 volumedetect → 算增益 →
    再 highpass + volume"。看着合理，实则不对：volume 之前先过了一道 highpass，
    而高通会把某些段落**抬高**（削掉低频后整体电平重心上移），于是实际峰值比
    测到的高 —— 实测 l2/l5 两条冲到了 0.98 / 1.00（1.00 就是满刻度，已经在削顶）。
    正确做法：**测量链与输出链逐字一致**，测量时只是不加 volume 那一段。
    """
    ff = ffmpeg_exe()
    pre = "highpass=f=70,aresample=%d" % RATE

    # ① 用与输出完全相同的滤波链测峰值（唯一差别是不加 volume）
    p = subprocess.run(
        [ff, "-hide_banner", "-i", src, "-af", pre + ",volumedetect", "-f", "null", "-"],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    m = re.search(r"max_volume:\s*(-?[\d.]+) dB", p.stderr)
    max_db = float(m.group(1)) if m else 0.0
    target_db = 20.0 * __import__("math").log10(PEAK)
    gain = max(-40.0, min(30.0, target_db - max_db))

    # ② 同一条链 + 算出来的增益
    r = subprocess.run(
        [ff, "-hide_banner", "-y", "-i", src, "-af", "%s,volume=%.2fdB" % (pre, gain),
         "-ac", "1", "-c:a", "pcm_s16le", dst],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if r.returncode != 0:
        die("ffmpeg 后处理失败：\n" + (r.stderr or "")[-800:])


# ============================================================ 头文件
def write_header(items: list[tuple[str, str, float]]) -> None:
    """给 C++ 一份 id + 文本 + 时长。CRLF 写入（本工程 src/ 一律 CRLF）。"""
    lines = [
        "// 本文件由 tools/gen_voice.py 生成 —— 不要手改，改稿子去改那个脚本。",
        "//",
        "// 【为什么要有这份表】语音文件（assets/audio/vo_<id>.wav）是烘出来的，",
        "// 但 C++ 侧还需要知道\"有几个 id、文本是什么、大概多长\"——日志要打名字、",
        "// 校验要报时长、HUD 要显示正在念哪一句。让脚本一并产出，是为了保证",
        "// \"念出来的\" 与 \"写出来的\" 永远是同一句话。",
        "#pragma once",
        "#include <vector>",
        "",
        "namespace volunteer_army {",
        "",
        "struct VoLine {",
        "    const char *id;      // 对应 assets/audio/vo_<id>.wav",
        "    const char *text;    // 口播稿原文（给日志 / 字幕用）",
        "    float       dur;     // 烘出来的时长（秒），0 = 素材缺失",
        "};",
        "",
        "inline const std::vector<VoLine> &vo_lines() {",
        "    static const std::vector<VoLine> k = {",
    ]
    for vid, text, dur in items:
        t = text.replace("\\", "\\\\").replace('"', '\\"')
        lines.append('        { "%s", "%s", %.2ff },' % (vid, t, dur))
    lines += [
        "    };",
        "    return k;",
        "}",
        "",
        "} // namespace volunteer_army",
        "",
    ]
    data = "\r\n".join(lines).encode("utf-8")
    if os.path.exists(HDR):
        old = open(HDR, "rb").read()
        if old == data:
            print("  头文件未变：" + os.path.relpath(HDR, ROOT))
            return
    open(HDR, "wb").write(data)
    print("  写头文件：" + os.path.relpath(HDR, ROOT))


# ============================================================ 主流程
def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default=None, help="只处理某个 id")
    ap.add_argument("--check", action="store_true", help="只校验现有素材，不合成")
    ap.add_argument("--voice", default="kangkang", choices=sorted(VOICES))
    ap.add_argument("--rate", type=float, default=1.0, help="语速倍率（1.0 = 正常）")
    args = ap.parse_args()

    ids = level_ids()
    print("关卡表里的 id：%s" % ", ".join(ids))
    for vid, _t, _r in LINES:
        if vid == "menu":
            continue
        if vid not in ids:
            die("口播稿里的 id %r 不在关卡表里（关卡改名/删除后要同步改本脚本的 LINES）" % vid)
    print("口播稿校验通过：%d 条" % len(LINES))

    os.makedirs(OUT_DIR, exist_ok=True)
    todo = [x for x in LINES if args.only is None or x[0] == args.only]
    if args.only and not todo:
        die("--only %r 在 LINES 里找不到" % args.only)

    items: list[tuple[str, str, float]] = []
    tmp = os.path.join(ROOT, "_vo_tmp.wav")
    for vid, text, rate in todo:
        dst = os.path.join(OUT_DIR, "vo_%s.wav" % vid)
        if args.check:
            if not os.path.exists(dst):
                print("  [缺失] %-16s %s" % (vid, os.path.relpath(dst, ROOT)))
                items.append((vid, text, 0.0))
                continue
            dur, sr, peak = wav_info(dst)
            # 菜单那条本来就短（一句话），长度门槛单独放；关卡简报要求 8~30 秒 ——
            # 短于 8 秒是"稿子被截断"，长于 30 秒是"念不完，玩家早按开始了"。
            lo = 3.0 if vid == "menu" else 8.0
            flag = "OK" if (sr == RATE and lo <= dur <= 30.0 and peak > 0.5) else "??"
            print("  [%s] %-16s %5.2fs  %dHz  peak=%.3f" % (flag, vid, dur, sr, peak))
            items.append((vid, text, dur))
            continue

        kind, vname = VOICES[args.voice]
        use_rate = rate * args.rate
        print("  合成 %-16s (%s / %s, rate=%.2f)…" % (vid, kind, vname, use_rate))
        if kind == "winrt":
            tts_winrt(text, vname, tmp, use_rate)
        else:
            tts_sapi(text, vname, tmp, use_rate)
        normalize(tmp, dst)
        dur, sr, peak = wav_info(dst)
        print("      → %s  %.2fs  %dHz  peak=%.3f  %d bytes"
              % (os.path.relpath(dst, ROOT), dur, sr, peak, os.path.getsize(dst)))
        items.append((vid, text, dur))

    if os.path.exists(tmp):
        os.remove(tmp)

    # 头文件要全量（--only 时也读一遍现有的补全），否则 C++ 侧会少条目
    if not args.check:
        full: list[tuple[str, str, float]] = []
        for vid, text, _r in LINES:
            dst = os.path.join(OUT_DIR, "vo_%s.wav" % vid)
            if os.path.exists(dst):
                d, _sr, _pk = wav_info(dst)
            else:
                d = 0.0
            full.append((vid, text, d))
        write_header(full)

    miss = [v for v, _t, d in items if d <= 0.0]
    if miss:
        print("\n注意：以下 id 没有素材：%s" % ", ".join(miss))
    print("完成。")


if __name__ == "__main__":
    main()
