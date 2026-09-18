# -*- coding: utf-8 -*-
"""VolunteerArmyPC —— 混元生3D（TokenHub 版）直连，图生3D

【为什么又加一个后端】前两条通道各有各的墙：
  · builtin（内置多模态通道）：**5 次提交 / 天，且同一天内不重置** —— 11 个角色要跨三天。
  · tc（腾讯云 CAM 签名 API）：需要 SecretId/SecretKey，还得在控制台开通「混元生3D」。
  · hy（本文件，TokenHub）：**只要一个 API Key**（`sk-` 开头，Bearer 认证），
    默认 3 并发、没有每日提交限制。它就是 `console.cloud.tencent.com/tokenhub/apikey`
    创建的那把 key，与老平台 `api.ai3d.cloud.tencent.com`（同样收 `sk-` 但走另一套
    域名与字段命名）**互不通用** —— 见 `selftest` 的判据。

【和内置通道的参数必须对齐 —— 差一个都会得到不同规格的模型】
    model          "hy-3d-3.1"   ← 内置通道 buddy-cloud.py 的 Model 默认就是 "3.1"
    generate_type  "Normal"      ← 带纹理的几何模型。**必须大驼峰**：
                                   传 "normal" 会被服务端拒（枚举值区分大小写），
                                   而这个接口的大小写是混着来的 ——
                                   别用"统一风格"去推，照文档抄。
    enable_pbr     true
    face_count     50000         ← **默认 500000**，不给就是 10 倍面数 / 15MB

【输入编码】裸 base64，**不加 `data:image/png;base64,` 前缀** —— 这是有依据的：
  · 官方文档里 `image_base64` 的示例值就是 `/9j/4QlQaHR0…`（裸 JPEG base64）
  · 内置通道（产出 char_rifleman 那次）经 buddy-cloud.py 传的也是裸 base64
  两边一致 → 输入处理与已接入模型对齐，不引入新的变量。

【输出格式】刻意与 buddy-cloud.py / gen3d_tc.py 的结果 JSON 保持一致
（`job_id` / `status` / `raw_result.Status` / `raw_result.ResultFile3Ds[].{Type,Url}`），
于是 tools/gen3d_batch.py 的解析、下载、瘦身、续跑判据一行都不用改。
注意状态词要**翻译**：TokenHub 说 `queued`/`in_progress`/`completed`/`failed`，
而下载链路认的是 `WAIT`/`RUN`/`DONE`/`FAIL`（源自老平台）。这里统一翻成后者。

【凭据】按优先级读取，**不接受命令行传参**（会落进进程列表与 shell 历史）：
    1. 环境变量 TOKENHUB_API_KEY
    2. 文件 ~/.workbuddy/va_3d_api_key.txt（单独一行）

用法：
    python tools/gen3d_hy.py selftest                       # 只验凭据，不消耗额度
    python tools/gen3d_hy.py submit <图> <结果json> [--face-count 50000] [--no-poll]
    python tools/gen3d_hy.py query <任务id> [--out <json>]
"""

import argparse
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request

BASE = "https://tokenhub.tencentmaas.com"
SUBMIT_PATH = "/v1/api/3d/submit"
QUERY_PATH = "/v1/api/3d/query"

# 官方枚举：hy-3d-3.0 / hy-3d-3.1。3.1 时 low_poly 不可用（我们用的是 normal，不受影响）。
# 与内置通道对齐用 3.1（buddy-cloud.py 的 default model 就是 "3.1"）。
MODEL = "hy-3d-3.1"

KEY_FILE = os.path.join(os.path.expanduser("~"), ".workbuddy", "va_3d_api_key.txt")

POLL_INTERVAL = 10          # 秒
POLL_TIMEOUT = 900          # 15 分钟；实测单个 3~5 分钟

# TokenHub 的状态词 → 下载链路认的状态词（后者源自老平台接口）
STATUS_MAP = {
    "queued": "WAIT",
    "in_progress": "RUN",
    "completed": "DONE",
    "failed": "FAIL",
}


class ApiError(Exception):
    """服务端明确返回的错误（区别于网络异常）。"""

    def __init__(self, code, message, request_id=""):
        super().__init__("%s: %s" % (code, message))
        self.code = code
        self.message = message
        self.request_id = request_id


