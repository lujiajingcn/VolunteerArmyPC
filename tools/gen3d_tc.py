# -*- coding: utf-8 -*-
"""VolunteerArmyPC —— 腾讯云混元生3D 官方 API 直连（图生3D）

【为什么需要这个脚本】
内置多模态通道（buddy-cloud.py 的 `3d` 子命令）对 hy-3d 维度限
**5 次提交 / 天**，且同一天内不重置；11 个角色要跨三天才做得完。
而内置通道转发的**就是腾讯云混元生3D**，所以官方直连可以：
  · 拿到**同一个模型、同一套参数**的产出（与已接入的 char_rifleman 风格一致）
  · 没有"每日提交"限制（只有并发限制，官方默认 3 并发、每秒 20 请求）
  · 失败与风控拦截都不扣积分

【与内置通道的参数对齐 —— 差一个都会得到不同规格的模型】
    Model          "3.1"     ← buddy-cloud.py 的默认值就是 3.1；不显式给会退回 3.0
    GenerateType   Normal
    EnablePBR      true
    FaceCount      50000     ← **默认是 500000**，不给就是 10 倍面数 / 15MB（见 gen3d_batch.py 的教训）
    Region         ap-guangzhou
    Version        2025-05-13（接口版本，不是模型版本）

【输出格式】刻意与 buddy-cloud.py 的结果 JSON 保持一致
（`job_id` / `status` / `raw_result.Status` / `raw_result.ResultFile3Ds[].{Type,Url}`），
这样 `tools/gen3d_batch.py` 的结果解析、下载、瘦身链路**一行都不用改**。

【签名】TC3-HMAC-SHA256，纯 stdlib 手写（本机 pip 装 tencentcloud-sdk 受代理影响）。
不引入 SDK 还有一个好处：签名过程完全可见，出错时能一眼看到是哪一步。

【凭据】按优先级读取，**不接受命令行传参**（会落进进程列表与 shell 历史）：
    1. 环境变量 TENCENTCLOUD_SECRET_ID / TENCENTCLOUD_SECRET_KEY（可选 TENCENTCLOUD_TOKEN）
    2. 文件 ~/.workbuddy/tencentcloud.json  {"secret_id": "...", "secret_key": "..."}
另：requests 走系统代理会连不上腾讯云（本机 https_proxy 指向不通的出口），
所以这里**显式禁用代理**用 urllib 直连。

用法：
    python tools/gen3d_tc.py selftest                     # 只验证签名与凭据，不消耗积分
    python tools/gen3d_tc.py submit <图> <结果json> [--face-count 50000] [--no-poll]
    python tools/gen3d_tc.py query <job_id> [--out <json>]
"""

import argparse
import base64
import hashlib
import hmac
import json
import os
import sys
import time
import urllib.error
import urllib.request

HOST = "ai3d.tencentcloudapi.com"
SERVICE = "ai3d"
VERSION = "2025-05-13"      # 接口版本（公共参数 Version），与 Model 3.1 是两回事
REGION = "ap-guangzhou"
MODEL = "3.1"               # 与 buddy-cloud.py 的默认值对齐

SUBMIT_ACTION = "SubmitHunyuanTo3DProJob"
QUERY_ACTION = "QueryHunyuanTo3DProJob"

CRED_FILE = os.path.join(os.path.expanduser("~"), ".workbuddy", "tencentcloud.json")

POLL_INTERVAL = 10          # 秒；服务端异步生成，官方建议不要高频轮询
POLL_TIMEOUT = 900          # 15 分钟；实测单个 3~5 分钟


