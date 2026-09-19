#!/usr/bin/env bash
# VolunteerArmyPC —— 「按住方向键」输入回归（真实按键注入 + 事件表判读）
#
# 用法：
#   tools/input_hold_test.sh hold      # 按住 W 三秒（其间按 Windows 节奏重发 keydown）
#   tools/input_hold_test.sh focus     # 按住 → 前台切走 → 在别的窗口松开 → 切回游戏
#
# 为什么需要它：输入手感问题**不能靠读代码判**。
#   · "按住不放"在事件层不是一个事件，而是一条 keydown + N 条**系统按键重复**（echo=1）；
#   · 人手没法精确复现"按住 2 秒、其间每 50ms 一次重复"；
#   · 画面上"走得慢"和"走两步就停"读不出量级。
# 所以：玩具 tools/inject_key.py 负责"打"，引擎侧 VA_DBG_INPUT 负责"记"，
#       本脚本负责"判"并给出 PASS/FAIL。
#
# 判据（脚本末尾自动算，全部要看）：
#   hold  —— ① 每条 echo=1 之后的 IN(w,s,a,d) 与按下那行**相同**；
#            ② 按住期间的心跳**没有一条**位移为 0；
#            ③ 松开之后的心跳**每一条**都是 0。
#   focus —— ① 事件表里只有一条 keydown（keyup 发生在别的窗口，游戏根本收不到）；
#            ② 失焦那一刻打出"清空按键闩锁"，此后心跳位移全 0；
#            ③ 日志 0 ERROR。
#
# 环境变量：PY（python 解释器）、VA_OTHER_WINDOW（抢焦点用的窗口标题）
set -u

CASE="${1:-hold}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="${PY:-python}"
WIN_ROOT="$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")"
GAME="$ROOT/sdk/godot/Godot_v4.5-stable_win64.exe"
DIR="res://sweep/in_$CASE"
LOG="$ROOT/sweep/in_$CASE.log"
TITLE="VolunteerArmyPC"                       # 最短的含此子串的标题 = 游戏窗口（VS 的标题更长）
OTHER="${VA_OTHER_WINDOW:-Microsoft Visual Studio}"

cd "$ROOT"
export VA_CAPTURE="26" VA_CAPTURE_DIR="$DIR" VA_DBG_INPUT=1
rm -rf "sweep/in_$CASE"; rm -f "$LOG"

"$GAME" --path "$WIN_ROOT" --log-file "$(cygpath -w "$LOG")" > "$ROOT/sweep/in_$CASE.out" 2>&1 &
GPID=$!
sleep 8      # 等加载：重场景 + 11 个三维模型 + 天气

case "$CASE" in
hold)
    "$PY" tools/inject_key.py --key W --hold 3.0 --focus "$TITLE"
    ;;
focus)
    "$PY" tools/inject_key.py --focus "$TITLE" --down-only      # ① 按住（不松）
    sleep 1.5
    "$PY" tools/inject_key.py --refocus "$OTHER" || echo "（抢焦点失败：$OTHER 不在前台候选里？）"
    sleep 1.6
    "$PY" tools/inject_key.py --up-only                          # ② keyup 发给别的窗口
    sleep 1.6
    ;;
*)
    echo "未知用例：$CASE（可选 hold / focus）"
    kill $GPID 2>/dev/null
    exit 2
    ;;
esac

wait $GPID
echo "GAME_EXIT=$?"

echo
echo "===== 事件表 ====="
grep -a "dbg-input] key" "$LOG" | head -6
echo "…（共 $(grep -ac 'dbg-input] key' "$LOG") 条）"
echo "----- 清空闩锁 -----"
grep -a "清空按键闩锁" "$LOG" || echo "（无）"

echo
echo "===== 判读 ====="
FAIL=0
chk() {  # chk <描述> <必须为0的数量> <实际值>
    if [ "$3" -eq 0 ]; then echo "  ✅ $1（$2 = 0）"; else echo "  ❌ $1（$2 = $3，应为 0）"; FAIL=1; fi
}
HB() { grep -a "心跳" "$LOG"; }

