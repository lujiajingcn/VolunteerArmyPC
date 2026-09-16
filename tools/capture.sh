#!/usr/bin/env bash
# VolunteerArmyPC —— 截图取证
#
# 用法：
#   tools/capture.sh "3,180,215,260"        # 在战局第 3/180/215/260 秒各拍一张
#   tools/capture.sh "6" out2               # 拍到指定目录
#
# 原理：引擎侧读 VA_CAPTURE（战局秒数，逗号分隔）与 VA_CAPTURE_DIR，
#       到点把视口存成 PNG，全部拍完后 get_tree()->quit() 自动退出。
#       这样无人值守也能拿到「3D 战场真的渲染成什么样」的硬证据。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TIMES="${1:-6}"
DIR="${2:-captures}"
LOG="${3:-run_cap.log}"

cd "$ROOT"
mkdir -p "$DIR"

export VA_CAPTURE="$TIMES"
export VA_CAPTURE_DIR="res://$DIR"

# Godot 的 --path 要的是 Windows 原生路径；MSYS 的 /e/... 它不认，
# 会直接报 "Invalid project path specified" 然后退出（exit 1，无任何截图）。
WIN_ROOT="$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")"

rm -f "$DIR"/cap_*.png
"$ROOT/sdk/godot/Godot_v4.5-stable_win64_console.exe" --path "$WIN_ROOT" > "$LOG" 2>&1
echo "EXIT=$?"
ls -la "$DIR"/cap_*.png 2>/dev/null || echo "（没有产出截图）"
