#!/usr/bin/env bash
# VolunteerArmyPC —— 「语音指挥」输入层取证（见 src/node/mic.h）
#
# 用法：
#   tools/capture_mic.sh probe [winrt|sapi|auto]   # headless：识别器建不建得起来
#   tools/capture_mic.sh wav   [winrt|sapi|auto]   # headless：把指令 wav 当输入，全自动端到端
#                                                  # ⚠️ 只有 sapi 后端能真识别（见下）
#   tools/capture_mic.sh shot                      # 窗口化截图：收起态（不按 Q）
#   tools/capture_mic.sh hold                      # 窗口化：按住 Q 跨过截图时刻 → 展开态
#   tools/capture_mic.sh pair                      # 窗口化：按一下 Q 再松 → 开始听/停止听
#   tools/capture_mic.sh say                       # 窗口化：**扬声器放指令音频让内置麦听见** + 按住 Q
#                                                  #   —— OneCore 唯一能自动证明"真的收到了音频"的路
#                                                  #   （pair 模式下没人说话，出了"开始听"什么都证明不了）
#   tools/capture_mic.sh off                       # VA_MIC=0 对照：整层不建
#
# 【wav 模式为什么要区分后端】VA_MIC_WAV 只有 SAPI5 这条路认 ——
#   OneCore 的 SpeechRecognizer **只能自己开麦克风**，WinRT 没给"从文件/流喂音频"
#   的入口。所以 `wav sapi` 是真验证（应当出现「sapi 听清 + 下发」），
#   而 `wav auto` 的用途是另一件事：auto 会先选 winrt，此时"起不来 → 回退 SAPI5"
#   才会被走到。系统隐私开关关着的时候跑 `wav auto`，日志里应当看到
#   start 失败 → 回退 SAPI5 → 听清 → 下发 这一整串。
#
# 【为什么这条链路必须专门取证】语音输入是**最容易静默失败**的一层：
#   识别器能建起来（日志写着「语音下令就绪」）、按键投进去、HUD 也画了，
#   但真正 StartAsync 时被系统挡掉 —— 玩家按半天 Q 一句话都出不去，
#   画面上看不出任何异常。本机就撞过一次（系统的「在线语音识别」隐私开关
#   是关的），值得把四层判据都钉死：
#     ① 建得起来：setup 后 backend + 语言（probe 模式，headless）
#     ② 真的听得到：喂 wav 进去能不能吐出正确的句子（wav 模式，headless）
#     ③ 会话起停：按住/松开 Q 时 [mic] 开始听 / 停止听 成对出现（pair 模式）
#     ④ 状态条切换：收起态一行小标 / 按住时展开成红点 + 实时转写（shot/hold）
#   ②是唯一能全自动跑完"识别 → 解析 → 下发"的判据：它把识别器的输入从麦克风
#   换成 wav 文件（mic.cpp 的 VA_MIC_WAV），不需要有人对着麦说话。
#
# 【采信尾窗 VA_MIC_TAIL —— "喊了没反应"就是这个（2026-09-27）】
#   识别器是**滞后**的：一句「全体撤退」说完要等一小段静音才定稿，而玩家的自然
#   动作是"说完就松手" ⇒ 定稿几乎必然落在松手**之后**。早先按 listen_ 一刀切
#   （松手即丢命令），症状就是"按住 Q 喊了没有任何反应"—— 不崩、不报错、不留日志。
#   现在：采信窗口 = 按住期间 + 松手后 tail 秒（默认 2.5）。
#   ⚠️ 这条判据**只能靠 wav 模式**拿到（真人说话的定稿没法自动复现，放喇叭的
#      音频 OneCore 定不了稿）：
#        VA_MIC_TAIL=0 bash tools/capture_mic.sh wav sapi   → 听清=1 下发=0
#            （run.log 里出现「丢弃窗口外结果 1 条（最后一条「全体开火」）」）
#        bash tools/capture_mic.sh wav sapi                  → 听清=1 下发=1
#            （「下发：「全体开火」→ fire」）
#      换素材验别的口令：VA_MIC_WAV_REL=sweep/mic_wav/2_retreat.wav
#        → 听清「全体撤退」→ 下发 retreat
#   ⚠️ VA_MIC_TAIL 已进目录名指纹 —— 不进的话两次对照会落进同一个目录互相覆盖。
#
# 【三个坑，写在这里免得下次再踩】
#   ⚠️ VA_CAPTURE 拍完最后一张会**自动退出**（capture_step → get_tree()->quit()）。
#     所以「按住 Q 跨过截图时刻」这条路验的是**展开态**，keyup 大概率发不出去 ——
#     验「松开就停」必须走 pair 模式（不带 VA_CAPTURE）。
#   ⚠️ 外部抓屏（ffmpeg gdigrab title=）**抓不到 Godot 窗口** —— 实测拿到的是
#     一个 1920x1000 的空窗口（DWM/遮挡下只剩个光标）。本脚本一律走游戏自带
#     的 VA_CAPTURE 探针，它对"谁在前台"免疫，与本工程其它取证脚本一致。
#   ⚠️ 所有窗口化模式都必须显式进战斗屏：语音会话只在 SCREEN_PLAY 且外壳未激活
#     时才收话（mic_step 里那两条 early-return），而 apply_key 在界面外壳期间被
#     shell_owns_input() 整条拦掉 —— pair 模式第一版漏了这句，Q 投进去一点
#     动静都没有（开始听=0）。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MODE="${1:-probe}"
BACKEND="${2:-auto}"

