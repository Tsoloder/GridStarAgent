"""GridStar 主程序进程：确认它在不在跑，不在就把它拉起来。

WebUI 左上角的图标点下去要有反应，而「有没有反应」取决于一件很具体的事：
工业软件的主进程 ``GridStar.exe`` 此刻是否还在。这里只负责两件事：

1. ``running_pids()`` 按**可执行文件名**枚举进程（默认 ``gridstar.exe``，可用
   环境变量 ``GRIDSTAR_PROCESS_NAMES`` 覆盖，与 screen_automation 用同一个开关）。
   判据刻意不用窗口标题——标题里恰好带 GridStar 的编辑器不该被当成目标进程。
2. ``ensure_running()`` 已经在跑就原样返回，绝不再起第二个实例（两个实例去抢
   同一个工程文件是实打实的损失）；不在跑才拉起，并且等它真的出现在进程表里
   之后再回「已启动」。``subprocess.Popen`` 返回成功只说明系统受理了创建请求，
   初始化阶段就崩掉的程序同样会给出 pid，不核实就报成功等于骗人。

找不到程序这件事必须自己解决，不能甩给使用者去配环境变量——换台机器、换个
安装目录，路径就不一样了。``resolve_executable()`` 按下面的顺序找，命中即停：

1. 环境变量 ``GRIDSTAR_EXE``（显式指定优先，给个目录也行）
2. 上次记录的位置（数据目录里的 ``gridstar.json``，由本模块自己发现后写入）
3. 与本程序同一层、上一层、上两层的 ``GridStar.exe``（整包被放在工业软件旁边时命中）
4. 注册表：App Paths 与卸载项里 DisplayName 含 GridStar 的安装目录
5. 已在运行的 gridstar.exe 自己的真实路径（顺手记下来，供下次不在跑时使用）

权限是这条路上绕不开的另一环：GridStar.exe 的可执行文件带 requireAdministrator
清单，普通权限直接创建会被系统拒绝（WinError 740）。所以先按普通权限试，被拒
再走 ``ShellExecute("runas")`` 让系统弹 UAC 确认——既不强行假设要提权，也不把
「需要管理员」当成失败。

枚举走 ``CreateToolhelp32Snapshot``（纯标准库），不引入 psutil：离线包只装
requirements 里声明过的依赖，多一个隐式依赖就多一处装机后才暴露的失败。
"""
import ctypes
import json
import os
import subprocess
import time
from datetime import datetime
from pathlib import Path

from paths import APP_ROOT, DATA_DIR

# 与 tools/screen_automation.py 共用的进程名开关，两边指的是同一批进程
PROCESS_ENV = "GRIDSTAR_PROCESS_NAMES"
EXE_ENV = "GRIDSTAR_EXE"
_DEFAULT_PROCESS_NAMES = "gridstar.exe"
_DEFAULT_EXE_NAME = "GridStar.exe"

# 发现过的路径记在这里：下次换台机器、或者这次程序还没起来时直接复用
RECORD_FILE = Path(DATA_DIR) / "gridstar.json"

_TH32CS_SNAPPROCESS = 0x00000002
_INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
_MAX_PATH = 260
_MAX_IMAGE_PATH = _MAX_PATH * 4
_SW_SHOWNORMAL = 1
_ERROR_ELEVATION_REQUIRED = 740
_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

# 启动后等它出现在进程表里的默认预算。CreateProcess 返回即建档，
# 正常机器上远小于这个数；给足余量是为了机械硬盘上冷启动也能等到。
DEFAULT_WAIT_SECONDS = 8.0
# 需要 UAC 确认时不等满：确认框挂在桌面上，等多久取决于人去点，
# 让它在这里干等只会把「正在启动」卡住。
ELEVATED_WAIT_SECONDS = 1.5
_POLL_INTERVAL = 0.15
# 同目录搜索只看这三层：再往上就撞到磁盘根，搜出一堆同名文件没有意义
_SEARCH_DEPTH = 3
# 安装器的习惯：程序本体常埋在 InstallLocation 下一层
_EXE_SUBDIRS = ("", "bin", "Bin", "app", "App", "application")


class _PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", ctypes.c_ulong),
        ("cntUsage", ctypes.c_ulong),
        ("th32ProcessID", ctypes.c_ulong),
        ("th32DefaultHeapID", ctypes.c_void_p),
        ("th32ModuleID", ctypes.c_ulong),
        ("cntThreads", ctypes.c_ulong),
        ("th32ParentProcessID", ctypes.c_ulong),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", ctypes.c_ulong),
        ("szExeFile", ctypes.c_wchar * _MAX_PATH),
    ]


