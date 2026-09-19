# -*- coding: utf-8 -*-
"""VolunteerArmyPC —— 批量图生3D：立绘/参考图 → assets/art/<档位>/model/<键>.glb

【两档：角色（char）与武器（wpn）】
    角色：输入 assets/art/char/<键>.png      → 成品 assets/art/char/model/<键>.glb
    武器：输入 assets/art/wpn/<键>.png       → 成品 assets/art/wpn/model/<键>.glb
             中间产物 sweep/gen3d_wpn/<键>.json
    用 `--kind wpn` 切换（默认 char）。**中间产物目录分家**是刻意的：
    两档的键名不同、任务 id 不同，混在一个目录里之后"这个 json 是哪批的"只能靠脑子记。
    键名一律带前缀（wpn_ / char_），所以就算目录合并也不会撞名 —— 分家是为了可读，不是防重名。

【为什么要有这一层，而不是在 shell 里 for 循环】
一次图生3D 是「提交 → 轮询 1~5 分钟 → 拿到 URL → 下载 42MB → 瘦身到 1.5MB」，
四个环节各有各的失败方式（提交被限流、轮询超时、COS 链接断流、GLB 里 BIN 块没取到）。
放进 shell 里意味着任何一个环节失败都只有一个 exit code，事后不知道卡在哪。
这里把每一步的结果都落到 `sweep/gen3d/<键>.*`，失败也能从中断处单点重跑。

【并发而不是串行】串行 11 个要 20~50 分钟，而且这期间没有任何产出。
但服务端对 hy-3d 维度**只放 2 个并发槽位**，超了直接 429 拒绝，
所以并发固定按 2 来（--jobs 可调，调大只会让多出来的任务去撞 429 然后退避等待，
并不会更快）。遇到 429 会自动退避重试 —— 槽位是会自己空出来的，
而"面数超限 / 图片过大"那类不可等，所以只对 429 重试，其余立刻报错退出。

【为什么要去水印后再喂】输入用 `assets/art/char/<键>.png`（tools/prep_char.py 的产物），
**不是** `assets/art/char/raw/` 里的原始立绘 —— 原始图上有半透明水印，
图生3D 会把它当成服装上的图案烘进贴图，而建好的模型不会再走一遍去水印，错就错到底了。

【三个后端，靠 VA_GEN3D_BACKEND 切换】
    builtin（默认）: tools/gen3d.py     → 内置多模态通道，**限 5 次提交/天且当天不重置**
    tc            : tools/gen3d_tc.py  → 腾讯云混元生3D 官方 API 直连（CAM 签名，要 SecretId/SecretKey）
    hy            : tools/gen3d_hy.py  → 混元生3D（TokenHub），**只要一把 sk- 开头的 API Key**（Bearer），
                                        默认 3 并发、无每日提交限制 —— 目前门槛最低的一条路
    三者**结果 JSON 格式相同**，所以下面的下载 / 瘦身 / 续跑逻辑完全共用。
    tc / hy 都不需要 stdin 的 token（凭据由各自脚本读环境变量或 ~/.workbuddy/ 下的文件）；
    并发可以按官方的 3 来（builtin 只有 2）。

用法：
    echo -n "<token>" | python tools/gen3d_batch.py [--jobs 4] [键 ...]      # builtin
    python tools/gen3d_batch.py --jobs 3 [键 ...]                            # VA_GEN3D_BACKEND=tc
    python tools/gen3d_batch.py --jobs 3 [键 ...]                            # VA_GEN3D_BACKEND=hy
    python tools/gen3d_batch.py --kind wpn --jobs 3 wpn_mosin                # 武器档位

    不给键就做该档位全部；已存在 <键>.glb 且非空的**默认跳过**（--force 覆盖），
    所以中断之后直接再跑一次就是"接着做没做完的"。
"""