# 按下那行写进去的 IN 状态，作为"按住期间应当保持的值"
PRESS_IN="$(grep -a 'dbg-input] key' "$LOG" | grep "pressed=1 echo=0" | head -1 | sed 's/.*IN(w,s,a,d)=\([01]*\).*/\1/')"
ANY_IN="$(HB | head -1 | sed 's/.*IN(w,s,a,d)=\([01]*\).*/\1/')"   # 第一帧还没按，取 0000

echo "  按下写进 IN = ${PRESS_IN:-（没抓到按下事件）}"
if [ "$CASE" = "hold" ]; then
    chk "echo（系统按键重复）没有改动 IN —— 按住不会被自己的重复按停" "被改动的 echo 条数" \
        "$(grep -a 'dbg-input] key' "$LOG" | grep "echo=1" | grep -vc "IN(w,s,a,d)=$PRESS_IN")"
    chk "按住期间没有'位移为 0'的心跳 —— 是连续移动而不是走两步就停" "此类心跳数" \
        "$(HB | grep -c "IN(w,s,a,d)=$PRESS_IN.*位移=0\.0 m")"
    chk "松开之后没有位移 —— 松开即停" "此类心跳数" \
        "$(HB | grep "IN(w,s,a,d)=$ANY_IN" | grep -vc '位移=0\.0 m')"
    echo "  按住期间位移合计：$(HB | grep "IN(w,s,a,d)=$PRESS_IN" | sed 's/.*位移=//;s/ m.*//' | awk '{s+=$1} END {printf "%.1f m\n", s}')"
else
    # 判据必须**只看"清空闩锁"那一行之后**的心跳：那之前角色本来就该在走。
    # （第一版没做这个切分，把失焦前 9 条正常位移当成了失败 —— 判据本身也会骗人。）
    chk "游戏一条 keyup 都没收到 —— 用例前提成立（keyup 丢在别的窗口）" "keyup 条数" \
        "$(grep -ac 'dbg-input] key.*pressed=0' "$LOG")"
    CLEAR_LN="$(grep -an '清空按键闩锁' "$LOG" | head -1 | cut -d: -f1)"
    if [ -z "$CLEAR_LN" ]; then
        echo "  ❌ 失焦没有触发'清空按键闩锁' —— 通知与每帧焦点核对都没生效"
        FAIL=1
    else
        AFTER="$(awk -v n="$CLEAR_LN" 'NR>n' "$LOG" | grep -a '心跳')"
        N_AFTER="$(printf '%s\n' "$AFTER" | grep -c '心跳')"
        echo "  第 $CLEAR_LN 行清空闩锁；其后心跳 $N_AFTER 条"
        if [ "$N_AFTER" -lt 5 ]; then
            echo "  ❌ 清空之后样本太少（$N_AFTER 条），判不出来"
            FAIL=1
        fi
        # 第一条要放过：它的"位移"是从上一条心跳算起的区间长度，**跨了失焦那一刻**，
        # 里面本来就有失焦前的正常运动。判据是"从第二条起必须全 0"，
        # 也就是"失焦后一次心跳（≈0.27 秒）之内必须停住"。
        REST="$(printf '%s\n' "$AFTER" | tail -n +2)"
        chk "失焦后一次心跳之内就停住（跨过渡的第一条不计）" "其后非零位移心跳数" \
            "$(printf '%s\n' "$REST" | grep -vc '位移=0\.0 m')"
    fi
fi
chk "日志里没有 ERROR" "ERROR 条数" "$(grep -ac ERROR "$LOG")"

echo
[ "$FAIL" -eq 0 ] && echo "结果：PASS（日志 $LOG）" || echo "结果：FAIL（日志 $LOG）"
exit $FAIL
