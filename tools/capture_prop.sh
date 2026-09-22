#!/usr/bin/env bash
# VolunteerArmyPC —— 按「战场地物外观」拍取证图（近景单体 / 四件陈列排）
#
# 用法：
#   tools/capture_prop.sh prop_rock_a        # 岩石 A 近景（附 1.68 m 参照兵）
#   tools/capture_prop.sh prop_rock_a 90     # 同上，但把模型再绕 Y 转 90°
#   tools/capture_prop.sh prop_pine          # 松树近景（取景按 8 m 高重算过）
#   tools/capture_prop.sh row                # 四件地物陈列排：rock_a/rock_b/pine/bush
#
# 为什么单独封一个脚本：与 capture_veh.sh 同一个理由 —— 拍一次要同时定住三件事，
# 少一件图就没法判读：
#   1) VA_SEED=1        晴天。天气由 init_world 首个 RNG 决定，不指定就会随机撞上
#                       雨战/夜战，地物的固有色（岩面灰、叶簇绿）无从比较。
#   2) VA_UNIT_SHOW     指定看哪一件。草与土的颜色会互相影响判读，所以背景也要
#                       稳定 —— 同一颗种子、同一个点、同一个机位。
#   3) VA_HIDE_HUD=1    藏掉 HUD；判"这块石头压扁了没有、底踩在地上没有"时，
#                       准星与提示条都是干扰。
#
# 【判读口径 —— 这一档要看的三件事，与角色/载具不同】
#   ① 比例：跟同框那个 1.68 m 的参照兵比。地物的尺寸**不是**照实物给的，
#      而是照"原来那套程序化图元的包围盒"给的（见 scene_builder.cpp 的 kPropArt），
#      所以判据是"跟改之前比没变形"，不是"跟真石头一样大"。
#   ② 底：脚下那条地面余量。相机高取 0.5×H 就是为了留住它 —— 压没了就读不出
#      "踩在地上还是悬空"。
#   ③ 压扁：岩石/灌木的高宽比被强制到 0.50 / 0.36。压过头会变成一块饼，
#      压不够则"能藏住"的读感会变 —— 这一条只能在近景里看。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TAG="${1:-}"
if [ -z "$TAG" ]; then
  sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi

DYAW="${2:-}"

if [ "$TAG" = "row" ]; then
  MODE="prop:row"
  OUT="sweep/v_prop_row"
else
  MODE="prop:${TAG}"
  [ -n "$DYAW" ] && MODE="${MODE}:${DYAW}"
  SUF=""
  [ -n "$DYAW" ] && SUF="_d${DYAW}"
  OUT="sweep/v_prop_${TAG}${SUF}"
fi

# ---- 定住取证条件 ----
export VA_SEED=1
export VA_HIDE_HUD=1
export VA_CAPTURE="${VA_CAPTURE:-1}"
export VA_CAPTURE_DIR="res://$OUT"
export VA_UNIT_SHOW="$MODE"

mkdir -p "$OUT"
rm -f "$OUT"/cap_*.png

# Godot 的 --path 要 Windows 原生路径，MSYS 的 /e/... 它会直接报
# "Invalid project path specified" 然后退出（exit 1，无任何截图）。
WIN_ROOT="$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")"
LOG="$OUT/run.log"

echo "模式: VA_UNIT_SHOW=$MODE"
echo "目录: $OUT"

"$ROOT/sdk/godot/Godot_v4.5-stable_win64_console.exe" --path "$WIN_ROOT" > "$LOG" 2>&1
echo "EXIT=$?"

ls -la "$OUT"/cap_*.png 2>/dev/null || echo "（没有产出截图）"

# 回归判据是"日志里有没有 ERROR"，所以顺手报一下。
ERRS="$(grep -c 'ERROR' "$LOG" 2>/dev/null)"
[ -z "$ERRS" ] && ERRS=0
echo "日志 ERROR 行数: $ERRS   （详见 $LOG）"
# 地物那行日志给的是"原始包围盒 → 水平尺度/高 → 归一化口径 → 高宽比 → 对轴"，
# 归一化取错维（拿短边当宽度）会直接体现在这几个数上，所以每次都打出来。
grep -E '^\[prop\]' "$LOG" | head -8
grep -E '^\[show\]' "$LOG" | head -5
grep 'ERROR' "$LOG" | head -5
exit 0