import json
import os
import re
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 档位表。**这三条路径是模块级全局**，由 main() 按 --kind 覆写 ——
# run_one() 是这份脚本里唯一带着"提交/下载/瘦身/校验"全部副作用的地方，
# 与其把它改成到处传参，不如让路径在进入批次之前就定下来、之后不再变。
# 覆写点只有一处（见 _select_kind），所以不存在"两个地方各写一份路径"的问题。
PROFILES = {
    "char": {
        "label": "角色",
        "src_dir": os.path.join(ROOT, "assets", "art", "char"),
        "model_dir": os.path.join(ROOT, "assets", "art", "char", "model"),
        "work": os.path.join(ROOT, "sweep", "gen3d"),
        # 键的顺序 = scene_builder.cpp 里 all_art_keys() 的顺序，方便两边对账
        "keys": [
            "char_leader", "char_rifleman", "char_mg", "char_sniper",
            "char_at", "char_demo", "char_medic", "char_ammo",
            "char_enemy_rifle", "char_enemy_mg", "char_enemy_officer",
        ],
    },
    "wpn": {
        "label": "武器",
        "src_dir": os.path.join(ROOT, "assets", "art", "wpn"),
        "model_dir": os.path.join(ROOT, "assets", "art", "wpn", "model"),
        "work": os.path.join(ROOT, "sweep", "gen3d_wpn"),
        # 顺序 = scene_builder.cpp 里 all_wpn_keys() 的顺序，也是游戏里 V 键循环的顺序。
        # 1951 年志愿军制式：步枪 / 冲锋枪 / 轻机枪。
        "keys": ["wpn_mosin", "wpn_ppsh", "wpn_dp27"],
    },
}

SRC_DIR = PROFILES["char"]["src_dir"]
MODEL_DIR = PROFILES["char"]["model_dir"]
WORK = PROFILES["char"]["work"]
KEYS = list(PROFILES["char"]["keys"])

PY = sys.executable
# 瘦身要 Pillow，而 Pillow 只装在隔离 venv 里（本项目唯一一个非纯 Python 的工具）。
# 若该 venv 不存在，脚本会明确报出来而不是悄悄产出一个 42MB 的 GLB 进仓库。
VENV_PY = ("C:/Users/lujiajing/.workbuddy/binaries/python/envs/default/Scripts/python.exe")

# 【`--face-count 50000` 必须显式给，这不是可选项】
# buddy-cloud.py 的 `--face-count` 默认值是 **500000**（10 万~150 万区间），
# 不传就等于向服务端要 50 万面。实测后果（2026-09-18 第二批 5 个）：
#   50 万面 → 单模型 15MB、三角面 500,000（网格自己就占掉约 13.5MB，
#             与贴图无关，瘦身工具压不动）
#   5 万面  → 单模型 1.56MB、三角面 50,162（= 已接入的 char_rifleman 规格）
# 10 倍面数对「1.7 米的人站在 110 米战场上只占几十像素」毫无收益，
# 却让 11 个角色的仓库体积从 17MB 涨到 165MB，并让同屏几何量翻十倍。
# 旁证：两者积分档位不同（5 万面 40 分 / 50 万面 30 分），
# 所以「积分对不上」其实是面数档位不一致的先兆，不只是计价差异。
# 教训：这里原本写着"与试接那次完全一致"，但试接显式带了 50000 ——
# **"参数一致"必须以双方的实际请求体为准，不能凭记忆断言**。
# 面数只在这一个地方写死 —— 内置后端的命令行参数与腾讯云后端的 --face-count
# 都引用它（"同一个数字出现两遍就一定会漏改一处"）。
FACE_COUNT = 50000
GEN_ARGS = ["--enable-pbr", "--generate-type", "Normal", "--face-count", str(FACE_COUNT)]

# 【模型档位】hy 后端用哪一版混元生3D，默认 3.1（与内置通道 / 已接入模型对齐）。
# 可被 VA_GEN3D_MODEL 覆盖 —— 典型场景：免费额度在 3.1 上耗尽后改试同族的 3.0。
# 依据：TokenHub 的免费体验包是**按模型**领取的，即每个模型各有独立额度；
# 而 3.0 / 3.1 同属混元生3D，风格差异远小于换到 tripo-3d-3.1 / hi3d-2.1 这类别的模型族。
# 只对 hy 生效：builtin 的 model 由 buddy-cloud.py 决定，tc 走 CAM 签名那套字段。
G3D_MODEL = os.environ.get("VA_GEN3D_MODEL", "hy-3d-3.1").strip()