# ------------------------------------------------------------------ 凭据
def load_credentials():
    sid = os.environ.get("TENCENTCLOUD_SECRET_ID", "").strip()
    skey = os.environ.get("TENCENTCLOUD_SECRET_KEY", "").strip()
    token = os.environ.get("TENCENTCLOUD_TOKEN", "").strip()
    if sid and skey:
        return sid, skey, token
    if os.path.isfile(CRED_FILE):
        try:
            d = json.load(open(CRED_FILE, encoding="utf-8"))
        except Exception as e:
            raise SystemExit("凭据文件不可读：%s（%r）" % (CRED_FILE, e))
        sid = (d.get("secret_id") or "").strip()
        skey = (d.get("secret_key") or "").strip()
        token = (d.get("token") or "").strip()
        if sid and skey:
            return sid, skey, token
        raise SystemExit("凭据文件里缺 secret_id / secret_key：%s" % CRED_FILE)
    raise SystemExit(
        "没有找到凭据。二选一：\n"
        "  ① 设环境变量 TENCENTCLOUD_SECRET_ID / TENCENTCLOUD_SECRET_KEY\n"
        "  ② 写文件 %s：{\"secret_id\": \"...\", \"secret_key\": \"...\"}\n"
        "（密钥不落命令行、不进仓库）" % CRED_FILE)


# ------------------------------------------------------------------ 签名
def _hmac(key: bytes, msg: str) -> bytes:
    return hmac.new(key, msg.encode("utf-8"), hashlib.sha256).digest()


def tc3_headers(secret_id, secret_key, action, payload_bytes, token="", timestamp=None):
    """返回 (headers, ts)。

    TC3-HMAC-SHA256 四步：
      ① 规范请求串：method + uri + query + 规范头 + 签名头列表 + 请求体哈希
      ② 待签串：算法 + 时间戳 + 凭证范围 + ①的哈希
      ③ 派生密钥：TC3+SecretKey → 日期 → 服务 → tc3_request 逐级 HMAC
      ④ 签名 = HMAC(派生密钥, ②)

    两个最容易踩的点：
      · 规范头里的 host 必须与实际发的 Host 头一致；服务端会按 SignedHeaders
        声明的集合重算，声明的和发的对不上就是 SignatureFailure。
      · Date 要从时间戳按 **UTC** 换算，不能用本地日期（东八区晚 8 小时会跨天）。
    """
    ts = int(timestamp if timestamp is not None else time.time())
    date = time.strftime("%Y-%m-%d", time.gmtime(ts))     # 必须 UTC

    content_type = "application/json; charset=utf-8"
    hashed_payload = hashlib.sha256(payload_bytes).hexdigest()

    signed_headers = "content-type;host"
    canonical_headers = "content-type:%s\nhost:%s\n" % (content_type, HOST)
    canonical_request = "\n".join([
        "POST", "/", "", canonical_headers, signed_headers, hashed_payload,
    ])
    credential_scope = "%s/%s/tc3_request" % (date, SERVICE)
    string_to_sign = "\n".join([
        "TC3-HMAC-SHA256", str(ts), credential_scope,
        hashlib.sha256(canonical_request.encode("utf-8")).hexdigest(),
    ])

    secret_date = _hmac(("TC3" + secret_key).encode("utf-8"), date)
    secret_service = _hmac(secret_date, SERVICE)
    secret_signing = _hmac(secret_service, "tc3_request")
    signature = hmac.new(secret_signing, string_to_sign.encode("utf-8"),
                         hashlib.sha256).hexdigest()

    headers = {
        "Authorization": ("TC3-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s"
                          % (secret_id, credential_scope, signed_headers, signature)),
        "Content-Type": content_type,
        "Host": HOST,
        "X-TC-Action": action,
        "X-TC-Version": VERSION,
        "X-TC-Timestamp": str(ts),
        "X-TC-Region": REGION,
    }
    if token:
        headers["X-TC-Token"] = token
    return headers, ts


