# -*- coding: utf-8 -*-
"""调用内置多模态 3D 生成能力（图生3D / 文生3D），把 base64 从文件读入。

【为什么不直接命令行调内置脚本】
它的 `--image-base64` 是一个普通字符串参数，而这两张立绘单张就有 2.7MB，
base64 之后约 3.6MB —— Windows 命令行上限 32767 字符，作为 argv 传必然失败。
所以这里把图片读成 base64 后**在进程内**改写 sys.argv 再调它的 main()，
绕开命令行长度限制；token 依旧走 stdin（不落 argv、不落进程列表）。

用法：
    echo -n "<token>" | python tools/gen3d.py <图片路径> <结果json输出路径> [额外参数...]
    echo -n "<token>" | python tools/gen3d.py --text <中文描述> <结果json输出路径> [额外参数...]

额外参数原样透传给内置脚本，例如：
    --no-poll          只提交不等待
    --enable-pbr       生成 PBR 材质
    --face-count 50000 面数上限

【为什么要加 `--text`（2026-09-23）】
图生3D 是**单视图重建**：给一张照片，反推它占的三维体积。对"靠半透明叶簇
表达体积"的东西（树 / 灌丛）这条路走不通 —— 参考图里枝干之间透出背景，
重建拿不到"叶子围成的体积"，于是要么只留一根主干、要么把地面剪影当成物体
（实测：树塌成水平只有高度 6.3% 的一根杆、灌丛塌成高只有最大维 0.16% 的圆盘）。
文生3D 走的是另一条路：**从头合成一个体量**，不受"只能看到一面"的约束，
所以对针叶树这种"体量简单的轴对称物体"是更合适的通道。
内置脚本的 `3d` 子命令本来就接受 `prompt` 位置参数，只是本文具此前只走图片那条。

【2026-09-24：内置脚本改名了，本文件因此改成"按目录发现"】
实测现象：提交时整批立刻失败、rc=2、**一条都没提交**——
    [prop_pine2] 生成失败（不可重试）rc=2：找不到 buddy-cloud.py：<...>/scripts/buddy-cloud.py
根因：插件在 09-23 更新过，`scripts/buddy-cloud.py` 已改名为
    `scripts/buddy-multimodal-generation.py`
而这里把**文件名写死**了 → 目录还在、文件没了 → 静默变成"整批失败"。
（好在这次是发生在**扣额度之前**的路径，0 积分。但"写死一个会变的名字"这件事
本身就该修，而不是把新名字再写死一次。）

改法：**按目录发现**，不认死名字 ——
    ① 环境变量 `VA_BUDDY_MM_SCRIPT` 显式指定（要切到别的安装位置时用）；
    ② 目录里名字含 "multimodal" 的 .py（当前版本的命名）；
    ③ 目录里名字含 "buddy" 的 .py（旧命名兼容）；
    ④ 目录里只有一个 .py 就用它。
真找不到时**把目录里实际有什么列出来**，让人一眼看出"是不是又改名了"。

另外：新脚本的 token 参数由 `--token-stdin` 改成了全局 `--token <ck_t_...>`，
且会校验前缀必须是 `ck_t_`。本文具仍然从 stdin 读 token、再放进**进程内的** sys.argv，
所以 token 不会出现在操作系统的进程列表里（新脚本自己也会把回显里的 token 打码）。
"""

import base64
import glob
import importlib.util
import io
import os
import sys

# 本机 WorkBuddy Desktop 的解包目录。必须是 app.asar.unpacked，
# app.asar/resources/... 那条路径在安装包里不存在。
SKILL_DIR = ("C:/Program Files/WorkBuddy/resources/app.asar.unpacked/resources/"
             "plugins/workbuddy-builtin/skills/buddy-multimodal-generation/")
SCRIPTS_DIR = SKILL_DIR + "scripts/"