# ---- 后端选择 ----
# builtin：内置多模态通道（gen3d.py），限 5 次提交/天，且**同一天内不重置**。
# tc     ：腾讯云混元生3D 官方 API 直连（gen3d_tc.py），CAM 签名，要 SecretId/SecretKey。
# hy     ：混元生3D（TokenHub，gen3d_hy.py），Bearer 认证，只要一把 sk- API Key，
#          默认 3 并发、无每日提交限制。凭据由后端脚本自己读，不走 stdin。
BACKEND = os.environ.get("VA_GEN3D_BACKEND", "builtin").strip().lower()
if BACKEND not in ("builtin", "tc", "hy"):
    raise SystemExit("VA_GEN3D_BACKEND 只能是 builtin / tc / hy，当前为 %r" % BACKEND)


LOG_LOCK = threading.Lock()

# 服务端对 hy-3d 维度只放 2 个并发槽位，超了直接 429 拒绝（**不扣积分**）。
# 所以并发默认就是 2：开 6 个不是"更快"，只是让 4 个任务在 15 秒内各报一次失败。
#
# 【为什么重试预算要给到 25 次而不是 8 次】实测这 2 个槽位**并不总是空着**：
# 连续跑了 6 分钟、每次都是 429，而我方一个任务都没提交成功 ——
# 说明占着槽位的东西不是我们自己（试接那次早就结束了）。
# 这类共享维度的配额什么时候空出来是外部决定的，只能等。
# 等不到就等不到，脚本会明确报"重试 N 次仍失败"，不会假装成功。
SUBMIT_TRIES = 12
SUBMIT_WAIT_CAP = 30        # 退避上限（秒）：token 只有 ~20 分钟寿命，等太久等于白等

# 产物规格上限（三角面）。已接入的 char_rifleman = 50,162 面，
# 而"没给 --face-count"时服务端会给 500,000 面 —— 两者相差 10 倍，
# 肉眼在战场上分辨不出，但体积差 10 倍、同屏几何量差 10 倍。
# 所以宁可让日志把这件事喊出来，也不要等到 11 个模型都接进场景才发现规格不齐。
MAX_TRIS_OK = 120000

# 429 有**两种含义完全不同**的 429，必须分开对待 —— 这是本轮花掉 10 分钟才看出来的：
#   "concurrent slot limit exceeded (2) for dimension hy-3d"  → 并发槽位被占，**可等**，槽位会空出来
#   "daily submit limit exceeded (5/5) for dimension hy-3d"   → **当日提交配额用尽**，
#                                                              同一个 token / 同一天内重试多少次都是白等
# 第一版把两者都当"可等"，于是日配额用尽之后仍然老老实实重试 12 次 × 最多 30s，
# 而且每次都打印"槽位已满"——把真正的结论（今天做不了了）盖在噪音底下。
# 两个后端的失败语义都在这一处归类（先判 fatal，再判 retry）：
#   内置通道： concurrent slot limit（可等） / daily submit limit（当日终止）
#   腾讯云官方：RequestLimitExceeded、LimitExceeded、InternalError（可等）
#               InsufficientBalance、AuthFailure.*、UnauthorizedOperation（重试无意义）
RETRYABLE_KEYS = (
    "slot limit", "concurrency",
    "requestlimitexceeded", "limitexceeded", "internalerror",
    "ratelimit", "toomanyrequests", "servererror",
)
# 【为什么"超时"故意不列进可重试】提交是**非幂等**的：一个超时的提交可能已经在服务端
# 排上了队。自动重试 = 同一个角色提交两次 = 双倍积分，而且两个模型只有一个能被采用。
# 超时归到 other（不重试、如实报失败），由人决定要不要重跑 —— 这时才该去看
# sweep/gen3d/<键>.json 里有没有已经拿到的任务 id（有就 query 补下载，别重新提交）。
FATAL_KEYS = (
    "daily submit limit", "quota", "daily limit",
    "insufficientbalance", "resourceinsufficient", "arrears",
    "authfailure", "secretidnotfound", "unauthorizedoperation",
    "invalid_api_key", "incorrect api key", "unauthorized", "forbidden",
    # TokenHub 的无效 Key 报的是 401002「The API Key does not exist or signature
    # verification failed…」—— 既不含 invalid_api_key 也不含 unauthorized，
    # 实测漏判过一次（于是坏 Key 会让每个键各自失败一遍而不是立刻停整批）。
    "api key", "401002",
)