export VA_SEED="${VA_SEED:-20260925}"
export VA_DBG_MIC=1
export VA_MIC="${VA_MIC:-1}"
export VA_MIC_BACKEND="$BACKEND"
[ "$MODE" = "off" ] && export VA_MIC=0

WIN_ROOT="$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")"
GODOT_CON="$ROOT/sdk/godot/Godot_v4.5-stable_win64_console.exe"
GODOT="$ROOT/sdk/godot/Godot_v4.5-stable_win64.exe"
PY="C:/Users/lujiajing/.workbuddy/binaries/python/versions/3.13.12/python.exe"

# 目录名吃进所有会改结果的输入（含后端选择）。本机 bash 的 `rm` 被安全策略拦
# （FAIL_CLOSED，静默不执行）——「先清空再拍」不可用，只能靠命名隔离。
TAG="$(printf '%s|%s|%s|%s|%s|%s|%s|%s' "$MODE" "$BACKEND" "$VA_SEED" "$VA_MIC" "${VA_FF:-1}" \
       "${VA_CAPTURE:-}" "${VA_MIC_WAV_REL:-}" "${VA_MIC_TAIL:-2.5}" \
       | cksum | awk '{print $1}')"
OUT="sweep/v_mic_${MODE}_${BACKEND}_${TAG}"
mkdir -p "$OUT"
export VA_CAPTURE_DIR="res://$OUT"

echo "模式=$MODE   后端=$BACKEND   VA_MIC=$VA_MIC   目录=$OUT"
echo