def find_script():
    """按目录发现内置脚本，返回 (路径, 说明)。找不到返回 (None, 原因)。"""
    env = os.environ.get("VA_BUDDY_MM_SCRIPT")
    if env:
        if os.path.isfile(env):
            return env, "来自 VA_BUDDY_MM_SCRIPT"
        return None, "VA_BUDDY_MM_SCRIPT 指向的文件不存在：%s" % env
    if not os.path.isdir(SCRIPTS_DIR):
        return None, "内置技能的 scripts 目录不存在：%s" % SCRIPTS_DIR
    pys = sorted(glob.glob(os.path.join(SCRIPTS_DIR, "*.py")))
    for needle, why in (("multimodal", "按名字含 multimodal 命中"),
                        ("buddy", "按名字含 buddy 命中（旧命名）")):
        for p in pys:
            if needle in os.path.basename(p).lower():
                return p, why
    if len(pys) == 1:
        return pys[0], "目录里只有一个 .py"
    return None, ("目录里有 %d 个 .py、没有一个看起来是主脚本：%s"
                  % (len(pys), ", ".join(os.path.basename(p) for p in pys)))


def load_bc(path):
    spec = importlib.util.spec_from_file_location("buddy_mm", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    argv = sys.argv[1:]
    text_mode = False
    if argv and argv[0] == "--text":
        text_mode = True
        argv = argv[1:]
    if len(argv) < 2:
        print(__doc__)
        return 2
    subject = argv[0]          # 图片路径（图生3D）或中文描述（文生3D）
    out_json = argv[1]
    extra = argv[2:]

    script, why = find_script()
    if script is None:
        # 【为什么要列目录内容】上一次就是因为改名，而报错只说"找不到 buddy-cloud.py"，
        # 看的人得自己想到去 ls 那个目录。把"现在有什么"直接打出来，
        # 这条报错就自带下一步动作。
        print("找不到内置的 3D 生成脚本：%s" % why)
        print("目录 %s 下当前的文件：" % SCRIPTS_DIR)
        if os.path.isdir(SCRIPTS_DIR):
            for n in sorted(os.listdir(SCRIPTS_DIR)):
                print("    %s" % n)
        else:
            print("    （目录本身不存在）")
        print("如果是脚本又改名了，改 find_script() 的候选名单，"
              "或用 VA_BUDDY_MM_SCRIPT 直接指定。")
        return 2

    token = sys.stdin.readline().strip()
    if not token:
        print("stdin 没有拿到 token")
        return 2

    if text_mode:
        if not subject.strip():
            print("--text 的描述是空的")
            return 2
        head = [subject]
    else:
        if not os.path.isfile(subject):
            print("找不到输入图片：%s" % subject)
            return 2
        with open(subject, "rb") as f:
            b64 = base64.b64encode(f.read()).decode()
        head = ["--image-base64", b64]

    bc = load_bc(script)
    # 注意顺序：`--token` 是**子命令**的参数（挂在 3d 下面），必须放在 "3d" 之后。
    sys.argv = ["buddy-multimodal-generation.py", "3d"] + head + ["--token", token] + extra
    sys.stdin = io.StringIO("")   # 新脚本不再从 stdin 读 token，留着空的免得被误读

    # 把 stdout 也截下来存盘：3D 任务要跑 1~5 分钟，
    # 万一后续步骤出错或会话中断，job_id 还在文件里能接着查。
    real_stdout = sys.stdout
    buf = io.StringIO()
    sys.stdout = buf
    try:
        bc.main()
    except SystemExit as e:
        if e.code not in (0, None):
            sys.stdout = real_stdout
            print("内置脚本退出码 %s（脚本：%s）" % (e.code, os.path.basename(script)))
            with open(out_json, "w", encoding="utf-8") as f:
                f.write(buf.getvalue())
            return 1
    finally:
        sys.stdout = real_stdout

    text = buf.getvalue()
    with open(out_json, "w", encoding="utf-8") as f:
        f.write(text)
    print(text)
    print("[gen3d] 结果已存 %s（内置脚本 %s，%s）"
          % (out_json, os.path.basename(script), why))
    return 0


if __name__ == "__main__":
    sys.exit(main())