class _Launched:
    """一次启动的结果。ShellExecute 不给我们进程句柄，pid 与退出码都可能没有。"""

    def __init__(self, process=None, elevated=False):
        self._process = process
        self.elevated = elevated

    @property
    def pid(self):
        return getattr(self._process, "pid", None)

    def poll(self):
        """已退出返回退出码；还活着或无从判断返回 None。"""
        if self._process is None:
            return None
        return self._process.poll()


def _target_names() -> tuple:
    """要认的进程名（小写）。逗号或分号分隔，空值退回默认。"""
    raw = os.environ.get(PROCESS_ENV, _DEFAULT_PROCESS_NAMES)
    names = [part.strip().lower() for part in raw.replace(";", ",").split(",")]
    names = [name for name in names if name]
    return tuple(names) or (_DEFAULT_PROCESS_NAMES,)


# ── 进程表 ──────────────────────────────────────────────────────────
def _windows_processes() -> list:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
    kernel32.CreateToolhelp32Snapshot.argtypes = [ctypes.c_ulong, ctypes.c_ulong]
    kernel32.Process32FirstW.restype = ctypes.c_int
    kernel32.Process32FirstW.argtypes = [ctypes.c_void_p, ctypes.POINTER(_PROCESSENTRY32W)]
    kernel32.Process32NextW.restype = ctypes.c_int
    kernel32.Process32NextW.argtypes = [ctypes.c_void_p, ctypes.POINTER(_PROCESSENTRY32W)]
    kernel32.CloseHandle.restype = ctypes.c_int
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

    snapshot = kernel32.CreateToolhelp32Snapshot(_TH32CS_SNAPPROCESS, 0)
    if not snapshot or snapshot == _INVALID_HANDLE_VALUE:
        raise OSError(ctypes.get_last_error(), "CreateToolhelp32Snapshot 失败")
    try:
        entry = _PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(_PROCESSENTRY32W)
        if not kernel32.Process32FirstW(snapshot, ctypes.byref(entry)):
            return []
        found = []
        while True:
            found.append((int(entry.th32ProcessID), entry.szExeFile))
            if not kernel32.Process32NextW(snapshot, ctypes.byref(entry)):
                break
        return found
    finally:
        kernel32.CloseHandle(snapshot)


def _posix_processes() -> list:
    """非 Windows 的兜底：只服务开发机与测试，不承诺完整性。"""
    try:
        import psutil  # noqa: WPS433 - 可选依赖，缺了就返回空表
    except Exception:
        return []
    found = []
    for proc in psutil.process_iter(["pid", "name"]):
        info = proc.info or {}
        if info.get("name"):
            found.append((int(info["pid"]), info["name"]))
    return found


def _processes() -> list:
    """当前进程表：[(pid, exe 文件名), ...]。读不到时返回空表，不抛。"""
    try:
        if os.name == "nt":
            return _windows_processes()
        return _posix_processes()
    except OSError:
        return []


def running_pids() -> list:
    """目标进程的 pid 列表，按名字精确匹配（大小写不敏感）。"""
    names = _target_names()
    return sorted(
        pid for pid, name in _processes()
        if str(name or "").strip().lower() in names
    )


def image_path(pid: int):
    """某进程的可执行文件真实路径。权限不够或读不到时返回 None。"""
    if os.name != "nt":
        return None
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    kernel32.QueryFullProcessImageNameW.restype = ctypes.c_int
    kernel32.QueryFullProcessImageNameW.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_ulong),
    ]
    kernel32.CloseHandle.restype = ctypes.c_int
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

    handle = kernel32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
    if not handle:
        return None
    try:
        buffer = ctypes.create_unicode_buffer(_MAX_IMAGE_PATH)
        size = ctypes.c_ulong(_MAX_IMAGE_PATH)
        if not kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            return None
        return Path(buffer.value) if buffer.value else None
    finally:
        kernel32.CloseHandle(handle)


# ── 程序位置 ────────────────────────────────────────────────────────
def _as_executable(raw) -> Path:
    """环境变量可能给的是目录，也可能带着引号：一律归一成 exe 路径。"""
    text = str(raw or "").strip().strip('"').strip()
    path = Path(text).expanduser()
    return path / _DEFAULT_EXE_NAME if path.is_dir() else path


