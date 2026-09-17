#!/usr/bin/env bash
# VolunteerArmyPC —— 界面外壳 + 战斗 HUD 的五屏取证
#
# 用法：
#   tools/capture_ui.sh [前缀]        # 默认 ui3，产出 sweep/<前缀>_menu 等 5 个目录
#
# 为什么不能直接复用 tools/capture.sh：
#   1) capture.sh 只设 VA_CAPTURE / VA_CAPTURE_DIR。而 VA_CAPTURE 一出现
#      就等价于 VA_SKIP_MENU（见 world_sim.cpp 的 _ready：取证要的是战局画面，
#      而菜单期间逻辑层 t 是冻结的），于是"拍菜单"这个需求本身就被它排除了。
#      必须显式给 VA_SCREEN 才能指定初始屏。
#   2) 外壳期间逻辑层的 t 冻结，按 t 触发的截图探针永远不响。
#      world_sim.cpp 因此在 shell 期间改用**墙钟** cap_sim_t_ = shell_t_，
#      这里沿用它 —— 所以同一个 VA_CAPTURE 值在菜单/简报里代表"墙钟秒"。
#
# 五屏分别对应：
#   menu  主菜单        VA_SCREEN=menu
#   brief 任务简报      VA_SCREEN=brief
#   win   结算·成功     VA_SCREEN=play VA_END=win
#   lose  结算·失败     VA_SCREEN=play VA_END=lose
#   play  战斗 HUD      VA_SCREEN=play
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PFX="${1:-ui3}"
LOG="run_ui_${PFX}.log"

cd "$ROOT"
# Godot 的 --path 要 Windows 原生路径；MSYS 的 /e/... 它不认。
WIN_ROOT="$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")"
EXE="$ROOT/sdk/godot/Godot_v4.5-stable_win64_console.exe"

shot() {   # shot <名字> <秒数> <环境变量...>
    local name="$1"; shift
    local secs="$1"; shift
    local dir="sweep/${PFX}_${name}"
    mkdir -p "$dir"
    echo "===== ${name} → ${dir} (t=${secs}s) ====="
    env VA_CAPTURE="$secs" VA_CAPTURE_DIR="res://$dir" "$@" \
        "$EXE" --path "$WIN_ROOT" > "${LOG}.${name}" 2>&1
    echo "EXIT=$?"
    ls -l "$dir"/cap_*.png 2>/dev/null || echo "（没有产出截图）"
}

shot menu  2  VA_SCREEN=menu
shot brief 2  VA_SCREEN=brief
shot win   3  VA_SCREEN=play VA_END=win
shot lose  3  VA_SCREEN=play VA_END=lose
shot play  45 VA_SCREEN=play

echo
echo "=== 汇总：素材载入 / 初始屏 / 报错 ==="
grep -hE "^\[ui\]|^\[VA_CAPTURE\]|ERROR" "${LOG}".* 2>/dev/null | sort | uniq -c | sort -rn | head -30
