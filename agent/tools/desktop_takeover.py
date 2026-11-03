# -*- coding: utf-8 -*-
"""虚拟桌面接管：把目标窗口所在的桌面切到眼前，画一圈保时捷蓝的接管边框。

背景：Windows 会把「不在当前虚拟桌面」的窗口打上 DWM_CLOAKED_SHELL 标记，
这类窗口 IsWindowVisible 仍为 True、IsIconic 也是 False，但 DWM 根本不合成它。
于是区域抓屏（BitBlt / mss）只能拿到当前桌面的画面，截图会静默给出一张别的
桌面内容。这个模块提供的是另一条路：先把桌面切过去，让「看」和「操作」重新
落回同一块地方，再用一圈会呼吸的蓝边告诉用户「AI 正在接管这个窗口」。

三条硬约束，写在最前面：

1. 切桌面必须可回收。任何一次接管都有截止时间，超时自动还原；用户一动鼠标
   就立刻还原。工具可以借桌面，不能扣着不还。
2. 切不回去要说出来。Windows 没有公开的 SwitchDesktop，只能盲切 + 校验，
   还原时按当初切过来的跳数反向补，补不回去就如实报 error，不假装已还原。
3. 边框不能被截图漏掉。层次窗口创建时就要带 WS_EX_TOPMOST，靠事后
   SetWindowPos 补置顶在刚切完桌面的那一瞬间会静默失效（实测：返回真、
   错误码 0、GWL_EXSTYLE 里却没有 0x8）。

本模块不 import screen_automation，避免循环依赖；窗口矩形的取法和那边一致
（DWMWA_EXTENDED_FRAME_BOUNDS），所以两边算出来的坐标是同一个。
"""

import ctypes
import math
import threading
import time
from ctypes import wintypes
from typing import Optional

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
try:
    dwmapi = ctypes.WinDLL("dwmapi", use_last_error=True)
except OSError:  # pragma: no cover - 只在极端残缺的系统上发生
    dwmapi = None


# ── 常量 ──────────────────────────────────────────────────────────
DWMWA_EXTENDED_FRAME_BOUNDS = 9
DWMWA_CLOAKED = 14
DWM_CLOAKED_APP = 0x1
DWM_CLOAKED_SHELL = 0x2
DWM_CLOAKED_INHERITED = 0x4

WS_POPUP = 0x80000000
WS_EX_LAYERED = 0x00080000
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000
WS_EX_TOPMOST = 0x00000008

ULW_ALPHA = 0x00000002
AC_SRC_OVER, AC_SRC_ALPHA = 0x00, 0x01
DIB_RGB_COLORS = 0
BI_RGB = 0
SW_SHOWNOACTIVATE = 4
SW_SHOWNA = 8
WM_NCHITTEST = 0x0084
WM_CLOSE = 0x0010
WM_DESTROY = 0x0002
WM_TIMER = 0x0113
WM_ERASEBKGND = 0x0014
HTTRANSPARENT = -1

VK_LWIN, VK_CONTROL = 0x5B, 0x11
VK_LEFT, VK_RIGHT = 0x25, 0x27
KEYEVENTF_KEYUP = 0x0002

# 保时捷蓝：深端到亮端，沿对角线铺开
COLOR_DARK = (0x0B, 0x3F, 0xA8)    # RGB
COLOR_LIGHT = (0x2F, 0x9B, 0xF6)   # RGB
BORDER_PX = 5
CORNER_RADIUS = 12
BREATH_PERIOD_S = 2.0
BREATH_LOW, BREATH_HIGH = 0.55, 1.0
BREATH_FPS = 30

# 一次接管最多占用用户桌面这么久。每次工具调用会续期，但总有上限；
# 到期由后台看门线程无条件还原，不依赖「下次调用时才想起来收」。
TAKEOVER_TIMEOUT_S = 45.0
# 盲切一次之后等多久再校验（桌面切换动画 200~300ms）
SWITCH_SETTLE_S = 0.42
SWITCH_MAX_HOPS = 8


# ── 结构体与函数签名 ──────────────────────────────────────────────
class _RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class _POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class _SIZE(ctypes.Structure):
    _fields_ = [("cx", ctypes.c_long), ("cy", ctypes.c_long)]


class _BLENDFUNCTION(ctypes.Structure):
    _fields_ = [("BlendOp", ctypes.c_ubyte), ("BlendFlags", ctypes.c_ubyte),
                ("SourceConstantAlpha", ctypes.c_ubyte), ("AlphaFormat", ctypes.c_ubyte)]


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


class _BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", _BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


