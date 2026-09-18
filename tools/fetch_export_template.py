# -*- coding: utf-8 -*-
"""只取 Godot 导出模板里你真正需要的那一个条目。

问题
----
Godot 4.5 的导出模板包 `Godot_v4.5-stable_export_templates.tpz` 有 **1294 MB**，
里面 android / ios / linux / macos / web / visionos 全都有，而导出 Windows 版
只需要其中一个 `templates/windows_release_x86_64.exe`（压缩 33.8 MB，占 2.6%）。

中国大陆网络下 GitHub release 资产（走 objects.githubusercontent.com）常常
直接不通：本机实测 `curl -L` 连续 45 秒零字节。第三方加速站能通，但
① 只有 1~2.5 MB/s，整包要 20 分钟以上；② 连接不稳，断了没有断点续传
（加速站大多不支持 Range）就得从头再来。

做法
----
ZIP 的中央目录（Central Directory）在文件**末尾**，记录着每个条目的
压缩大小与数据偏移。于是：

    拉尾部 512 KB → 解析中央目录 → 定位目标条目 → 只拉它的数据段 → inflate

下载量从 1294 MB 降到 33.8 MB。

实测踩到的三个坑（都已在代码里规避）
------------------------------------
1. **下载必须用 curl，不能用 Python 的 urllib。** 同一个偏移、同一个镜像，
   curl 稳定返回 `206` + 精确字节数；urllib 却会把整个 1.29 GB 响应读进内存
   （进程内存涨到 1.7 GB 且永不返回）。原因未深究 —— 不缠斗，小段用 urllib
   （中央目录、本地头），大段一律交给 curl。
2. **加速站要横向测速，且行为不一致。** 实测 `gh.xxooo.cf` 2.5 MB/s、
   `gitproxy.mrhjx.cn` 1.8 MB/s，而 `ghproxy.net` 88 KB/s；
   `gh-proxy.com` 更干脆 —— 它**忽略 Range**，任何分段请求都回 200 + 全量。
3. **模板目录名是 `4.5.stable`，不是 `4.5-stable`。** 以 tpz 里的
   `templates/version.txt` 内容为准。

用法
----
    # 只列远端条目（约 1 秒，拉尾部 512 KB）
    python tools/fetch_export_template.py --list

    # 下载并安装 Windows Release 模板到 %APPDATA%\\Godot\\export_templates\\4.5.stable\\
    python tools/fetch_export_template.py

    # 指定条目与输出文件
    python tools/fetch_export_template.py \\
        --entry templates/windows_release_x86_64_console.exe \\
        --out C:/somewhere/win_console.exe

注意：`--out` 请用 **Windows 原生路径**。本脚本是原生 Windows Python，
不认 MSYS 的 `/c/...` —— 那会被当成"当前盘符根目录下的 c 目录"静默写歪。
"""
import argparse
import os
import subprocess
import sys
import tempfile
import urllib.request
import zlib

RAW_URL = ("https://github.com/godotengine/godot/releases/download/"
           "4.5-stable/Godot_v4.5-stable_export_templates.tpz")

# 加速前缀（按测速结果排序）。第一个不通就换下一个。
MIRRORS = [
    "https://gh.xxooo.cf/",
    "https://gitproxy.mrhjx.cn/",
    "https://gh.ddlc.top/",
    "https://ghproxy.net/",
]

TAIL_BYTES = 512 * 1024

# urllib 在 Windows 上默认读注册表的 WinInet 代理设置，本机那套对长连接不友好
_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def _small(url, a, b):
    """读一小段（≤ 1 MB）。不大段用它是为了绕开 urllib 吞全量的行为。"""
    req = urllib.request.Request(url, headers={"Range": "bytes=%d-%d" % (a, b)})
    with _OPENER.open(req, timeout=90) as r:
        if getattr(r, "status", 0) != 206:
            raise RuntimeError("%s 不认 Range（http=%s）" % (url[:40], getattr(r, "status", "?")))
        return r.read(b - a + 1)


def pick_mirror():
    """返回第一个对 bytes=0-0 返回 206 的镜像 URL。"""
    errs = []
    for p in MIRRORS:
        u = p + RAW_URL
        try:
            req = urllib.request.Request(u, headers={"Range": "bytes=0-0"})
            with _OPENER.open(req, timeout=45) as r:
                if getattr(r, "status", 0) == 206:
                    total = int(r.headers["Content-Range"].rsplit("/", 1)[1])
                    r.read(8)
                    print("[镜像] %s" % p, file=sys.stderr)
                    return u, total
                errs.append("%s http=%s" % (p, getattr(r, "status", "?")))
        except Exception as e:                                   # noqa: BLE001
            errs.append("%s %s" % (p, type(e).__name__))
    raise SystemExit("所有镜像都不可用：\n  " + "\n  ".join(errs))


def _u16(b, o):
    return int.from_bytes(b[o:o + 2], "little")


def _u32(b, o):
    return int.from_bytes(b[o:o + 4], "little")