def _recalled_path():
    """上次记录的位置。文件坏了就当没记录，不让它挡住本次启动。"""
    try:
        payload = json.loads(RECORD_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    raw = str(payload.get("exe") or "").strip() if isinstance(payload, dict) else ""
    return Path(raw) if raw else None


def remember(path: Path) -> None:
    """把发现到的位置记下来，供下次不在跑时直接复用。写不进去就算了。"""
    try:
        RECORD_FILE.parent.mkdir(parents=True, exist_ok=True)
        temp = RECORD_FILE.with_name(RECORD_FILE.name + ".tmp")
        temp.write_text(
            json.dumps(
                {"exe": str(path), "updated": datetime.now().isoformat(timespec="seconds")},
                ensure_ascii=False,
                indent=2,
            ),
            encoding="utf-8",
        )
        os.replace(str(temp), str(RECORD_FILE))
    except OSError:
        pass


def _search_roots() -> list:
    """程序自身所在目录，往上三层。整包被放在工业软件旁边时靠这个命中。"""
    roots, current = [], Path(APP_ROOT)
    for _ in range(_SEARCH_DEPTH):
        if current.parent == current:
            break
        roots.append(current)
        current = current.parent
    return roots


def _winreg_module():
    """Windows 注册表模块；非 Windows 或不可用时返回 None。"""
    if os.name != "nt":
        return None
    try:
        import winreg  # noqa: WPS433 - 只在 Windows 上存在
    except ImportError:
        return None
    return winreg


def _query_key(module, key, name: str) -> str:
    try:
        value, _ = module.QueryValueEx(key, name)
    except OSError:
        return ""
    return str(value or "").strip()


def _icon_anchors(icon: Path) -> list:
    """DisplayIcon 给的多半是图标文件，但它的上层目录里就有程序。"""
    if icon.suffix.lower() == ".exe":
        return [icon]
    anchors, current = [], icon.parent
    for _ in range(3):
        anchors.append(current / _DEFAULT_EXE_NAME)
        if current.parent == current:
            break
        current = current.parent
    return anchors


def _registry_paths() -> list:
    """注册表里的线索：App Paths 给得最准，卸载项里的安装目录次之。"""
    _winreg = _winreg_module()
    if _winreg is None:
        return []

    found = []
    app_paths = r"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\%s" % _DEFAULT_EXE_NAME
    views = [(getattr(_winreg, "HKEY_LOCAL_MACHINE", None), _winreg.KEY_WOW64_64KEY),
             (getattr(_winreg, "HKEY_LOCAL_MACHINE", None), _winreg.KEY_WOW64_32KEY),
             (_winreg.HKEY_CURRENT_USER, 0)]
    for hive, view in views:
        if hive is None:
            continue
        try:
            with _winreg.OpenKey(hive, app_paths, 0, _winreg.KEY_READ | view) as key:
                value = _query_key(_winreg, key, "")
        except OSError:
            continue
        if value:
            found.append(_as_executable(value))

    uninstall = r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"
    for hive, view in views:
        if hive is None:
            continue
        try:
            root = _winreg.OpenKey(hive, uninstall, 0, _winreg.KEY_READ | view)
        except OSError:
            continue
        try:
            index = 0
            while True:
                try:
                    name = _winreg.EnumKey(root, index)
                except OSError:
                    break
                index += 1
                try:
                    with _winreg.OpenKey(root, name) as sub:
                        display = _query_key(_winreg, sub, "DisplayName")
                        if "gridstar" not in display.lower():
                            continue
                        location = _query_key(_winreg, sub, "InstallLocation")
                        icon = _query_key(_winreg, sub, "DisplayIcon")
                except OSError:
                    continue
                if location:
                    # InstallLocation 通常指在安装根上，程序本体在其下的 bin 之类目录里
                    for folder in _EXE_SUBDIRS:
                        found.append(
                            (Path(location) / folder / _DEFAULT_EXE_NAME) if folder
                            else Path(location) / _DEFAULT_EXE_NAME
                        )
                if icon:
                    found.extend(_icon_anchors(_as_executable(icon.split(",")[0])))
        finally:
            _winreg.CloseKey(root)
    return found


def _candidates() -> list:
    """候选路径[(来源, 路径)]，按可信度排。"""
    candidates = []
    raw_env = os.environ.get(EXE_ENV, "").strip()
    if raw_env:
        candidates.append((EXE_ENV, _as_executable(raw_env)))
    recalled = _recalled_path()
    if recalled:
        candidates.append(("上次记录", recalled))
    for root in _search_roots():
        candidates.append(("同目录", root / _DEFAULT_EXE_NAME))
    for path in _registry_paths():
        candidates.append(("注册表", path))
    for pid in running_pids():
        located = image_path(pid)
        if located:
            candidates.append(("运行中的进程", located))
    return candidates


def resolve_executable() -> tuple:
    """找到 GridStar 主程序：返回 (路径, 来源, 检查过的候选)。

    找不到时路径为 None，检查过的候选一并带出——报错要能让现场的人一眼看出
    该把 GridStar.exe 放哪，或者该设哪个环境变量。
    """
    checked = []
    for source, path in _candidates():
        if not path:
            continue
        checked.append("%s: %s" % (source, path))
        try:
            if path.is_file():
                return path, source, checked
        except OSError:
            continue
    return None, "", checked


# ── 启动 ────────────────────────────────────────────────────────────
def _shell_runas(exe: Path) -> None:
    """以管理员身份启动：由系统弹 UAC，用户确认后进程才真正出现。"""
    shell32 = ctypes.WinDLL("shell32", use_last_error=True)
    shell32.ShellExecuteW.restype = ctypes.c_void_p
    shell32.ShellExecuteW.argtypes = [
        ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p,
        ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int,
    ]
    result = shell32.ShellExecuteW(
        None, "runas", str(exe), None, str(exe.parent), _SW_SHOWNORMAL
    )
    code = int(result or 0)
    if code <= 32:
        # 5 = 被 UAC 拒绝；其它值同样是 ShellExecute 的失败码
        raise OSError(code, "以管理员身份启动被系统拒绝（ShellExecute 返回 %d）" % code)


def _spawn(exe: Path):
    """把 GUI 主程序拉起来，必要时提权。

    先按普通权限创建进程：不假设所有机器都要提权，多弹一个 UAC 也是打扰。
    被 WinError 740 拒掉（可执行文件要求管理员）时改走 ``ShellExecute("runas")``。
    工作目录一律设成程序自己那一层，否则同目录的 DLL 找不到。
    """
    flags = 0
    if os.name == "nt":
        flags = getattr(subprocess, "DETACHED_PROCESS", 0) | getattr(
            subprocess, "CREATE_NEW_PROCESS_GROUP", 0
        )
    try:
        process = subprocess.Popen(
            [str(exe)], cwd=str(exe.parent), close_fds=True, creationflags=flags
        )
        return _Launched(process, elevated=False)
    except OSError as exc:
        if os.name != "nt" or getattr(exc, "winerror", None) != _ERROR_ELEVATION_REQUIRED:
            raise
        _shell_runas(exe)
        return _Launched(None, elevated=True)


def ensure_running(timeout: float = DEFAULT_WAIT_SECONDS) -> dict:
    """确认主程序在跑，不在就启动。

    返回 ``running``（核实过确实在进程表里）、``launched``（本次提交了启动）、
    ``elevated``（这次走的是 UAC 提权路径）、``pids``、``executable``、
    ``executable_source``，以及可选的 ``error`` / ``warning``。硬失败只体现在
    ``error`` 上：找不到程序、系统拒绝创建、进程起来后立刻退出。
    """
    pids = running_pids()
    if pids:
        # 已经在跑：顺手把它自己的真实位置记下来，下次不在跑时直接用
        for pid in pids:
            located = image_path(pid)
            if located:
                remember(located)
                break
        return {
            "running": True,
            "launched": False,
            "elevated": False,
            "pids": pids,
            "executable": str(image_path(pids[0]) or ""),
            "executable_source": "运行中的进程",
            "executable_exists": True,
            "process_names": list(_target_names()),
        }

    exe, source, checked = resolve_executable()
    result = {
        "running": False,
        "launched": False,
        "elevated": False,
        "pids": [],
        "process_names": list(_target_names()),
        "executable": str(exe or ""),
        "executable_source": source,
        "executable_exists": exe is not None,
        "checked": checked,
    }

    if exe is None:
        # 错误文案给现场人看：说清怎么办，不在 toast 里喷一长串路径，
        # 具体查过哪些位置放在 checked 里，留给排查时看
        result["error"] = (
            "找不到 GridStar 程序（已查过环境变量、上次记录、同目录与注册表）；"
            "可以设置环境变量 %s 指向它，或把 GridStar.exe 放在本程序所在目录" % EXE_ENV
        )
        return result

    try:
        launched = _spawn(exe)
    except OSError as exc:
        result["error"] = "启动 GridStar 失败：%s" % exc
        return result

    remember(exe)
    result["launched"] = True
    result["elevated"] = bool(getattr(launched, "elevated", False))
    budget = ELEVATED_WAIT_SECONDS if result["elevated"] else timeout
    deadline = time.monotonic() + max(0.5, float(budget))
    while time.monotonic() < deadline:
        exit_code = launched.poll()
        if exit_code is not None:
            result["error"] = "GridStar 启动后立即退出（退出码 %s），请检查程序本身能否正常运行" % exit_code
            return result
        pids = running_pids()
        if pids:
            result.update(running=True, pids=pids)
            return result
        time.sleep(_POLL_INTERVAL)

    if result["elevated"]:
        # 提权请求被系统受理，但确认框还在等人点：这不是失败，也不是已启动
        result["warning"] = "已请求以管理员身份启动，请在系统 UAC 提示里点「是」，GridStar 随后会自己起来"
    else:
        result["warning"] = "已提交启动，但 %.0f 秒内没在进程表里看到它，可能仍在初始化" % budget
    return result