class _MSG(ctypes.Structure):
    _fields_ = [("hwnd", wintypes.HWND), ("message", wintypes.UINT),
                ("wParam", wintypes.WPARAM), ("lParam", wintypes.LPARAM),
                ("time", wintypes.DWORD), ("pt", _POINT)]


_WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wintypes.HWND, wintypes.UINT,
                              wintypes.WPARAM, wintypes.LPARAM)


class _WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", wintypes.UINT), ("lpfnWndProc", _WNDPROC),
                ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", wintypes.HINSTANCE), ("hIcon", wintypes.HICON),
                ("hCursor", wintypes.HANDLE), ("hbrBackground", wintypes.HBRUSH),
                ("lpszMenuName", wintypes.LPCWSTR), ("lpszClassName", wintypes.LPCWSTR)]


user32.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.DefWindowProcW.restype = ctypes.c_ssize_t
user32.CreateWindowExW.argtypes = [
    wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID]
user32.CreateWindowExW.restype = wintypes.HWND
user32.GetForegroundWindow.restype = wintypes.HWND
user32.GetForegroundWindow.argtypes = []
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
user32.AttachThreadInput.restype = wintypes.BOOL
user32.SetForegroundWindow.argtypes = [wintypes.HWND]
user32.SetForegroundWindow.restype = wintypes.BOOL
user32.BringWindowToTop.argtypes = [wintypes.HWND]
user32.BringWindowToTop.restype = wintypes.BOOL
user32.IsWindow.argtypes = [wintypes.HWND]
user32.IsWindow.restype = wintypes.BOOL
user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.SendMessageW.restype = ctypes.c_ssize_t
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.PostMessageW.restype = wintypes.BOOL
user32.GetMessageW.argtypes = [ctypes.POINTER(_MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT]
user32.GetMessageW.restype = wintypes.BOOL
user32.TranslateMessage.argtypes = [ctypes.POINTER(_MSG)]
user32.TranslateMessage.restype = wintypes.BOOL
user32.DispatchMessageW.argtypes = [ctypes.POINTER(_MSG)]
user32.DispatchMessageW.restype = ctypes.c_ssize_t
user32.PostQuitMessage.argtypes = [ctypes.c_int]
user32.SetTimer.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.UINT, wintypes.HANDLE]
user32.SetTimer.restype = wintypes.UINT
user32.KillTimer.argtypes = [wintypes.HWND, wintypes.UINT]
user32.KillTimer.restype = wintypes.BOOL
user32.GetWindowRect.restype = wintypes.BOOL
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.IsWindowVisible.restype = wintypes.BOOL
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
user32.ShowWindow.restype = wintypes.BOOL
user32.DestroyWindow.argtypes = [wintypes.HWND]
user32.DestroyWindow.restype = wintypes.BOOL
user32.GetDC.argtypes = [wintypes.HWND]
user32.GetDC.restype = wintypes.HDC
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
user32.ReleaseDC.restype = ctypes.c_int
user32.UpdateLayeredWindow.argtypes = [
    wintypes.HWND, wintypes.HDC, ctypes.POINTER(_POINT), ctypes.POINTER(_SIZE),
    wintypes.HDC, ctypes.POINTER(_POINT), wintypes.DWORD,
    ctypes.POINTER(_BLENDFUNCTION), wintypes.DWORD]
user32.UpdateLayeredWindow.restype = wintypes.BOOL
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.GetModuleHandleW.restype = wintypes.HMODULE
kernel32.GetCurrentThreadId.restype = wintypes.DWORD
gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
gdi32.SelectObject.restype = wintypes.HGDIOBJ
gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wintypes.HDC]
gdi32.CreateDIBSection.argtypes = [
    wintypes.HDC, ctypes.POINTER(_BITMAPINFO), wintypes.UINT,
    ctypes.POINTER(ctypes.c_void_p), wintypes.HANDLE, wintypes.DWORD]
gdi32.CreateDIBSection.restype = wintypes.HBITMAP


# ── 虚拟桌面判定 ──────────────────────────────────────────────────

def cloaked_state(hwnd: int) -> int:
    """返回 DWMWA_CLOAKED 的原始值：0 未遮蔽、2 被外壳遮蔽、其它见 DWM_CLOAKED_*。

    虚拟桌面切换正是 SHELL 遮蔽的主要来源，所以 2 基本可以判成「在别的桌面」。
    """
    if dwmapi is None:
        return 0
    value = ctypes.c_int(0)
    try:
        hres = dwmapi.DwmGetWindowAttribute(
            wintypes.HWND(int(hwnd)), wintypes.DWORD(DWMWA_CLOAKED),
            ctypes.byref(value), ctypes.sizeof(value))
    except Exception:
        return 0
    return int(value.value) if hres == 0 else 0