def _opener():
    """显式禁用代理：本机设了 https_proxy 而出口不通，直连才能到腾讯云。"""
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def call_api(action, payload, secret_id, secret_key, token="", timeout=30, dump=None):
    """调用一次 API，返回 Response 字典（已把 Error 提成异常）。"""
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    headers, _ = tc3_headers(secret_id, secret_key, action, body, token)
    req = urllib.request.Request("https://" + HOST, data=body, headers=headers, method="POST")
    try:
        with _opener().open(req, timeout=timeout) as r:
            raw = r.read().decode("utf-8")
    except urllib.error.HTTPError as e:            # 4xx/5xx 也带着 JSON body
        raw = e.read().decode("utf-8", "replace")
    except Exception as e:
        raise RuntimeError("网络不可达（%s）：%r" % (HOST, e))

    if dump:
        with open(dump, "w", encoding="utf-8") as f:
            f.write(raw)
    try:
        d = json.loads(raw)
    except Exception:
        raise RuntimeError("返回不是 JSON：%s" % raw[:400])
    resp = d.get("Response", {})
    if "Error" in resp:
        err = resp["Error"]
        raise ApiError(err.get("Code", "?"), err.get("Message", ""), resp.get("RequestId", ""))
    return resp


class ApiError(Exception):
    """服务端明确返回的错误（区别于网络异常）。"""

    def __init__(self, code, message, request_id=""):
        super().__init__("%s: %s" % (code, message))
        self.code = code
        self.message = message
        self.request_id = request_id


# ------------------------------------------------------------------ 子命令
def cmd_selftest(args):
    """签名自检：用一个不存在的 JobId 查询。

    判据（不需要真业务权限，也不消耗积分）：
      · 返回 InvalidParameter / ResourceNotFound 之类 → **签名与凭据都被接受** ✓
      · AuthFailure.SignatureFailure / SignatureExpire → 签名算法或时间有问题 ✗
      · AuthFailure.SecretIdNotFound / InvalidSecretId → 凭据本身不对 ✗
      · AuthFailure.UnauthorizedOperation → 凭据对但没开通服务/没授权该接口
    """
    sid, skey, token = load_credentials()
    print("凭据来源：%s（SecretId 尾部 %s）"
          % ("环境变量" if os.environ.get("TENCENTCLOUD_SECRET_ID") else CRED_FILE,
             sid[-4:] if len(sid) >= 4 else "?"))
    try:
        resp = call_api(QUERY_ACTION, {"JobId": "0"}, sid, skey, token)
        print("✓ 签名被接受，服务端返回：%s" % json.dumps(resp, ensure_ascii=False)[:300])
        return 0
    except ApiError as e:
        print("服务端错误码：%s\n  %s\n  RequestId=%s" % (e.code, e.message, e.request_id))
        if e.code.startswith("AuthFailure.SignatureFailure") or \
           e.code.startswith("AuthFailure.SignatureExpire"):
            print("✗ 签名没通过 —— 检查 SecretKey 是否完整（含不可见字符）、"
                  "系统时间是否与标准时间相差超过 5 分钟。")
            return 1
        if e.code.startswith("AuthFailure.SecretIdNotFound") or \
           e.code.startswith("AuthFailure.InvalidSecretId"):
            print("✗ SecretId 不存在或无效 —— 确认密钥来自已开通混元生3D 的账号。")
            return 1
        if e.code.startswith("AuthFailure.UnauthorizedOperation"):
            print("✗ 凭据有效但没有该接口权限 —— 子账号需授予 QcloudAI3DFullAccess。")
            return 1
        if e.code.startswith("InvalidParameter") or e.code.startswith("ResourceNotFound"):
            print("✓ 签名与凭据都被接受（业务层参数报错是预期的，因为 JobId 是假的）。")
            return 0
        print("? 未归类的错误码，请人工判断（上面原文）。")
        return 1
    except RuntimeError as e:
        print("✗ %s" % e)
        return 1


def cmd_query(args):
    sid, skey, token = load_credentials()
    resp = call_api(QUERY_ACTION, {"JobId": args.job_id}, sid, skey, token)
    _emit_query(resp, args.out)
    return 0


