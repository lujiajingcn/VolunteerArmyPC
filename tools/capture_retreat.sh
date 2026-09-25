#!/usr/bin/env bash
# VolunteerArmyPC —— 「伤亡过半 → 1 撤 / 2 守」选择条取证
#
# 用法：
#   tools/capture_retreat.sh shot            # 拍选择条（HUD 实况）
#   tools/capture_retreat.sh key 1           # 弹出后按 1（撤退），打印链路证据
#   tools/capture_retreat.sh key 2           # 弹出后按 2（死守），打印链路证据
#
# 为什么单独封一个脚本：这条 HUD 是一个**等玩家决策的常驻状态**，
# 光靠读代码不能证明它"弹出来了、按得动"。要把它变成可复现的证据，得同时定住四件事：
#   1) VA_SEED        固定种子（天气、敌人部署都由它决定）。
#   2) VA_RETREAT_AT  强制弹出时刻。**没有它这条路径离线以外根本测不到** ——
#                     机械剧本永远打不到"伤亡过半"（最硬的一局也只伤亡 3/10），
#                     真玩到一半人躺下全看运气，两次运行对不上同一时刻。
#   3) VA_FF          快进到弹出时刻，否则一次取证就是几分钟真实时间。
#   4) VA_CAPTURE     指定战局秒数自动截图（整数秒，落盘名 cap_<整秒>s.png）。
#
# ⚠️ VA_RETREAT_AT 只置 `W.retreatOffered` 这一个标志，**不替玩家做选择**：
#    撤还是守、什么时候结算、转进哪一关，全部仍由 src/sim/ 的真实代码走完。
#    所以"按 1 之后日志里出现「伤亡过半，主动撤出 … 转进 …」"这条判据是有效的，
#    它不是本脚本自己打印的装饰文字。
#
# 判读口径（能证伪，不是"好不好看"）：
#   shot —— 指定时刻的截图里，屏幕中上部应出现一条琥珀色描边的面板
#           （"伤亡过半 — 撤还是守？" + 两条 [1] / [2]）。
#           面板是**常驻**的：晚 30 秒那张还在（不像 toast 会淡出）。
#   key 1 —— 日志末段应同时出现「强制弹出撤退选择条」与
#            「伤亡过半，主动撤出 …（只拖住 N 秒）。转进 …」，
#            且随后有「[战役] 第 2 关」。三者缺一都算这条链路没通。
#   key 2 —— 不应出现「主动撤出」；选择条此后不再重弹（retreatChoice != 0）。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MODE="${1:-shot}"
KEY="${2:-1}"

# 弹出时刻取 150 秒：晚于"部署阶段"（tDeploy=120）以保证已经进入交火期，
# 又早于本关时限（missionEnd=275），留够窗口观察"常驻"这件事。
RETREAT_AT="${VA_RETREAT_AT:-150}"

case "$MODE" in
  shot) TIMES="${TIMES:-152,170,200}" ;;
  key)  case "$KEY" in 1|2) ;; *) echo "key 模式只接受 1 或 2"; exit 1 ;; esac
        TIMES="${TIMES:-260}" ;;
  *) echo "未知模式：$MODE（应为 shot | key）"; exit 1 ;;
esac

# 目录名吃进模式 / 弹出时刻 / 快进倍率 / 时刻列表的指纹。
# 为什么不用 `rm -f` 清空：这台机器上 bash 的 rm 被安全策略拦掉（FAIL_CLOSED，静默不执行），
# "清空"会悄无声息失效、两批截图混在一处，而失败只表现为"图上少了一张"，不报任何错。
TAG="$(printf '%s|%s|%s|%s|%s' "$MODE" "$KEY" "$RETREAT_AT" "${VA_FF:-6}" "$TIMES" | cksum | awk '{print $1}')"
OUT="sweep/v_retreat_${MODE}${KEY}_${TAG}"
mkdir -p "$OUT"

export VA_SEED="${VA_SEED:-20260925}"
export VA_SCREEN="${VA_SCREEN:-play}"
export VA_SKIP_MENU=1
export VA_RETREAT_AT="$RETREAT_AT"
export VA_FF="${VA_FF:-6}"
export VA_CAPTURE="$TIMES"
export VA_CAPTURE_DIR="res://$OUT"

WIN_ROOT="$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")"
LOG="$OUT/run.log"

echo "模式=$MODE   键=${KEY:--}   弹出时刻=${RETREAT_AT}s   时刻=$TIMES   VA_FF=$VA_FF   VA_SEED=$VA_SEED"
echo "目录=$OUT"

if [ "$MODE" = "shot" ]; then
  echo "启动…（跑到 ${TIMES##*,} 战局秒，ff=$VA_FF 时约 $(( ${TIMES##*,} / VA_FF )) 秒真实时间）"
  "$ROOT/sdk/godot/Godot_v4.5-stable_win64_console.exe" --path "$WIN_ROOT" > "$LOG" 2>&1
  echo "EXIT=$?"
  N="$(ls "$OUT"/cap_*.png 2>/dev/null | wc -l)"
  echo "截图数: $N"
  echo "日志 ERROR 行数: $(grep -c 'ERROR' "$LOG" 2>/dev/null)"
  echo "--- 弹出证据 ---"
  grep -E '强制弹出撤退选择条|取证注入' "$LOG" | head -5
  echo "--- ERROR ---"
  grep 'ERROR' "$LOG" | head -5
  exit 0
fi

# ---- key 模式：起游戏 → 等弹出 → 投一次按键 → 等收尾 ----
"$ROOT/sdk/godot/Godot_v4.5-stable_win64_console.exe" --path "$WIN_ROOT" > "$LOG" 2>&1 &
GPID=$!

echo "等待选择条弹出（日志里出现触发时刻那行）…"
# ⚠️ 判据必须是 retreat_inject() 在**触发时刻**打的那行（带 t=…），
#    不能是 _ready 里的「取证注入：强制弹出撤退选择条 t=150.0 秒」——
#    那行在游戏一起步就打了，拿它当判据会把按键投在弹出之前（实测踩到：
#    第 3 次轮询就"已弹出"，实际战局才跑到 t≈20，按键落空）。
for i in $(seq 1 180); do
  if grep -qE 't=[0-9.]+ 强制弹出撤退选择条' "$LOG" 2>/dev/null; then
    echo "  已弹出（第 ${i} 次轮询）"
    break
  fi
  sleep 1
done

if ! grep -qE 't=[0-9.]+ 强制弹出撤退选择条' "$LOG" 2>/dev/null; then
  echo "!! 选择条始终没弹出，按键无从谈起。看 $LOG"
  kill "$GPID" 2>/dev/null
  exit 1
fi

# 等窗口标题就绪再投键：起窗口比日志那行晚一点，投早了会"找不到窗口"。
sleep 3
echo "投键 $KEY …"
"C:/Users/lujiajing/.workbuddy/binaries/python/versions/3.13.12/python.exe" \
  "$ROOT/tools/press_key_post.py" "$KEY" "VolunteerArmyPC"
# 转进要经过 capture_carry + 结算 + 换关，给足时间（ff=6 下 25 真实秒 ≈ 150 战局秒）。
sleep 25
kill "$GPID" 2>/dev/null
sleep 2

echo "--- 弹出（触发时刻）---"
grep -E 't=[0-9.]+ 强制弹出撤退选择条' "$LOG" | head -3
echo "--- 结果（判据在这一段）---"
grep -E '伤亡过半，主动撤出|结束原因|\[战役\]|转进|你在最后一个阵地' "$LOG" | head -10
echo "--- ERROR ---"
grep 'ERROR' "$LOG" | head -5
exit 0
