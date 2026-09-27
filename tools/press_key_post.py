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
      python tools/press_key_post.py Q "VolunteerArmyPC" 6      # 按住 6 秒
      python tools/press_key_post.py Q "" 6 --pid 8116          # 按 PID 精准投递

【第 3 个参数：按住多久（秒）】默认 0.06（"点一下"）。语音输入是**按住说话**，
0.06 秒只说得出一个字的一半，所以那类验证必须显式给时长。
⚠️ 这里按住时**不会**产生系统按键重复（PostMessage 不生成）——
本工程恰好需要这样：Q 是 is_hold_key，靠 keydown/keyup 两个状态即可，
不需要 echo 重建。若要验"按住类手感"，仍得走 inject_key.py。

【--pid：为什么非要有】标题是**猜**的，PID 是**准**的。两种场合标题法必错：
  · 同一份工程开着两个实例（比如 VS 里 F5 调着一份，脚本又起一份做取证）——
    标题一模一样，`find_window` 只会挑"最短的那个"，投给谁全看 EnumWindows 的
    遍历顺序，实测会**投进正在调试的那个窗口**（把人家的会话搅了）；
  · 标题里带 (DEBUG) 的窗口同时属于 VS 自己（VS 的标题也含工程名）。
给了 --pid 就完全绕开标题：只枚举属于该 PID 的可见窗口，没有就安全中止。
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101

VK = {
    "R": 0x52, "ESC": 0x1B, "ENTER": 0x0D, "V": 0x56, "B": 0x42, "Q": 0x51,
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


def find_window_by_pid(pid):
    """只认属于 pid 的**可见且带标题**的顶层窗口 —— 多实例并存时唯一可靠的办法。"""
    hits = []

    def cb(hwnd, _):
        wpid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value != pid or not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if n > 0:
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            hits.append((hwnd, buf.value))
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    if not hits:
        return None, None
    hits.sort(key=lambda t: len(t[1]))
    return hits[0]


def main():
    argv = sys.argv[1:]
    pid = None
    if "--pid" in argv:
        i = argv.index("--pid")
        try:
            pid = int(argv[i + 1])
        except (IndexError, ValueError):
            print("[press] --pid 后面要跟一个整数")
            return 2
        del argv[i:i + 2]

    key = (argv[0] if len(argv) > 0 else "R").upper()
    title = argv[1] if len(argv) > 1 else "VolunteerArmyPC"
    hold = float(argv[2]) if len(argv) > 2 else 0.06
    if key not in VK:
        print("[press] 未知键:", key)
        return 2
    if pid is not None:
        hwnd, found = find_window_by_pid(pid)
        if hwnd is None:
            print("[press] pid %d 没有可见的带标题窗口（进程起了吗？）" % pid)
            return 1
    else:
        hwnd, found = find_window(title)
        if hwnd is None:
            print("[press] 找不到标题含 %r 的窗口" % title)
            return 1
    vk = VK[key]
    # lParam 让 Godot 能算出"是不是系统重复"：bit30(0x40000000)=上一次键态。
    # 这里按下/抬起各一次，bit30 都是 0 → 不会被标成 echo。
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, 0x00100001)
    time.sleep(hold)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, 0xC0100001)
    print("[press] 已向 pid=%s 窗口 %r (hwnd=0x%08X) 投递 %s（按住 %.2fs）"
          % (pid if pid is not None else "(按标题)", found, hwnd, key, hold))
    return 0


if __name__ == "__main__":
    sys.exit(main())