def _emit_query(resp, out_path):
    """把 Query 结果写成与 buddy-cloud.py 一致的 JSON，供 gen3d_batch.py 复用。"""
    d = {
        "job_id": resp.get("JobId", ""),
        "status": resp.get("Status", ""),
        "raw_result": {
            "Status": resp.get("Status", ""),
            "ErrorCode": resp.get("ErrorCode", ""),
            "ErrorMessage": resp.get("ErrorMessage", ""),
            "ResultFile3Ds": resp.get("ResultFile3Ds", []) or [],
            "ResultCreditConsumed": resp.get("ResultCreditConsumed"),
        },
    }
    txt = json.dumps(d, ensure_ascii=False, indent=2)
    if out_path:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(txt)
    print(txt)


def cmd_submit(args):
    sid, skey, token = load_credentials()
    img = args.image
    if not os.path.isfile(img):
        raise SystemExit("找不到输入图片：%s" % img)
    size = os.path.getsize(img)
    b64 = base64.b64encode(open(img, "rb").read()).decode("ascii")
    # 官方限制：单边 128~5000px、base64 后不超过约 8MB。
    if len(b64) > 7 * 1024 * 1024:
        raise SystemExit("图片 base64 后 %.1fMB，超过官方 8MB 上限：%s"
                         % (len(b64) / 2 ** 20, img))

    payload = {
        "Model": MODEL,
        "ImageBase64": b64,
        "GenerateType": "Normal",
        "EnablePBR": True,
        "FaceCount": args.face_count,
    }
    print("提交中（输入 %.2f MB，FaceCount=%d，Model=%s）…" % (size / 2 ** 20, args.face_count, MODEL))
    resp = call_api(SUBMIT_ACTION, payload, sid, skey, token, timeout=60, dump=args.dump_submit)
    job_id = resp.get("JobId", "")
    if not job_id:
        raise SystemExit("提交成功但没拿到 JobId：%s" % json.dumps(resp, ensure_ascii=False)[:300])
    print("JobId = %s" % job_id)

    if args.no_poll:
        _emit_query({"JobId": job_id, "Status": "WAIT"}, args.out)
        return 0

    t0 = time.time()
    delay = POLL_INTERVAL
    while True:
        time.sleep(delay)
        resp = call_api(QUERY_ACTION, {"JobId": job_id}, sid, skey, token)
        st = resp.get("Status", "")
        el = time.time() - t0
        print("  [%.0fs] %s" % (el, st))
        if st == "DONE":
            _emit_query(resp, args.out)
            print("生成完成，用时 %.0fs，积分 %s"
                  % (el, resp.get("ResultCreditConsumed")))
            return 0
        if st == "FAIL":
            _emit_query(resp, args.out)
            raise SystemExit("任务失败：%s %s（失败不扣积分）"
                             % (resp.get("ErrorCode"), resp.get("ErrorMessage")))
        if el > POLL_TIMEOUT:
            _emit_query(resp, args.out)
            raise SystemExit("轮询超过 %ds 仍未完成（JobId 24 小时内有效，可用 query 子命令接着查）"
                             % POLL_TIMEOUT)
        delay = min(20, delay + 2)


def main():
    ap = argparse.ArgumentParser(description="腾讯云混元生3D 官方 API 直连（图生3D）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("selftest", help="验证签名与凭据（不消耗积分）")
    p.set_defaults(func=cmd_selftest)

    p = sub.add_parser("submit", help="提交图生3D 并轮询到完成")
    p.add_argument("image")
    p.add_argument("out", help="结果 JSON 输出路径（格式与 buddy-cloud.py 一致）")
    p.add_argument("--face-count", type=int, default=50000,
                   help="面数，默认 50000（官方默认 500000，注意差 10 倍）")
    p.add_argument("--no-poll", action="store_true", help="只提交，不等待")
    p.add_argument("--dump-submit", default=None, help="把提交响应原文另存一份便于排查")
    p.set_defaults(func=cmd_submit)

    p = sub.add_parser("query", help="查询已有任务")
    p.add_argument("job_id")
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
