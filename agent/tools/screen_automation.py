"""GridStar 桌面屏幕自动化工具。

窗口识别方式（与 dsh-orb-cordis 的 Computer Use 层一致）：
    先按进程定位，再用 HWND 作为唯一身份贯穿后续所有操作。

    1. GetWindowThreadProcessId(hwnd)   → 窗口属于哪个进程
    2. OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) + QueryFullProcessImageNameW
                                        → 该进程可执行文件的完整路径
    3. basename(exe) 与 gridstar.exe 比对 → 这才是「GridStar 进程」的判据
    4. HWND 从此作为身份：截图矩形、UIA 根元素、前台校验全部由同一个 hwnd 派生

标题不再作为识别判据，只在进程匹配之后做次级过滤与展示。进程可执行文件名可用环境变量
GRIDSTAR_PROCESS_NAMES 覆盖（逗号分隔，例如 "gridstar.exe,gridstar_dev.exe"）。

窗口身份的每一步都可验证：截图结果带 is_foreground，UIA 根元素回校验 ProcessId，
输入前检查完整性级别（UIPI），不做「以为点了」的静默成功。

依赖：pywin32（已有）、mss、uiautomation。后两者缺失时本模块仍可导入，
相关工具返回明确的缺依赖错误，不会连带整个 MCP Server 起不来。
"""

from __future__ import annotations

import base64
import ctypes
import json
import logging
import os
import queue
import threading
import time
from ctypes import wintypes
from typing import Callable, Optional

logger = logging.getLogger(__name__)

# ── 可选依赖：缺失时降级为工具级错误，不阻断模块导入 ────────────────
_MISSING_DEPS: list = []
_MSS_ERROR: Optional[str] = None
_UIA_ERROR: Optional[str] = None

try:
    import mss
except Exception as _exc:  # pragma: no cover - 取决于运行环境
    mss = None
    _MSS_ERROR = str(_exc)
    _MISSING_DEPS.append("mss (%s)" % _exc)

try:
    import uiautomation as uia
except Exception as _exc:  # pragma: no cover - 取决于运行环境
    uia = None
    _UIA_ERROR = str(_exc)
    _MISSING_DEPS.append("uiautomation (%s)" % _exc)

# pywin32 的 DLL 搜索顺序坑：Anaconda 的 Library\bin 下常留着一份旧版
# pywintypes313.dll，它会在 PATH 搜索里抢在 pywin32_system32 里那份正确的前面，
# 之后 win32api.pyd 会以 ERROR_PROC_NOT_FOUND（「找不到指定的程序」）导入失败，
# 而且失败与否取决于「谁先被导入」——现场已复现（先 import win32gui 必炸）。
# 因此这里先按绝对路径把正确的 pywintypes 载进来，再导入其它子模块。
# 整个 pywin32 也做成守卫式导入：坏掉的 pywin32 只让 screen 工具报缺依赖，
# 不会让整个 MCP Server 起不来。
_PYWIN32_ERROR: Optional[Exception] = None
try:
    import pywintypes  # noqa: F401  必须排在 win32api / win32gui 之前
    import pythoncom
    import win32api
    import win32con
    import win32gui
    import win32security
except Exception as _exc:  # pragma: no cover - 取决于运行环境
    pythoncom = win32api = win32con = win32gui = win32security = None
    _PYWIN32_ERROR = _exc
    _MISSING_DEPS.append("pywin32 (%s)" % _exc)

# ── 目标进程 ─────────────────────────────────────────────────────
_DEFAULT_PROCESS_NAMES = "gridstar.exe"


def _target_process_names() -> tuple:
    raw = os.environ.get("GRIDSTAR_PROCESS_NAMES", _DEFAULT_PROCESS_NAMES)
    names = tuple(name.strip().lower() for name in raw.split(",") if name.strip())
    return names or (_DEFAULT_PROCESS_NAMES,)


TARGET_PROCESS_NAMES = _target_process_names()

# ── Win32 常量 ───────────────────────────────────────────────────
_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
_TOKEN_QUERY = 0x0008
_DWMWA_EXTENDED_FRAME_BOUNDS = 9
_DWMWA_CLOAKED = 14
_WS_EX_TOOLWINDOW = 0x00000080
_SM_XVIRTUALSCREEN, _SM_YVIRTUALSCREEN = 76, 77
_SM_CXVIRTUALSCREEN, _SM_CYVIRTUALSCREEN = 78, 79
_TH32CS_SNAPPROCESS = 0x00000002
_VK_MENU = 0x12
_KEYEVENTF_KEYUP = 0x0002
_WM_SYSCOMMAND = 0x0112
_SC_RESTORE = 0xF120
_SMTO_ABORTIFHUNG = 0x0002

# 鼠标注入（SendInput）
_MOUSEEVENTF_MOVE = 0x0001
_MOUSEEVENTF_LEFTDOWN = 0x0002
_MOUSEEVENTF_LEFTUP = 0x0004
_MOUSEEVENTF_RIGHTDOWN = 0x0008
_MOUSEEVENTF_RIGHTUP = 0x0010
_MOUSEEVENTF_MIDDLEDOWN = 0x0020
_MOUSEEVENTF_MIDDLEUP = 0x0040
_MOUSEEVENTF_WHEEL = 0x0800
_MOUSEEVENTF_ABSOLUTE = 0x8000
_MOUSEEVENTF_VIRTUALDESK = 0x4000
_INPUT_MOUSE = 0
_WHEEL_DELTA = 120

# PrintWindow 抓窗口自身内容
_PW_RENDERFULLCONTENT = 0x00000002
_SRCCOPY = 0x00CC0020
_DIB_RGB_COLORS = 0
_BI_RGB = 0

# 坐标空间：默认 0-1000 归一化。模型看到的截图会被缩放，像素坐标不可靠，
# 而归一化坐标只依赖画面比例，缩放后依然成立。
_COORD_MAX = 1000.0
_CLICK_GAP_S = 0.06
_DRAG_STEPS = 12
_PRESS_SETTLE_S = 0.02
# PrintWindow 结果的空白判据：单一颜色占比超过上限即认为没画出来
_BLANK_DOMINANT_RATIO = 0.98
_SUSPECT_DOMINANT_RATIO = 0.55

# 前台设置失败后的重试节奏：补发一次 Alt 键换取 SetForegroundWindow 的许可
_FOREGROUND_RETRY_S = 0.05
_FOREGROUND_TIMEOUT_S = 1.0
_CLIPBOARD_SETTLE_S = 0.03
_PASTE_SETTLE_S = 0.08
_UI_SETTLE_S = 0.12
# 等待窗口从最小化状态还原的上限
_RESTORE_TIMEOUT_S = 1.0

# UIA 调用超时：超过则判定该 STA 线程卡死，重建工作线程
_UIA_CALL_TIMEOUT_S = float(os.environ.get("GRIDSTAR_UIA_TIMEOUT", "20"))

_user32 = ctypes.WinDLL("user32", use_last_error=True)
_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
try:
    _dwmapi = ctypes.WinDLL("dwmapi", use_last_error=True)
except OSError:  # pragma: no cover - 极老系统
    _dwmapi = None


class _RECT(ctypes.Structure):
    _fields_ = [
        ("left", wintypes.LONG), ("top", wintypes.LONG),
        ("right", wintypes.LONG), ("bottom", wintypes.LONG),
    ]


class _PROCESSENTRY32W(ctypes.Structure):
    """PROCESSENTRY32W。

    th32DefaultHeapID 声明成 C 的 ULONG_PTR（指针宽度整数），ctypes 会按平台
    ABI 对齐，x64 下 szExeFile 落在偏移 44，与 Win32 头文件一致。
    """

    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", wintypes.LONG),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


class _POINT(ctypes.Structure):
    _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]


class _MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", wintypes.LONG),
        ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.c_size_t),
    ]


class _INPUT(ctypes.Structure):
    """SendInput 的 INPUT 结构。

    INPUT 是一个「类型 + 联合体」的结构，x64 下联合体里最大的成员是 MOUSEINPUT
    （32 字节），整个结构 40 字节。只用到鼠标事件，所以联合体里只声明 mi。
    """

    class _INPUTUNION(ctypes.Union):
        _fields_ = [("mi", _MOUSEINPUT)]

    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _INPUTUNION)]


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


class _BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", _BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


_kernel32.CreateToolhelp32Snapshot.argtypes = (wintypes.DWORD, wintypes.DWORD)
_kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
_kernel32.Process32FirstW.argtypes = (wintypes.HANDLE, ctypes.POINTER(_PROCESSENTRY32W))
_kernel32.Process32FirstW.restype = wintypes.BOOL
_kernel32.Process32NextW.argtypes = (wintypes.HANDLE, ctypes.POINTER(_PROCESSENTRY32W))
_kernel32.Process32NextW.restype = wintypes.BOOL
_user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
_user32.GetWindowThreadProcessId.restype = wintypes.DWORD
_kernel32.OpenProcess.restype = wintypes.HANDLE
_kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
_kernel32.QueryFullProcessImageNameW.argtypes = (
    wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD),
)
_kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
_kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
_user32.keybd_event.argtypes = (wintypes.BYTE, wintypes.BYTE, wintypes.DWORD, ctypes.c_void_p)
# LRESULT SendMessageTimeoutW(HWND, UINT, WPARAM, LPARAM, UINT fuFlags, UINT uTimeout, PDWORD_PTR)
_user32.SendMessageTimeoutW.argtypes = (
    wintypes.HWND, wintypes.UINT, ctypes.c_size_t, ctypes.c_ssize_t,
    wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t),
)
_user32.SendMessageTimeoutW.restype = ctypes.c_ssize_t

# 鼠标注入：SendInput 一次投递一个事件数组，返回值是「被插入输入流的条数」。
_user32.SendInput.argtypes = (wintypes.UINT, ctypes.POINTER(_INPUT), ctypes.c_int)
_user32.SendInput.restype = wintypes.UINT
_user32.GetCursorPos.argtypes = (ctypes.POINTER(_POINT),)
_user32.GetCursorPos.restype = wintypes.BOOL
_user32.GetForegroundWindow.restype = wintypes.HWND
_user32.SetForegroundWindow.argtypes = (wintypes.HWND,)
_user32.SetForegroundWindow.restype = wintypes.BOOL
_user32.BringWindowToTop.argtypes = (wintypes.HWND,)
_user32.BringWindowToTop.restype = wintypes.BOOL
_user32.AttachThreadInput.argtypes = (wintypes.DWORD, wintypes.DWORD, wintypes.BOOL)
_user32.AttachThreadInput.restype = wintypes.BOOL
_user32.IsHungAppWindow.argtypes = (wintypes.HWND,)
_user32.IsHungAppWindow.restype = wintypes.BOOL
_kernel32.GetCurrentThreadId.restype = wintypes.DWORD

# PrintWindow 路径要用的 GDI 原型。GetDIBits 的第五个参数是裸缓冲区，
# 用 c_void_p 接，避免 ctypes 对字符串缓冲区做隐式转换。
_gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
_user32.GetWindowDC.argtypes = (wintypes.HWND,)
_user32.GetWindowDC.restype = wintypes.HDC
_user32.ReleaseDC.argtypes = (wintypes.HWND, wintypes.HDC)
_user32.ReleaseDC.restype = ctypes.c_int
_user32.PrintWindow.argtypes = (wintypes.HWND, wintypes.HDC, wintypes.UINT)
_user32.PrintWindow.restype = wintypes.BOOL
_gdi32.CreateCompatibleDC.argtypes = (wintypes.HDC,)
_gdi32.CreateCompatibleDC.restype = wintypes.HDC
_gdi32.CreateCompatibleBitmap.argtypes = (wintypes.HDC, ctypes.c_int, ctypes.c_int)
_gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
_gdi32.SelectObject.argtypes = (wintypes.HDC, wintypes.HGDIOBJ)
_gdi32.SelectObject.restype = wintypes.HGDIOBJ
_gdi32.DeleteObject.argtypes = (wintypes.HGDIOBJ,)
_gdi32.DeleteObject.restype = wintypes.BOOL
_gdi32.DeleteDC.argtypes = (wintypes.HDC,)
_gdi32.DeleteDC.restype = wintypes.BOOL
_gdi32.GetDIBits.argtypes = (
    wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT,
    ctypes.c_void_p, ctypes.POINTER(_BITMAPINFO), wintypes.UINT,
)
_gdi32.GetDIBits.restype = ctypes.c_int


