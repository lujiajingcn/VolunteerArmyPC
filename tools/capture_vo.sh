#!/usr/bin/env bash
# VolunteerArmyPC —— 「任务介绍语音」取证
#
# 用法：
#   tools/capture_vo.sh shot      # 简报页：截图 + 进页自动播报
#   tools/capture_vo.sh replay    # 简报页按 B：重播
#   tools/capture_vo.sh entry     # 简报按 Enter 进关：旁白被停掉
#   tools/capture_vo.sh inplay    # 战斗中按 B：部署期重听
#   tools/capture_vo.sh next      # 转进下一阵地：新关介绍自动念
#   tools/capture_vo.sh off       # VA_VO=0 对照：整层不载入
#
# 【为什么这条链路需要专门取证】"有没有声音"是最容易被想当然的一件事：
# 代码里写了 play()、素材文件也在，**照样可能一点声没有** ——
# 可能 wav 没被读进来（本工程的 wav 从不走编辑器导入，res:// 下没有 .import）、
# 可能时长对不上（有对象但数据段是空的）、可能音频设备不可用。
# 所以判据分三层，一层比一层强：
#   ① 字节层：tools/gen_voice.py --check（采样率 / 时长 / 峰值）
#   ② 引擎层：日志里 [vo] 语音载入 6/6，且逐条时长与脚本烘素材时记的对得上
#   ③ 链路层：真按下去 / 真换关，日志里出现 [vo] 播放 <id>（带稿子原文）
# 前两层 headless 就能拿；第三层必须带窗口（headless 是 dummy 音频驱动）。
#
# 【听不见不等于没播】本脚本**不判断声音好不好听**，只判断"播没播、什么时候停、
# 换关时换没换"。音色、语速、稿件内容属于主观项，归人耳，脚本管不了也不该管。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MODE="${1:-shot}"

export VA_SEED="${VA_SEED:-20260925}"
export VA_DBG_VO=1
export VA_CAPTURE_DIR_PREFIX="res://sweep"

# ---- 模式解析：决定初始屏、要投哪个键、等什么信号 ----
NEED_BRIEF=1
KEY=""
PRE_SLEEP=3      # 投键前额外等的秒数（等窗口标题就绪）
WAIT_RE="\\[vo\\] 播放"
case "$MODE" in
  shot)   ;;                                        # 只截图，不投键
  off)    ;;                                        # 只做关闭对照
  replay) KEY=B ;;
  entry)  KEY=ENTER ;;
  # 战斗中按 B：PLAY 屏不会自动播报，没有"[vo] 播放"可等 ——
  # 只能等"世界已建好"再留一段部署时间。等 `单体` 那行（_ready 结束的标志）
  # 是必要的但**不够**：那行在游戏一起步就打，那时窗口还没影，
  # 真正兜住的是下面那个 PRE_SLEEP。
  inplay) KEY=B;     NEED_BRIEF=0
          WAIT_RE="单体"
          PRE_SLEEP=12 ;;
  next)   KEY=1;     NEED_BRIEF=0
          export VA_RETREAT_AT="${VA_RETREAT_AT:-150}"
          export VA_FF="${VA_FF:-6}"
          WAIT_RE="t=[0-9.]+ 强制弹出撤退选择条" ;;
  *) echo "未知模式：$MODE（shot | replay | entry | inplay | next | off）"; exit 1 ;;
esac

if [ "$NEED_BRIEF" = 1 ]; then
  export VA_SCREEN=brief
else
  export VA_SCREEN=play
  export VA_SKIP_MENU=1
fi
export VA_FF="${VA_FF:-1}"
# 截图时刻在算指纹之前定下来，否则默认值进不了指纹（改默认值会覆盖上一批）。
[ "$MODE" = "shot" ] && export VA_CAPTURE="${VA_CAPTURE:-1,4,8,13,18}"

# 目录名吃进所有会改结果的输入：模式 / 种子 / VO 开关 / 快进 / 截图时刻。
# ⚠️ 与 capture_retreat.sh 同一条教训（那层连同机位都踩过一次）：指纹漏一项，
#    两批证据就写进同一目录互相覆盖，而失败只表现为"图上少了一版"，不报错。
#    本机 bash 的 `rm` 还被安全策略拦着（FAIL_CLOSED，静默不执行），
#    "先清空再拍"这条路根本不可用 —— 只能靠命名隔离。
TAG="$(printf '%s|%s|%s|%s|%s' "$MODE" "$VA_SEED" "${VA_VO:-1}" "$VA_FF" "${VA_CAPTURE:-}" \
       | cksum | awk '{print $1}')"
OUT="sweep/v_vo_${MODE}_${TAG}"
mkdir -p "$OUT"
export VA_CAPTURE_DIR="res://$OUT"