def on_other_virtual_desktop(hwnd: int) -> bool:
    """窗口在别的虚拟桌面上吗。注意此时 IsWindowVisible 仍为 True。"""
    return cloaked_state(hwnd) == DWM_CLOAKED_SHELL


def frame_rect(hwnd: int) -> dict:
    """DWM 实际可见边框，与 screen_automation._extended_frame_bounds 同源。"""
    if dwmapi is not None:
        rect = _RECT()
        try:
            hres = dwmapi.DwmGetWindowAttribute(
                wintypes.HWND(int(hwnd)), wintypes.DWORD(DWMWA_EXTENDED_FRAME_BOUNDS),
                ctypes.byref(rect), ctypes.sizeof(rect))
            if hres == 0 and rect.right > rect.left and rect.bottom > rect.top:
                return {"left": int(rect.left), "top": int(rect.top),
                        "right": int(rect.right), "bottom": int(rect.bottom)}
        except Exception:
            pass
    rect = _RECT()
    user32.GetWindowRect(wintypes.HWND(int(hwnd)), ctypes.byref(rect))
    return {"left": int(rect.left), "top": int(rect.top),
            "right": int(rect.right), "bottom": int(rect.bottom)}


# ── 桌面切换 ──────────────────────────────────────────────────────

_FOCUS_HWND = 0


def _focus_window() -> int:
    """专门用来接键盘焦点的小窗（1x1、屏外、不在任务栏）。"""
    global _FOCUS_HWND
    if _FOCUS_HWND and user32.IsWindow(wintypes.HWND(_FOCUS_HWND)):
        return _FOCUS_HWND
    hwnd = user32.CreateWindowExW(
        WS_EX_TOOLWINDOW, "STATIC", "GridStarAgentKeys", WS_POPUP,
        -32000, -32000, 8, 8, None, None, kernel32.GetModuleHandleW(None), None)
    _FOCUS_HWND = int(hwnd or 0) or 0
    return _FOCUS_HWND


def _ensure_own_foreground() -> bool:
    """把键盘焦点抢回本进程，否则模拟的 Win+Ctrl 到不了外壳。

    这是切桌面能不能收尾的关键。切到 GridStar 那块桌面之后，前台会变成
    GridStar 自己；它以管理员运行时完整性级别是 0x3000，而本服务是 0x2000，
    UIPI 会把我们模拟的按键静默丢掉——于是切得过去、切不回来。
    先把焦点挪到自己进程的窗口上（自己进程不受 UIPI 限制），按键才递得出去。

    Returns:
        当前前台是不是已经变成我们自己的窗口。
    """
    hwnd = _focus_window()
    if not hwnd:
        return False
    fg = int(user32.GetForegroundWindow() or 0)
    if fg == hwnd:
        return True
    user32.ShowWindow(wintypes.HWND(hwnd), SW_SHOWNA)
    fg_tid = int(user32.GetWindowThreadProcessId(wintypes.HWND(fg), None) or 0) if fg else 0
    cur_tid = int(kernel32.GetCurrentThreadId())
    attached = False
    if fg_tid and fg_tid != cur_tid:
        attached = bool(user32.AttachThreadInput(fg_tid, cur_tid, True))
    try:
        user32.BringWindowToTop(wintypes.HWND(hwnd))
        user32.SetForegroundWindow(wintypes.HWND(hwnd))
    finally:
        if attached:
            user32.AttachThreadInput(fg_tid, cur_tid, False)
    return int(user32.GetForegroundWindow() or 0) == hwnd


def _send_key(vk: int, up: bool = False) -> None:
    user32.keybd_event(vk, 0, KEYEVENTF_KEYUP if up else 0, 0)


def _switch_once(direction: str, settle_s: float = SWITCH_SETTLE_S) -> None:
    """发一次 Win+Ctrl+方向键。keybd_event 已废弃但够用，不引新依赖。"""
    arrow = VK_RIGHT if direction == "right" else VK_LEFT
    _send_key(VK_LWIN)
    _send_key(VK_CONTROL)
    _send_key(arrow)
    _send_key(arrow, up=True)
    _send_key(VK_CONTROL, up=True)
    _send_key(VK_LWIN, up=True)
    time.sleep(settle_s)