def _enable_dpi_awareness() -> bool:
    """让本进程按物理像素工作。

    非 DPI 感知进程的 GetWindowRect 返回的是被系统虚拟化过的逻辑坐标，
    而 mss 抓的是物理像素，125%/150% 缩放下截图会偏移缺角。必须在任何
    窗口 API 调用之前设置，所以放在导入时执行。
    """
    try:
        # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -3（Win10 1703+）
        if _user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-3)):
            return True
    except Exception:
        pass
    try:
        ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)  # PER_MONITOR_DPI_AWARE
        return True
    except Exception:
        pass
    try:
        return bool(_user32.SetProcessDPIAware())
    except Exception:
        return False


_DPI_AWARE = _enable_dpi_awareness()


# ── 进程与窗口事实 ───────────────────────────────────────────────

_exe_cache: dict = {}
_exe_cache_lock = threading.Lock()


def _process_exe_path(pid: int) -> str:
    """取进程可执行文件全路径；权限不足或无权限查询时返回空串。

    用 PROCESS_QUERY_LIMITED_INFORMATION（0x1000）而不是 PROCESS_QUERY_INFORMATION，
    高权限目标进程下也能拿到映像名，不需要 SeDebugPrivilege。
    """
    if not pid:
        return ""
    with _exe_cache_lock:
        cached = _exe_cache.get(pid)
    if cached is not None:
        return cached
    path = ""
    handle = _kernel32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if handle:
        try:
            size = wintypes.DWORD(32768)
            buf = ctypes.create_unicode_buffer(size.value)
            if _kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
                path = buf.value or ""
        finally:
            _kernel32.CloseHandle(handle)
    if path:
        # 只缓存成功结果：失败通常意味着权限不足或进程已退出，把它缓存下来
        # 等于把一个永久为空的结论钉在 pid 上（pid 还会被系统回收复用）。
        with _exe_cache_lock:
            _exe_cache[pid] = path
    return path


# pid → 可执行文件名。走 Toolhelp32 快照一次取全系统，而不是逐个 OpenProcess：
#   * 一次调用覆盖所有进程，枚举顶层窗口时不会变成几百次 OpenProcess；
#   * szExeFile 由内核直接填写，不受目标进程权限影响（对以管理员身份运行、
#     或 DACL 收紧的进程照样能读到），而 OpenProcess 那条路会被拒。
# 它的弱点是快照有时效，所以带一个很短的 TTL，超时就重取。
_PROCESS_NAME_TTL_S = 1.0
_name_map_cache: dict = {}
_name_map_at = 0.0
_name_map_lock = threading.Lock()
_INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


def _process_name_map(force: bool = False) -> dict:
    """{pid: 可执行文件名}，来自 TH32CS_SNAPPROCESS 快照。"""
    global _name_map_at
    now = time.monotonic()
    if not force:
        with _name_map_lock:
            if _name_map_cache and now - _name_map_at < _PROCESS_NAME_TTL_S:
                return dict(_name_map_cache)
    mapping: dict = {}
    snapshot = _kernel32.CreateToolhelp32Snapshot(_TH32CS_SNAPPROCESS, 0)
    if snapshot and snapshot != _INVALID_HANDLE_VALUE:
        try:
            entry = _PROCESSENTRY32W()
            entry.dwSize = ctypes.sizeof(_PROCESSENTRY32W)
            if _kernel32.Process32FirstW(snapshot, ctypes.byref(entry)):
                while True:
                    mapping[int(entry.th32ProcessID)] = entry.szExeFile or ""
                    if not _kernel32.Process32NextW(snapshot, ctypes.byref(entry)):
                        break
        except Exception:
            logger.debug("Toolhelp32 快照遍历失败", exc_info=True)
        finally:
            _kernel32.CloseHandle(snapshot)
    if mapping:
        with _name_map_lock:
            _name_map_cache.clear()
            _name_map_cache.update(mapping)
            _name_map_at = time.monotonic()
    return mapping


def _process_basename(pid: int) -> str:
    """pid → 可执行文件名（不含目录）。进程判据统一走这里。"""
    if not pid:
        return ""
    name = _process_name_map().get(int(pid))
    if name:
        return name
    # 快照没覆盖到（例如进程刚启动）时，退回按 pid 查映像路径。
    return os.path.basename(_process_exe_path(pid))


def _window_pid(hwnd: int) -> int:
    """窗口所属进程 id。

    这里刻意不用 pywin32 的 win32process.GetWindowThreadProcessId：它返回
    (线程 id, 进程 id)，两个 id 落在同一个数字空间里，取错那一位会得到一个
    看起来完全合法的数字。现场就踩过：hwnd 331652 被解析成它的 UI 线程 id，
    于是 OpenProcess 打不开这个「进程」，可执行名取不到，最终在 GridStar 明明
    正在运行的情况下报「系统里没有该进程的窗口」——一个静默失败加误导性结论。
    更危险的镜像情况是线程 id 恰好等于另一个真实进程的 pid，那样会 OpenProcess
    到无关进程上。ctypes 直接调 user32 没有这种歧义：返回值是线程 id，进程 id
    只从出参里取。
    """
    try:
        pid = wintypes.DWORD(0)
        if not _user32.GetWindowThreadProcessId(wintypes.HWND(hwnd), ctypes.byref(pid)):
            return 0
        return int(pid.value)
    except Exception:
        return 0


def _window_cloaked(hwnd: int) -> bool:
    """UWP/虚拟桌面下窗口可能「存在但被 DWM 遮蔽」，这类窗口不可见也不可点。"""
    if _dwmapi is None:
        return False
    value = ctypes.c_int(0)
    try:
        hres = _dwmapi.DwmGetWindowAttribute(
            wintypes.HWND(hwnd), wintypes.DWORD(_DWMWA_CLOAKED),
            ctypes.byref(value), ctypes.sizeof(value),
        )
        return hres == 0 and bool(value.value)
    except Exception:
        return False


def _extended_frame_bounds(hwnd: int) -> Optional[tuple]:
    """DWM 实际可见边框。

    GetWindowRect 含 DWM 的隐形拖拽边框（现场实测窗口 1936x1048 而屏幕只有
    1920x1080），直接拿它截图会多出一圈不属于任何内容的黑边。
    """
    if _dwmapi is None:
        return None
    rect = _RECT()
    try:
        hres = _dwmapi.DwmGetWindowAttribute(
            wintypes.HWND(hwnd), wintypes.DWORD(_DWMWA_EXTENDED_FRAME_BOUNDS),
            ctypes.byref(rect), ctypes.sizeof(rect),
        )
        if hres != 0:
            return None
        if rect.right <= rect.left or rect.bottom <= rect.top:
            return None
        return (rect.left, rect.top, rect.right, rect.bottom)
    except Exception:
        return None


_self_rid: Optional[int] = None


def _integrity_rid(pid: int) -> Optional[int]:
    """进程令牌完整性级别 RID：0x1000 低、0x2000 中、0x3000 高（管理员）。

    UIPI 会静默丢弃低完整性进程发往高完整性进程的模拟输入，所以这是输入前的
    必要闸门：GridStar 以管理员启动而本服务不是时，点击/键入必然无效。
    """
    handle = None
    token = None
    try:
        handle = win32api.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        token = win32security.OpenProcessToken(handle, _TOKEN_QUERY)
        sid, _attrs = win32security.GetTokenInformation(token, win32security.TokenIntegrityLevel)
        return int(sid.GetSubAuthority(sid.GetSubAuthorityCount() - 1))
    except Exception:
        return None
    finally:
        for obj in (token, handle):
            try:
                if obj is not None:
                    obj.Close()
            except Exception:
                pass


def _self_integrity_rid() -> Optional[int]:
    global _self_rid
    if _self_rid is None:
        _self_rid = _integrity_rid(os.getpid())
    return _self_rid


def _window_facts(hwnd: int, foreground_hwnd: int = 0) -> dict:
    """采集单个窗口的可判定事实。"""
    pid = _window_pid(hwnd)
    exe = _process_exe_path(pid)
    try:
        title = win32gui.GetWindowText(hwnd) or ""
    except Exception:
        title = ""
    try:
        class_name = win32gui.GetClassName(hwnd) or ""
    except Exception:
        class_name = ""
    try:
        rect = tuple(int(v) for v in win32gui.GetWindowRect(hwnd))
    except Exception:
        rect = (0, 0, 0, 0)
    frame = _extended_frame_bounds(hwnd) or rect
    try:
        owner = int(win32gui.GetWindow(hwnd, win32con.GW_OWNER))
    except Exception:
        owner = 0
    try:
        ex_style = int(win32gui.GetWindowLong(hwnd, win32con.GWL_EXSTYLE))
    except Exception:
        ex_style = 0
    try:
        is_iconic = bool(win32gui.IsIconic(hwnd))
    except Exception:
        is_iconic = False
    process_name = _process_basename(pid) or (os.path.basename(exe) if exe else "")
    return {
        "hwnd": int(hwnd),
        "pid": pid,
        "exe": exe,
        "process_name": process_name or None,
        "title": title,
        "class_name": class_name,
        "rect": {"left": rect[0], "top": rect[1], "right": rect[2], "bottom": rect[3]},
        "frame_rect": {"left": frame[0], "top": frame[1], "right": frame[2], "bottom": frame[3]},
        "width": max(0, frame[2] - frame[0]),
        "height": max(0, frame[3] - frame[1]),
        "owner_hwnd": owner,
        "is_visible": bool(win32gui.IsWindowVisible(hwnd)),
        "is_enabled": bool(win32gui.IsWindowEnabled(hwnd)),
        "is_iconic": is_iconic,
        "is_cloaked": _window_cloaked(hwnd),
        "is_foreground": bool(foreground_hwnd) and int(hwnd) == int(foreground_hwnd),
        "is_tool_window": bool(ex_style & _WS_EX_TOOLWINDOW),
        "is_main_window": owner == 0 and not (ex_style & _WS_EX_TOOLWINDOW),
    }


def _enum_top_level_windows() -> list:
    hwnds: list = []

    def _callback(hwnd, _param):
        hwnds.append(hwnd)
        return True

    win32gui.EnumWindows(_callback, None)
    return hwnds


def _title_decoys(keywords: list, foreground: int, limit: int = 5) -> list:
    """标题里带目标关键字、但进程不是目标进程的窗口。

    老实现拿窗口标题当识别判据，这类窗口正是它当年会误认的对象（现场就有一个
    标题含「GridStar」的 DeepSeek Harness 窗口）。列出来是为了让「没找到」这个
    结论可以被核对，而不是一句无法反驳的断言。
    """
    decoys = []
    lowered = [k.lower() for k in keywords if k]
    if not lowered:
        return decoys
    for hwnd in _enum_top_level_windows():
        try:
            title = win32gui.GetWindowText(hwnd) or ""
        except Exception:
            continue
        if not any(k in title.lower() for k in lowered):
            continue
        pid = _window_pid(hwnd)
        name = _process_basename(pid) or "(进程名不可读)"
        if name.lower() in TARGET_PROCESS_NAMES:
            continue
        decoys.append({
            "hwnd": int(hwnd), "title": title, "pid": pid,
            "process_name": name, "is_foreground": int(hwnd) == int(foreground or 0),
        })
        if len(decoys) >= limit:
            break
    return decoys