def _classify(body):
    """把一次提交失败分成 'retry'(可等) / 'fatal'(今天别再试) / 'other'(不可重试)。"""
    b = body.lower()
    if any(k in b for k in FATAL_KEYS):
        return "fatal"
    if any(k in b for k in RETRYABLE_KEYS):
        return "retry"
    return "other"


def log(*a):
    with LOG_LOCK:
        print(*a)
        sys.stdout.flush()


class QuotaExhausted(Exception):
    """当日提交配额用尽。**整批必须立刻停**，不是这一个键失败。"""


def _select_kind(p_kind):
    """按档位覆写 SRC_DIR / MODEL_DIR / WORK / KEYS。

    【为什么用 global 而不是把路径传进 run_one】run_one 里那四条路径要在
    "续跑判据 / 输入校验 / 结果落盘 / 成品落盘 / 日志"五处以同样的值出现，
    传参等于给每一处都增加一个可以说错话的机会。这里覆写一次、之后再没人改，
    语义上就是"这一批的路径"。覆写点唯一，所以不会出现两份路径不一致。
    """
    global SRC_DIR, MODEL_DIR, WORK, KEYS
    if p_kind not in PROFILES:
        raise SystemExit("--kind 只能是 %s，当前为 %r"
                         % (" / ".join(sorted(PROFILES)), p_kind))
    p = PROFILES[p_kind]
    SRC_DIR = p["src_dir"]
    MODEL_DIR = p["model_dir"]
    WORK = p["work"]
    KEYS = list(p["keys"])
    return p


def _probe_tris(glb):
    """读一个 GLB 的三角面数；读不出来返回 None（视为"规格未知"）。

    【为什么要把它接进续跑判据】2026-09-18 的一次真实事故：那批 50 万面的成品被移走后，
    结果 JSON 与 raw 还在，于是脚本按"json 里 status==DONE"判定 char_leader 已完成，
    **跳过提交、直接拿旧 URL 重新瘦身** —— 又把一个 15MB / 50 万面的模型放回了模型目录。
    只判"任务成功"是不够的：产物规格也是完成度的一部分。
    """
    if not os.path.isfile(VENV_PY) or not os.path.isfile(glb):
        return None
    try:
        p = subprocess.run(
            [VENV_PY, os.path.join(ROOT, "tools", "slim_glb.py"), "probe", glb],
            capture_output=True, text=True, encoding="utf-8", errors="replace", cwd=ROOT,
        )
        m = re.search(r"三角面\s*(\d+)", p.stdout or "")
        return int(m.group(1)) if m else None
    except Exception:
        return None