def switch_to_window_desktop(hwnd: int, direction: str = "right",
                             max_hops: int = SWITCH_MAX_HOPS) -> dict:
    """朝一个方向盲切，直到目标窗口不再被外壳遮蔽。

    Windows 没有公开的 SwitchDesktop（IVirtualDesktopManager 只有
    IsWindowOnCurrentVirtualDesktop / GetWindowDesktopId / MoveWindowToDesktop），
    所以只能试探 + 轮询。命中就停；跑完预算还没命中就如实返回 error，并把
    实际走了几跳也一并返回——调用方要靠它算净位移，好用反方向补回来。

    Returns:
        {"switched": bool, "hops": 实际走了几跳, "direction", "error"}
    """
    if not on_other_virtual_desktop(hwnd):
        return {"switched": True, "hops": 0, "direction": direction, "error": None}
    _ensure_own_foreground()
    for hop in range(1, max_hops + 1):
        _switch_once(direction)
        if not on_other_virtual_desktop(hwnd):
            return {"switched": True, "hops": hop, "direction": direction, "error": None}
    return {
        "switched": False, "hops": max_hops, "direction": direction,
        "error": (
            "已朝 %s 连切 %s 次仍没找到目标窗口所在桌面（cloaked=%s）。"
            % (direction, max_hops, cloaked_state(hwnd))
        ),
    }


def switch_back(offset: int, hwnd: Optional[int] = None) -> dict:
    """按净位移补回去：offset>0 说明现在在右边几块，往左按回去。

    盲切没法保证原路返回，但净位移是确定的，因此按它反推就是最接近真实的
    还原。给了 hwnd 就验证一下是不是真的离开了那块桌面——刚销毁完覆盖窗时
    前台处于瞬时状态，抢焦点会失败，按键就被 UIPI 吞了，所以这里允许重试。
    """
    offset = int(offset or 0)
    if not offset:
        return {"restored": True, "hops": 0, "error": None}
    direction = "left" if offset > 0 else "right"
    last_error = None
    for _attempt in range(3):
        for _wait in range(5):
            if _ensure_own_foreground():
                break
            time.sleep(0.12)
        else:
            last_error = ("抢不回键盘焦点：当前前台仍是高权限窗口，模拟按键会被 UIPI 丢弃。"
                          "需要手动切回去。")
        for _index in range(abs(offset)):
            _switch_once(direction)
        if hwnd is None or on_other_virtual_desktop(int(hwnd)):
            # 重试成功就把上一轮的中途报错降级成备注，别让「已还原」配一句告警
            return {
                "restored": True,
                "hops": abs(offset),
                "retried": bool(_attempt),
                "error": None,
                "note": ("上一次抢焦点失败、按键被 UIPI 吞了，重试第 %s 次才切回去。"
                         % (_attempt + 1)) if _attempt else None,
            }
        last_error = ("已按 %s 次反向切换，但目标窗口仍不在别的桌面上，桌面可能没真的切回去。"
                      % abs(offset))
        time.sleep(0.25)
    return {"restored": False, "hops": abs(offset), "error": last_error}


def wait_until_stable(hwnd: int, timeout_s: float = 3.0) -> bool:
    """切换动画跑完的标志：连续两次采到的矩形与遮蔽状态一致。

    比拍脑袋 sleep 400ms 可靠，命中时也更快。
    """
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        rect = frame_rect(hwnd)
        cur = (rect["left"], rect["top"], rect["right"], rect["bottom"],
               cloaked_state(hwnd))
        if cur == last:
            return True
        last = cur
        time.sleep(0.08)
    return False


# ── 接管边框 ──────────────────────────────────────────────────────

def build_border_bitmap(width: int, height: int,
                        thickness: int = BORDER_PX,
                        radius: int = CORNER_RADIUS) -> bytes:
    """生成 32 位 BGRA 边框位图（自上而下），只填四条边带。

    只遍历边框带上的像素，几万个点，纯 Python 也是毫秒级；整屏遍历要几秒。
    """
    stride = width * 4
    data = bytearray(stride * height)
    dark, light = COLOR_DARK, COLOR_LIGHT
    total = float(max(1, width + height))

    def corner_alpha(x, y):
        """圆角：四个角落在圆外的像素不画，视觉上把直角削圆。"""
        cx = cy = None
        if x < radius and y < radius:
            cx, cy = radius, radius
        elif x >= width - radius and y < radius:
            cx, cy = width - radius - 1, radius
        elif x < radius and y >= height - radius:
            cx, cy = radius, height - radius - 1
        elif x >= width - radius and y >= height - radius:
            cx, cy = width - radius - 1, height - radius - 1
        if cx is None:
            return 255
        dx, dy = x - cx, y - cy
        return 255 if (dx * dx + dy * dy) ** 0.5 <= radius else 0

    def paint(x, y):
        t = (x + y) / total
        r = int(dark[0] + (light[0] - dark[0]) * t)
        g = int(dark[1] + (light[1] - dark[1]) * t)
        b = int(dark[2] + (light[2] - dark[2]) * t)
        a = corner_alpha(x, y)
        if not a:
            return
        off = y * stride + x * 4
        data[off:off + 4] = bytes((b, g, r, a))

    for y in range(height):
        for x in range(thickness):
            paint(x, y)
        for x in range(max(thickness, width - thickness), width):
            paint(x, y)
    for x in range(thickness, width - thickness):
        for y in range(thickness):
            paint(x, y)
        for y in range(max(thickness, height - thickness), height):
            paint(x, y)
    return bytes(data)


