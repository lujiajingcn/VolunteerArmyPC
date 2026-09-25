# -*- coding: utf-8 -*-
"""往指定标题的窗口**投递**一次按键（PostMessage，不需要抢前台）。

为什么不用 tools/inject_key.py：那个用的是 SendInput，语义最真（连"按住"时
系统发的按键重复都能复现），代价是**必须目标窗口在前台** —— 它自己也会在
抢不到前台时安全中止。本轮验证"结算面板按 R 转进下一阵地"时正好卡在这里：
游戏窗口找得到、前台抢不到（本机跑脚本的进程没有前台权限），
于是 32 条事件一条都发不出去。

这里只按**一下**功能键，不需要重复语义，所以可以退到 PostMessage：
消息直接进目标窗口的消息队列，Godot 的窗口过程照常处理，
与"谁在前台"无关。代价是它**不产生**系统按键重复，
所以这个脚本不能用来验"按住"类手感问题（那类问题仍然必须走 inject_key.py）。

用法: python tools/press_key_post.py R "VolunteerArmyPC (DEBUG)"
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101

VK = {
    "R": 0x52, "ESC": 0x1B, "ENTER": 0x0D, "V": 0x56,
    "W": 0x57, "A": 0x41, "S": 0x53, "D": 0x44, "G": 0x47, "F": 0x46, "Z": 0x5A,
    # 数字键：伤亡过半那条选择条是 1 撤 / 2 守（F=烟雾、G=手雷已占，只能用数字键）
    "1": 0x31, "2": 0x32, "3": 0x33, "4": 0x34,
}

WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def find_window(substr):
    hits = []

    def cb(hwnd, _):
        n = user32.GetWindowTextLengthW(hwnd)
        if n > 0:
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            if substr in buf.value:
                hits.append((hwnd, buf.value))
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    if not hits:
        return None, None
    # 子串可能命中多个（本机实测：Visual Studio 的标题里也含工程名）→ 取最短的那个
    hits.sort(key=lambda t: len(t[1]))
    return hits[0]


def main():
    key = (sys.argv[1] if len(sys.argv) > 1 else "R").upper()
    title = sys.argv[2] if len(sys.argv) > 2 else "VolunteerArmyPC"
    if key not in VK:
        print("[press] 未知键:", key)
        return 2
    hwnd, found = find_window(title)
    if hwnd is None:
        print("[press] 找不到标题含 %r 的窗口" % title)
        return 1
    vk = VK[key]
    # lParam 让 Godot 能算出"是不是系统重复"：bit30(0x40000000)=上一次键态。
    # 这里按下/抬起各一次，bit30 都是 0 → 不会被标成 echo。
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, 0x00100001)
    time.sleep(0.06)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, 0xC0100001)
    print("[press] 已向 pid 窗口 %r (hwnd=0x%08X) 投递 %s" % (found, hwnd, key))
    return 0


if __name__ == "__main__":
    sys.exit(main())
