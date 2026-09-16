# -*- coding: utf-8 -*-
"""
稳健下载器：Range 断点续传 + 重试退避。
本机实测：GitHub API 小请求通，release 大文件（走 objects.githubusercontent.com）会中途断开，
所以必须支持"断了从断点接着下"，而不是从头再来。
"""
import os, sys, ssl, time, urllib.request, urllib.error

def log(*a):
    print(*a)
    sys.stdout.flush()

def remote_size(url):
    ctx = ssl.create_default_context()
    req = urllib.request.Request(url, method='HEAD', headers={'User-Agent': 'dl'})
    with urllib.request.urlopen(req, timeout=30, context=ctx) as r:
        return int(r.headers.get('Content-Length') or 0), r.geturl()

def fetch(url, dst, tries=40, chunk=128 * 1024, idle_timeout=25):
    """把 url 下到 dst；支持续传。返回最终大小。"""
    part = dst + '.part'
    if os.path.exists(dst) and os.path.getsize(dst) > 0:
        log('   目标已存在: %s (%.1f MB)' % (dst, os.path.getsize(dst) / 2**20))
        return os.path.getsize(dst)

    total, final_url = 0, url
    # 先探知远端大小，好判断「没有 .part 但 dst 存在」的残留是不是上次的半成品
    if os.path.exists(dst):
        log('   dst 存在但走续传路径，将其并入 .part')
        if not os.path.exists(part):
            os.replace(dst, part)
    try:
        total, final_url = remote_size(url)
        log('   远端大小 %.1f MB' % (total / 2**20))
        log('   真实地址 %s' % final_url[:110])
    except Exception as e:
        log('   HEAD 失败(继续尝试 GET): %r' % e)

    ctx = ssl.create_default_context()
    t0 = time.time()
    stall = 0
    for attempt in range(1, tries + 1):
        have = os.path.getsize(part) if os.path.exists(part) else 0
        if total and have >= total:
            break
        hdr = {'User-Agent': 'dl', 'Accept-Encoding': 'identity'}
        if have:
            hdr['Range'] = 'bytes=%d-' % have
        try:
            req = urllib.request.Request(url, headers=hdr)
            with urllib.request.urlopen(req, timeout=idle_timeout, context=ctx) as r:
                code = r.status
                if have and code != 206:
                    log('   服务器不支持续传(HTTP %d)，从头开始' % code)
                    have = 0
                    os.remove(part) if os.path.exists(part) else None
                mode = 'ab' if have else 'wb'
                with open(part, mode) as f:
                    while True:
                        c = r.read(chunk)
                        if not c:
                            break
                        f.write(c)
                        have += len(c)
                        if have % (8 * 2**20) < chunk:
                            pct = (have * 100.0 / total) if total else 0
                            log('      %7.1f MB  %5.1f%%  %.0fs' % (have / 2**20, pct, time.time() - t0))
        except Exception as e:
            stall += 1
            have = os.path.getsize(part) if os.path.exists(part) else 0
            wait = min(12, 1 + stall)
            log('   [第 %d 次中断] %s  已下 %.1f MB，%ds 后重试'
                % (attempt, type(e).__name__, have / 2**20, wait))
            time.sleep(wait)
            continue
        # 干净结束
        if total and have < total:
            log('   连接正常关闭但未下完 (%d/%d)，重试' % (have, total))
            time.sleep(2)
            continue
        break

    if not os.path.exists(part):
        raise RuntimeError('下载失败：没有产出文件')
    sz = os.path.getsize(part)
    if total and sz != total:
        raise RuntimeError('下载不完整: %d / %d 字节' % (sz, total))
    os.replace(part, dst)
    log('   完成 %s  %.1f MB  %.0f s' % (dst, sz / 2**20, time.time() - t0))
    return sz
