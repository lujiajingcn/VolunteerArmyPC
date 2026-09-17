# -*- coding: utf-8 -*-
"""调用内置多模态 3D 生成能力（图生3D），把 base64 从文件读入。

【为什么不直接命令行调 buddy-cloud.py】
它的 `--image-base64` 是一个普通字符串参数，而这两张立绘单张就有 2.7MB，
base64 之后约 3.6MB —— Windows 命令行上限 32767 字符，作为 argv 传必然失败。
所以这里把图片读成 base64 后**在进程内**改写 sys.argv 再调它的 main()，
绕开命令行长度限制；token 依旧走 stdin（不落 argv、不落进程列表）。

用法：
    echo -n "<token>" | python tools/gen3d.py <图片路径> <结果json输出路径> [额外参数...]

额外参数原样透传给 buddy-cloud.py，例如：
    --no-poll          只提交不等待
    --enable-pbr       生成 PBR 材质
    --face-count 50000 面数上限
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
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    img_path = sys.argv[1]
    out_json = sys.argv[2]
    extra = sys.argv[3:]

    if not os.path.isfile(SCRIPT):
        print("找不到 buddy-cloud.py：%s" % SCRIPT)
        return 2
    if not os.path.isfile(img_path):
        print("找不到输入图片：%s" % img_path)
        return 2

    token = sys.stdin.readline().strip()
    if not token:
        print("stdin 没有拿到 token")
        return 2

    with open(img_path, "rb") as f:
        b64 = base64.b64encode(f.read()).decode()

    bc = load_bc()
    sys.argv = ["buddy-cloud.py", "3d", "--image-base64", b64,
                "--token-stdin"] + extra
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
