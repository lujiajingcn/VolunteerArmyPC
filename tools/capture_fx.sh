#!/usr/bin/env bash
# VolunteerArmyPC —— 射击光效取证（枪口焰 / 曳光弹 / 弹着火花）
#
# 用法：
#   tools/capture_fx.sh look  [时刻列表]   # 交火实况，保留 HUD 与枪模（看观感）
#   tools/capture_fx.sh diff  [时刻列表]   # 消融对照：藏 HUD / 枪模，供 VA_FX=0/1 差分
#   tools/capture_fx.sh probe [时刻列表]   # 阵营染色：友军绿 / 敌军红 / 火花蓝
#
# 例：
#   tools/capture_fx.sh look
#   VA_FX=0 tools/capture_fx.sh diff        # 对照组（同名文件一一对应，可直接差分）
#   tools/capture_fx.sh probe
#   VA_FF=6 tools/capture_fx.sh look        # 想快点跑完就把快进倍率抬上去
#
# 为什么单独封一个脚本：拍一次要同时定住五件事，少一件图就没法判读 ——
#   1) VA_SEED      固定种子。交火时刻、敌人站位、天气都由它决定；不钉死的话
#                   两次运行对不到同一个战局，VA_FX=0/1 的差分也就无从谈起。
#   2) VA_SCRIPT_A  伏击是按**位置**起爆的（先头车 x<1180 触雷）。只开 VA_AUTO 的话
#                   玩家一开始没有射界、伏击永不触发、车队一路开出西口 —— 整局一枪不放。
#   3) VA_AUTO      顶替玩家的"手"：每帧朝最近活敌对准 + 扣扳机。
#   4) VA_FF        快进到交火。CFG.convoyIn=175 秒车队才进场，不快进一次取证就是
#                   五分钟真实时间起步。
#                   ⚠️ 默认取 3 而不是 6：一帧推进 0.05 逻辑秒 ≈ 子弹走 2.4 m，
#                   与曳光弹线段长 2.3 m 同量级，弹道看着是连续的；
#                   ff=6 时一帧跳 4.75 m，曳光会变成断续的短划痕（静态图上倒也无妨）。
#                   枪口焰的判据已按"帧区间相交"实现（见 fx_layer.h），快进不会漏。
#   5) VA_CAPTURE   用**密集整数秒**：枪口焰只活 0.055 s，稀疏采样基本拍不到。
#                   截图落盘名是 cap_<整秒>s.png（world_sim.cpp:946），所以时刻必须是整数。
#
# 判读口径（能证伪的判据，不是"好不好看"）：
#   look  —— 交火帧里应能看到暖橙色的枪口光斑与弹道亮线，且光斑出现在
#            **开枪那一侧**（敌方单位的枪口方向），不是在友军身上。
#   probe —— 友军绿 / 敌军红。整片同色 ⇒ team 取错了；
#            一个都没有 ⇒ 交火没打起来（先看日志里 [fx] 的计数，再调时刻列表）。
#   diff  —— VA_FX=0 的同名张应当看不到任何枪口光与曳光弹，
#            差分非零像素集中在弹道与枪口位置（拿 tools/diff_png.py 做）。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MODE="${1:-look}"
# 默认采样窗口是**实测出来的**，不是拍的：
#   车队 175 秒进场 → 交火 → 战局在 222 秒左右结算
#   （VA_SEED=20260925 实测：224 秒那张已经是结算面板，日志里 "行动时长 04:16"）。
# 所以窗口取 176~222、**步长 2 秒** —— 枪口焰只活 0.055 s，
# 步长 4 秒时 22 张里能撞上的只有零星几张。
TIMES="${2:-}"
if [ -z "$TIMES" ]; then
  TIMES=""
  for t in $(seq 176 2 222); do TIMES="${TIMES}${t},"; done
  TIMES="${TIMES%,}"
fi

case "$MODE" in
  look|diff|probe) ;;
  *) echo "未知模式：$MODE（应为 look | diff | probe）"; exit 1 ;;
esac