WIN_ROOT="$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")"
GODOT="$ROOT/sdk/godot/Godot_v4.5-stable_win64_console.exe"
LOG="$OUT/run.log"
PY="C:/Users/lujiajing/.workbuddy/binaries/python/versions/3.13.12/python.exe"

echo "模式=$MODE   初始屏=$VA_SCREEN   种子=$VA_SEED   VA_VO=${VA_VO:-默认开}   VA_FF=$VA_FF"
echo "目录=$OUT"
echo

# ---------------------------------------------------------------- off：对照
if [ "$MODE" = "off" ]; then
  VA_VO=0 "$GODOT" --headless --path "$WIN_ROOT" --quit > "$LOG" 2>&1
  echo "EXIT=$?"
  echo "--- 语音层（应当只有一行「关闭」）---"
  grep -E '\[vo\]' "$LOG" | head -5
  echo "--- ERROR ---"
  grep 'ERROR' "$LOG" | head -5
  exit 0
fi

# ---------------------------------------------------------------- shot：简报页
if [ "$MODE" = "shot" ]; then
  # VA_SCREEN 显式指定初始屏，优先级高于 VA_CAPTURE 的"跳过菜单"规则。
  # 简报期间逻辑层一步都没走，截图探针由墙钟驱动（见 _process 的 shell 分支）。
  "$GODOT" --path "$WIN_ROOT" > "$LOG" 2>&1
  echo "EXIT=$?"
  echo "截图数: $(ls "$OUT"/cap_*.png 2>/dev/null | wc -l)"
  echo "--- 引擎层：载入 + 时长对照 ---"
  grep -E '\[vo\] 语音载入|\[vo\]   ' "$LOG" | head -10
  echo "--- 链路层：进页自动播报 ---"
  grep -E '\[vo\] 播放|\[ui\] 初始屏' "$LOG" | head -5
  echo "--- ERROR ---"
  grep 'ERROR' "$LOG" | head -5
  exit 0
fi

# ------------------------------------------------- 其余模式：起窗口 → 等信号 → 投键
"$GODOT" --path "$WIN_ROOT" > "$LOG" 2>&1 &
GPID=$!

echo "等待信号（日志里出现：$WAIT_RE）…"
for i in $(seq 1 180); do
  if grep -qE "$WAIT_RE" "$LOG" 2>/dev/null; then
    echo "  已就绪（第 ${i} 次轮询）"
    break
  fi
  sleep 1
done
if ! grep -qE "$WAIT_RE" "$LOG" 2>/dev/null; then
  echo "!! 等不到信号，按键无从谈起。看 $LOG"
  kill "$GPID" 2>/dev/null
  exit 1
fi

# 等窗口标题就绪再投键（起窗口比日志那行晚一点，投早了会"找不到窗口"）。
sleep "$PRE_SLEEP"
echo "投键 $KEY …"
"$PY" "$ROOT/tools/press_key_post.py" "$KEY" "VolunteerArmyPC"

if [ "$MODE" = "next" ]; then
  # ⚠️ 转进是**两步**，只投一次键到不了下一关：
  #   ① 投 1 → 逻辑层结算，弹结算面板（日志：[结算] 转进：…）
  #   ② 面板上按 R → advance_level() 才真的换关、才轮到语音播新阵地。
  # 第一版只投了 ①，日志里明明写着"转进 233.2 高地"，播放次数却是 0 ——
  # 换关压根没发生，看着像"语音没接上"，其实是流程少走了一步。
  echo "等结算面板…"
  for i in $(seq 1 90); do
    grep -q '\[结算\]' "$LOG" 2>/dev/null && { echo "  面板已弹出（第 ${i} 次轮询）"; break; }
    sleep 1
  done
  sleep 2
  echo "投键 R（转进下一阵地）…"
  "$PY" "$ROOT/tools/press_key_post.py" R "VolunteerArmyPC"
  sleep 18
else
  sleep 5
fi

kill "$GPID" 2>/dev/null
sleep 2

echo "--- 播放记录（判据在这一段）---"
grep -E '\[vo\] 播放' "$LOG"
echo "播放次数: $(grep -c '\[vo\] 播放' "$LOG")"
case "$MODE" in
  entry)
    echo "--- 进关 ---"
    grep -E '\[ui\] 进入战斗|\[vo\] 进入战斗' "$LOG" | head -3 ;;
  next)
    echo "--- 转进 ---"
    grep -E 't=[0-9.]+ 强制弹出撤退选择条|伤亡过半，主动撤出|\[战役\] 第' "$LOG" | head -6 ;;
esac
echo "--- ERROR（关停噪音不算：「Unreferenced static string」/「RID allocations … leaked at exit」/
     「PagedAllocator」都是 kill 掉进程时引擎清理阶段自己吐的，不是本层的问题）---"
grep 'ERROR' "$LOG" | grep -vE 'Unreferenced static string|RID allocations|PagedAllocator' | head -5
exit 0
