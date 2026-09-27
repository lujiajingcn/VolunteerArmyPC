# -*- coding: utf-8 -*-
"""往指定标题的窗口**投递鼠标点击**（PostMessage，不需要抢前台）。

为什么需要它：`press_key_post.py` / `inject_key.py` 都只管键盘，而本工程
有一类行为**只有鼠标能触发** —— 最典型的就是「右键切换开镜」
（`world_sim.cpp::_input` 里 `va::IN.ads` 的闩锁）。没有注入工具，
这条需求就只能靠人手点，判据也就无从自动化。

语义与 `press_key_post.py` 一致：消息直接进目标窗口的消息队列，
Godot 的窗口过程照常处理，与"谁在前台"无关。
⚠️ 它**不产生** WM_MOUSEMOVE 之外的任何东西，也不移动真实光标 ——
所以它验的是"按钮事件本身"，不是"鼠标拖动"。

用法：
    python tools/click_mouse_post.py LMB "VolunteerArmyPC"
    python tools/click_mouse_post.py LMB,RMB,RMB "VolunteerArmyPC" 0.6
    python tools/click_mouse_post.py RMB "VolunteerArmyPC" 0.6 540,360

  argv[1] 点击序列（`,` 或 `:` 分隔）：LMB / RMB / MMB（大小写不敏感）
  argv[2] 窗口标题**子串**（默认 VolunteerArmyPC；取最短命中，见 press_key_post.py）
  argv[3] 每步之间的间隔秒（默认 0.5）—— 太短会被 Godot 合帧吃掉
  argv[4] 可选客户端坐标 `x,y`（默认窗口客户区中心）

【本机踩过的坑】
  ⚠️ 本工程进战斗时鼠标是 `MOUSE_MODE_CAPTURED`，而 `_input` 里右键的闩锁
     只在 `captured` 时生效。所以序列**必须先来一发 LMB** —— 那是游戏自己
     的"点一下捕获鼠标"路径（`!captured` 分支），否则后面的 RMB 全被丢掉。
  ⚠️ 窗口标题取最短命中：本机同时开着 Visual Studio，标题里也含工程名，
     按子串会先命中 VS。
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)

WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
WM_RBUTTONDOWN, WM_RBUTTONUP = 0x0204, 0x0205
WM_MBUTTONDOWN, WM_MBUTTONUP = 0x0207, 0x0208
MK_LBUTTON, MK_RBUTTON, MK_MBUTTON = 0x0001, 0x0002, 0x0010

BTN = {
    "LMB": (WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON),
    "RMB": (WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON),
    "MMB": (WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON),
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
    hits.sort(key=lambda t: len(t[1]))   # 子串可能命中多个 → 取最短
    return hits[0]


def client_size(hwnd):
    r = wt.RECT()
    if not user32.GetClientRect(hwnd, ctypes.byref(r)):
        return 0, 0
    return r.right - r.left, r.bottom - r.top


def main():
    seq = (sys.argv[1] if len(sys.argv) > 1 else "RMB").upper()
    title = sys.argv[2] if len(sys.argv) > 2 else "VolunteerArmyPC"
    gap = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
    pos = sys.argv[4] if len(sys.argv) > 4 else None

    steps = [s.strip() for s in seq.replace(":", ",").split(",") if s.strip()]
    bad = [s for s in steps if s not in BTN]
    if bad:
        print("[click] 未知按钮:", ",".join(bad), "（可用 LMB / RMB / MMB）")
        return 2

    hwnd, found = find_window(title)
    if hwnd is None:
        print("[click] 找不到标题含 %r 的窗口" % title)
        return 1

    if pos:
        try:
            x, y = (int(v) for v in pos.split(","))
        except ValueError:
            print("[click] 坐标要写成 x,y 两个整数")
            return 2
    else:
        w, h = client_size(hwnd)
        x, y = w // 2, h // 2

    lp = (x & 0xFFFF) | ((y & 0xFFFF) << 16)
    for i, s in enumerate(steps):
        down, up, mask = BTN[s]
        # 按下带按钮掩码、抬起 wParam=0，与真实消息一致（Godot 不看它，但别造假的）
        user32.PostMessageW(hwnd, down, mask, lp)
        time.sleep(0.03)
        user32.PostMessageW(hwnd, up, 0, lp)
        print("[click] %d/%d %s @ (%d,%d)" % (i + 1, len(steps), s, x, y))
        if i + 1 < len(steps):
            time.sleep(gap)

    print("[click] 已向 %r (hwnd=0x%08X) 投递 %d 次点击（间隔 %.2fs）"
          % (found, hwnd, len(steps), gap))
    return 0


if __name__ == "__main__":
    sys.exit(main())