class TakeoverBorder:
    """覆盖在目标窗口上的一圈渐变边框，鼠标完全穿透，不抢焦点。

    两个关键点，都是现场撞出来的：

    1. WS_EX_TOPMOST 必须在创建时就带上。创建后再用 SetWindowPos 补置顶，
       在刚切换完虚拟桌面的那一瞬间会静默失效（返回真、错误码 0，但
       GWL_EXSTYLE 里没有 0x8），表现就是「边框创建成功却看不见、也拍不到」。
    2. 窗口必须归一个自己的线程。DestroyWindow 只能由创建窗口的线程调用，
       别处调会以 ERROR_ACCESS_DENIED 失败；而接管超时是后台看门线程在收，
       于是边框永远不消失。这里让边框线程自己建窗口、自己跑消息循环、自己
       销毁自己，外部只投消息，不再有跨线程调窗口函数的问题。
    """

    CLASS_NAME = "GridStarAgentTakeoverBorder"
    TIMER_ID = 1
    TIMER_MS = 33          # 约 30fps

    def __init__(self, target_hwnd: int):
        self.target_hwnd = int(target_hwnd)
        self.hwnd = None
        self._mem_dc = None
        self._bitmap = None
        self._old_bitmap = None
        self._pos = None
        self._size = None
        self._thread = None
        self._ready = threading.Event()
        self._error = None
        self._breathing = True
        self._period = BREATH_PERIOD_S
        self._low, self._high = BREATH_LOW, BREATH_HIGH
        self._started = 0.0
        self._manual_alpha = None

    # ── 对外 ────────────────────────────────────────────────────

    def show(self) -> int:
        if self.hwnd:
            return self.hwnd
        self._ready.clear()
        self._thread = threading.Thread(target=self._run, daemon=True,
                                        name="takeover-border")
        self._thread.start()
        self._ready.wait(timeout=4.0)
        if not self.hwnd:
            raise RuntimeError(self._error or "接管边框创建超时")
        return self.hwnd

    def set_alpha(self, ratio: float) -> None:
        """手动指定整层透明度；传 None 恢复到自动呼吸。"""
        self._manual_alpha = None if ratio is None else max(0.0, min(1.0, float(ratio)))

    def start_breathing(self, period_s: float = BREATH_PERIOD_S,
                        low: float = BREATH_LOW, high: float = BREATH_HIGH,
                        fps: int = BREATH_FPS) -> None:
        self._period = float(period_s)
        self._low, self._high = float(low), float(high)
        self._breathing = True
        self._manual_alpha = None

    def stop_breathing(self) -> None:
        self._breathing = False

    def destroy(self) -> bool:
        """投 WM_CLOSE 给边框线程，由它自己销毁窗口，然后等它退场。

        Returns:
            窗口是不是真的没了。没没就如实返回 False，不假装收干净了。
        """
        hwnd = self.hwnd
        if hwnd:
            try:
                user32.PostMessageW(wintypes.HWND(hwnd), WM_CLOSE, 0, 0)
            except Exception:
                pass
        thread = self._thread
        if thread is not None:
            thread.join(timeout=2.0)
            self._thread = None
        self.hwnd = None
        if hwnd:
            try:
                return not bool(user32.IsWindow(wintypes.HWND(hwnd)))
            except Exception:
                return True
        return True

    # ── 边框线程内部 ──────────────────────────────────────────────

    def _run(self) -> None:
        try:
            self._create_window()
        except Exception as exc:  # 建不出来就把原因交回去，不让 show() 卡住
            self._error = str(exc)
            self._ready.set()
            return
        self._started = time.time()
        user32.SetTimer(wintypes.HWND(self.hwnd), self.TIMER_ID, self.TIMER_MS, None)
        self._ready.set()
        msg = _MSG()
        while True:
            ret = user32.GetMessageW(ctypes.byref(msg), None, 0, 0)
            if ret <= 0:      # 0 = WM_QUIT，-1 = 出错
                break
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        self._cleanup_gdi()

    def _create_window(self) -> None:
        hinst = kernel32.GetModuleHandleW(None)

        @_WNDPROC
        def _proc(hwnd, msg, wparam, lparam):
            if msg == WM_NCHITTEST:
                return HTTRANSPARENT     # 鼠标完全穿透，不挡住底下的 GridStar
            if msg == WM_ERASEBKGND:
                return 1
            if msg == WM_TIMER and wparam == self.TIMER_ID:
                self._tick()
                return 0
            if msg == WM_CLOSE:
                user32.DestroyWindow(wintypes.HWND(hwnd))
                return 0
            if msg == WM_DESTROY:
                user32.KillTimer(wintypes.HWND(hwnd), self.TIMER_ID)
                user32.PostQuitMessage(0)
                return 0
            return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

        self._proc = _proc            # 留住引用，否则回调被回收
        wc = _WNDCLASSW()
        wc.lpfnWndProc = _proc
        wc.hInstance = hinst
        wc.lpszClassName = self.CLASS_NAME
        atom = user32.RegisterClassW(ctypes.byref(wc))
        if not atom and ctypes.get_last_error() not in (0, 1410):  # 1410 = 类已注册
            raise RuntimeError("RegisterClassW 失败：%s" % ctypes.get_last_error())

        rect = frame_rect(self.target_hwnd)
        left, top = rect["left"], rect["top"]
        width, height = rect["right"] - left, rect["bottom"] - top
        if width <= 0 or height <= 0:
            raise RuntimeError("目标窗口矩形无效：%s" % rect)

        ex_style = (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW
                    | WS_EX_NOACTIVATE | WS_EX_TOPMOST)
        hwnd = user32.CreateWindowExW(
            ex_style, self.CLASS_NAME, "AI 接管中", WS_POPUP,
            left, top, width, height, None, None, hinst, None)
        if not hwnd:
            raise RuntimeError("CreateWindowExW 失败：%s" % ctypes.get_last_error())
        self.hwnd = int(hwnd)
        self._paint(width, height, left, top)
        user32.ShowWindow(wintypes.HWND(hwnd), SW_SHOWNOACTIVATE)

    def _tick(self) -> None:
        """定时器回来一次就重新算一下透明度，呼吸和手动指定都在这里落地。"""
        if self._manual_alpha is not None:
            alpha = self._manual_alpha
        elif not self._breathing:
            alpha = 1.0
        else:
            phase = ((time.time() - self._started) % self._period) / self._period
            wave = 0.5 - 0.5 * math.cos(2.0 * math.pi * phase)
            alpha = self._low + (self._high - self._low) * wave
        self._apply_alpha(alpha)

    def _apply_alpha(self, ratio: float) -> bool:
        """只改整层透明度，复用已算好的位图。

        SourceConstantAlpha 与逐像素 alpha 相乘，渐变不变，只是整体变淡变浓。
        """
        if not self.hwnd or not self._mem_dc or not self._size:
            return False
        value = int(max(0.0, min(1.0, ratio)) * 255)
        blend = _BLENDFUNCTION(AC_SRC_OVER, 0, value, AC_SRC_ALPHA)
        screen_dc = user32.GetDC(None)
        try:
            src = _POINT()
            return bool(user32.UpdateLayeredWindow(
                wintypes.HWND(self.hwnd), screen_dc, ctypes.byref(self._pos),
                ctypes.byref(self._size), self._mem_dc, ctypes.byref(src), 0,
                ctypes.byref(blend), ULW_ALPHA))
        finally:
            user32.ReleaseDC(None, screen_dc)

    def _paint(self, width: int, height: int, left: int, top: int) -> None:
        data = build_border_bitmap(width, height)
        screen_dc = user32.GetDC(None)
        self._mem_dc = gdi32.CreateCompatibleDC(screen_dc)
        bmi = _BITMAPINFO()
        header = bmi.bmiHeader
        header.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
        header.biWidth = width
        header.biHeight = -height          # 负数 = 自上而下，省一次行翻转
        header.biPlanes = 1
        header.biBitCount = 32
        header.biCompression = BI_RGB
        bits = ctypes.c_void_p()
        self._bitmap = gdi32.CreateDIBSection(
            self._mem_dc, ctypes.byref(bmi), DIB_RGB_COLORS, ctypes.byref(bits), None, 0)
        if not self._bitmap or not bits:
            user32.ReleaseDC(None, screen_dc)
            raise RuntimeError("CreateDIBSection 失败：%s" % ctypes.get_last_error())
        self._old_bitmap = gdi32.SelectObject(self._mem_dc, self._bitmap)
        ctypes.memmove(bits, data, len(data))

        blend = _BLENDFUNCTION(AC_SRC_OVER, 0, 255, AC_SRC_ALPHA)
        # pptDst 是窗口的新屏幕位置：给错坐标，窗口会被整体挪走
        self._pos = _POINT(left, top)
        self._size = _SIZE(width, height)
        src = _POINT()
        ok = user32.UpdateLayeredWindow(
            wintypes.HWND(self.hwnd), screen_dc, ctypes.byref(self._pos),
            ctypes.byref(self._size), self._mem_dc, ctypes.byref(src), 0,
            ctypes.byref(blend), ULW_ALPHA)
        user32.ReleaseDC(None, screen_dc)
        if not ok:
            raise RuntimeError("UpdateLayeredWindow 失败：%s" % ctypes.get_last_error())

    def _cleanup_gdi(self) -> None:
        try:
            if self._mem_dc and self._old_bitmap:
                gdi32.SelectObject(self._mem_dc, self._old_bitmap)
            if self._bitmap:
                gdi32.DeleteObject(self._bitmap)
            if self._mem_dc:
                gdi32.DeleteDC(self._mem_dc)
        except Exception:
            pass
        finally:
            self._mem_dc = self._bitmap = self._old_bitmap = None


