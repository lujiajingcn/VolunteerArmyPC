#!/usr/bin/env bash
# VolunteerArmyPC —— 角色「双腿交替」取证
#
# 用法：
#   tools/capture_leg.sh run   <键> [速度]   # 跑动排：**一张静态图看完整个步态循环**
#   tools/capture_leg.sh probe <键>          # 染色探针：验髋高选在哪、左右分得对不对
#   tools/capture_leg.sh one   <键>          # 近景单体，摆角恒为 0（材质对照的基准）
#
# 例：
#   tools/capture_leg.sh probe char_rifleman
#   tools/capture_leg.sh run   char_rifleman
#   VA_LEG=0 tools/capture_leg.sh run char_rifleman     # 对照组：腿摆关闭
#
# 为什么单独封一个脚本：拍一次要同时定住几件事，少一件图就没法判读 ——
#   1) VA_SEED=1       晴天。天气由 init_world 的首个 RNG 决定，不指定会随机撞上
#                      雨战/夜战 —— 而"腿摆幅度"要看的是轮廓差，压暗之后判不出来。
#   2) VA_UNIT_SHOW    决定看哪一排（跑动排 / 近景单体），与战场共用同一套
#                      make_unit_node_by_key + unit_transform，看到的就是场上那个人。
#   3) VA_HIDE_HUD=1   藏掉 HUD：罗盘与提示条是**墙钟动画**，两次运行不可能对齐，
#                      做 VA_LEG=0/1 像素差时会整片污染差值（这一条是本工程踩过的）。
#   4) VA_VM_HIDE=1    藏枪模：视图模型的摇摆同样是墙钟驱动（同上）。
#   5) VA_CAPTURE_DIR  按模式+键分目录，避免几次拍摄互相覆盖。
#
# 判读口径（不是"好不好看"，是能证伪的判据）：
#   probe —— 彩色区必须**只覆盖两条腿**；髋以上应是中性灰。
#            头顶/胳膊/枪上色 ⇒ VA_LEG_HIP 或左右符号错了。
#   run   —— 6 格里腿的前后关系必须随相位**单调**推移，
#            且第 0 格与第 3 格（相差 180°）左右腿应当互换。整排同手同脚 ⇒ 相位接错了。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MODE="${1:-}"
KEY="${2:-}"
SPEED="${3:-}"

if [ -z "$MODE" ] || [ -z "$KEY" ]; then
  sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi

case "$MODE" in
  run)
    SHOW="run:${KEY}"
    [ -n "$SPEED" ] && SHOW="${SHOW}:${SPEED}" && SUF="_s${SPEED}"
    SUF="${SUF:-}"
    ;;
  probe)
    SHOW="one:${KEY}"
    export VA_LEG_PROBE=1
    SUF="_probe"
    ;;
  one)
    SHOW="one:${KEY}"
    SUF=""
    ;;
  *)
    echo "未知模式：$MODE（应为 run | probe | one）"
    exit 1
    ;;
esac

# 对照组（VA_LEG=0）单独落一个目录：不分开的话两次拍摄会写进同一处、
# 后者覆盖前者，而这个失败只表现为"图上少了一张"，不会报任何错。
LEGTAG=""
[ "${VA_LEG:-1}" = "0" ] && LEGTAG="_off"
OUT="sweep/v_leg_${MODE}_${KEY}${SUF}${LEGTAG}"

# ---- 定住取证条件 ----
export VA_SEED=1
export VA_HIDE_HUD=1
export VA_VM_HIDE=1
export VA_CAPTURE="${VA_CAPTURE:-1}"
export VA_CAPTURE_DIR="res://$OUT"
export VA_UNIT_SHOW="$SHOW"

mkdir -p "$OUT"
rm -f "$OUT"/cap_*.png

# Godot 的 --path 要 Windows 原生路径；MSYS 的 /e/... 它不认，
# 会直接报 "Invalid project path specified" 然后退出（exit 1，无任何截图）。
WIN_ROOT="$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")"
LOG="$OUT/run.log"

echo "模式: VA_UNIT_SHOW=$SHOW   VA_LEG=${VA_LEG:-1}${VA_LEG_PROBE:+  VA_LEG_PROBE=$VA_LEG_PROBE}"
echo "目录: $OUT"

"$ROOT/sdk/godot/Godot_v4.5-stable_win64_console.exe" --path "$WIN_ROOT" > "$LOG" 2>&1
echo "EXIT=$?"

ls -la "$OUT"/cap_*.png 2>/dev/null || echo "（没有产出截图）"

# 回归判据是"日志里有没有 ERROR"，顺手报一下。
ERRS="$(grep -c 'ERROR' "$LOG" 2>/dev/null)"
[ -z "$ERRS" ] && ERRS=0
echo "日志 ERROR 行数: $ERRS   （详见 $LOG）"
# 腿摆层自己那几行：髋高是从网格 AABB 推的，错了会直接体现在这几个数上。
grep -E '^\[leg\]' "$LOG" | head -16
grep -E '^\[show\]' "$LOG" | head -8
grep -E '^\[unit\]' "$LOG" | head -4
grep 'ERROR' "$LOG" | head -5
exit 0