# 对照组单独落一个目录：不分开的话两次拍摄会写进同一处、后者覆盖前者，
# 而这个失败只表现为"图上少了一张"，不报任何错（腿摆那一层踩过同样的坑）。
# 目录名再缀上**本次时刻列表的指纹**：采样窗口一变（如"步长 4"改成"步长 2"），
# 旧批次的文件就不会混进新批次里对不上号。
#
# ⚠️ 为什么不用 `rm -f` 清空目录：这台机器上 bash 的 `rm` 被安全策略拦掉
#    （FAIL_CLOSED，静默不执行），于是"清空"这一步悄无声息地失效、两批截图
#    混在一处 —— 而这个失败**不报任何错**，只表现为"对照组张数与实验组对不上"
#    （2026-09-25 实际踩到：对照组 34 张 vs 实验组 24 张，多出的 10 张是上一次
#    采样窗口 176~260 的残留，时间戳差了半小时）。
#    带指纹的目录名从根上绕开它：不同批次天然隔离，同名即为同一次采样。
FXOFF=""
[ "${VA_FX:-1}" = "0" ] && FXOFF="_off"
# 指纹要吃进**所有会改变画面的输入**，不只是时刻列表 ——
# 只按 TIMES 打指纹时，同一时刻、不同机位（VA_CAM_H / VA_CAM_PITCH）的两次拍摄
# 会撞进同一个目录、后者覆盖前者，而失败表现同样是"图上少了一版"（2026-09-25 踩到：
# ANG 判别实验的近景组与俯视组写到了同一处，只剩俯视那 3 张）。
TAG="$(printf '%s|%s|%s|%s|%s|%s|%s' "$TIMES" "${VA_FF:-3}" "${VA_CAM_H:-}" \
       "${VA_CAM_PITCH:-}" "${VA_FX_ANG:-}" "${VA_FX_TW:-}" "${VA_FX_PROBE:-}" \
       | cksum | awk '{print $1}')"
OUT="sweep/v_fx_${MODE}${FXOFF}_${TAG}"

export VA_SEED="${VA_SEED:-20260925}"
export VA_SCRIPT_A=1
export VA_AUTO=1
export VA_FF="${VA_FF:-3}"
export VA_DBG_FX=1
export VA_CAPTURE="$TIMES"
export VA_CAPTURE_DIR="res://$OUT"

# diff / probe 要藏 HUD 与枪模：罗盘、雷达、提示条、枪模摇摆都是**墙钟动画**，
# 两次运行不可能对齐，做像素差时会整片污染差值（腿摆那一层实测踩过）。
if [ "$MODE" != "look" ]; then
  export VA_HIDE_HUD=1
  export VA_VM_HIDE=1
fi
[ "$MODE" = "probe" ] && export VA_FX_PROBE=1

mkdir -p "$OUT"

# 自检：目录里若有**不属于本次时刻列表**的旧截图，明确报出来（不删 —— 见上面 rm 那段）。
# 悄悄留着才是真正的坑：差分时"缺对照"只是跳过，而多出来的旧张会让人以为跑了新窗口。
STALE=0
for f in "$OUT"/cap_*.png; do
  [ -e "$f" ] || continue
  case ",$TIMES," in
    *",$(basename "$f" | sed 's/^cap_//; s/s\.png$//'),"*) ;;
    *) STALE=$((STALE + 1)) ;;
  esac
done
[ "$STALE" -gt 0 ] && echo "⚠️  $OUT 里有 $STALE 张不属于本次时刻列表的旧截图（差分时会显示「缺对照」）"

# Godot 的 --path 要 Windows 原生路径；MSYS 的 /e/... 它不认，
# 会直接报 "Invalid project path specified" 然后退出（exit 1，无任何截图）。
WIN_ROOT="$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")"
LOG="$OUT/run.log"

echo "模式=$MODE   VA_FX=${VA_FX:-1}${VA_FX_PROBE:+   VA_FX_PROBE=$VA_FX_PROBE}   VA_FF=$VA_FF   VA_SEED=$VA_SEED"
echo "时刻=$TIMES"
echo "目录=$OUT"
echo "启动…（交火窗口跑到 260 战局秒，ff=$VA_FF 时约 $((260 / VA_FF)) 秒真实时间）"

"$ROOT/sdk/godot/Godot_v4.5-stable_win64_console.exe" --path "$WIN_ROOT" > "$LOG" 2>&1
echo "EXIT=$?"

N="$(ls "$OUT"/cap_*.png 2>/dev/null | wc -l)"
echo "截图数: $N"

# 回归判据是"日志里有没有 ERROR"，顺手报一下。
ERRS="$(grep -c 'ERROR' "$LOG" 2>/dev/null)"
[ -z "$ERRS" ] && ERRS=0
echo "日志 ERROR 行数: $ERRS   （详见 $LOG）"

# 光效层自己那几行：先报就绪参数，再报交火期最后几条统计。
# **「一个光效都没画」和「画了但都在镜头外」是两种故障**，画面上都表现为"没看见光"，
# 这组数字就是把它们分开的那一条。
echo "--- [fx] 就绪 ---"
grep -E '^\[fx\]' "$LOG" | head -4
echo "--- [fx] 交火期尾部统计（每 0.5 秒墙钟一行）---"
grep -E '^\[fx\]' "$LOG" | tail -6
echo "--- ERROR ---"
grep 'ERROR' "$LOG" | head -5
exit 0