# ── 会话级接管状态 ────────────────────────────────────────────────

_LOCK = threading.RLock()
_SESSION = {
    "active": False,
    "hwnd": 0,
    "offset": 0,          # 桌面净位移：>0 在右边几块，<0 在左边几块
    "hops": 0,            # 本次切换一共按了多少下（仅作报告）
    "started_at": 0.0,
    "deadline": 0.0,
    "border": None,
    "last_error": None,
}


def _session_locked() -> dict:
    """当前接管状态的快照（不含 border 对象本身）。"""
    border = _SESSION.get("border")
    return {
        "active": bool(_SESSION["active"]),
        "hwnd": int(_SESSION["hwnd"] or 0),
        "hops": int(_SESSION["hops"] or 0),
        "desktop_offset": int(_SESSION["offset"] or 0),
        "started_at": _SESSION["started_at"],
        "deadline": _SESSION["deadline"],
        "expires_in": max(0.0, (_SESSION["deadline"] or 0.0) - time.time()),
        "border_visible": bool(border and border.hwnd),
        "last_error": _SESSION["last_error"],
    }


def border_hwnd() -> int:
    """当前接管边框的窗口句柄，没有就返回 0。

    坐标类工具要用它：覆盖层对真实鼠标是穿透的（WM_NCHITTEST 返 HTTRANSPARENT），
    正常情况下 WindowFromPoint 会跳过它。留着这个句柄是给体检代码一个确切身份，
    以便万一它被报回来时能认得出来，而不是当成「这个位置上压着别的窗口」。
    """
    with _LOCK:
        border = _SESSION.get("border")
        return int(getattr(border, "hwnd", 0) or 0)