def _scan_target_windows(window_title: Optional[str] = None,
                         include_hidden: bool = False,
                         include_cloaked: bool = False) -> tuple:
    """枚举属于目标进程的顶层窗口，返回 (候选列表, 可核对的扫描统计)。

    判据只有一条：窗口所属进程的可执行文件名在 TARGET_PROCESS_NAMES 里。
    window_title 只在这批候选内部做次级过滤，永远不会把非目标进程的窗口捞进来。
    """
    try:
        foreground = int(win32gui.GetForegroundWindow())
    except Exception:
        foreground = 0
    keyword = (window_title or "").strip().lower()
    hwnds = _enum_top_level_windows()
    report = {
        "top_level_windows": len(hwnds),
        "visible_windows": 0,
        "process_name_unreadable": 0,
        "process_names": list(TARGET_PROCESS_NAMES),
    }
    candidates = []
    for hwnd in hwnds:
        try:
            visible = bool(win32gui.IsWindowVisible(hwnd))
        except Exception:
            continue
        if not include_hidden and not visible:
            continue
        report["visible_windows"] += 1
        pid = _window_pid(hwnd)
        if not pid:
            report["process_name_unreadable"] += 1
            continue
        name = _process_basename(pid)
        if not name:
            # 读不到进程名时如实计数，不再静默丢弃：老实现就是在这一步把 GridStar
            # 自己的窗口扔掉的，最后报出一句「系统里没有该进程的窗口」。
            report["process_name_unreadable"] += 1
            continue
        if name.lower() not in TARGET_PROCESS_NAMES:
            continue  # ← 进程闸门：这一步之前的标题匹配全是假阳性来源
        facts = _window_facts(hwnd, foreground)
        if facts["is_cloaked"] and not include_cloaked:
            continue
        if keyword and keyword not in facts["title"].lower():
            continue
        candidates.append(facts)
    return candidates, report


def _gridstar_candidates(window_title: Optional[str] = None,
                         include_hidden: bool = False,
                         include_cloaked: bool = False) -> list:
    """枚举属于目标进程的顶层窗口（判据是进程，标题只做次级过滤）。"""
    candidates, _report = _scan_target_windows(window_title, include_hidden, include_cloaked)
    return candidates


def _select_main_window(candidates: list) -> dict:
    """从候选里挑一个「当前该看的窗口」。

    前台优先（用户此刻在看的就是它），其次是未被拥有的主窗口，避免把某个
    无标题的辅助窗口当成主界面。
    """

    def score(item: dict) -> tuple:
        points = 0
        if item["is_foreground"]:
            points += 8
        if item["is_main_window"]:
            points += 4
        if not item["is_iconic"]:
            points += 2
        if item["title"]:
            points += 1
        if item["width"] > 200 and item["height"] > 200:
            points += 1
        return (points, item["width"] * item["height"])

    return max(candidates, key=score)


def _find_gridstar_window(window_title: Optional[str] = None,
                          include_hidden: bool = False) -> Optional[dict]:
    """定位 GridStar 窗口（进程判据），返回窗口身份信息或 None。

    Returns:
        {"matched_by": "process", "hwnd", "pid", "exe", "process_name", "title",
         "class_name", "rect", "frame_rect", "width", "height", "owner_hwnd",
         "is_visible", "is_enabled", "is_iconic", "is_cloaked", "is_foreground",
         "is_main_window", "is_elevated", "integrity_rid", "input_blocked",
         "input_block_reason", "transient_windows": [...], "candidate_count": int}
    """
    candidates = _gridstar_candidates(window_title, include_hidden)
    if not candidates:
        return None
    chosen = dict(_select_main_window(candidates))
    chosen["transient_windows"] = [
        {
            "hwnd": item["hwnd"], "title": item["title"], "class_name": item["class_name"],
            "pid": item["pid"], "rect": item["rect"], "is_visible": item["is_visible"],
            "is_iconic": item["is_iconic"], "is_main_window": item["is_main_window"],
        }
        for item in candidates if item["hwnd"] != chosen["hwnd"]
    ]
    chosen["candidate_count"] = len(candidates)
    chosen["matched_by"] = "process"
    rid = _integrity_rid(chosen["pid"])
    chosen["integrity_rid"] = rid
    self_rid = _self_integrity_rid()
    chosen["is_elevated"] = bool(rid is not None and self_rid is not None and rid > self_rid)
    allowed, reason = _input_gate(chosen)
    chosen["input_blocked"] = not allowed
    chosen["input_block_reason"] = reason or None
    return chosen


def _input_gate(info: dict) -> tuple:
    """输入前的 UIPI 闸门，返回 (允许?, 拒绝原因)。"""
    rid = info.get("integrity_rid")
    self_rid = _self_integrity_rid()
    if rid is None or self_rid is None:
        return True, ""  # 查不到完整性级别时不阻塞，但结果里会如实标注
    if rid > self_rid:
        return False, (
            "GridStar 进程以更高权限运行（完整性级别 0x%X > 本进程 0x%X），"
            "Windows UIPI 会静默丢弃模拟输入，点击和键入都不会生效。"
            "请以相同权限运行 GridStar 与本服务。" % (rid, self_rid)
        )
    return True, ""


def _pywin32_error(tool: str) -> Optional[str]:
    """pywin32 不可用时的统一拒绝。

    Find/Activate 这两个工具不需要 mss 与 uiautomation，但整个模块的窗口识别都
    建立在 pywin32 上，所以它们只受这一条闸门约束。
    """
    if _PYWIN32_ERROR is None:
        return None
    return _err(
        "%s 不可用：pywin32 导入失败（%s）。窗口识别与输入都依赖 pywin32，"
        "常见原因是环境里存在多份 pywintypes DLL、PATH 上那份旧版被抢先加载，"
        "可在 MCP Server 所在环境执行 pip install --force-reinstall pywin32 修复。"
        % (tool, _PYWIN32_ERROR)
    )


def _missing_deps_error(tool: str, needs: tuple = ("mss", "uiautomation")) -> Optional[str]:
    """按工具真正的依赖报告缺失项：截图只需要 mss，控件工具才需要 uiautomation。"""
    if _PYWIN32_ERROR is not None:
        return _pywin32_error(tool)
    missing = []
    if "mss" in needs and _MSS_ERROR:
        missing.append("mss (%s)" % _MSS_ERROR)
    if "uiautomation" in needs and _UIA_ERROR:
        missing.append("uiautomation (%s)" % _UIA_ERROR)
    if not missing:
        return None
    return _err(
        "%s 不可用：缺少依赖 %s。请在 MCP Server 所在环境执行 "
        "pip install -r requirements.txt 安装 mss 与 uiautomation。"
        % (tool, "、".join(missing))
    )


def _ok(payload) -> str:
    """统一包装成功响应。"""
    return json.dumps({"status": "success", "result": payload}, ensure_ascii=False)


def _err(msg: str) -> str:
    """统一包装错误响应。"""
    return json.dumps({"status": "error", "result": msg}, ensure_ascii=False)


def _no_window_error(window_title: Optional[str]) -> str:
    """找不到窗口时，把「为什么找不到」讲清楚，避免模型反复猜测标题。"""
    found, report = _scan_target_windows(None, include_hidden=True)
    detail = [
        "未找到 GridStar 窗口。判据是窗口所属进程的可执行文件名 %s，不是窗口标题。"
        % "、".join(TARGET_PROCESS_NAMES)
    ]
    detail.append(
        "本次扫描：顶层窗口 %s 个，参与判定的可见窗口 %s 个，进程名读不到 %s 个。"
        % (report["top_level_windows"], report["visible_windows"],
           report["process_name_unreadable"])
    )
    if window_title:
        detail.append("当前还附加了标题过滤 '%s'。" % window_title)
    if found:
        titles = ["%s（pid=%s，可见=%s，最小化=%s）"
                  % (item["title"] or "<无标题>", item["pid"], item["is_visible"], item["is_iconic"])
                  for item in found]
        detail.append("已找到同进程窗口：%s，但没有一个满足标题过滤。" % "；".join(titles))
    else:
        detail.append("当前系统里没有该进程的窗口（GridStar 可能未启动，"
                      "或进程名不同，可用环境变量 GRIDSTAR_PROCESS_NAMES 覆盖）。")
        decoys = _title_decoys([os.path.splitext(n)[0] for n in TARGET_PROCESS_NAMES], 0)
        if decoys:
            detail.append(
                "另外，标题里含该名称、但进程不是目标进程的窗口有：%s。"
                "这些正是「按标题识别」会误认的对象，不要拿它们当 GridStar 用。"
                % "；".join("%s（%s, pid=%s）" % (d["title"] or "<无标题>", d["process_name"], d["pid"])
                            for d in decoys)
            )
    return _err(" ".join(detail))


# ── 前台设置 ─────────────────────────────────────────────────────

def _tap_alt() -> None:
    """补发一次 Alt 键。

    SetForegroundWindow 有一条「只有当前拥有输入焦点的进程才能设置前台窗口」的
    限制，模拟一次按键输入可让本进程满足该条件（dsh-orb-cordis 的 becomeForeground
    用了同样的手法）。
    """
    try:
        _user32.keybd_event(_VK_MENU, 0, 0, None)
        _user32.keybd_event(_VK_MENU, 0, _KEYEVENTF_KEYUP, None)
    except Exception:
        pass


def _send_syscommand(hwnd: int, command: int, timeout_ms: int = 1000) -> bool:
    """给窗口发一条 WM_SYSCOMMAND，返回是否被处理。

    用 SendMessageTimeoutW 而不是 SendMessage：目标窗口若是未响应状态，
    SendMessage 会把本线程一起挂死，超时版本会自己返回。
    """
    try:
        result = ctypes.c_size_t(0)
        sent = _user32.SendMessageTimeoutW(
            wintypes.HWND(hwnd), _WM_SYSCOMMAND, command, 0,
            _SMTO_ABORTIFHUNG, timeout_ms, ctypes.byref(result),
        )
        return bool(sent)
    except Exception as exc:
        logger.debug("WM_SYSCOMMAND(0x%X) 发送失败: %s", command, exc)
        return False


def _restore_window(hwnd: int, settle_s: float = _UI_SETTLE_S) -> bool:
    """把最小化的窗口还原，返回它是否已不在最小化状态。

    还原走 WM_SYSCOMMAND/SC_RESTORE，而不是 ShowWindow：当 GridStar 以更高
    完整性级别运行时，UIPI 会静默丢弃下层进程对上层窗口的窗口状态操作，
    ShowWindow(SW_RESTORE)、ShowWindowAsync、SetWindowPos 全部无效，而
    WM_SYSCOMMAND/SC_RESTORE 能真正把它还原（本机逐项实测的结论）。
    ShowWindow 只作为同权限场景下的兜底。
    """
    try:
        if not win32gui.IsIconic(hwnd):
            return True
    except Exception as exc:
        logger.debug("IsIconic(%s) 失败: %s", hwnd, exc)
        return False

    _send_syscommand(hwnd, _SC_RESTORE)
    deadline = time.monotonic() + _RESTORE_TIMEOUT_S
    while time.monotonic() < deadline:
        time.sleep(_FOREGROUND_RETRY_S)
        try:
            if not win32gui.IsIconic(hwnd):
                time.sleep(settle_s)
                return True
        except Exception:
            return False
    try:
        win32gui.ShowWindow(hwnd, win32con.SW_RESTORE)
        time.sleep(settle_s)
        restored = not win32gui.IsIconic(hwnd)
    except Exception:
        restored = False
    if not restored:
        logger.info("还原窗口 %s 失败：已尝试 WM_SYSCOMMAND/SC_RESTORE 与 ShowWindow", hwnd)
    return restored


def _force_foreground(hwnd: int) -> bool:
    """前台锁的硬拆法：把自己的输入队列临时挂到当前前台线程上，再抢前台。

    SetForegroundWindow 有一条限制：只有「当前前台进程」或者刚拿到用户输入的进程
    才能改前台窗口。用户此刻正在别的程序里打字/点鼠标时，前台锁握在那个进程手里，
    常规路径（SetForegroundWindow + 补按 Alt）会一直失败。AttachThreadInput 让两个
    线程共用一个输入队列，前台限制随之失效，这是 Windows 自带的绕行办法；用完
    立刻摘掉，不长期共用队列。

    只作为常规路径失败后的最后手段，并且当前前台线程如果已经卡死
    （IsHungAppWindow）就直接放弃，避免挂在 AttachThreadInput 上。
    """
    try:
        fg = int(_user32.GetForegroundWindow() or 0)
        if not fg or fg == int(hwnd):
            return _is_foreground_and_visible(hwnd)
        if _user32.IsHungAppWindow(wintypes.HWND(fg)):
            return False
        fg_tid = _user32.GetWindowThreadProcessId(wintypes.HWND(fg), None)
        our_tid = _kernel32.GetCurrentThreadId()
        attached = False
        if fg_tid and fg_tid != our_tid:
            attached = bool(_user32.AttachThreadInput(fg_tid, our_tid, True))
        try:
            _user32.BringWindowToTop(wintypes.HWND(hwnd))
            _user32.SetForegroundWindow(wintypes.HWND(hwnd))
        finally:
            if attached:
                _user32.AttachThreadInput(fg_tid, our_tid, False)
    except Exception:
        logger.debug("_force_foreground 失败 hwnd=%s", hwnd, exc_info=True)
        return False
    time.sleep(_FOREGROUND_RETRY_S)
    return _is_foreground_and_visible(hwnd)


