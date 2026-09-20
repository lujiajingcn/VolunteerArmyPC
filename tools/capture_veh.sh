#!/usr/bin/env bash
# VolunteerArmyPC —— 按「载具外观」拍取证图（近景单体 / 四车陈列排）
#
# 用法：
#   tools/capture_veh.sh jeep              # 吉普近景（dyaw=0，应当看到车头正面）
#   tools/capture_veh.sh jeep 90           # 同上，但把模型再绕 Y 转 90°（侧面对镜头）
#   tools/capture_veh.sh jeep 0 180        # 第三参 = VA_VEH_YAW（写进标定常量的那个）
#   tools/capture_veh.sh row               # 四辆车陈列排：jeep/apc/tank/truck
#
# 为什么单独封一个脚本：拍一次要同时定住四件事，少一件图就没法判读 ——
#   1) VA_SEED=1        晴天。天气由 init_world 首个 RNG 决定，不指定就会随机撞上
#                       雨战/夜战，车身的固有色无从比较（武器那轮踩过）。
#                       这一点对载具比对枪更要紧：车是**大块带色面**，
#                       雨天整体压暗之后"这个绿是不是我要的绿"就判不出来了。
#   2) VA_UNIT_SHOW     指定看哪辆车。载具陈列排藏在同一套检阅台里，
#                       与角色共用 make_vehicle_node_by_key —— 这里看到的**就是**场上那辆。
#   3) VA_HIDE_HUD=1    藏掉 HUD；判车漆过曝/死黑时，准星与提示条都是干扰。
#   4) VA_CAPTURE_DIR   落盘目录，按 tag 分开，避免几次拍摄互相覆盖。
#
# 关于第三参 VA_VEH_YAW：它是**临时覆盖**，写进 scene_builder 的
# VEH_MODEL_YAW_DEG 才是标定。第二参 dyaw 只影响这一个陈列节点，
# 用来在**同一帧光照**下比对"转 0° 和转 90° 哪个才对"。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TAG="${1:-}"
if [ -z "$TAG" ]; then
  sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi

DYAW="${2:-}"
VEH_YAW="${3:-}"

# 模式串：row 走陈列排，其余当车辆键
if [ "$TAG" = "row" ]; then
  MODE="veh:row"
  OUT="sweep/v_veh_row"
else
  MODE="veh:${TAG}"
  [ -n "$DYAW" ] && MODE="${MODE}:${DYAW}"
  SUF=""
  [ -n "$DYAW" ] && SUF="_d${DYAW}"
  OUT="sweep/v_veh_${TAG}${SUF}"
fi

# ---- 定住取证条件 ----
export VA_SEED=1
export VA_HIDE_HUD=1
export VA_CAPTURE="${VA_CAPTURE:-1}"
export VA_CAPTURE_DIR="res://$OUT"
export VA_UNIT_SHOW="$MODE"
[ -n "$VEH_YAW" ] && export VA_VEH_YAW="$VEH_YAW"

mkdir -p "$OUT"
rm -f "$OUT"/cap_*.png

# Godot 的 --path 要 Windows 原生路径，MSYS 的 /e/... 它会直接报
# "Invalid project path specified" 然后退出（exit 1，无任何截图）。
WIN_ROOT="$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")"
LOG="$OUT/run.log"

echo "模式: VA_UNIT_SHOW=$MODE${VEH_YAW:+（VA_VEH_YAW=$VEH_YAW）}"
echo "目录: $OUT"

"$ROOT/sdk/godot/Godot_v4.5-stable_win64_console.exe" --path "$WIN_ROOT" > "$LOG" 2>&1
echo "EXIT=$?"

ls -la "$OUT"/cap_*.png 2>/dev/null || echo "（没有产出截图）"

# 回归判据是"日志里有没有 ERROR"，所以顺手报一下。
# grep -c 在计数为 0 时也会打印 "0" 并返回非零，所以别再写 `|| echo 0`
# ——那样会打印两遍（capture_wpn.sh 第一版就写成这样了）。
ERRS="$(grep -c 'ERROR' "$LOG" 2>/dev/null)"
[ -z "$ERRS" ] && ERRS=0
echo "日志 ERROR 行数: $ERRS   （详见 $LOG）"
# 载具那行日志给的是"原始包围盒 → 校正后包围盒 → 缩放 k → yaw → 目标车长"，
# 朝向标定错了会直接体现在 k 与目标车长上，所以每次都打出来。
grep -E '^\[veh\]' "$LOG" | head -8
grep -E '^\[show\]' "$LOG" | head -5
grep 'ERROR' "$LOG" | head -5
exit 0