def status() -> dict:
    with _LOCK:
        return _session_locked()


def schedule_release(seconds: float = 3.0) -> dict:
    """把接管截止时间改到 seconds 之后，交给看门线程到点收。

    截图用完就该还桌面：领域识别只截一张图，没必要占着 45 秒。但也不能截完
    立刻就收，因为下一步可能紧跟着一次点击。所以截完给一个短的宽限，真正的
    界面操作会把它重新续长。
    """
    with _LOCK:
        if not _SESSION["active"]:
            return {"scheduled": False, "release_in": 0.0}
        _SESSION["deadline"] = time.time() + max(0.0, float(seconds))
    return {"scheduled": True, "release_in": float(seconds)}


def end_takeover(reason: str = "") -> dict:
    """收尾：熄边框 → 反方向切回去。幂等，重复调用不会切乱桌面。"""
    with _LOCK:
        if not _SESSION["active"]:
            return {"ended": False, "reason": "当前没有进行中的接管"}
        border = _SESSION.get("border")
        offset = int(_SESSION["offset"] or 0)
        target_hwnd = int(_SESSION["hwnd"] or 0)
        _SESSION["active"] = False
        _SESSION["border"] = None
        _SESSION["hwnd"] = 0
        _SESSION["offset"] = 0
        _SESSION["hops"] = 0
        _SESSION["deadline"] = 0.0
    if border is not None:
        try:
            border_removed = border.destroy()
        except Exception:
            border_removed = False
    else:
        border_removed = True
    restored = {"restored": True, "hops": 0, "error": None}
    if offset:
        restored = switch_back(offset, target_hwnd or None)
    return {
        "ended": True,
        "reason": reason,
        "border_removed": bool(border_removed),
        "desktop_restored": bool(restored.get("restored")),
        "restore_error": restored.get("error"),
        "restore_note": restored.get("note"),
    }