def _bring_to_foreground(hwnd: int, timeout_s: float = _FOREGROUND_TIMEOUT_S) -> bool:
    """把窗口还原并切到前台，返回是否真的达成。

    pywin32 的 SetForegroundWindow 有时会抛出 'error 0'，返回值不可靠，
    所以判据始终是「前台窗口是否真的变成了它」。判据里还必须带上
    not IsIconic：最小化的窗口本身就可以是前台窗口，只看前台会把
    「仍然最小化」误判成切换成功（本机实测：iconic=True 时
    GetForegroundWindow() 就已经等于目标 hwnd）。
    """
    _restore_window(hwnd)
    deadline = time.monotonic() + timeout_s
    while True:
        if _is_foreground_and_visible(hwnd):
            return True
        try:
            win32gui.SetForegroundWindow(hwnd)
        except Exception:
            pass
        if _is_foreground_and_visible(hwnd):
            return True
        if time.monotonic() >= deadline:
            # 常规路径超时：用户可能正在别的程序里操作，前台锁握在它手里。
            # 这时才动用「临时共用输入队列」这一手，代价是会把焦点从用户手上抢走。
            return _force_foreground(hwnd)
        _tap_alt()
        time.sleep(_FOREGROUND_RETRY_S)


def _is_foreground_and_visible(hwnd: int) -> bool:
    """前台窗口是它，并且它不在最小化状态。"""
    try:
        if int(win32gui.GetForegroundWindow()) != int(hwnd):
            return False
        return not win32gui.IsIconic(hwnd)
    except Exception:
        return False


# ── 截图 ─────────────────────────────────────────────────────────

def _virtual_screen() -> tuple:
    try:
        return (
            int(win32api.GetSystemMetrics(_SM_XVIRTUALSCREEN)),
            int(win32api.GetSystemMetrics(_SM_YVIRTUALSCREEN)),
            int(win32api.GetSystemMetrics(_SM_CXVIRTUALSCREEN)),
            int(win32api.GetSystemMetrics(_SM_CYVIRTUALSCREEN)),
        )
    except Exception:
        return (0, 0, 1920, 1080)


def _clamp_to_virtual_screen(rect: dict) -> dict:
    """窗口可以超出虚拟屏（部分在屏外），抓图前必须裁剪，否则 mss 取到黑边或报错。"""
    vx, vy, vw, vh = _virtual_screen()
    left = max(int(rect["left"]), vx)
    top = max(int(rect["top"]), vy)
    right = min(int(rect["right"]), vx + vw)
    bottom = min(int(rect["bottom"]), vy + vh)
    return {
        "left": left, "top": top,
        "width": max(0, right - left), "height": max(0, bottom - top),
    }


def _grab_png(region: dict) -> bytes:
    with mss.mss() as sct:
        shot = sct.grab(region)
        return mss.tools.to_png(shot.rgb, shot.size)