# ------------------------------------------------------------------ 凭据
def load_key():
    k = os.environ.get("TOKENHUB_API_KEY", "").strip()
    if k:
        return k, "环境变量 TOKENHUB_API_KEY"
    if os.path.isfile(KEY_FILE):
        k = open(KEY_FILE, encoding="utf-8").read().strip()
        if k:
            return k, KEY_FILE
        raise SystemExit("凭据文件是空的：%s" % KEY_FILE)
    raise SystemExit(
        "没有找到 TokenHub API Key。二选一：\n"
        "  ① 设环境变量 TOKENHUB_API_KEY\n"
        "  ② 写文件 %s（单独一行，sk- 开头）\n"
        "在 https://console.cloud.tencent.com/tokenhub/apikey 创建。\n"
        "（密钥不落命令行、不进仓库）" % KEY_FILE)


def _opener():
    """显式禁用代理直连：本机 https_proxy 指向的出口对部分域名不通，
    而 TokenHub 直连实测可用，少一个环节就少一种失败方式。"""
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def call_api(path, payload, key, timeout=30, dump=None):
    """POST 一次，返回解析后的字典（服务端报 error 就抛 ApiError）。"""
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        BASE + path, data=body, method="POST",
        headers={
            "Authorization": "Bearer " + key,
            "Content-Type": "application/json",
        },
    )
    try:
        with _opener().open(req, timeout=timeout) as r:
            raw = r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:            # 4xx/5xx 也带着 JSON body
        raw = e.read().decode("utf-8", "replace")
    except Exception as e:
        raise RuntimeError("网络不可达（%s%s）：%r" % (BASE, path, e))

    if dump:
        with open(dump, "w", encoding="utf-8") as f:
            f.write(raw)
    try:
        d = json.loads(raw)
    except Exception:
        raise RuntimeError("返回不是 JSON：%s" % raw[:400])
    # 错误体形如 {"error":{"message":"…","type":"api_error","code":"FailedOperation.JobNotFound"}}
    if isinstance(d.get("error"), dict):
        err = d["error"]
        raise ApiError(err.get("code", "?"), err.get("message", ""), d.get("request_id", ""))
    if d.get("error"):
        raise ApiError("?", str(d["error"]), d.get("request_id", ""))
    return d


# ------------------------------------------------------------------ 结果翻译
def _to_compat(job_id, d):
    """把 TokenHub 的查询结果翻成下载链路认的那套字段。"""
    st = STATUS_MAP.get(str(d.get("status", "")).lower(), str(d.get("status", "")).upper())
    files = []
    for f in d.get("data") or []:
        files.append({
            "Type": str(f.get("type", "")).upper(),
            "Url": f.get("url", ""),
            "PreviewImageUrl": f.get("preview_image_url", ""),
        })
    err = d.get("error") or {}
    return {
        "job_id": job_id,
        "status": st,
        "provider": "tokenhub",
        "request_id": d.get("request_id", ""),
        "raw_result": {
            "Status": st,
            "ErrorCode": err.get("code", "") if isinstance(err, dict) else "",
            "ErrorMessage": err.get("message", "") if isinstance(err, dict) else str(err or ""),
            "ResultFile3Ds": files,
            "ResultCreditConsumed": d.get("credit_consumed"),
        },
    }


def _emit(d, out_path):
    txt = json.dumps(d, ensure_ascii=False, indent=2)
    if out_path:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(txt)
    print(txt)


# ------------------------------------------------------------------ 子命令
def cmd_selftest(args):
    """凭据自检：查一个不存在的任务 id。

    判据（不消耗额度 —— 生成都没开始）：
      · `FailedOperation.JobNotFound` / 其它业务报错 → **Key 有效并被接受** ✓
      · `invalid_api_key` / 401                         → Key 不对，或不是 TokenHub 的 Key ✗
    顺带排除一个高频误判：老平台 `api.ai3d.cloud.tencent.com` 也收 `sk-` 开头的 key，
    但**域名不同、key 不通用**，在那里查会直接回 invalid_api_key。
    """
    key, src = load_key()
    print("凭据来源：%s（尾部 %s，长度 %d）" % (src, key[-4:] if len(key) >= 4 else "?", len(key)))
    try:
        d = call_api(QUERY_PATH, {"model": MODEL, "id": "1"}, key, timeout=30)
        print("? 未预期的成功响应：%s" % json.dumps(d, ensure_ascii=False)[:300])
        return 1
    except ApiError as e:
        print("服务端错误码：%s\n  %s\n  RequestId=%s" % (e.code, e.message, e.request_id))
        low = (e.code + " " + e.message).lower()
        if "invalid_api_key" in low or "incorrect api key" in low:
            print("✗ Key 无效 —— 在 https://console.cloud.tencent.com/tokenhub/apikey 重新创建。")
            return 1
        if "jobnotfound" in low or "任务不存在" in e.message:
            print("✓ Key 有效且被接受（业务层报「任务不存在」是预期的，因为 id 是假的）。")
            return 0
        print("? 未归类的错误码，请人工判断（上面原文）。")
        return 1
    except RuntimeError as e:
        print("✗ %s" % e)
        return 1


