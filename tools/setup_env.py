# -*- coding: utf-8 -*-
"""
VolunteerArmyPC —— 环境准备
1. 建目录骨架
2. 下载 Godot 4.5-stable Windows x64 编辑器并解压
3. git clone godot-cpp @ godot-4.5-stable
"""
import os, sys, json, zipfile, ssl, shutil, time
import urllib.request

ROOT = r'E:\study_lujiajing\Games\WorkSpace_VolunteerArmy\VolunteerArmyPC'
SDK = os.path.join(ROOT, 'sdk')
EXT = os.path.join(ROOT, 'ext')
CACHE = os.path.join(ROOT, 'sdk', '_cache')

EDITOR_URL = 'https://github.com/godotengine/godot/releases/download/4.5-stable/Godot_v4.5-stable_win64.exe.zip'
CPP_REPO = 'https://github.com/godotengine/godot-cpp'
CPP_TAG = 'godot-4.5-stable'

def log(*a):
    print(*a)
    sys.stdout.flush()

def mkdirs():
    log('=== 1. 建目录骨架 ===')
    for d in [ROOT, SDK, EXT, CACHE,
              os.path.join(ROOT, 'bin'),
              os.path.join(ROOT, 'src'),
              os.path.join(ROOT, 'scenes'),
              os.path.join(ROOT, 'scripts'),
              os.path.join(ROOT, 'tools'),
              os.path.join(ROOT, 'assets')]:
        os.makedirs(d, exist_ok=True)
        log('   ok  ' + d)

def download(url, dst):
    """带断点复用的下载；已存在且大小合理则跳过"""
    if os.path.exists(dst) and os.path.getsize(dst) > 1024 * 1024:
        log('   已存在，跳过下载: %s (%.1f MB)' % (os.path.basename(dst), os.path.getsize(dst) / 2**20))
        return dst
    log('   下载 %s' % url)
    ctx = ssl.create_default_context()
    req = urllib.request.Request(url, headers={'User-Agent': 'VolunteerArmyPC-setup'})
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=60, context=ctx) as r, open(dst, 'wb') as f:
        total = int(r.headers.get('Content-Length') or 0)
        got = 0
        last = 0
        while True:
            chunk = r.read(256 * 1024)
            if not chunk:
                break
            f.write(chunk)
            got += len(chunk)
            if got - last > 8 * 2**20 or (total and got == total):
                last = got
                pct = (got * 100.0 / total) if total else 0
                log('      %6.1f / %.1f MB  (%.0f%%)  %.1f s' % (got / 2**20, total / 2**20, pct, time.time() - t0))
    log('   完成 %.1f MB，用时 %.1f s' % (os.path.getsize(dst) / 2**20, time.time() - t0))
    return dst

def step2_editor():
    log()
    log('=== 2. Godot 4.5-stable 编辑器 ===')
    zp = os.path.join(CACHE, 'Godot_v4.5-stable_win64.exe.zip')
    download(EDITOR_URL, zp)
    exe_dst = os.path.join(SDK, 'godot', 'Godot_v4.5-stable_win64.exe')
    os.makedirs(os.path.dirname(exe_dst), exist_ok=True)
    if os.path.exists(exe_dst) and os.path.getsize(exe_dst) > 10 * 2**20:
        log('   已解压，跳过: %s (%.1f MB)' % (exe_dst, os.path.getsize(exe_dst) / 2**20))
        return exe_dst
    log('   解压...')
    with zipfile.ZipFile(zp) as z:
        names = z.namelist()
        log('   压缩包内容: %s' % names)
        member = [n for n in names if n.lower().endswith('.exe')]
        if not member:
            raise RuntimeError('压缩包里没有 .exe')
        with z.open(member[0]) as src, open(exe_dst, 'wb') as dst:
            shutil.copyfileobj(src, dst, 1024 * 1024)
    log('   解压完成: %s (%.1f MB)' % (exe_dst, os.path.getsize(exe_dst) / 2**20))
    return exe_dst

def step3_godotcpp():
    log()
    log('=== 3. godot-cpp @ %s ===' % CPP_TAG)
    dst = os.path.join(EXT, 'godot-cpp')
    if os.path.isdir(os.path.join(dst, '.git')):
        log('   已存在: %s' % dst)
        return dst
    log('   需要 git clone —— 见下一步（由 shell 执行）')
    return dst

if __name__ == '__main__':
    mkdirs()
    step2_editor()
    log()
    log('环境准备（下载部分）完成。')