# -------------------------------------------------- probe / off / wav：headless
if [ "$MODE" = "probe" ] || [ "$MODE" = "off" ] || [ "$MODE" = "wav" ]; then
  LOG="$OUT/run.log"
  if [ "$MODE" = "wav" ]; then
    # 把识别器的输入从麦克风换成 wav：整条"识别 → 解析 → 下发"不用人说话就能跑完。
    # 默认喂 sweeps/mic_wav/1_fire.wav（tools/gen_mic_wav.py 烘的「全体开火」）。
    WAV_REL="${VA_MIC_WAV_REL:-sweep/mic_wav/1_fire.wav}"
    export VA_MIC_WAV="$(cygpath -w "$ROOT/$WAV_REL")"
    export VA_SCREEN=play
    export VA_SKIP_MENU=1
    echo "喂 wav=$WAV_REL"
    "$GODOT_CON" --headless --path "$WIN_ROOT" > "$LOG" 2>&1 &
    GPID=$!
    sleep 14
    kill "$GPID" 2>/dev/null
  else
    "$GODOT_CON" --headless --path "$WIN_ROOT" --quit > "$LOG" 2>&1
  fi
  echo "EXIT=$?"
  case "$MODE" in
    probe)
      echo "--- 建得起来吗 ---"
      grep -E '\[mic\]' "$LOG" | head -12
      echo "语音层行数: $(grep -c '\[mic\]' "$LOG")"
      echo "识别器就绪: $(grep -c '语音下令就绪' "$LOG")   失败: $(grep -c '不可用' "$LOG")" ;;
    off)
      echo "--- VA_MIC=0 对照（应当只有一行「关闭」）---"
      grep -E '\[mic\]' "$LOG" | head -5 ;;
    wav)
      echo "--- 引擎层：用的是哪条后端 ---"
      grep -E '\[mic\].*(识别器已建|后端就绪|就绪|不可用|回退|start 失败)' "$LOG" | head -8
      echo "--- 链路层：会话 ---"
      grep -E '\[mic\] (开始听|停止听)' "$LOG" | head -4
      echo "--- 判据：听清 / 下发 ---"
      grep -E '\[mic\] sapi 听清|\[mic\] 下发' "$LOG" | head -6
      echo "听清=$(grep -c 'sapi 听清' "$LOG")   下发=$(grep -c ' \[mic\] 下发\|\[mic\] 下发' "$LOG")"
      # 采信尾窗：**非 0 就是有命令来晚了被丢**（玩家侧看到的是"喊了没反应"）。
      # 正常应为 0；故意 VA_MIC_TAIL=0 做对照时应当是 1（并伴随 下发=0）。
      echo "窗口外丢弃=$(grep -c '丢弃窗口外结果' "$LOG")（正常 0；VA_MIC_TAIL=0 对照时=1）" ;;
  esac
  echo "--- ERROR（关停噪音不算）---"
  grep 'ERROR' "$LOG" | grep -vE 'Unreferenced static string|RID allocations|PagedAllocator' | head -5
  exit 0
fi

# ---------------------------------------------------------------- 窗口化各模式
case "$MODE" in
  shot) export VA_CAPTURE="${VA_CAPTURE:-10}" ;;                 # 不投键 → 收起态
  hold) export VA_CAPTURE="${VA_CAPTURE:-12,20}"; HOLD=40; KEY=Q ;;  # 按住跨过 12s/20s
  pair) unset VA_CAPTURE; HOLD=4; KEY=Q ;;                        # 起停成对，靠日志判
  say)  unset VA_CAPTURE; HOLD="${VA_SAY_SECS:-30}"; KEY=Q; SAY=1 ;;  # 放音频让内置麦听见 + 按住 Q
  *) echo "未知模式：$MODE（probe | wav | shot | hold | pair | say | off）"; exit 1 ;;
esac
# 所有窗口化模式都必须直接进战斗屏：语音会话只在 SCREEN_PLAY 且外壳未激活时收话
# （mic_step 里那两条 early-return），而 apply_key 在界面外壳期间被 shell_owns_input()
# 整条拦掉 —— pair 模式第一版漏了这句，Q 投进去一点动静都没有（开始听=0）。
export VA_SCREEN=play
export VA_SKIP_MENU=1

LOG="$OUT/run.log"
echo "VA_CAPTURE=${VA_CAPTURE:-（无）}   按住=${HOLD:-0}s"
"$GODOT" --path "$WIN_ROOT" > "$LOG" 2>&1 &
GPID=$!

echo "等「语音下令就绪」…"
for i in $(seq 1 60); do
  grep -qE '\[mic\] .*就绪|\[mic\] .*不可用' "$LOG" 2>/dev/null && { echo "  就绪（第 ${i} 次轮询）"; break; }
  sleep 1
done
sleep 2   # 窗口标题比日志那行晚一点，投早了会「找不到窗口」

# ⚠️ 窗口标题是**猜**的：小卢经常同时用 VS 调着一份（标题一模一样），
#    按标题投键会投进**正在调试**的那个窗口、把人家会话搅了。
#    所以这里按 PID 投 —— 取刚启动的这台（pid 最大的那个 Godot 主进程）。
WINPID="$(ps -W 2>/dev/null | grep -F 'Godot_v4.5-stable_win64' | grep -v console \
          | awk '{print $4}' | tail -1)"