def run_one(key, token, force):
    tag = key

    def say(msg):
        log("[%s] %s" % (tag, msg))

    out_json = os.path.join(WORK, key + ".json")
    raw_glb = os.path.join(WORK, key + ".raw.glb")
    dst_glb = os.path.join(MODEL_DIR, key + ".glb")

    if not force and os.path.exists(dst_glb) and os.path.getsize(dst_glb) > 0:
        tris = _probe_tris(dst_glb)
        if tris is None or tris <= MAX_TRIS_OK:
            say("已有成品 %.2f MB%s，跳过"
                % (os.path.getsize(dst_glb) / 2 ** 20,
                   "" if tris is None else "（%d 面）" % tris))
            return True
        say("已有成品但规格不对（%d 面 > %d），重做" % (tris, MAX_TRIS_OK))

    src = os.path.join(SRC_DIR, key + ".png")
    if not os.path.isfile(src):
        say("缺立绘 %s" % src)
        return False

    # ---- 0. 上次的 json 是"成功的"还是"失败留下的"？----
    # 【为什么必须看内容而不是只看文件在不在】第一次跑（并发 6）11 个全被 429 拒了，
    # 但每个都留下了一个**非空的** json（里面是错误对象）。只判"文件存在且非空"
    # 就会把这一批全部当成"已完成"跳过 —— 脚本报一切正常，仓库里一个模型都没有。
    # 所以判据是 json 里的状态是不是 DONE。
    done_json = False
    if not force and os.path.exists(out_json) and os.path.getsize(out_json) > 0:
        try:
            j = json.load(open(out_json, encoding="utf-8"))
            done_json = (j.get("status") or j.get("raw_result", {}).get("Status")) == "DONE"
        except Exception:
            done_json = False
        # 光"任务成功"不算完成：成品必须在、且规格正确。
        # 否则一份旧 JSON（比如规格错的那批）会让后续每一次续跑都"跳过提交、
        # 拿旧结果重新瘦身"，把错误规格一次次复活。见 _probe_tris 的注释。
        if done_json:
            tris = _probe_tris(dst_glb) if os.path.exists(dst_glb) else None
            if tris is None or tris > MAX_TRIS_OK:
                done_json = False

    # ---- 1. 提交并轮询（交给各自的后端脚本；两者都改用进程内 argv 绕开命令行长度上限）----
    if not done_json:
        t0 = time.time()
        say("提交中（输入 %.2f MB，后端 %s）…" % (os.path.getsize(src) / 2 ** 20, BACKEND))
        # PYTHONIOENCODING：子进程的 stdout 是管道，Python 会按系统区域（GBK）编码它，
        # 而父进程按 UTF-8 解 —— 不解这一下，日志里所有中文都是乱码，
        # 而"哪一步说了什么"正是这个脚本存在的意义。
        env = dict(os.environ, PYTHONIOENCODING="utf-8")
        if BACKEND == "tc":
            # 腾讯云直连不吃 stdin：凭据由 gen3d_tc.py 自己从环境变量或
            # ~/.workbuddy/tencentcloud.json 读（不落命令行、不进仓库）。
            cmd = [PY, os.path.join(ROOT, "tools", "gen3d_tc.py"), "submit",
                   src, out_json, "--face-count", str(FACE_COUNT)]
            stdin_text = None
        elif BACKEND == "hy":
            # TokenHub 同理：凭据由 gen3d_hy.py 读环境变量 TOKENHUB_API_KEY 或
            # ~/.workbuddy/va_3d_api_key.txt。面数走同一个 FACE_COUNT 真值来源。
            cmd = [PY, os.path.join(ROOT, "tools", "gen3d_hy.py"), "submit",
                   src, out_json, "--face-count", str(FACE_COUNT),
                   "--model", G3D_MODEL]
            stdin_text = None
        else:
            cmd = [PY, os.path.join(ROOT, "tools", "gen3d.py"), src, out_json] + GEN_ARGS
            stdin_text = token + "\n"
        for attempt in range(1, SUBMIT_TRIES + 1):
            p = subprocess.run(
                cmd, input=stdin_text, capture_output=True, text=True, encoding="utf-8",
                errors="replace", cwd=ROOT, env=env,
            )
            if p.returncode == 0:
                say("生成完成，用时 %.0fs" % (time.time() - t0))
                break
            # 提交阶段失败多半是并发槽位用满，服务端返回
            #   {"error":"HTTP_ERROR","message":"concurrent slot limit exceeded (2) for
            #    dimension hy-3d","http_status":429}
            # 这是**可等**的：槽位会自己空出来。而"面数超限/图片过大"那种不可等，
            # 所以只对 429 / slot limit 这类关键词重试，其余立刻放弃并把原因打出来，
            # 免得把 11 个任务都拖成"重试 6 次 × 每次 1 分钟"却什么都没做。
            # 【为什么把两处输出拼起来而不是二选一】内置后端的错误体写在结果 JSON 里
            # （它无论如何都会落盘）；tc 后端的错误只打在 stdout/stderr（失败时不写 JSON）。
            # 只读一边，另一边后端的失败原因就会"看起来像空字符串"而被归成 other。
            body = (p.stderr or "") + (p.stdout or "")
            try:
                body += open(out_json, encoding="utf-8").read()
            except Exception:
                pass
            kind = _classify(body)
            if kind == "fatal":
                # 当日配额用尽：**继续重试是纯粹浪费时间**，而且会把真正的结论
                # （"今天做不了了"）盖在"槽位已满"的噪音底下。直接终止整个批次 ——
                # 剩下的键今天也不会有结果，让脚本立刻以明确的结论退出，
                # 而不是再花 10 分钟逐个报"失败"。
                say("不可恢复的提交失败（当日配额 / 凭据 / 余额），终止整批：%s"
                    % body.strip()[:200])
                raise QuotaExhausted(body.strip()[:300])
            if kind != "retry":
                say("生成失败（不可重试）rc=%d：%s" % (p.returncode, body.strip()[:300]))
                return False
            if attempt < SUBMIT_TRIES:
                wait = min(SUBMIT_WAIT_CAP, 15 * attempt)
                say("提交被拒（可重试：并发槽位 / 限频），%ds 后重试（第 %d/%d 次）"
                    % (wait, attempt, SUBMIT_TRIES))
                time.sleep(wait)
            else:
                say("提交重试 %d 次仍失败：%s" % (SUBMIT_TRIES, body.strip()[:200]))
                return False
        else:
            return False

    # ---- 2. 从结果里取 GLB 链接 ----
    try:
        d = json.load(open(out_json, encoding="utf-8"))
    except Exception as e:
        say("结果 json 不可读：%r" % e)
        return False

    st = d.get("status") or d.get("raw_result", {}).get("Status")
    if st != "DONE":
        say("任务状态 %s，不是 DONE" % st)
        return False

    url = None
    for f in d.get("raw_result", {}).get("ResultFile3Ds", []) or []:
        if str(f.get("Type", "")).upper() == "GLB":
            url = f.get("Url")
            break
    if not url:
        say("结果里没有 GLB 链接")
        return False
    say("积分 %s" % d.get("raw_result", {}).get("ResultCreditConsumed"))

    # ---- 3. 下载 ----
    if not os.path.exists(raw_glb) or os.path.getsize(raw_glb) < 1024:
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import dl  # 复用它那套 Range 续传 + 退避重试
        try:
            dl.fetch(url, raw_glb)
        except Exception as e:
            say("下载失败：%r" % e)
            return False

    # ---- 4. 瘦身 ----
    if not os.path.isfile(VENV_PY):
        say("找不到带 Pillow 的隔离 venv：%s" % VENV_PY)
        return False
    p = subprocess.run(
        [VENV_PY, os.path.join(ROOT, "tools", "slim_glb.py"),
         "slim", raw_glb, dst_glb],
        capture_output=True, text=True, encoding="utf-8", errors="replace", cwd=ROOT,
    )
    if p.returncode != 0:
        say("瘦身失败 rc=%d：%s" % (p.returncode, (p.stderr or p.stdout or "")[-400:]))
        return False
    say("成品 %.2f MB（原 %.2f MB）" % (
        os.path.getsize(dst_glb) / 2 ** 20, os.path.getsize(raw_glb) / 2 ** 20))

    # ---- 5. 规格校验：面数是不是我们要的那一档 ----
    # 【为什么值得单独查一次】"参数给了"不等于"服务端照做了"，而面数不对在战场上
    # 完全看不出来（单位只有几十像素高），只有体积和帧率会说话 ——
    # 等到 11 个模型全接进场景才发现规格不齐，代价是重做一整天。
    # 这里只报警不判失败：参数是固定的，报警意味着服务端行为变了，该由人来判断；
    # 而"续跑判据"那边（见 _probe_tris 与 done_json）会直接把不合格的成品重做。
    tris = _probe_tris(dst_glb)
    if tris is None:
        say("规格校验跳过（读不出三角面数）")
    else:
        say("三角面 %d%s" % (tris, "" if tris <= MAX_TRIS_OK else
                            "  ← 警告：超过预期上限 %d，规格与已接入模型不一致" % MAX_TRIS_OK))
    return True