def read_cd(url, total):
    """解析中央目录，返回 [(name, method, csize, usize, local_offset)]。"""
    tail_start = max(0, total - TAIL_BYTES)
    tail = _small(url, tail_start, total - 1)
    eocd = tail.rfind(b"PK\x05\x06")
    if eocd < 0:
        raise SystemExit("尾部找不到 EOCD")
    n_ent = _u16(tail, eocd + 10)
    cd_size = _u32(tail, eocd + 12)
    cd_off = _u32(tail, eocd + 16)

    if cd_off == 0xFFFFFFFF or cd_size == 0xFFFFFFFF or n_ent == 0xFFFF:
        z64 = tail.rfind(b"PK\x06\x06")
        if z64 < 0:
            raise SystemExit("需要 ZIP64 但找不到 ZIP64 EOCD")
        n_ent = int.from_bytes(tail[z64 + 32:z64 + 40], "little")
        cd_size = int.from_bytes(tail[z64 + 40:z64 + 48], "little")
        cd_off = int.from_bytes(tail[z64 + 48:z64 + 56], "little")

    if cd_off >= tail_start:
        cd = tail[cd_off - tail_start: cd_off - tail_start + cd_size]
    else:
        cd = _small(url, cd_off, cd_off + cd_size - 1)

    out, p = [], 0
    while p + 46 <= len(cd) and cd[p:p + 4] == b"PK\x01\x02":
        method = _u16(cd, p + 10)
        csize = _u32(cd, p + 20)
        usize = _u32(cd, p + 24)
        fnlen = _u16(cd, p + 28)
        exlen = _u16(cd, p + 30)
        cmtlen = _u16(cd, p + 32)
        lho = _u32(cd, p + 42)
        name = cd[p + 46: p + 46 + fnlen].decode("utf-8", "replace")

        if usize == 0xFFFFFFFF or csize == 0xFFFFFFFF or lho == 0xFFFFFFFF:
            q, end = p + 46 + fnlen, p + 46 + fnlen + exlen
            while q + 4 <= end:
                hid, hsz = _u16(cd, q), _u16(cd, q + 2)
                if hid == 0x0001:
                    v = cd[q + 4: q + 4 + hsz]
                    o = 0
                    if usize == 0xFFFFFFFF:
                        usize = int.from_bytes(v[o:o + 8], "little"); o += 8
                    if csize == 0xFFFFFFFF:
                        csize = int.from_bytes(v[o:o + 8], "little"); o += 8
                    if lho == 0xFFFFFFFF:
                        lho = int.from_bytes(v[o:o + 8], "little"); o += 8
                    break
                q += 4 + hsz
        out.append((name, method, csize, usize, lho))
        p += 46 + fnlen + exlen + cmtlen
    return out


def curl_range(url, a, b, out_path, timeout=1200):
    """用 curl 拉 [a,b] 到文件。curl 对 Range 的行为比 urllib 可靠得多。"""
    cmd = ["curl", "-L", "--fail", "--retry", "5", "--retry-delay", "3",
           "--retry-all-errors", "--max-time", str(timeout),
           "-r", "%d-%d" % (a, b), "-o", out_path, url]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("curl 失败（%d）：%s" % (r.returncode, (r.stderr or "")[-400:]))


def fetch(entry, out_path):
    url, total = pick_mirror()
    entries = read_cd(url, total)
    hit = [e for e in entries if e[0] == entry]
    if not hit:
        raise SystemExit("远端没有条目 %r" % entry)
    _, method, csize, usize, lho = hit[0]

    hdr = _small(url, lho, lho + 29)
    if hdr[:4] != b"PK\x03\x04":
        raise SystemExit("本地头签名不对")
    start = lho + 30 + _u16(hdr, 26) + _u16(hdr, 28)
    print("[取件] %s  压缩 %d → 原始 %d" % (entry, csize, usize), flush=True)

    tmp = os.path.join(tempfile.gettempdir(), "tpl_%d.deflate" % os.getpid())
    try:
        curl_range(url, start, start + csize - 1, tmp)
        got = os.path.getsize(tmp)
        if got != csize:
            raise SystemExit("下载长度不对：%d != %d" % (got, csize))
        raw = open(tmp, "rb").read()
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)

    data = raw if method == 0 else zlib.decompress(raw, -15)
    if len(data) != usize:
        raise SystemExit("解压长度不符：%d != %d" % (len(data), usize))

    d = os.path.dirname(out_path)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(data)
    print("[完成] %s  %d 字节" % (out_path, len(data)), flush=True)


def default_dest(entry):
    """把条目落到 Godot 的模板目录（版本号取自 version.txt 的约定名）。"""
    base = os.path.join(os.environ.get("APPDATA", "."), "Godot", "export_templates")
    return os.path.join(base, "4.5.stable", os.path.basename(entry))


def main():
    ap = argparse.ArgumentParser(description="按需取 Godot 导出模板条目")
    ap.add_argument("--list", action="store_true", help="只列远端条目")
    ap.add_argument("--entry", default="templates/windows_release_x86_64.exe")
    ap.add_argument("--out", default=None, help="输出文件（Windows 原生路径）")
    a = ap.parse_args()

    if a.list:
        url, total = pick_mirror()
        print("总大小 %d 字节（%.1f MB）" % (total, total / 1048576))
        for nm, method, csize, usize, lho in read_cd(url, total):
            print("  %-46s comp=%-11d raw=%d" % (nm, csize, usize))
        return

    fetch(a.entry, a.out or default_dest(a.entry))


if __name__ == "__main__":
    main()
