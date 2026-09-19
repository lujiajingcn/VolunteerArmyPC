# -*- coding: utf-8 -*-
"""
VolunteerArmyPC —— 把一段"人按住方向键"的按键流打进前台窗口

用途：输入类手感问题（按住该不该一直走、松开该不该立刻停）要的是**真实事件流**下的
      证据，不是读代码推出来的结论。人手没法精确复现"按住 2 秒、其间系统每 50 ms 重发一次"，
      这个脚本按毫秒级节奏把这段流打进去；配合游戏里的 VA_DBG_INPUT=1，两张表（事件表 +
      位置心跳）就能对起来。

关键：SendInput 本身**不会**产生"按键重复" —— 重复是操作系统的行为。所以要验这条路径，
      必须自己按 Windows 的节奏重发 keydown：先发一次 keydown，之后在重复延迟（默认 500 ms）
      起每 repeat-ms 再发一次 keydown，**中间不发 keyup**。
      这样窗口过程收到的 WM_KEYDOWN 里 lParam bit30（previous key state）是 1，
      Godot 才会把它标成 echo=1 —— 这正是"按住不放"与"点一下"在事件层的唯一区别。

用法：
    python tools/inject_key.py --list                                 # 先看有哪些窗口（含 PID）
    python tools/inject_key.py --key W --hold 2.0 --focus VolunteerArmyPC
    python tools/inject_key.py --key W --hold 2.0 --no-repeat           # 对照：只按下、松开，中间没有重复

失焦场景三连（验"松开的 keyup 被别的窗口吃掉了"）：
    python tools/inject_key.py --focus VolunteerArmyPC --down-only      # 按住不放
    python tools/inject_key.py --refocus "某窗口标题"                     # 抢走焦点 = 游戏失焦
    python tools/inject_key.py --up-only                                # keyup 发给别的窗口，游戏收不到
    python tools/inject_key.py --refocus VolunteerArmyPC                # 回到游戏（此时键已抬起）

安全（踩过一次）：本机同时开着 Visual Studio，而它的标题是
    "VolunteerArmyPC - Microsoft Visual Studio"
—— 按子串匹配会**先命中 VS**，于是 32 条 W 全进了编辑器。所以：
    ① 标题优先**精确相等**，退而求其次取"最短的含子串标题"；
    ② 每条事件发出前都核实目标窗口仍在前台，不是就立刻中止（宁可一条不发）。

输出：每条事件的相对时刻（ms）与 down/repeat/up，以及窗口焦点是否抢到，供与游戏日志对表。
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

# ---------------------------------------------------------------- Win32 接口
user32 = ctypes.WinDLL("user32", use_last_error=True)

ULONG_PTR = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_ulong

INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_EXTENDEDKEY = 0x0001

VK = {
    "W": 0x57, "A": 0x41, "S": 0x53, "D": 0x44,
    "Q": 0x51, "E": 0x45, "R": 0x52, "F": 0x46, "G": 0x47, "Z": 0x5A,
    "SHIFT": 0x10, "CTRL": 0x11,
    "UP": 0x26, "DOWN": 0x28, "LEFT": 0x25, "RIGHT": 0x27,
    "ESC": 0x1B, "SPACE": 0x20,
}
EXTENDED = {"UP", "DOWN", "LEFT", "RIGHT"}


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ULONG_PTR)]


class MOUSEINPUT(ctypes.Structure):      # 只为把 INPUT 的联合体撑到正确大小（x64 上 40 字节）
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD),
                ("dwFlags", wt.DWORD), ("time", wt.DWORD), ("dwExtraInfo", ULONG_PTR)]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", wt.DWORD), ("wParamL", wt.WORD), ("wParamH", wt.WORD)]


class _INPUTUNION(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", _INPUTUNION)]


def send_vk(vk, up=False, extended=False):
    flags = (KEYEVENTF_KEYUP if up else 0) | (KEYEVENTF_EXTENDEDKEY if extended else 0)
    inp = INPUT(type=INPUT_KEYBOARD)
    inp.u.ki = KEYBDINPUT(wVk=vk, wScan=0, dwFlags=flags, time=0, dwExtraInfo=0)
    sent = user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    if sent != 1:
        err = ctypes.get_last_error()
        raise OSError("SendInput 失败：%d（差错码 %d）" % (sent, err))


def _window_title(hwnd):
    n = user32.GetWindowTextLengthW(hwnd)
    if n <= 0:
        return ""
    buf = ctypes.create_unicode_buffer(n + 1)
    user32.GetWindowTextW(hwnd, buf, n + 1)
    return buf.value


def _window_pid(hwnd):
    pid = wt.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def _window_class(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def enum_windows():
    """所有可见、有标题的顶层窗口：(hwnd, pid, class, title)。"""
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _lp):
        if user32.IsWindowVisible(hwnd):
            t = _window_title(hwnd)
            if t:
                out.append((hwnd, _window_pid(hwnd), _window_class(hwnd), t))
        return True

    user32.EnumWindows(cb, 0)
    return out


def pick_window(title_sub, pid=None):
    """标题精确相等优先；否则取最短的含子串标题（最长的那条通常挂着一堆后缀）。"""
    cands = [w for w in enum_windows() if pid is None or w[1] == pid]
    exact = [w for w in cands if w[3] == title_sub]
    if exact:
        return exact[0]
    sub = [w for w in cands if title_sub.lower() in w[3].lower()]
    if not sub:
        return None
    return sorted(sub, key=lambda w: len(w[3]))[0]


def foreground_is(hwnd):
    return user32.GetForegroundWindow() == hwnd


def force_foreground(hwnd):
    """尽力把 hwnd 抢到前台。

    Windows 有前台锁：非前台进程直接调 SetForegroundWindow 常常被忽略
    （实测过一次"从 Visual Studio 切回游戏"直接失败）。常见解法是把本线程的
    输入队列**附加到当前前台窗口所属线程**上，附加期间就享有前台进程的权限。
    """
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    fg = user32.GetForegroundWindow()
    fg_tid = user32.GetWindowThreadProcessId(fg, None)
    my_tid = kernel32.GetCurrentThreadId()
    attached = False
    if fg_tid and fg_tid != my_tid:
        attached = bool(user32.AttachThreadInput(my_tid, fg_tid, True))
    try:
        user32.ShowWindow(hwnd, 9)           # SW_RESTORE
        user32.BringWindowToTop(hwnd)
        user32.SetForegroundWindow(hwnd)
    finally:
        if attached:
            user32.AttachThreadInput(my_tid, fg_tid, False)
    time.sleep(0.15)
    return foreground_is(hwnd)


def main():
    args = sys.argv[1:]
    key, hold, delay_ms, repeat_ms, focus, pid, refocus = "W", 2.0, 500, 50, None, None, None
    repeat, listing, down_only, up_only = True, False, False, False
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--key":
            key = args[i + 1].upper(); i += 2
        elif a == "--hold":
            hold = float(args[i + 1]); i += 2
        elif a == "--delay-ms":
            delay_ms = int(args[i + 1]); i += 2
        elif a == "--repeat-ms":
            repeat_ms = int(args[i + 1]); i += 2
        elif a == "--focus":
            focus = args[i + 1]; i += 2
        elif a == "--pid":
            pid = int(args[i + 1]); i += 2
        elif a == "--refocus":            # 只把前台切到某个窗口（用来制造"游戏失焦"）
            refocus = args[i + 1]; i += 2
        elif a == "--down-only":          # 只发 keydown（模拟"按住不放，人离开了游戏窗口"）
            down_only = True; i += 1
        elif a == "--up-only":            # 只发 keyup（发给别的窗口，游戏永远收不到这条松开）
            up_only = True; i += 1
        elif a == "--no-repeat":
            repeat = False; i += 1
        elif a == "--list":
            listing = True; i += 1
        else:
            print("未知参数：%s" % a); return 2

    if listing:
        for hwnd, wpid, cls, title in sorted(enum_windows(), key=lambda w: w[1]):
            print("0x%08X  pid=%-6d  class=%-24s  %s" % (hwnd, wpid, cls, title))
        return 0

    if refocus is not None:
        picked = pick_window(refocus, pid)
        if picked is None:
            print("[inject] 找不到标题含 %r 的窗口" % refocus)
            return 3
        hwnd, _pid, cls, title = picked
        ok = any(force_foreground(hwnd) for _ in range(6))
        print("[inject] 前台切到 0x%08X pid=%d class=%r 标题=%r 成功=%s"
              % (hwnd, _pid, cls, title, ok))
        return 0 if ok else 3

    if key not in VK:
        print("不认识的键：%s（可选 %s）" % (key, "/".join(sorted(VK))))
        return 2
    vk = VK[key]
    ext = key in EXTENDED

    hwnd = None
    if focus:
        picked = pick_window(focus, pid)
        if picked is None:
            print("[inject] 找不到标题含 %r 的窗口 —— 先 --list 看一眼，别猜" % focus)
            return 3
        hwnd, _pid, cls, title = picked
        ok = any(force_foreground(hwnd) for _ in range(6))
        print("[inject] 目标窗口 0x%08X pid=%d class=%r 标题=%r 已在前台=%s"
              % (hwnd, _pid, cls, title, ok))
        if not ok:
            print("[inject] 抢不到前台 —— 中止（宁可不注入，也不能把按键打进别的窗口）")
            return 3
    else:
        fg = user32.GetForegroundWindow()
        print("[inject] 当前前台窗口 0x%08X %r（未指定 --focus，直接打给它）"
              % (fg, _window_title(fg)))

    # 单发模式也走上面那套焦点处理：否则 --down-only 会打给"碰巧在前台的那个窗口"，
    # 这种静默打偏正是当初把 W 打进 Visual Studio 的原因。
    if down_only or up_only:
        send_vk(vk, up=up_only, extended=ext)
        print("[inject] 单发 %s vk=0x%02X" % ("keyup" if up_only else "keydown", vk))
        return 0

    print("[inject] 计划：%s 按下 → %d ms 后开始重复（每 %d ms 一次，%s）→ 共按住 %.2f s → 松开"
          % (key, delay_ms, repeat_ms, "有" if repeat else "无", hold))

    t0 = time.perf_counter()

    def stamp():
        return (time.perf_counter() - t0) * 1000.0

    steps = [(0.0, "down", False)]
    if repeat:
        t = delay_ms
        while t < hold * 1000.0:                 # 系统按键重复：只重发 keydown，不发 keyup
            steps.append((t, "repeat", False))
            t += repeat_ms
    steps.append((hold * 1000.0, "up", True))

    for idx, (at, what, up) in enumerate(steps):
        wait = at / 1000.0 - (time.perf_counter() - t0)
        if wait > 0:
            time.sleep(wait)
        if hwnd is not None and not foreground_is(hwnd):
            print("[inject] +%.1f ms 目标窗口已不在前台 —— 中止，后续 %d 条未发"
                  % (stamp(), len(steps) - idx))
            return 3
        send_vk(vk, up=up, extended=ext)
        print("[inject] %+8.1f ms  %-6s  vk=0x%02X%s" % (stamp(), what, vk, "（扩展键）" if ext else ""))

    print("[inject] 共 %d 条事件" % len(steps))
    return 0


if __name__ == "__main__":
    sys.exit(main())