def main():
    argv = sys.argv[1:]
    jobs = 2
    force = False
    kind = "char"
    keys = []
    i = 0
    while i < len(argv):
        if argv[i] == "--jobs":
            jobs = int(argv[i + 1]); i += 2
        elif argv[i] == "--force":
            force = True; i += 1
        elif argv[i] == "--kind":
            kind = argv[i + 1]; i += 2
        else:
            keys.append(argv[i]); i += 1

    # 档位必须在"读 token / 建目录 / 报批次"之前定下来 ——
    # 它决定的是这一批的输入目录与成品目录，晚一步就会把武器写进角色目录里。
    prof = _select_kind(kind)

    # tc / hy 后端的凭据由各自脚本自己读，不吃 stdin（所以这里不检查、也不阻塞等待）。
    token = ""
    if BACKEND == "builtin":
        token = sys.stdin.readline().strip()
        if not token:
            print("stdin 没有拿到 token")
            return 2
    if not keys:
        keys = list(KEYS)

    os.makedirs(WORK, exist_ok=True)
    os.makedirs(MODEL_DIR, exist_ok=True)

    log("批次：%d 个%s，并发 %d，后端 %s%s"
        % (len(keys), prof["label"], jobs, BACKEND, "，强制重做" if force else ""))
    log("  输入 %s" % SRC_DIR)
    log("  成品 %s" % MODEL_DIR)

    lock = threading.Lock()
    todo = list(keys)
    fail = []
    quota = []          # 一旦非空，说明当日配额用尽，整批停

    def worker():
        while True:
            with lock:
                if not todo or quota:
                    return
                k = todo.pop(0)
            try:
                ok = run_one(k, token, force)
            except QuotaExhausted as e:
                # 整批停：把剩下的任务全部记为未完成，各线程看 quota 非空就会退出。
                # 这里不把 quota 当作"异常"往上抛 —— 它是**一个明确的业务结论**，
                # 不是脚本坏了，所以走正常收尾路径把话说清楚。
                with lock:
                    quota.append(str(e))
                    # 触发终止的那个键也要记进去：它在 pop 时就离开了 todo，
                    # 只 extend(todo) 会把它漏掉，结论行就会少报一个。
                    fail.append(k)
                    fail.extend(todo)
                    todo.clear()
                return
            except Exception as e:      # 任何意外都不该带走整个批次
                log("[%s] 异常 %r" % (k, e))
                ok = False
            if not ok:
                with lock:
                    fail.append(k)

    ths = [threading.Thread(target=worker, daemon=True) for _ in range(max(1, jobs))]
    for t in ths:
        t.start()
    for t in ths:
        t.join()

    if quota:
        log("")
        log("=== 结论：提交无法继续（配额 / 凭据 / 余额），剩余 %d 个没做成 ===" % len(fail))
        log("=== 原文：%s" % quota[0])
        if BACKEND in ("tc", "hy"):
            log("=== 提示：%s 后端没有每日提交上限；若是余额或凭据问题，"
                "处理后在控制台确认即可重跑（已完成的会自动跳过）===" % BACKEND)
        else:
            log("=== 提示：内置通道按天限 5 次提交且当天不重置，明天直接重跑本脚本即可 ===")
        log("=== 也可以换后端：VA_GEN3D_BACKEND=hy（TokenHub，一把 sk- Key 即可）"
            " 或 tc（CAM 签名，要 SecretId/SecretKey）===")

    have = [k for k in keys if os.path.exists(os.path.join(MODEL_DIR, k + ".glb"))]
    log("")
    log("=== 齐备 %d / %d：%s" % (len(have), len(keys), ", ".join(have)))
    if fail:
        log("=== 失败：%s（重跑本脚本即可只补这些）" % ", ".join(fail))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