# say 模式：从**扬声器**放指令音频，让**内置麦克风**听见。
# 这是唯一能自动证明"OneCore 真的收到了音频"的路子 —— 音量受系统音量影响，
# 所以判据是"有没有 [mic] 听到声音了 / 定稿结果"，而不是"识别得对不对"。
if [ "${SAY:-0}" = "1" ]; then
  FFPLAY="${VA_FFPLAY:-D:/ffmpeg/ffplay.exe}"
  if [ ! -f "$FFPLAY" ]; then
    echo "⚠️ 找不到 ffplay（$FFPLAY）→ say 模式退化成 pair，判据只剩起停。"
    echo "   可用 VA_FFPLAY=<路径> 指定；素材在 sweep/mic_wav/。"
    SAY=0
  fi
fi
if [ "${SAY:-0}" = "1" ]; then
  echo "播指令音频（$FFPLAY，让内置麦听见）…"
  ( for r in $(seq 1 10); do
      for f in 1_fire 2_retreat 3_cover; do
        "$FFPLAY" -nodisp -autoexit -volume 100 -loglevel quiet \
                  "$(cygpath -w "$ROOT/sweep/mic_wav/$f.wav")"
      done
    done ) > "$OUT/play.log" 2>&1 &
  PLAYPID=$!
fi

if [ -n "${KEY:-}" ]; then
  echo "投键 $KEY（按住 ${HOLD}s，投给 pid=$WINPID）…"
  "$PY" "$ROOT/tools/press_key_post.py" "$KEY" "" "$HOLD" --pid "$WINPID" || true
fi

if [ -n "${PLAYPID:-}" ]; then kill "$PLAYPID" 2>/dev/null; fi

if [ "$MODE" = "hold" ]; then
  # 游戏会在最后一张截图后自己退出；等一会儿，等不到就手动收 ——
  # ⚠️ 实测出现过"日志已打『全部完成，退出』但进程还活着"的情况（占住
  #    bin/volunteer_army_pc.dll，下一次构建直接 LNK1104）。所以这里兜一刀。
  echo "等截图完成（游戏会自行退出）…"
  for i in $(seq 1 90); do
    grep -q '全部完成，退出' "$LOG" 2>/dev/null && { echo "  完毕（第 ${i} 次轮询）"; break; }
    sleep 1
  done
  sleep 3
  kill "$GPID" 2>/dev/null
else
  sleep 6
  kill "$GPID" 2>/dev/null
fi
sleep 1

echo
echo "--- 引擎层：后端 + 语言 ---"
grep -E '\[mic\].*(识别器已建|后端就绪|就绪|不可用|回退|start 失败)' "$LOG" | head -6
echo "--- 链路层：会话起停（判据在这一段）---"
grep -E '\[mic\] (开始听|停止听|下发)' "$LOG" | head -10
echo "开始听=$(grep -c '开始听' "$LOG")  停止听=$(grep -c '停止听' "$LOG")  start失败=$(grep -c 'start 失败' "$LOG")  下发=$(grep -c '\[mic\] 下发' "$LOG")"
# 这两行是 2026-09-27 加的 —— 语音层最容易"静默失败"：
# "开始听"打出来了也可能一个字节音频都没进来。所以判据必须有"收到过音频"的正证。
echo "听到声音了=$(grep -c '听到声音了' "$LOG")  定稿结果=$(grep -c '定稿结果' "$LOG")  回调异常=$(grep -c '接住' "$LOG")  StartAsync状态=$(grep -c 'StartAsync' "$LOG")"
# 2026-09-27 加：采信尾窗。**非零就是"有命令被丢"** —— 玩家侧的表现是"喊了没反应"，
# 所以这条计数必须是 0；不是 0 就把 VA_MIC_TAIL 调大（见文件头那段说明）。
echo "窗口外丢弃=$(grep -c '丢弃窗口外结果' "$LOG")（应为 0；>0 = 有命令来晚了被丢、VA_MIC_TAIL 偏小。故意设 VA_MIC_TAIL=0 做对照时才允许非 0）"
if [ -n "${VA_CAPTURE:-}" ]; then
  echo "--- 截图 ---"
  ls -1 "$OUT"/cap_*.png 2>/dev/null | head
fi
echo "--- ERROR（关停噪音不算）---"
grep 'ERROR' "$LOG" | grep -vE 'Unreferenced static string|RID allocations|PagedAllocator' | head -5
exit 0
