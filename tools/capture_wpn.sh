#!/usr/bin/env bash
# VolunteerArmyPC —— 按「武器外观」拍第一人称取证图
#
# 用法：
#   tools/capture_wpn.sh wpn_ppsh                # 波波沙，腰射，拍 1s 那一帧
#   tools/capture_wpn.sh wpn_dp27 --ads          # DP-27，开镜
#   tools/capture_wpn.sh wpn_dp27 --ads --hud    # 开镜并保留 HUD（验准星规则）
#   tools/capture_wpn.sh - --ads                 # 程序化枪模（消融对照）
#   tools/capture_wpn.sh wpn_mosin "" my/out     # 自定义输出目录
#   VA_VM_ART_ALB=0.8 tools/capture_wpn.sh wpn_mosin    # 顺带扫材质旋钮
#
# 为什么单独封一个脚本：拍一次要同时定住四件事，少一件图就没法判读 ——
#   1) VA_SEED=1        晴天。天气由 init_world 首个 RNG 决定，不指定就会
#                       随机撞上雨战/夜战，枪身亮度无从比较（踩过一次）。
#   2) VA_WM_SKIN=<键>  指定用哪把真模型。按键注入要定位窗口 + 碰鼠标捕获，
#                       而"拍第 2 把枪"这种事不该依赖按键。
#   3) VA_HIDE_HUD=1    藏掉 HUD；判枪身过曝/死黑时，准星与提示条都是干扰。
#                       **但验准星本身时要用 --hud 把它放回来**。
#                       （VA_CAPTURE 本身也会跳过主菜单，见 world_sim.cpp。）
#   4) VA_CAPTURE_DIR   落盘目录。--ads 时自动加后缀，避免覆盖腰射那张。
#
# 环境变量是**继承**的：调用前自己 export 的 VA_VM_ART_ALB / VA_VM_ART_METAL /
# VA_VM_ART_ROUGH / VA_VM_HANDS / VA_VM_MAT 等都会原样传进去，用来扫旋钮。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SKIN="${1:-}"
shift 2>/dev/null || true
ADS=""
KEEP_HUD=""
OUT=""
for a in "$@"; do
  case "$a" in
    --ads) ADS="1" ;;
    --hud) KEEP_HUD="1" ;;
    "")    ;;
    *)     OUT="$a" ;;
  esac
done

if [ -z "$SKIN" ]; then
  sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi

SUF=""
[ -n "$ADS" ] && SUF="${SUF}_ads"
[ -n "$KEEP_HUD" ] && SUF="${SUF}_hud"
if [ -z "$OUT" ]; then
  if [ "$SKIN" = "-" ] || [ "$SKIN" = "proc" ]; then
    OUT="sweep/v_proc${SUF}"
  else
    OUT="sweep/v_${SKIN}${SUF}"
  fi
fi

# ---- 定住取证条件 ----
export VA_SEED=1
[ -z "$KEEP_HUD" ] && export VA_HIDE_HUD=1
export VA_CAPTURE="1"
export VA_CAPTURE_DIR="res://$OUT"
[ -n "$ADS" ] && export VA_ADS=1

if [ "$SKIN" = "-" ] || [ "$SKIN" = "proc" ]; then
  # 消融：强制程序化枪模，用来对比"真模型到底改了什么"。
  export VA_VM_NO_ART=1
  LABEL="程序化枪模"
else
  export VA_WM_SKIN="$SKIN"
  LABEL="$SKIN"
fi

mkdir -p "$OUT"
rm -f "$OUT"/cap_*.png

# Godot 的 --path 要 Windows 原生路径，MSYS 的 /e/... 它会直接报
# "Invalid project path specified" 然后退出（exit 1，无任何截图）。
WIN_ROOT="$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")"
LOG="$OUT/run.log"

echo "外观: $LABEL${ADS:+（开镜）}"
echo "目录: $OUT"
echo "旋钮: ALB=${VA_VM_ART_ALB:-默认} METAL=${VA_VM_ART_METAL:-默认} ROUGH=${VA_VM_ART_ROUGH:-默认} HANDS=${VA_VM_HANDS:-默认藏}"

"$ROOT/sdk/godot/Godot_v4.5-stable_win64_console.exe" --path "$WIN_ROOT" > "$LOG" 2>&1
echo "EXIT=$?"

ls -la "$OUT"/cap_*.png 2>/dev/null || echo "（没有产出截图）"

# 回归判据是"日志里有没有 ERROR"，所以顺手报一下。
# grep -c 在计数为 0 时也会打印 "0" 并返回非零，所以别再写 `|| echo 0`
# ——那样会打印两遍（第一版就写成这样了）。
ERRS="$(grep -c 'ERROR' "$LOG" 2>/dev/null)"
[ -z "$ERRS" ] && ERRS=0
echo "日志 ERROR 行数: $ERRS   （详见 $LOG）"
grep -E '^\[vm\]' "$LOG" | head -8
grep 'ERROR' "$LOG" | head -5
exit 0
