# -*- coding: utf-8 -*-
"""调用内置多模态 3D 生成能力（图生3D / 文生3D），把 base64 从文件读入。

【为什么不直接命令行调 buddy-cloud.py】
它的 `--image-base64` 是一个普通字符串参数，而这两张立绘单张就有 2.7MB，
base64 之后约 3.6MB —— Windows 命令行上限 32767 字符，作为 argv 传必然失败。
所以这里把图片读成 base64 后**在进程内**改写 sys.argv 再调它的 main()，
绕开命令行长度限制；token 依旧走 stdin（不落 argv、不落进程列表）。

用法：
    echo -n "<token>" | python tools/gen3d.py <图片路径> <结果json输出路径> [额外参数...]
    echo -n "<token>" | python tools/gen3d.py --text <中文描述> <结果json输出路径> [额外参数...]

额外参数原样透传给 buddy-cloud.py，例如：
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
buddy-cloud.py 的 `3d` 子命令本来就接受 `prompt` 位置参数
（`if prompt: body["Prompt"] = prompt`），只是本文具此前只走图片那条。
"""

import base64
import importlib.util
import io
import os
import sys

# 本机 WorkBuddy Desktop 的解包目录。必须是 app.asar.unpacked，
# app.asar/resources/... 那条路径在安装包里不存在。
SCRIPT = ("C:/Program Files/WorkBuddy/resources/app.asar.unpacked/resources/"
          "plugins/workbuddy-builtin/skills/buddy-multimodal-generation/"
          "scripts/buddy-cloud.py")


def load_bc():
    spec = importlib.util.spec_from_file_location("buddy_cloud", SCRIPT)
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

    if not os.path.isfile(SCRIPT):
        print("找不到 buddy-cloud.py：%s" % SCRIPT)
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

    bc = load_bc()
    sys.argv = ["buddy-cloud.py", "3d"] + head + ["--token-stdin"] + extra
    sys.stdin = io.StringIO(token + "\n")

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
            print("buddy-cloud 退出码 %s" % e.code)
            with open(out_json, "w", encoding="utf-8") as f:
                f.write(buf.getvalue())
            return 1
    finally:
        sys.stdout = real_stdout

    text = buf.getvalue()
    with open(out_json, "w", encoding="utf-8") as f:
        f.write(text)
    print(text)
    print("[gen3d] 结果已存 %s" % out_json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
