# -*- coding: utf-8 -*-
"""GridStarAgent 桌面入口。

由「启动.bat」用包内 runtime\\python.exe 拉起，做四件事：

  1. 检查 WebView2 运行时。缺了就明确报出来，不让用户对着白屏猜。
  2. 把 MCP(5656) 与 Web(1231) 两个服务起来。端口已在监听就复用，
     绝不起第二个实例。
  3. 两个端口都就绪后再开窗口，避免窗口先出来、页面打不开。
  4. 窗口关掉时只结束本次拉起的进程，不动用户自己开着的浏览器版服务。

窗口用系统标题栏（Snap Layouts、拖边吸附、双击最大化都保留），
只把标题栏刷成深色，跟界面配色对齐。

想继续用浏览器访问的老路径没有被拆掉：同目录下的「启动_浏览器版.bat」。
"""

from __future__ import annotations

import ctypes
import os
import socket
import subprocess
import sys
import time
import webbrowser
from pathlib import Path

ROOT = Path(__file__).resolve().parent
AGENT_DIR = ROOT / "agent"
APP_DIR = ROOT / "agent" / "agent"
PY = Path(sys.executable)

MCP_PORT = int(os.environ.get("GRIDSTAR_MCP_PORT", "5656"))
WEB_PORT = int(os.environ.get("GRIDSTAR_WEB_PORT", "1231"))
HOST = "127.0.0.1"

WINDOW_TITLE = "GridStar Agent"
MUTEX_NAME = "Global\\GridStarAgentDesktop.v1"
WEBVIEW2_CLIENT = "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"

# Windows 上不让子进程弹控制台窗口
CREATE_NO_WINDOW = 0x08000000
CREATE_NEW_PROCESS_GROUP = 0x00000200


# ------------------------------------------------------------------ 数据目录
def _data_dir() -> Path:
    """优先用 agent 自己的 paths 模块，保证跟后台写的是同一个地方。"""
    try:
        if str(APP_DIR) not in sys.path:
            sys.path.insert(0, str(APP_DIR))
        import paths  # noqa: WPS433 - 延迟导入，桌面入口不该依赖后台能起来

        return Path(paths.DATA_DIR)
    except Exception:
        return Path.home() / "AppData" / "Roaming" / "GridstarAgent.data"


DATA_DIR = _data_dir()
LOG_DIR = DATA_DIR / "logs"

# 控制台是 GBK 时个别符号可能编不出来，不要因为一行日志把程序打断
try:
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass


def say(msg: str = "") -> None:
    print(msg, flush=True)


# ------------------------------------------------------------ WebView2 检测
def webview2_version() -> str:
    """返回 WebView2 运行时版本号，没装返回空串。

    没有它 pywebview 会退到 mshtml（IE11），界面直接白屏，而且报错
    完全指不到根因，所以启动前先问一次。
    """
    if os.name != "nt":
        return ""
    import winreg

    keys = (
        r"SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\%s" % WEBVIEW2_CLIENT,
        r"SOFTWARE\Microsoft\EdgeUpdate\Clients\%s" % WEBVIEW2_CLIENT,
    )
    for path in keys:
        for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
            try:
                with winreg.OpenKey(hive, path) as key:
                    value, _ = winreg.QueryValueEx(key, "pv")
                    if value:
                        return str(value)
            except OSError:
                continue
    return ""


# ---------------------------------------------------------------- 单实例锁
def take_mutex():
    """返回 (句柄, 是否已有实例)。句柄要留住，回收就没了。"""
    if os.name != "nt":
        return None, False
    kernel32 = ctypes.windll.kernel32
    handle = kernel32.CreateMutexW(None, False, MUTEX_NAME)
    already = kernel32.GetLastError() == 183  # ERROR_ALREADY_EXISTS
    return handle, already


def focus_existing() -> bool:
    """把已经在跑的那个窗口叫到前面来。"""
    if os.name != "nt":
        return False
    user32 = ctypes.windll.user32
    hwnd = user32.FindWindowW(None, WINDOW_TITLE)
    if not hwnd:
        return False
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    user32.SetForegroundWindow(hwnd)
    return True