def _watch_loop() -> None:
    """看门：接管过了截止时间就无条件还原桌面。

    没有这个线程，一次截图之后如果模型不再调工具，用户的桌面就被扣在那儿了。
    超时自归还比任何调用方都可靠。
    """
    while True:
        time.sleep(0.5)
        with _LOCK:
            active = bool(_SESSION["active"])
            deadline = float(_SESSION["deadline"] or 0.0)
        if active and deadline and time.time() > deadline:
            try:
                end_takeover("接管超时，自动还原桌面")
            except Exception:
                pass


_WATCH_THREAD = None


def _ensure_watchdog() -> None:
    global _WATCH_THREAD
    if _WATCH_THREAD is None or not _WATCH_THREAD.is_alive():
        _WATCH_THREAD = threading.Thread(target=_watch_loop, daemon=True,
                                         name="takeover-watchdog")
        _WATCH_THREAD.start()


def ensure_takeover(hwnd: int, timeout_s: float = TAKEOVER_TIMEOUT_S) -> dict:
    """确保目标窗口在眼前的桌面上，并点亮接管边框。

    幂等：同一次任务里反复调用只切一次桌面，边框也复用。已经过了截止时间就
    先还原再重来，避免上一轮没收拾干净把桌面留在别人那边。

    Returns:
        {"ok": bool, "switched": bool, "hops": int, "border": bool,
         "reused": bool, "error": str|None}
    """
    hwnd = int(hwnd)
    _ensure_watchdog()
    now = time.time()
    with _LOCK:
        if _SESSION["active"]:
            if now > _SESSION["deadline"]:
                # 上一轮超时没回收：先老实收掉，再重新接管
                need_recycle = True
            else:
                need_recycle = False
        else:
            need_recycle = False
    if need_recycle:
        end_takeover("上一轮接管超时，先回收再重新接管")

    with _LOCK:
        if _SESSION["active"] and int(_SESSION["hwnd"]) == hwnd:
            _SESSION["deadline"] = now + timeout_s
            border = _SESSION.get("border")
            if border is not None and border.hwnd:
                return {"ok": True, "switched": False, "hops": 0,
                        "desktop_offset": int(_SESSION["offset"] or 0),
                        "border": True, "reused": True, "error": None}
        elif _SESSION["active"]:
            # 换了窗口：旧的对不上，收掉重来
            pass
    if not _SESSION["active"] or int(_SESSION["hwnd"] or 0) != hwnd:
        if _SESSION["active"]:
            end_takeover("接管目标窗口变了，先回收旧的")

    error = None
    switched = False
    hops = 0
    offset = 0
    if on_other_virtual_desktop(hwnd):
        # 从近处找起：绝大多数情况相邻一块就到了。先右 1 再左 3（先补回刚才
        # 那 1 跳再往前 2），然后右 5、左 7。盲切从一边硬跑 8 跳会白白多等
        # 两三秒，命中后还不能保证原路返回。
        for direction, budget in (("right", 1), ("left", 3), ("right", 5), ("left", 7)):
            result = switch_to_window_desktop(hwnd, direction, max_hops=budget)
            offset += result["hops"] * (1 if direction == "right" else -1)
            hops += result["hops"]
            if result["switched"]:
                switched = True
                error = None
                break
            error = result["error"]
        if not switched:
            with _LOCK:
                _SESSION["last_error"] = error
            return {"ok": False, "switched": False, "hops": hops, "border": False,
                    "reused": False, "desktop_offset": offset, "error": error}
        wait_until_stable(hwnd)

    border = TakeoverBorder(hwnd)
    try:
        border.show()
        border.start_breathing()
    except Exception as exc:  # 边框失败不该让截图整体失败
        try:
            border.destroy()
        except Exception:
            pass
        border = None
        error = "接管边框创建失败（不影响截图）：%s" % exc

    with _LOCK:
        _SESSION.update({
            "active": True,
            "hwnd": hwnd,
            "offset": offset,
            "hops": hops,
            "started_at": now,
            "deadline": now + timeout_s,
            "border": border,
            "last_error": error,
        })
    return {
        "ok": True,
        "switched": switched,
        "hops": hops,
        "desktop_offset": offset,
        "border": bool(border and border.hwnd),
        "reused": False,
        "error": error,
    }