def cmd_query(args):
    key, _ = load_key()
    d = call_api(QUERY_PATH, {"model": args.model, "id": args.job_id}, key)
    _emit(_to_compat(args.job_id, d), args.out)
    return 0


def cmd_submit(args):
    key, _ = load_key()
    img = args.image
    if not os.path.isfile(img):
        raise SystemExit("找不到输入图片：%s" % img)
    size = os.path.getsize(img)
    b64 = base64.b64encode(open(img, "rb").read()).decode("ascii")
    # 官方限制：单边 128~5000px，image_base64 ≤6MB（base64 编码后按 1.3 倍膨胀算）。
    if len(b64) > 8 * 1024 * 1024:
        raise SystemExit("图片 base64 后 %.1fMB，超过官方限制：%s"
                         % (len(b64) / 2 ** 20, img))

    payload = {
        "model": args.model,
        "image_base64": b64,                 # 裸 base64，无 data: 前缀
        # 【枚举值必须大驼峰】实测传 "normal" 会被拒：
        #   InvalidParameter：【GenerateType】仅支持Normal,LowPoly,Geometry,Sketch
        # 而且这个接口的大小写规则是**混着来的**，不能靠"统一风格"推：
        #   model        → "hy-3d-3.1"（小写）
        #   generate_type→ "Normal"（大驼峰）
        # 好在参数校验发生在计费之前，写错不扣额度、只多花一次往返。
        "generate_type": "Normal",
        "enable_pbr": True,
        "face_count": args.face_count,
    }
    print("提交中（输入 %.2f MB，face_count=%d，model=%s）…"
          % (size / 2 ** 20, args.face_count, args.model))
    d = call_api(SUBMIT_PATH, payload, key, timeout=120, dump=args.dump_submit)
    job_id = str(d.get("id", ""))
    if not job_id:
        raise SystemExit("提交成功但没拿到任务 id：%s" % json.dumps(d, ensure_ascii=False)[:300])
    print("任务 id = %s（status=%s）" % (job_id, d.get("status")))

    if args.no_poll:
        _emit(_to_compat(job_id, {"status": "queued"}), args.out)
        return 0

    t0 = time.time()
    delay = POLL_INTERVAL
    while True:
        time.sleep(delay)
        d = call_api(QUERY_PATH, {"model": args.model, "id": job_id}, key)
        st = str(d.get("status", ""))
        el = time.time() - t0
        print("  [%.0fs] %s" % (el, st))
        if st == "completed":
            _emit(_to_compat(job_id, d), args.out)
            print("生成完成，用时 %.0fs" % el)
            return 0
        if st == "failed":
            _emit(_to_compat(job_id, d), args.out)
            err = d.get("error") or {}
            raise SystemExit("任务失败：%s %s"
                             % (err.get("code", "?"), err.get("message", "")))
        if el > POLL_TIMEOUT:
            _emit(_to_compat(job_id, d), args.out)
            raise SystemExit("轮询超过 %ds 仍未完成（任务 id 仍有效，可用 query 子命令接着查）"
                             % POLL_TIMEOUT)
        delay = min(20, delay + 2)


def main():
    ap = argparse.ArgumentParser(description="混元生3D（TokenHub）直连，图生3D")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("selftest", help="验证 API Key（不消耗额度）")
    p.set_defaults(func=cmd_selftest)

    p = sub.add_parser("submit", help="提交图生3D 并轮询到完成")
    p.add_argument("image")
    p.add_argument("out", help="结果 JSON 输出路径（格式与 buddy-cloud.py 一致）")
    p.add_argument("--face-count", type=int, default=50000,
                   help="面数，默认 50000（服务端默认 500000，差 10 倍）")
    p.add_argument("--model", default=MODEL)
    p.add_argument("--no-poll", action="store_true", help="只提交，不等待")
    p.add_argument("--dump-submit", default=None, help="把提交响应原文另存一份便于排查")
    p.set_defaults(func=cmd_submit)

    p = sub.add_parser("query", help="查询已有任务")
    p.add_argument("job_id")
    p.add_argument("--model", default=MODEL)
    p.add_argument("--out", default=None)
    p.set_defaults(func=cmd_query)

    args = ap.parse_args()
    try:
        return args.func(args)
    except ApiError as e:
        print("服务端错误 %s：%s（RequestId=%s）" % (e.code, e.message, e.request_id))
        return 1
    except RuntimeError as e:
        print("失败：%s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