def _buffer_dominant_ratio(data: bytes, sample_limit: int = 20000) -> float:
    """抽样统计「出现最多的那一种颜色占多大比例」，用来判断图是不是空白。

    全量遍历 200 万像素太慢，按步长抽样两万个点足够看出是不是整片单色。
    """
    total_px = len(data) // 4
    if total_px <= 0:
        return 1.0
    step = max(1, total_px // max(1, sample_limit))
    view = memoryview(data)
    counts: dict = {}
    seen = 0
    for index in range(0, total_px, step):
        offset = index * 4
        key = bytes(view[offset:offset + 4])
        counts[key] = counts.get(key, 0) + 1
        seen += 1
    if not seen:
        return 1.0
    return max(counts.values()) / float(seen)


def _grab_window_clean(hwnd: int, frame_rect: dict, region: dict) -> tuple:
    """用 PrintWindow(PW_RENDERFULLCONTENT) 让窗口把自己的绘制结果画到内存 DC。

    与区域抓屏的差别：抓的是窗口自身内容，压在 GridStar 上面的其它窗口不会被
    拍进去。代价是有些画面（GPU 直出、没实现 WM_PRINT 的控件）画不出来，
    所以要么返回可用的 PNG，要么返回原因，绝不悄悄给一张黑图。

    Args:
        hwnd: 目标窗口句柄。
        frame_rect: 窗口的真实边框矩形（含 left/top/right/bottom），PrintWindow
            按窗口自身尺寸渲染，越界部分由 region 裁掉。
        region: 期望输出的矩形（含 left/top/width/height），通常是 frame_rect 裁剪
            到虚拟屏之后的结果，保证与坐标工具的基准矩形一致。

    Returns:
        (png_bytes, reason, warning)。png_bytes 为 None 时 reason 说明失败原因；
        warning 非空表示图可用但可疑（大面积单色，可能画得不完整）。
    """
    if mss is None:
        return None, "mss 不可用", ""
    frame_w = int(frame_rect["right"]) - int(frame_rect["left"])
    frame_h = int(frame_rect["bottom"]) - int(frame_rect["top"])
    if frame_w <= 0 or frame_h <= 0:
        return None, "窗口矩形为空（%sx%s）" % (frame_w, frame_h), ""
    dx = max(0, int(region["left"]) - int(frame_rect["left"]))
    dy = max(0, int(region["top"]) - int(frame_rect["top"]))
    want_w = min(int(region["width"]), frame_w - dx)
    want_h = min(int(region["height"]), frame_h - dy)
    if want_w <= 0 or want_h <= 0:
        return None, "裁剪后的矩形为空（dx=%s dy=%s）" % (dx, dy), ""

    hwnd_dc = None
    mem_dc = None
    bitmap = None
    old_obj = None
    try:
        hwnd_dc = _user32.GetWindowDC(wintypes.HWND(hwnd))
        if not hwnd_dc:
            return None, "GetWindowDC 失败（错误码 %s）" % ctypes.get_last_error(), ""
        mem_dc = _gdi32.CreateCompatibleDC(hwnd_dc)
        bitmap = _gdi32.CreateCompatibleBitmap(hwnd_dc, frame_w, frame_h)
        if not mem_dc or not bitmap:
            return None, "创建兼容 DC/位图失败", ""
        old_obj = _gdi32.SelectObject(mem_dc, bitmap)
        if not _user32.PrintWindow(wintypes.HWND(hwnd), mem_dc, _PW_RENDERFULLCONTENT):
            return None, "PrintWindow 返回失败（错误码 %s）" % ctypes.get_last_error(), ""

        header = _BITMAPINFOHEADER()
        header.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
        header.biWidth = frame_w
        header.biHeight = -frame_h  # 负数 = 自上而下，省一次行翻转
        header.biPlanes = 1
        header.biBitCount = 32
        header.biCompression = _BI_RGB
        bmi = _BITMAPINFO()
        bmi.bmiHeader = header
        stride = frame_w * 4
        buffer = ctypes.create_string_buffer(stride * frame_h)
        lines = _gdi32.GetDIBits(
            mem_dc, bitmap, 0, frame_h, buffer, ctypes.byref(bmi), _DIB_RGB_COLORS
        )
        if lines != frame_h:
            return None, "GetDIBits 只取到 %s/%s 行" % (lines, frame_h), ""
        data = buffer.raw
        if dx or dy or want_w != frame_w or want_h != frame_h:
            data = b"".join(
                data[(dy + row) * stride + dx * 4:(dy + row) * stride + dx * 4 + want_w * 4]
                for row in range(want_h)
            )
        ratio = _buffer_dominant_ratio(data)
        if ratio >= _BLANK_DOMINANT_RATIO:
            return None, (
                "PrintWindow 画出的是单色空白图（单色占比 %.1f%%）" % (ratio * 100.0)
            ), ""
        warning = ""
        if ratio >= _SUSPECT_DOMINANT_RATIO:
            warning = (
                "PrintWindow 结果里 %.1f%% 的像素是同一个颜色，画面可能不完整"
                "（走 OpenGL 直出的视图 PrintWindow 常常画成一片黑）" % (ratio * 100.0)
            )
        return mss.tools.to_png(data, (want_w, want_h)), "", warning
    except Exception as exc:
        return None, "PrintWindow 异常: %s" % exc, ""
    finally:
        try:
            if mem_dc and old_obj:
                _gdi32.SelectObject(mem_dc, old_obj)
            if bitmap:
                _gdi32.DeleteObject(bitmap)
            if mem_dc:
                _gdi32.DeleteDC(mem_dc)
            if hwnd_dc:
                _user32.ReleaseDC(wintypes.HWND(hwnd), hwnd_dc)
        except Exception:
            pass


# ── 坐标输入：SendInput 注入鼠标 ──────────────────────────────────

_BUTTON_FLAGS = {
    "left": (_MOUSEEVENTF_LEFTDOWN, _MOUSEEVENTF_LEFTUP),
    "right": (_MOUSEEVENTF_RIGHTDOWN, _MOUSEEVENTF_RIGHTUP),
    "middle": (_MOUSEEVENTF_MIDDLEDOWN, _MOUSEEVENTF_MIDDLEUP),
}
_MOVE_FLAGS = _MOUSEEVENTF_MOVE | _MOUSEEVENTF_ABSOLUTE | _MOUSEEVENTF_VIRTUALDESK


def _cursor_pos() -> Optional[tuple]:
    point = _POINT()
    if not _user32.GetCursorPos(ctypes.byref(point)):
        return None
    return (int(point.x), int(point.y))


def _to_absolute(px: int, py: int) -> tuple:
    """物理像素 → SendInput 要求的 0-65535 虚拟屏归一化坐标。

    绝对坐标默认以主屏为基准，多屏（含负坐标的副屏）必须带上 VIRTUALDESK
    标志，否则副屏上的点会算到主屏之外。
    """
    vx, vy, vw, vh = _virtual_screen()
    nx = int(round((int(px) - vx) * 65535.0 / max(1, vw - 1)))
    ny = int(round((int(py) - vy) * 65535.0 / max(1, vh - 1)))
    return max(0, min(65535, nx)), max(0, min(65535, ny))


def _send_mouse_events(events: list) -> int:
    """投递鼠标事件，返回被系统接受的条数（0 说明注入被挡下来了）。"""
    if not events:
        return 0
    batch = (_INPUT * len(events))()
    for index, (dx, dy, mouse_data, flags) in enumerate(events):
        batch[index].type = _INPUT_MOUSE
        batch[index].mi.dx = int(dx)
        batch[index].mi.dy = int(dy)
        batch[index].mi.mouseData = int(mouse_data)
        batch[index].mi.dwFlags = int(flags)
    return int(_user32.SendInput(len(events), batch, ctypes.sizeof(_INPUT)))


def _button_flags(button: str) -> tuple:
    key = (button or "left").strip().lower()
    if key not in _BUTTON_FLAGS:
        raise ValueError("不支持的鼠标键 %r，可选：left / right / middle" % button)
    return _BUTTON_FLAGS[key]


def _resolve_point(x: float, y: float, coord_space: str, rect: dict) -> tuple:
    """把调用方给的坐标换算成屏幕物理像素。

    默认 normalized：0-1000 归一化，相对 capture_rect 算。模型看到的截图会被
    缩放，像素坐标并不可信，归一化坐标只依赖画面比例，缩放后依然成立。
    pixel 模式则直接当成 capture_rect 内的像素偏移。

    Returns:
        (像素 x, 像素 y)。
    """
    space = (coord_space or "normalized").strip().lower()
    try:
        fx = float(x)
        fy = float(y)
    except (TypeError, ValueError):
        raise ValueError("坐标必须是数字，收到 x=%r y=%r" % (x, y))
    left = int(rect["left"])
    top = int(rect["top"])
    width = int(rect["width"])
    height = int(rect["height"])
    if width <= 0 or height <= 0:
        raise ValueError(
            "基准矩形是空的（%s），窗口可能处于最小化状态或完全在屏幕之外" % (rect,)
        )
    if space in ("normalized", "norm", "ratio", "0-1000"):
        if not (0.0 <= fx <= _COORD_MAX and 0.0 <= fy <= _COORD_MAX):
            raise ValueError(
                "归一化坐标范围是 0-1000，收到 x=%s y=%s" % (fx, fy)
            )
        px = left + int(round(fx * (width - 1) / _COORD_MAX))
        py = top + int(round(fy * (height - 1) / _COORD_MAX))
    elif space in ("pixel", "px"):
        if not (0.0 <= fx <= width - 1 and 0.0 <= fy <= height - 1):
            raise ValueError(
                "像素坐标必须落在截图矩形内（0-%s / 0-%s），收到 x=%s y=%s"
                % (width - 1, height - 1, fx, fy)
            )
        px = left + int(round(fx))
        py = top + int(round(fy))
    else:
        raise ValueError("coord_space 只支持 normalized（默认）或 pixel，收到 %r" % coord_space)
    # 归一化取整后可能正好越界一像素，夹回矩形内
    px = max(left, min(left + width - 1, px))
    py = max(top, min(top + height - 1, py))
    return px, py


def _point_payload(info: dict, region: dict, x: float, y: float, coord_space: str,
                   px: int, py: int, injected: int, expected: int,
                   cursor: Optional[tuple], extra: dict) -> dict:
    """坐标类工具统一的成功回报：把「给了什么坐标、落在哪个像素」讲清楚。"""
    payload = {
        "point": {
            "input": {"x": x, "y": y, "coord_space": (coord_space or "normalized").lower()},
            "normalized": {
                "x": round((px - region["left"]) * _COORD_MAX / max(1, region["width"] - 1), 1),
                "y": round((py - region["top"]) * _COORD_MAX / max(1, region["height"] - 1), 1),
            },
            "screen_pixel": {"x": px, "y": py},
        },
        "coord_space": {"max": 1000, "rect": region},
        "window": {
            "hwnd": info["hwnd"], "pid": info["pid"], "process_name": info.get("process_name"),
            "title": info["title"], "frame_rect": info["frame_rect"],
            "is_foreground": bool(info["is_foreground"]), "is_iconic": bool(info["is_iconic"]),
        },
        "injected": injected,
        "events_expected": expected,
        "cursor_after": {"x": cursor[0], "y": cursor[1]} if cursor else None,
        "cursor_on_target": bool(cursor and cursor == (px, py)),
        "verified": False,
        "verification_note": (
            "只能验证光标是否真的落到了目标像素，无法验证 GridStar 是否响应该操作"
            "（它不提供任何可读的界面回执）。要确认结果请再截一张图看画面变化。"
        ),
    }
    payload.update(extra)
    return payload


def _point_context(window_title: str, tool: str) -> tuple:
    """坐标类工具的公共前置：定位窗口 → UIPI 闸门 → 还原并切前台 → 取基准矩形。

    坐标点击落在屏幕像素上，窗口不在前台就会点到别的窗口，所以这里把「必须
    真的在前台」当作硬条件，不满足直接拒绝。

    Returns:
        (info, region, activated, error_json)。error_json 非空时必须原样返回给调用方。
    """
    win_error = _pywin32_error(tool)
    if win_error:
        return None, None, False, win_error
    info = _find_gridstar_window(window_title or None)
    if info is None:
        return None, None, False, _no_window_error(window_title)
    allowed, reason = _input_gate(info)
    if not allowed:
        return None, None, False, _err("已识别到 GridStar 进程，但无法向它发送输入：%s" % reason)

    activated = False
    if info["is_iconic"]:
        _restore_window(info["hwnd"])
        info = _find_gridstar_window(window_title or None) or info
    if not _is_foreground_and_visible(info["hwnd"]):
        activated = _bring_to_foreground(info["hwnd"])
        info = _find_gridstar_window(window_title or None) or info
    if not _is_foreground_and_visible(info["hwnd"]):
        return None, None, activated, _err(
            "坐标输入前没能把 GridStar 切到前台（hwnd=%s，is_iconic=%s，当前前台 hwnd=%s）。"
            "坐标事件落在屏幕像素上，窗口不在前台时会点到别的窗口，已拒绝执行。"
            "若 GridStar 以管理员权限运行而本服务不是，请以相同权限运行本服务后重试。"
            % (info["hwnd"], info["is_iconic"], win32gui.GetForegroundWindow())
        )

    region = _clamp_to_virtual_screen(info["frame_rect"])
    if region["width"] <= 0 or region["height"] <= 0:
        return None, None, activated, _err(
            "GridStar 窗口完全位于屏幕之外，无法做坐标操作：frame_rect=%s" % info["frame_rect"]
        )
    return info, region, activated, None


def _hit_test_error(info: dict, px: int, py: int) -> Optional[str]:
    """这一像素上现在压着的，是不是目标窗口自己。

    注入的鼠标消息按命中测试派发：像素上如果压着别的窗口（编辑器、悬浮球、桌面宠物、
    另一个程序的弹窗），事件就落到它身上了。与其照点不误再回报一句 success，
    不如先问系统「这个像素上是谁」，不是目标程序就拒绝。
    """
    try:
        hit = win32gui.WindowFromPoint((int(px), int(py)))
    except Exception:
        return None
    if not hit:
        return _err(
            "目标像素 (%s, %s) 上没有窗口（可能落在屏幕之外或虚拟屏的空隙），已拒绝执行。"
            % (px, py)
        )
    try:
        top = win32gui.GetAncestor(hit, win32con.GA_ROOT) or hit
    except Exception:
        top = hit
    if top == info["hwnd"]:
        return None
    try:
        if int(_window_pid(hit)) == int(info["pid"]):
            return None
    except Exception:
        pass
    title = ""
    cls = ""
    try:
        title = win32gui.GetWindowText(hit) or ""
        cls = win32gui.GetClassName(hit) or ""
    except Exception:
        pass
    return _err(
        "目标像素 (%s, %s) 上压着别的窗口（hwnd=%s，类 %s，标题「%s」），鼠标事件会落到它身上，"
        "已拒绝执行。请先把它移开、最小化或关掉，再重试。"
        % (px, py, hit, cls, title[:60])
    )


def _mouse_click(px: int, py: int, button: str = "left", clicks: int = 1) -> dict:
    """点击。每条事件自带绝对坐标：定位与按键在同一条 INPUT 里投递，系统先移动再按下，
    中间没有缝隙，所以此刻用户自己（或别的程序）在动鼠标也偷不走这一下。
    双击必须分两次投递并留出时间间隔，系统才认。"""
    down, up = _button_flags(button)
    count = max(1, min(3, int(clicks)))
    ax, ay = _to_absolute(px, py)
    pair = [(ax, ay, 0, _MOVE_FLAGS | down), (ax, ay, 0, _MOVE_FLAGS | up)]
    injected = _send_mouse_events(pair)
    for _ in range(1, count):
        time.sleep(_CLICK_GAP_S)
        injected += _send_mouse_events(pair)
    time.sleep(_PRESS_SETTLE_S)
    return {
        "injected": injected,
        "expected": 2 * count,
        "cursor": _cursor_pos(),
        "button": (button or "left").lower(),
        "clicks": count,
    }


def _mouse_drag(px0: int, py0: int, px1: int, py1: int, button: str = "left",
                duration_ms: int = 400) -> dict:
    """按下 → 分步移动到终点 → 抬起。

    中间必须真的投递 WM_MOUSEMOVE：3D 视图的旋转、拖拽全靠拖动过程中的
    移动事件累计角度，只给起点和终点是转不动的。
    """
    down, up = _button_flags(button)
    total_ms = max(0, min(5000, int(duration_ms or 0)))
    steps = max(_DRAG_STEPS, min(40, int(total_ms / 25) or _DRAG_STEPS))
    pause = (total_ms / 1000.0) / steps if total_ms else 0.0
    start = _to_absolute(px0, py0)
    end = _to_absolute(px1, py1)
    injected = _send_mouse_events([(start[0], start[1], 0, _MOVE_FLAGS | down)])
    for index in range(1, steps + 1):
        ratio = index / float(steps)
        mix = int(round(start[0] + (end[0] - start[0]) * ratio))
        miy = int(round(start[1] + (end[1] - start[1]) * ratio))
        injected += _send_mouse_events([(mix, miy, 0, _MOVE_FLAGS)])
        if pause:
            time.sleep(pause)
    injected += _send_mouse_events([(end[0], end[1], 0, _MOVE_FLAGS | up)])
    time.sleep(_PRESS_SETTLE_S)
    return {
        "injected": injected,
        "expected": 2 + steps,
        "cursor": _cursor_pos(),
        "button": (button or "left").lower(),
        "steps": steps,
        "duration_ms": total_ms,
    }


def _mouse_wheel(px: int, py: int, notches: int) -> dict:
    """滚动滚轮。滚轮消息发给光标下的窗口，所以每条滚轮事件都自带绝对坐标，
    先把光标钉到目标点再滚，避免滚到别的窗口上去。"""
    count = max(-20, min(20, int(notches)))
    if count == 0:
        raise ValueError("amount 不能是 0（正数向上滚、负数向下滚）")
    ax, ay = _to_absolute(px, py)
    step = _WHEEL_DELTA if count > 0 else -_WHEEL_DELTA
    injected = 0
    for _ in range(abs(count)):
        injected += _send_mouse_events([(ax, ay, step, _MOVE_FLAGS | _MOUSEEVENTF_WHEEL)])
        time.sleep(_PRESS_SETTLE_S)
    return {
        "injected": injected,
        "expected": abs(count),
        "cursor": _cursor_pos(),
        "amount": count,
        "notches": abs(count),
    }


# ── UIA：固定在一个专用 STA 线程上 ────────────────────────────────

class _StaTimeout(RuntimeError):
    pass


class _StaWorker:
    """把 uiautomation 的全部调用固定在同一个 STA 线程上执行。

    uiautomation 的 UIA 客户端是进程级单例（uiautomation.py:50-58），且它明确要求
    「不能把某个线程创建的 Control/Pattern 交给另一个线程使用」
    （uiautomation.py:10252-10259）。FastMCP 对同步工具走线程池，调用线程不固定，
    直接在调用线程里用会撞 RPC_E_WRONG_THREAD。这里用队列把请求串行化到一个
    自带 COM 初始化的常驻线程。
    """

    def __init__(self, name: str = "gridstar-uia"):
        self._queue: "queue.Queue" = queue.Queue()
        self._ready = threading.Event()
        self._init_error: Optional[BaseException] = None
        self._thread = threading.Thread(target=self._run, name=name, daemon=True)
        self._thread.start()

    def _run(self) -> None:
        try:
            if uia is not None:
                uia.InitializeUIAutomationInCurrentThread()  # comtypes.CoInitializeEx()
            else:
                pythoncom.CoInitializeEx(pythoncom.COINIT_APARTMENTTHREADED)
        except BaseException as exc:  # noqa: BLE001 - 启动失败也要让调用方看到原因
            self._init_error = exc
        finally:
            self._ready.set()
        try:
            while True:
                item = self._queue.get()
                if item is None:
                    return
                fn, box, done = item
                try:
                    box["value"] = fn()
                except BaseException as exc:  # noqa: BLE001 - 异常要原样带回调用线程
                    box["error"] = exc
                finally:
                    done.set()
        finally:
            try:
                if uia is not None:
                    uia.UninitializeUIAutomationInCurrentThread()
            except Exception:
                pass

    def call(self, fn: Callable, timeout: float):
        if not self._ready.wait(10):
            raise _StaTimeout("UIA 工作线程启动超时")
        if self._init_error is not None:
            raise RuntimeError("UIA 初始化失败：%s" % self._init_error)
        box: dict = {}
        done = threading.Event()
        self._queue.put((fn, box, done))
        if not done.wait(timeout):
            raise _StaTimeout("UIA 调用超过 %.1f 秒未返回" % timeout)
        if "error" in box:
            raise box["error"]
        return box.get("value")


_sta_lock = threading.Lock()
_sta_worker: Optional[_StaWorker] = None


def _sta_call(fn: Callable, timeout: float = _UIA_CALL_TIMEOUT_S):
    """在专用 STA 线程上执行 fn。超时则判定线程卡死并重建，避免后续调用全部排队等死。"""
    global _sta_worker
    with _sta_lock:
        if _sta_worker is None:
            _sta_worker = _StaWorker()
        worker = _sta_worker
    try:
        return worker.call(fn, timeout)
    except _StaTimeout:
        with _sta_lock:
            if _sta_worker is worker:
                _sta_worker = None  # 卡死的线程无法回收，换一个新的继续服务
        raise


def _uia_root(hwnd: int):
    """在 STA 线程内把 HWND 换成 UIA 根元素（截图与操作从此用同一个窗口身份）。"""
    root = uia.ControlFromHandle(hwnd)
    if root is None:
        raise RuntimeError("ControlFromHandle 失败：hwnd=%s 无效或窗口已销毁" % hwnd)
    return root


def _uia_root_for(info: dict):
    """取 UIA 根元素并回校验进程 id，防止 hwnd 被复用后操作到别的进程。"""
    root = _uia_root(info["hwnd"])
    try:
        pid = int(root.ProcessId)
    except Exception:
        pid = info["pid"]
    if pid != info["pid"]:
        raise RuntimeError(
            "窗口身份校验失败：hwnd=%s 当前属于 pid=%s，而识别结果是 pid=%s"
            "（窗口可能已关闭、句柄被复用）" % (info["hwnd"], pid, info["pid"])
        )
    return root


# ── 控件查找与序列化 ─────────────────────────────────────────────

def _match_predicate(name: Optional[str], automation_id: Optional[str]) -> Callable:
    """统一的匹配语义：名称大小写不敏感子串，AutomationId 大小写不敏感全等。"""
    lowered_name = (name or "").lower()
    lowered_aid = (automation_id or "").lower()

    def _compare(control, _depth) -> bool:
        if lowered_name and lowered_name not in (control.Name or "").lower():
            return False
        if lowered_aid and (control.AutomationId or "").lower() != lowered_aid:
            return False
        return True

    return _compare


def _find_control_dfs(root, name: Optional[str], automation_id: Optional[str],
                      depth: int):
    """兜底实现：有界深度优先遍历。仅在原生查找不可用时使用。"""
    predicate = _match_predicate(name, automation_id)

    def _search(parent, remaining: int):
        if remaining <= 0:
            return None
        try:
            children = parent.GetChildren()
        except Exception:
            return None
        for child in children:
            try:
                if predicate(child, depth - remaining):
                    return child
            except Exception:
                continue
            found = _search(child, remaining - 1)
            if found is not None:
                return found
        return None

    return _search(root, depth)


def _find_control(root, name: Optional[str] = None,
                  automation_id: Optional[str] = None, depth: int = 10):
    """查找控件：优先 uiautomation 原生查找，失败回退到 DFS。

    原生查找把整棵子树交给 UIA 的跨进程搜索一次完成，而手工 DFS 是每个节点
    若干次跨进程调用，深度 10 的树差别是数量级的。
    """
    if uia is None:
        return None
    try:
        control = uia.Control(
            searchFromControl=root, searchDepth=int(depth),
            Compare=_match_predicate(name, automation_id),
        )
        if control.Exists(0):
            return control
    except Exception as exc:
        logger.debug("原生控件查找失败，回退 DFS: %s", exc)
    return _find_control_dfs(root, name, automation_id, int(depth))


# 只在控件类型确实支持时才查询对应模式，避免每个节点付 4 次跨进程调用
_PATTERN_BY_TYPE = {
    "CheckBoxControl": "toggle",
    "RadioButtonControl": "toggle",
    "ListItemControl": "selection",
    "TabItemControl": "selection",
    "TreeItemControl": "selection",
    "SliderControl": "range",
    "SpinnerControl": "range",
    "ProgressBarControl": "range",
}


def _describe_control(control) -> dict:
    """序列化单个 UIA 控件为可读 dict。"""
    try:
        r = control.BoundingRectangle
        rect = {"left": r.left, "top": r.top, "right": r.right, "bottom": r.bottom}
    except Exception:
        rect = None
    type_name = control.ControlTypeName
    info = {
        "type": type_name,
        "name": control.Name,
        "class_name": control.ClassName,
        "automation_id": control.AutomationId,
        "rect": rect,
        "is_enabled": bool(control.IsEnabled),
        "is_visible": not bool(control.IsOffscreen),
    }
    try:
        vp = control.GetValuePattern()
        if vp:
            info["value"] = vp.Value
    except Exception:
        pass
    wanted = _PATTERN_BY_TYPE.get(type_name or "")
    try:
        if wanted == "toggle":
            tp = control.GetTogglePattern()
            if tp:
                info["toggle_state"] = str(tp.ToggleState)
        elif wanted == "selection":
            sip = control.GetSelectionItemPattern()
            if sip:
                info["is_selected"] = bool(sip.IsSelected)
        elif wanted == "range":
            rvp = control.GetRangeValuePattern()
            if rvp:
                info["range_value"] = {"value": rvp.Value, "min": rvp.Minimum, "max": rvp.Maximum}
    except Exception:
        pass
    return info


def _enumerate_control_tree(control, max_depth: int,
                            type_filter: Optional[str] = None,
                            cur_depth: int = 0,
                            max_siblings: int = 30,
                            budget: Optional[list] = None) -> list:
    """遍历 UIA 控件树。

    type_filter 命中的控件才做序列化（被过滤掉的节点仍然向下遍历，但不再付出
    每个节点约 6 次跨进程调用的描述成本），命中节点的层级会被压平。
    """
    if budget is None:
        budget = [300]
    items: list = []
    if cur_depth > max_depth or budget[0] <= 0:
        return items
    try:
        children = control.GetChildren()
    except Exception:
        return items
    for child in children[:max_siblings]:
        if budget[0] <= 0:
            break
        sub = []
        if cur_depth < max_depth:
            sub = _enumerate_control_tree(
                child, max_depth, type_filter, cur_depth + 1, max_siblings, budget,
            )
        keep = not type_filter or child.ControlTypeName == type_filter
        if not keep:
            items.extend(sub)
            continue
        budget[0] -= 1
        try:
            desc = _describe_control(child)
        except Exception as exc:
            logger.debug("控件描述失败: %s", exc)
            continue
        if sub:
            desc["children"] = sub
        items.append(desc)
    return items


# ── 文本输入 ─────────────────────────────────────────────────────

def _read_control_value(control) -> Optional[str]:
    try:
        vp = control.GetValuePattern()
        return vp.Value if vp else None
    except Exception:
        return None


def _paste_text(text: str) -> bool:
    """用剪贴板粘贴代替逐字符 SendKeys。

    逐字符模拟在 Qt 界面里会因为输入法/焦点竞态丢字，粘贴是一次原子操作。
    剪贴板原文若是文本则还原；图文/文件类型的剪贴板内容无法用文本 API 还原，
    此时只记录日志不假装成功。
    """
    original: Optional[str] = None
    try:
        original = uia.GetClipboardText()
    except Exception:
        original = None
    try:
        if not uia.SetClipboardText(text):
            return False
        time.sleep(_CLIPBOARD_SETTLE_S)
        uia.SendKeys("{Ctrl}v")
        time.sleep(_PASTE_SETTLE_S)
        return True
    except Exception as exc:
        logger.warning("剪贴板粘贴失败: %s", exc)
        return False
    finally:
        if original is not None:
            try:
                uia.SetClipboardText(original)
            except Exception:
                pass


def _type_into(control, text: str, clear_first: bool) -> dict:
    """向控件写入文本，返回实际使用的方式与读回校验结果（在 STA 线程内执行）。"""
    result: dict = {"method": None, "verified": False}
    # 1) ValuePattern.SetValue：原子操作，不依赖焦点与剪贴板
    try:
        vp = control.GetValuePattern()
        if vp and not vp.IsReadOnly:
            vp.SetValue(text)
            time.sleep(_CLIPBOARD_SETTLE_S)
            value = _read_control_value(control)
            result.update(method="value_pattern", verified=(value == text), value=value)
            if result["verified"]:
                return result
    except Exception as exc:
        logger.debug("ValuePattern 写入失败: %s", exc)
    # 2) 聚焦后粘贴
    try:
        control.SetFocus()
    except Exception:
        try:
            control.Click()
        except Exception:
            pass
    time.sleep(_UI_SETTLE_S)
    if clear_first:
        try:
            control.SendKeys("{Ctrl}a")
            time.sleep(_CLIPBOARD_SETTLE_S)
            control.SendKeys("{Delete}")
            time.sleep(_CLIPBOARD_SETTLE_S)
        except Exception as exc:
            logger.debug("清空原有内容失败: %s", exc)
    if _paste_text(text):
        result.update(method="clipboard_paste")
    else:
        try:
            control.SendKeys(text)
            result.update(method="sendkeys")
        except Exception as exc:
            result.update(method="failed", error=str(exc))
            return result
    time.sleep(_PASTE_SETTLE_S)
    value = _read_control_value(control)
    result["value"] = value
    result["verified"] = (value == text) if value is not None else None
    return result


# ── 破坏性快捷键闸门 ──────────────────────────────────────────────

_DESTRUCTIVE_KEY_PATTERNS = ("{alt}{f4}", "alt+f4", "{alt}{space}", "alt+space")


def _is_destructive_shortcut(keys: str) -> bool:
    normalized = (keys or "").replace(" ", "").lower()
    return any(pattern in normalized for pattern in _DESTRUCTIVE_KEY_PATTERNS)


# ── MCP 工具函数 ─────────────────────────────────────────────────

def FindGridStarWindow(window_title: str = "", include_hidden: bool = False) -> str:
    """通过进程定位 GridStar 窗口，返回它的身份、位置与可操作性。

    识别判据是窗口所属进程的可执行文件名（默认 gridstar.exe），不是窗口标题，
    因此不会把标题里恰好含 "GridStar" 的编辑器窗口（例如打开本仓库的 VS Code）误认成 GridStar。
    返回结果里的 exe/pid 可以核对到底是哪个程序，transient_windows 列出同进程的对话框。
    输入类操作前建议先调用本工具确认 input_blocked 为 false（管理员权限不一致时 UIPI 会拦掉输入）。

    Args:
        window_title: 可选，进程匹配之后的标题过滤关键字（不再用于识别进程）。
        include_hidden: 是否把隐藏窗口也算作候选（默认 False，只看可见窗口）。

    Returns:
        JSON 字符串。成功时 result 含 window（hwnd/pid/exe/title/rect/is_foreground/
        input_blocked 等）、other_windows（同进程的其它窗口）与 matched_by。
    """
    missing = _pywin32_error("FindGridStarWindow")
    if missing:
        return missing
    info = _find_gridstar_window(window_title or None, include_hidden)
    if info is None:
        return _no_window_error(window_title)
    others = info.pop("transient_windows", [])
    return _ok({
        "matched_by": "process",
        "process_names": list(TARGET_PROCESS_NAMES),
        "window": info,
        "other_windows": others,
    })


def ActivateGridStarWindow(window_title: str = "") -> str:
    """把 GridStar 窗口切到前台，并验证切换是否真的成功。

    先按进程定位窗口，最小化则还原，然后调用 SetForegroundWindow 并用
    GetForegroundWindow 回校验（SetForegroundWindow 的返回值本身不可靠，
    失败时会做一次 Alt 键补发后重试）。

    Args:
        window_title: 可选，进程匹配之后的标题过滤关键字。

    Returns:
        JSON 字符串，包含 action、is_foreground（真实前台状态）与窗口信息。
        切换失败时 status 仍为 success，但 is_foreground 为 false 且带 error 说明。
    """
    missing = _pywin32_error("ActivateGridStarWindow")
    if missing:
        return missing
    info = _find_gridstar_window(window_title or None)
    if info is None:
        return _no_window_error(window_title)
    hwnd = info["hwnd"]
    activated = _bring_to_foreground(hwnd)
    refreshed = _find_gridstar_window(window_title or None) or info
    refreshed.pop("transient_windows", None)
    payload = {
        "action": "activate",
        "is_foreground": bool(activated),
        "window": refreshed,
    }
    if not activated:
        payload["error"] = (
            "已找到 GridStar 窗口（hwnd=%s）但无法把它切到前台；"
            "前台窗口仍是 hwnd=%s。可能是系统前台锁定或权限级别不一致。"
            % (hwnd, win32gui.GetForegroundWindow())
        )
    return _ok(payload)


def CaptureGridStarWindow(window_title: str = "", activate: bool = True,
                          clean: bool = False) -> str:
    """截取 GridStar 窗口的可见区域截图。

    截图矩形取自 DWM 的实际可见边框（DWMWA_EXTENDED_FRAME_BOUNDS），并裁剪到
    虚拟屏幕范围内：GetWindowRect 含一圈 DWM 隐形边框，直接用会多出黑边，
    窗口部分移出屏幕时又会让抓图越界。
    默认先把窗口切到前台再截图（activate=True），否则截到的可能是压在 GridStar
    上面的别的窗口；结果里的 is_foreground 会如实反映这一点。

    Args:
        window_title: 可选，进程匹配之后的标题过滤关键字。
        activate: 是否先把 GridStar 切到前台再截图（默认 True）。
        clean: 是否改用 PrintWindow 抓窗口自身内容（默认 False）。为 True 时不会
            把压在 GridStar 上面的窗口拍进来，但若视口是 OpenGL 直出，PrintWindow
            可能画不出内容；那些情况会自动回退到区域抓取，并在结果的 method /
            clean_fallback 里说明实际用了哪种方式。

    Returns:
        JSON 字符串。成功时 result 含 image（base64 PNG）、width、height、format、
        method（region=区域抓屏 / printwindow=窗口自绘）、capture_rect（坐标工具的
        基准矩形），以及窗口身份与 is_foreground 等现场信息。
        图片部分会被 Agent 侧转成附件图片发送给模型，不会以 base64 文本进上下文。
    """
    missing = _missing_deps_error("CaptureGridStarWindow", needs=("mss",))
    if missing:
        return missing
    info = _find_gridstar_window(window_title or None)
    if info is None:
        return _no_window_error(window_title)

    activated = False
    if info["is_iconic"]:
        # 最小化窗口没有可截的矩形（rect 全是 -32000），还原是截图的前提，
        # 与「要不要切前台」无关，所以放在 activate 判断之前无条件尝试。
        _restore_window(info["hwnd"])
        info = _find_gridstar_window(window_title or None) or info
    if activate:
        activated = _bring_to_foreground(info["hwnd"])
        # 还原/切前台可能改变窗口位置，重新采集一次事实再截图
        info = _find_gridstar_window(window_title or None) or info

    if info["is_iconic"]:
        return _err(
            "GridStar 窗口处于最小化状态且还原失败，无法截图：hwnd=%s。"
            "已尝试 WM_SYSCOMMAND/SC_RESTORE 与 ShowWindow；"
            "若 GridStar 以管理员权限运行而本服务不是，请以相同权限运行本服务后重试。"
            % info["hwnd"]
        )

    region = _clamp_to_virtual_screen(info["frame_rect"])
    if region["width"] <= 0 or region["height"] <= 0:
        return _err("GridStar 窗口完全位于屏幕之外，无法截图：frame_rect=%s" % info["frame_rect"])

    method = "region"
    clean_warning = ""
    clean_fallback = ""
    png_bytes = None
    if clean:
        png_bytes, clean_reason, clean_warning = _grab_window_clean(
            info["hwnd"], info["frame_rect"], region
        )
        if png_bytes is not None:
            method = "printwindow"
        else:
            clean_fallback = clean_reason
    if png_bytes is None:
        try:
            png_bytes = _grab_png(region)
        except Exception as exc:
            logger.error("截图失败: %s", exc)
            detail = "（clean=True 的窗口自绘路径也不可用：%s）" % clean_fallback if clean_fallback else ""
            return _err("截图失败: %s%s" % (exc, detail))

    payload = {
        "image": base64.b64encode(png_bytes).decode("utf-8"),
        "width": region["width"],
        "height": region["height"],
        "format": "png",
        "method": method,
        "clean_requested": bool(clean),
        "capture_rect": region,
        "coord_space": {
            "max": 1000,
            "rect": region,
            "usage": "ClickAtPoint / DragAtPoint / ScrollAtPoint 默认按 0-1000 归一化坐标落在 capture_rect 内",
        },
        "window": {
            "hwnd": info["hwnd"], "pid": info["pid"], "exe": info["exe"],
            "title": info["title"], "class_name": info["class_name"],
            "frame_rect": info["frame_rect"],
        },
        "is_foreground": bool(info["is_foreground"]),
        "activated": bool(activated),
        "dpi_aware": bool(_DPI_AWARE),
    }
    if clean_fallback:
        payload["clean_fallback"] = (
            "clean=True 的窗口自绘（PrintWindow）路径拿不到可用画面，已回退区域抓取：%s"
            % clean_fallback
        )
    if clean_warning:
        payload["clean_warning"] = clean_warning
    if not info["is_foreground"]:
        payload["occluded_warning"] = (
            "截图时 GridStar 不是前台窗口，画面里可能包含压在上面的其它窗口"
            "（前台 hwnd=%s）。需要干净画面时先调用 ActivateGridStarWindow。"
            % win32gui.GetForegroundWindow()
        )
    return _ok(payload)


def _root_child_count(root) -> int:
    """根元素下的直接子控件数；读不到时返回 -1（未知，不能当成空树）。"""
    try:
        return len(root.GetChildren() or [])
    except Exception as exc:
        logger.debug("读取根元素子控件数失败: %s", exc)
        return -1


_EMPTY_TREE_NOTE = (
    "该窗口的 UIA 控件树是空的：根元素下没有任何子控件。这不是「没找到某个控件」，"
    "而是这个界面根本没有向上层暴露控件——Qt 程序未启用无障碍桥、或界面完全自绘时就是这样"
    "（本机对 GridStar 实测：还原前后根元素的子控件数都是 0）。"
    "控件级定位在此窗口上不可用，请改用 CaptureGridStarWindow 截图，按画面坐标操作。"
)


def GetUIElementInfo(control_name: str = "", depth: int = 3,
                     control_type: str = "", window_title: str = "") -> str:
    """读取 GridStar 窗口的 UI 控件树。

    控件树的根元素由识别到的 hwnd 直接换来（ControlFromHandle），并回校验
    控件所属进程 id，保证读的是截图上那个窗口，而不是另一个同名窗口。
    所有 UIA 调用都固定在一个专用 STA 线程上执行，不受 MCP 线程池调度影响。

    Args:
        control_name: 可选，查找名称含该子串的控件，只返回它的详细信息。
        depth: 控件树遍历深度（默认 3；越深越慢，且总节点数有上限）。
        control_type: 可选，按类型筛选，如 'ButtonControl'、'EditControl'、'TreeItemControl'。
            指定后只返回该类型的控件，层级会被压平。
        window_title: 可选，进程匹配之后的标题过滤关键字。

    Returns:
        JSON 字符串，包含控件信息列表（含 name/type/automation_id/rect/value 等）。
    """
    missing = _missing_deps_error("GetUIElementInfo")
    if missing:
        return missing
    info = _find_gridstar_window(window_title or None)
    if info is None:
        return _no_window_error(window_title)

    def _work():
        root = _uia_root_for(info)
        root_children = _root_child_count(root)
        if control_name:
            control = _find_control(root, name=control_name, depth=max(depth, 10))
            if control is None or not control.Exists(0):
                return {"found": False, "root_children": root_children}
            return {"found": True, "root_children": root_children,
                    "control": _describe_control(control)}
        return {
            "found": True,
            "root_children": root_children,
            "tree": _enumerate_control_tree(root, max_depth=max(0, depth),
                                            type_filter=control_type or None),
        }

    try:
        result = _sta_call(_work)
    except _StaTimeout as exc:
        return _err("读取控件树超时：%s。该界面可能正在自绘或被模态对话框阻塞。" % exc)
    except Exception as exc:
        logger.error("获取 UI 控件信息失败: %s", exc)
        return _err("获取 UI 控件信息失败: %s" % exc)

    window_ref = {"hwnd": info["hwnd"], "pid": info["pid"], "title": info["title"]}
    if not result.get("found"):
        message = "未找到控件: name='%s'（窗口 hwnd=%s 内）" % (control_name, info["hwnd"])
        if not result.get("root_children"):
            return _err(message + "。" + _EMPTY_TREE_NOTE)
        return _err(message)
    if control_name:
        payload = {"window": window_ref, "control": result["control"]}
    else:
        payload = {"window": window_ref, "controls": result["tree"]}
    if not result.get("root_children"):
        payload["note"] = _EMPTY_TREE_NOTE
    return _ok(payload)


def ClickUIElement(name: str = "", automation_id: str = "",
                   window_title: str = "") -> str:
    """点击 GridStar 窗口中的指定 UI 控件。

    点击前会先把 GridStar 切到前台并确认切换成功（`Control.Click()` 是真实鼠标
    模拟，窗口不在前台时点到的会是别的窗口），并检查完整性级别（UIPI）：
    GridStar 以管理员运行时输入会被系统静默丢弃，此时直接返回错误而不是假成功。

    Args:
        name: 控件名称，大小写不敏感子串匹配（如 '生成网格' 匹配 '生成表面网格'）。
        automation_id: 控件的 AutomationId，大小写不敏感全等匹配。
        window_title: 可选，进程匹配之后的标题过滤关键字。

    Returns:
        JSON 字符串，包含点击的控件信息与点击后窗口 id/前台状态。
    """
    missing = _missing_deps_error("ClickUIElement")
    if missing:
        return missing
    if not name and not automation_id:
        return _err("必须提供 name 或 automation_id")
    info = _find_gridstar_window(window_title or None)
    if info is None:
        return _no_window_error(window_title)
    allowed, reason = _input_gate(info)
    if not allowed:
        return _err("已识别到 GridStar 进程，但无法向它发送输入：%s" % reason)
    if not _bring_to_foreground(info["hwnd"]):
        return _err(
            "无法把 GridStar 切到前台（当前前台 hwnd=%s），此时点击会落到别的窗口上，已中止。"
            % win32gui.GetForegroundWindow()
        )

    def _work():
        root = _uia_root_for(info)
        control = _find_control(root, name or None, automation_id or None, depth=12)
        if control is None or not control.Exists(0):
            return None, _root_child_count(root)
        clicked = _describe_control(control)
        control.Click()
        return clicked, None

    try:
        clicked, root_children = _sta_call(_work)
    except _StaTimeout as exc:
        return _err("点击超时：%s" % exc)
    except Exception as exc:
        logger.error("点击失败: %s", exc)
        return _err("点击失败: %s" % exc)
    if clicked is None:
        detail = "name='%s'" % name if name else "automation_id='%s'" % automation_id
        message = "未找到控件: %s（窗口 hwnd=%s 内）" % (detail, info["hwnd"])
        if not root_children:
            return _err(message + "。" + _EMPTY_TREE_NOTE)
        return _err(message)

    time.sleep(_UI_SETTLE_S)
    return _ok({
        "action": "click",
        "control": clicked,
        "window": {"hwnd": info["hwnd"], "pid": info["pid"], "title": info["title"]},
        "is_foreground": int(win32gui.GetForegroundWindow()) == int(info["hwnd"]),
    })


def TypeTextInUIElement(text: str, name: str = "", automation_id: str = "",
                        clear_first: bool = True, window_title: str = "") -> str:
    """向 GridStar 窗口的指定输入框写入文本。

    优先用 UIA 的 ValuePattern.SetValue（原子写入，不抢焦点、不碰剪贴板），
    不支持时退化为聚焦后经剪贴板粘贴（Ctrl+V），最后才逐字符 SendKeys。
    写入后会读回控件当前值做校验，结果里的 method 与 verified 说明实际用了哪种方式、
    有没有写进去，不做「以为输入了」的静默成功。

    Args:
        text: 要写入的文本。
        name: 控件名称，大小写不敏感子串匹配。
        automation_id: 控件的 AutomationId，大小写不敏感全等匹配。
        clear_first: 退化到键盘输入时是否先清空原有内容（默认 True）。
        window_title: 可选，进程匹配之后的标题过滤关键字。

    Returns:
        JSON 字符串，包含 method（value_pattern/clipboard_paste/sendkeys）、
        verified（读回是否等于写入内容）与控件信息。
    """
    missing = _missing_deps_error("TypeTextInUIElement")
    if missing:
        return missing
    if not name and not automation_id:
        return _err("必须提供 name 或 automation_id")
    info = _find_gridstar_window(window_title or None)
    if info is None:
        return _no_window_error(window_title)
    allowed, reason = _input_gate(info)
    if not allowed:
        return _err("已识别到 GridStar 进程，但无法向它发送输入：%s" % reason)

    set_value_only = not clear_first

    def _work():
        root = _uia_root_for(info)
        control = _find_control(root, name or None, automation_id or None, depth=12)
        if control is None or not control.Exists(0):
            return None, _root_child_count(root)
        target = _describe_control(control)
        outcome = _type_into(control, text, clear_first=clear_first)
        return {"control": target, "outcome": outcome}, None

    try:
        result, root_children = _sta_call(_work)
    except _StaTimeout as exc:
        return _err("输入超时：%s" % exc)
    except Exception as exc:
        logger.error("输入文本失败: %s", exc)
        return _err("输入文本失败: %s" % exc)
    if result is None:
        detail = "name='%s'" % name if name else "automation_id='%s'" % automation_id
        message = "未找到控件: %s（窗口 hwnd=%s 内）" % (detail, info["hwnd"])
        if not root_children:
            return _err(message + "。" + _EMPTY_TREE_NOTE)
        return _err(message)

    outcome = result["outcome"]
    payload = {
        "action": "type_text",
        "control": result["control"],
        "method": outcome.get("method"),
        "verified": outcome.get("verified"),
        "current_value": outcome.get("value"),
        "window": {"hwnd": info["hwnd"], "pid": info["pid"], "title": info["title"]},
    }
    if set_value_only and outcome.get("method") == "value_pattern":
        payload["note"] = "clear_first=False 时按整值替换处理"
    return _ok(payload)


def SendKeyboardShortcut(keys: str, window_title: str = "", allow_close: bool = False) -> str:
    """向 GridStar 窗口发送键盘快捷键。

    发送前会把 GridStar 切到前台并确认成功，检查 UIPI 完整性级别，并确认
    前台窗口确实变成了目标窗口：否则快捷键会打到别的程序上（在编辑器里被按到
    Ctrl+S 这类组合键的代价很高）。

    Args:
        keys: 快捷键，格式遵循 uiautomation.SendKeys，例如 "Ctrl+S"（保存）、
            "Ctrl+Z"（撤销）、"Ctrl+C"（复制）、"{Control}{Shift}E"。
        window_title: 可选，进程匹配之后的标题过滤关键字。
        allow_close: 是否允许 Alt+F4 / Alt+Space 这类会关闭或收起窗口的组合，
            默认 False（需要显式确认才放行）。

    Returns:
        JSON 字符串，包含实际发送的按键与发送时的前台窗口状态。
    """
    missing = _missing_deps_error("SendKeyboardShortcut")
    if missing:
        return missing
    if not keys or not keys.strip():
        return _err("必须提供 keys")
    if _is_destructive_shortcut(keys) and not allow_close:
        return _err(
            "拒绝发送 %s：该组合会关闭或收起 GridStar 窗口。"
            "确认要这么做时请显式传入 allow_close=True。" % keys
        )
    info = _find_gridstar_window(window_title or None)
    if info is None:
        return _no_window_error(window_title)
    allowed, reason = _input_gate(info)
    if not allowed:
        return _err("已识别到 GridStar 进程，但无法向它发送输入：%s" % reason)
    if not _bring_to_foreground(info["hwnd"]):
        return _err(
            "无法把 GridStar 切到前台（当前前台 hwnd=%s），快捷键会打到别的程序上，已中止。"
            % win32gui.GetForegroundWindow()
        )
    try:
        uia.SendKeys(keys)
    except Exception as exc:
        logger.error("发送快捷键失败: %s", exc)
        return _err("发送快捷键失败: %s" % exc)
    time.sleep(_UI_SETTLE_S)
    return _ok({
        "action": "send_shortcut",
        "keys": keys,
        "window": {"hwnd": info["hwnd"], "pid": info["pid"], "title": info["title"]},
        "is_foreground": int(win32gui.GetForegroundWindow()) == int(info["hwnd"]),
    })


# ── 坐标操作：GridStar 不暴露 UIA 控件，只能按画面坐标动手 ─────────


def ClickAtPoint(x: float, y: float, coord_space: str = "normalized",
                 button: str = "left", clicks: int = 1,
                 window_title: str = "") -> str:
    """在 GridStar 窗口的指定画面坐标上点击。

    这条通道是为 GridStar 这类「不提供任何 UI 自动化控件」的程序准备的：它的 UIA
    控件树是空的，找不到可点的元素，但画面是能看见的，所以先截图、看图定位、再按
    坐标点击。

    前置条件（不满足一律拒绝，不会盲点）：GridStar 进程存在且完整性级别不高于本
    进程（否则 UIPI 会丢掉输入）、窗口已从最小化还原并确实处于前台、目标点落在
    截图矩形内、目标点上压着的确实是 GridStar 自己的窗口（否则事件会落到别人身上）。

    注意：这里只能确认光标真的落到了目标像素，无法确认 GridStar 是否响应。点击
    可能触发不可逆的操作（生成网格、删除几何），所以先用 CaptureGridStarWindow
    看清画面再点，点完再截一张图确认结果。

    Args:
        x: 横坐标。默认 0-1000 归一化，0=左边缘，1000=右边缘。
        y: 纵坐标。默认 0-1000 归一化，0=上边缘，1000=下边缘。
        coord_space: "normalized"（默认）或 "pixel"。
        button: "left"（默认）/ "right" / "middle"。右键常用于弹出上下文菜单。
        clicks: 点击次数，1（默认）单击、2 双击。
        window_title: 可选，进程匹配之后的标题过滤关键字。

    Returns:
        JSON 字符串，含实际落点的屏幕像素、归一化回读值、注入事件条数与光标校验。
    """
    win_error = _pywin32_error("ClickAtPoint")
    if win_error:
        return win_error
    try:
        _button_flags(button)
        if int(clicks) < 1 or int(clicks) > 3:
            raise ValueError("clicks 只支持 1-3，收到 %r" % clicks)
    except ValueError as exc:
        return _err(str(exc))

    info, region, activated, context_error = _point_context(window_title, "ClickAtPoint")
    if context_error:
        return context_error
    try:
        px, py = _resolve_point(x, y, coord_space, region)
    except ValueError as exc:
        return _err("%s（基准矩形 capture_rect=%s）" % (exc, region))

    hit_error = _hit_test_error(info, px, py)
    if hit_error:
        return hit_error

    outcome = _mouse_click(px, py, button=button, clicks=int(clicks))
    payload = _point_payload(
        info, region, x, y, coord_space, px, py,
        outcome["injected"], outcome["expected"], outcome["cursor"],
        {
            "action": "click_at_point",
            "button": outcome["button"],
            "clicks": outcome["clicks"],
            "activated": bool(activated),
            "dpi_aware": bool(_DPI_AWARE),
        },
    )
    if outcome["injected"] < outcome["expected"]:
        payload["injection_warning"] = (
            "SendInput 只接受了 %s/%s 个事件，部分输入可能没进系统输入流。"
            % (outcome["injected"], outcome["expected"])
        )
    return _ok(payload)


def DragAtPoint(from_x: float, from_y: float, to_x: float, to_y: float,
                coord_space: str = "normalized", button: str = "left",
                duration_ms: int = 400, window_title: str = "") -> str:
    """在 GridStar 窗口里按下、拖动、松开（用于旋转/平移三维视图、拖拽对象）。

    拖动过程会分步投递中间的鼠标移动事件：三维视图的旋转角度是靠拖动过程中的
    移动累计出来的，只给起点和终点转不动。

    前置条件与 ClickAtPoint 相同（进程存在、UIPI 放行、窗口真的在前台、两点都在
    截图矩形内）。和点击一样，这里无法确认 GridStar 是否真的响应了拖动。

    Args:
        from_x: 起点横坐标（默认 0-1000 归一化）。
        from_y: 起点纵坐标（默认 0-1000 归一化）。
        to_x: 终点横坐标。
        to_y: 终点纵坐标。
        coord_space: "normalized"（默认）或 "pixel"。
        button: "left"（默认）/ "right" / "middle"。
        duration_ms: 拖动耗时，默认 400 毫秒（越长越平滑，上限 5000）。
        window_title: 可选，进程匹配之后的标题过滤关键字。

    Returns:
        JSON 字符串，含起点/终点的屏幕像素、注入事件条数与光标最终位置。
    """
    win_error = _pywin32_error("DragAtPoint")
    if win_error:
        return win_error
    try:
        _button_flags(button)
    except ValueError as exc:
        return _err(str(exc))

    info, region, activated, context_error = _point_context(window_title, "DragAtPoint")
    if context_error:
        return context_error
    try:
        px0, py0 = _resolve_point(from_x, from_y, coord_space, region)
        px1, py1 = _resolve_point(to_x, to_y, coord_space, region)
    except ValueError as exc:
        return _err("%s（基准矩形 capture_rect=%s）" % (exc, region))

    for hx, hy in ((px0, py0), (px1, py1)):
        hit_error = _hit_test_error(info, hx, hy)
        if hit_error:
            return hit_error

    outcome = _mouse_drag(px0, py0, px1, py1, button=button, duration_ms=duration_ms)
    payload = _point_payload(
        info, region, to_x, to_y, coord_space, px1, py1,
        outcome["injected"], outcome["expected"], outcome["cursor"],
        {
            "action": "drag_at_point",
            "button": outcome["button"],
            "from_screen_pixel": {"x": px0, "y": py0},
            "steps": outcome["steps"],
            "duration_ms": outcome["duration_ms"],
            "activated": bool(activated),
            "dpi_aware": bool(_DPI_AWARE),
        },
    )
    payload["point"]["input_from"] = {
        "x": from_x, "y": from_y, "coord_space": (coord_space or "normalized").lower()
    }
    if outcome["injected"] < outcome["expected"]:
        payload["injection_warning"] = (
            "SendInput 只接受了 %s/%s 个事件，拖动可能不完整。"
            % (outcome["injected"], outcome["expected"])
        )
    return _ok(payload)


def ScrollAtPoint(x: float, y: float, amount: int,
                  coord_space: str = "normalized", window_title: str = "") -> str:
    """在 GridStar 窗口的指定画面坐标上滚动滚轮（缩放视图、滚动列表）。

    滚轮消息发给光标下的窗口，所以会先把光标移到目标点再滚。

    前置条件与 ClickAtPoint 相同（进程存在、UIPI 放行、窗口真的在前台、点落在
    截图矩形内）。

    Args:
        x: 横坐标（默认 0-1000 归一化）。
        y: 纵坐标（默认 0-1000 归一化）。
        amount: 滚动格数，正数向上/向前（放大），负数向下/向后（缩小），非 0。
        coord_space: "normalized"（默认）或 "pixel"。
        window_title: 可选，进程匹配之后的标题过滤关键字。

    Returns:
        JSON 字符串，含落点屏幕像素、实际滚动格数与光标位置。
    """
    win_error = _pywin32_error("ScrollAtPoint")
    if win_error:
        return win_error
    # 参数校验放在定位窗口之前：参数写错时不该报成权限/窗口问题
    try:
        notches = int(amount)
    except (TypeError, ValueError):
        return _err("amount 必须是整数，收到 %r" % (amount,))
    if notches == 0:
        return _err("amount 不能是 0（正数向上滚/放大，负数向下滚/缩小）")
    if abs(notches) > 20:
        return _err("amount 一次最多 ±20 格，收到 %s" % notches)

    info, region, activated, context_error = _point_context(window_title, "ScrollAtPoint")
    if context_error:
        return context_error
    try:
        px, py = _resolve_point(x, y, coord_space, region)
    except ValueError as exc:
        return _err("%s（基准矩形 capture_rect=%s）" % (exc, region))

    hit_error = _hit_test_error(info, px, py)
    if hit_error:
        return hit_error

    outcome = _mouse_wheel(px, py, notches)

    payload = _point_payload(
        info, region, x, y, coord_space, px, py,
        outcome["injected"], outcome["expected"], outcome["cursor"],
        {
            "action": "scroll_at_point",
            "amount": outcome["amount"],
            "notches": outcome["notches"],
            "activated": bool(activated),
            "dpi_aware": bool(_DPI_AWARE),
        },
    )
    if outcome["injected"] < outcome["expected"]:
        payload["injection_warning"] = (
            "SendInput 只接受了 %s/%s 个事件，部分滚动可能没生效。"
            % (outcome["injected"], outcome["expected"])
        )
    return _ok(payload)