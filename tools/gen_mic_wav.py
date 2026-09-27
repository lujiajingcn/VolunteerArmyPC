# -*- coding: utf-8 -*-
"""为「语音指挥」层烘几条**指令** wav，用来做无麦克风的离线端到端验证。

为什么需要它：mic.cpp 有两条识别后端，但"能不能建起来"（后端 = setup OK）和
"能不能真的听清一句话并把指令下发下去"是两回事 —— 前者 headless 就能验，
后者需要有人对着麦克风说话。这台机器上没有人替脚本说话，于是把上一轮
烘任务介绍语音的那条离线 TTS 链路（OneCore SpeechSynthesis，男声 Kangkang）
反过来用：烘几条指令 wav，喂给识别器当输入（mic.cpp 的 VA_MIC_WAV）。

这样能覆盖到的：COM 装配 → 事件线程 → 队列 → parse_command → issue_command 下发。
覆盖不到的：真的麦克风采集（那一步只能由人来做）。

产出落在 sweep/mic_wav/（临时目录，.gitignore 已忽略 sweep/），不入库。

用法: python tools/gen_mic_wav.py
"""
import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "sweep", "mic_wav")

# 复用 gen_voice.py 的 TTS —— 同一个嗓子，免得再写一遍 OneCore 那段。
# gen_voice.py 自己的入口有 `if __name__ == "__main__"` 守卫，import 不会跑去烘素材。
_spec = importlib.util.spec_from_file_location(
    "gen_voice", os.path.join(ROOT, "tools", "gen_voice.py"))
gv = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gv)

# 一条 wav 一句指令。用逻辑层真正认的词（src/sim/va_grammar.cpp 的 ACTIONS 与 CS_ALL）。
# 顺序就是取证顺序，文件名前缀是 1/2/3，方便脚本按序跑。
PHRASES = [
    ("1_fire",    "全体开火"),
    ("2_retreat", "全体撤退"),
    ("3_cover",   "全体隐蔽"),
]

VOICE = "Microsoft Kangkang"


def main() -> None:
    os.makedirs(OUT_DIR, exist_ok=True)
    ok = 0
    for name, text in PHRASES:
        dst = os.path.join(OUT_DIR, "%s.wav" % name)
        try:
            gv.tts_winrt(text, VOICE, dst, 1.0)
        except Exception as e:          # noqa: BLE001 —— 逐条报，一条失败不拖累其余
            print("  [失败] %-10s %s  (%s)" % (name, text, e))
            continue
        size = os.path.getsize(dst) if os.path.exists(dst) else 0
        print("  [ok]   %-10s %-8s %8d B   %s" % (name, text, size, dst))
        ok += 1
    print("完成：%d/%d" % (ok, len(PHRASES)))
    if ok == 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