# ------------------------------------------------------------------ 端口
def port_busy(port: int) -> bool:
    sock = socket.socket()
    sock.settimeout(0.4)
    try:
        return sock.connect_ex((HOST, port)) == 0
    finally:
        sock.close()


def wait_port(port: int, timeout: float, proc: "subprocess.Popen | None" = None) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if port_busy(port):
            return True
        # 进程已经死了就别再干等
        if proc is not None and proc.poll() is not None:
            return False
        time.sleep(0.4)
    return False


# ---------------------------------------------------------------- 起服务
class Services:
    """本次会话拉起的子进程。关窗时只清理自己起的那些。"""

    def __init__(self) -> None:
        self.procs: list[tuple[str, subprocess.Popen]] = []
        LOG_DIR.mkdir(parents=True, exist_ok=True)

    def _spawn(self, label: str, script: Path, cwd: Path, args: list[str]) -> subprocess.Popen:
        log_path = LOG_DIR / ("desktop-%s.log" % label)
        log_file = open(log_path, "ab")
        log_file.write(b"\n==== %s %s ====\n" % (label.encode(), time.strftime("%Y-%m-%d %H:%M:%S").encode()))
        log_file.flush()
        flags = 0
        if os.name == "nt":
            flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP
        proc = subprocess.Popen(
            [str(PY), str(script)] + args,
            cwd=str(cwd),
            stdout=log_file,
            stderr=subprocess.STDOUT,
            creationflags=flags,
        )
        self.procs.append((label, proc))
        say("   %s 已拉起 (pid %d)，日志 %s" % (label, proc.pid, log_path.name))
        return proc

    def ensure(self) -> bool:
        say("== 检查服务")

        if port_busy(MCP_PORT):
            say("   MCP   %d 已在监听，复用" % MCP_PORT)
        else:
            say("   MCP   %d 未启动，拉起 server.py" % MCP_PORT)
            proc = self._spawn("mcp", AGENT_DIR / "server.py", AGENT_DIR, [])
            if not wait_port(MCP_PORT, 90, proc):
                self.tail("mcp")
                return False
            say("   MCP   %d 就绪" % MCP_PORT)

        if port_busy(WEB_PORT):
            say("   Web   %d 已在监听，复用" % WEB_PORT)
        else:
            say("   Web   %d 未启动，拉起 app.py" % WEB_PORT)
            proc = self._spawn(
                "web", APP_DIR / "app.py", APP_DIR,
                ["--host", HOST, "--port", str(WEB_PORT)],
            )
            if not wait_port(WEB_PORT, 150, proc):
                self.tail("web")
                return False
            say("   Web   %d 就绪" % WEB_PORT)
        return True

    def tail(self, label: str, lines: int = 20) -> None:
        path = LOG_DIR / ("desktop-%s.log" % label)
        say("   [失败] %s 没起来，最近日志：" % label)
        try:
            text = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            text = []
        for line in (text or ["(无日志)"])[-lines:]:
            say("   " + line)

    def shutdown(self) -> None:
        if not self.procs:
            return
        say("== 结束本次拉起的服务")
        for label, proc in self.procs:
            if proc.poll() is not None:
                continue
            try:
                subprocess.run(
                    ["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                    capture_output=True,
                    creationflags=CREATE_NO_WINDOW if os.name == "nt" else 0,
                )
                say("   %s (pid %d) 已结束" % (label, proc.pid))
            except Exception as exc:  # noqa: BLE001 - 退出路径上不再抛
                say("   %s 结束失败: %s" % (label, exc))


# ------------------------------------------------------ 系统标题栏刷深色
def paint_caption(hwnd: int) -> None:
    """把系统标题栏和边框刷成界面底色。

    四个调用在 Win11 上返回 0 表示成功。Win10 上 34/35/36 会失败，
    只剩暗色模式生效，这里不报错，退化成系统默认外观。
    """
    if os.name != "nt" or not hwnd:
        return
    try:
        dwm = ctypes.windll.dwmapi

        def set_attr(attr: int, value: int) -> None:
            v = ctypes.c_uint32(value)
            dwm.DwmSetWindowAttribute(hwnd, attr, ctypes.byref(v), ctypes.sizeof(v))

        # COLORREF 是 0x00BBGGRR，跟 CSS 的 #RRGGBB 反着写
        set_attr(20, 1)             # DWMWA_USE_IMMERSIVE_DARK_MODE
        set_attr(35, 0x001E1610)    # DWMWA_CAPTION_COLOR  -> #10181E
        set_attr(36, 0x00E6E8EA)    # DWMWA_TEXT_COLOR     -> #EAE8E6
        set_attr(34, 0x002A2F33)    # DWMWA_BORDER_COLOR
    except Exception:
        pass


def after_shown() -> None:
    """窗口出来之后：找 hwnd 上色，顺便把焦点拿过来。"""
    if os.name != "nt":
        return
    user32 = ctypes.windll.user32
    hwnd = 0
    for _ in range(40):  # 窗口刚创建时 FindWindow 可能还找不到，短暂重试
        hwnd = user32.FindWindowW(None, WINDOW_TITLE)
        if hwnd:
            break
        time.sleep(0.1)
    if not hwnd:
        return
    paint_caption(hwnd)
    user32.ShowWindow(hwnd, 9)
    user32.SetForegroundWindow(hwnd)


# ------------------------------------------------------------------ 主流程
def browser_fallback(reason: str) -> int:
    """pywebview 用不了时退回浏览器，至少能用。"""
    say()
    say("== %s" % reason)
    say("   退回浏览器模式，界面功能不受影响")
    url = "http://%s:%d/ui/" % (HOST, WEB_PORT)
    say("   %s" % url)
    webbrowser.open(url)
    if os.name == "nt":
        say()
        say("   这个窗口可以直接关掉，服务在后台继续跑")
        say("   要停止服务请双击「停止.bat」")
        input("   按回车键退出本窗口…")
    return 0


def main() -> int:
    say("=" * 62)
    say("  GridStarAgent 桌面版")
    say("  根目录 : %s" % ROOT)
    say("  解释器 : %s" % PY)
    say("=" * 62)
    say()

    version = webview2_version()
    if os.name == "nt" and not version:
        say("== [警告] 没检测到 WebView2 运行时")
        say("   Windows 10 1803 之后的系统一般自带；没有的话界面会白屏。")
        say("   离线安装包：MicrosoftEdgeWebView2RuntimeInstallerX64.exe")
        say()
        return browser_fallback("缺少 WebView2 运行时")

    if version:
        say("== WebView2 运行时: %s" % version)

    try:
        import webview
    except Exception as exc:  # noqa: BLE001
        return browser_fallback("pywebview 未安装 (%s)" % exc)

    mutex, already = take_mutex()
    if already:
        say("== 已经有一个桌面窗口在运行")
        if not focus_existing():
            say("   没找到它的窗口，可能刚关掉，稍等几秒再双击")
        time.sleep(1.2)
        return 0

    services = Services()
    try:
        if not services.ensure():
            say()
            say("[失败] 服务没能起来，窗口不会打开。")
            say("       日志目录：%s" % LOG_DIR)
            if os.name == "nt":
                input("按回车键退出…")
            return 1

        say()
        say("== 打开窗口")
        webview.create_window(
            WINDOW_TITLE,
            "http://%s:%d/ui/" % (HOST, WEB_PORT),
            width=1200,
            height=820,
            min_size=(680, 460),
            background_color="#10181e",
        )
        # 这里会阻塞到窗口关闭
        webview.start(after_shown)
        return 0
    finally:
        services.shutdown()
        say()
        say("== 已退出")
        if mutex:
            try:
                ctypes.windll.kernel32.CloseHandle(mutex)
            except Exception:
                pass


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)