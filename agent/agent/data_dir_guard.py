"""数据目录可用性守卫：写不进去就当场修权限，修不动才请求提权。

内网 WinServer2019 上最常见的故障是：数据目录由别的账号（SYSTEM、早先的服务
账号、另一个管理员）创建过，当前账号没有写权限。程序要的效果是数据只能待在
那一个固定目录里，所以这里不做任何「换个地方写」的降级，只做三件事：

1. `probe_writable` 当场实测能不能写，不看 ACL 猜；
2. `repair_acl` 在当前进程内当场修权限。多数情况下目录属主就是当前账号，
   而属主天然持有 WRITE_DAC，启用 SeTakeOwnership/SeRestore 令牌特权后
   icacls 授权给自己即可生效，不需要重启进程、不弹 UAC；
3. 当场修不动且当前进程未提权时，`relaunch_elevated` 用 UAC 提权重启一次，
   并把解析好的目标目录钉给子进程。提权后 %APPDATA% 可能变成提权账号的目录，
   不钉住就会落到别的用户目录下，与「只能放到那个目录」直接冲突。钉目录同时
   走环境变量与命令行参数两条路：runas 出来的子进程不保证继承父进程的环境块，
   命令行参数是确定的那个。

只用标准库：ctypes 调 advapi32 启特权，takeown / icacls 是系统自带工具。
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wintypes
import os
import subprocess
import sys
import threading
import uuid
from pathlib import Path
from typing import List, Optional, Tuple

IS_WINDOWS = os.name == "nt"

# 提权重启时钉住目标目录的变量。外部设置它会被当作「父进程已经解析好了」，
# 用于让提权后的子进程落回同一个目录。命令行侧的同名参数见 DATA_DIR_ARG。
PIN_ENV = "GRIDSTAR_DATA_DIR_PINNED"
DATA_DIR_ARG = "--data-dir"

TOKEN_ADJUST_PRIVILEGES = 0x0020
TOKEN_QUERY = 0x0008
SE_PRIVILEGE_ENABLED = 0x00000002
ERROR_NOT_ALL_ASSIGNED = 1300
TokenElevation = 20  # TOKEN_INFORMATION_CLASS::TokenElevation

# 文件属性：重解析点（junction / 符号链接）。递归接管属主时遇到它必须跳过，
# 否则一条指回自身的链接就能让 takeown/icacls 无限绕圈。
_FILE_ATTRIBUTE_REPARSE_POINT = 0x00000400

# 递归前扫描目录树的上限，超了就只做根目录级的修复。
_MAX_SCAN_DIRS = 5000

# 拿到属主身份、绕过 ACL 读取、修改 DACL 所需的三项特权
_REPAIR_PRIVILEGES = (
    "SeTakeOwnershipPrivilege",
    "SeRestorePrivilege",
    "SeBackupPrivilege",
)

# use_last_error=True 才能用 ctypes.get_last_error() 可靠地读到调用后的错误码，
# 裸 windll + kernel32.GetLastError() 会被中间调用污染。
_advapi32 = ctypes.WinDLL("advapi32", use_last_error=True) if IS_WINDOWS else None
_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True) if IS_WINDOWS else None
_shell32 = ctypes.WinDLL("shell32", use_last_error=True) if IS_WINDOWS else None


class _LUID(ctypes.Structure):
    _fields_ = [("LowPart", wintypes.DWORD), ("HighPart", wintypes.LONG)]


class _LUID_AND_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Luid", _LUID), ("Attributes", wintypes.DWORD)]


class _TOKEN_PRIVILEGES(ctypes.Structure):
    _fields_ = [
        ("PrivilegeCount", wintypes.DWORD),
        ("Privileges", _LUID_AND_ATTRIBUTES * 1),
    ]


def _configure_prototypes() -> None:
    """声明 Win32 原型。

    这一步不是装饰：不声明的话，ctypes 会把返回值按 c_int 处理。64 位上
    ``GetCurrentProcess()`` 返回的伪句柄是 0xFFFFFFFFFFFFFFFF，一旦被当成
    32 位整数读回，再当参数传出去就变成 0xFFFFFFFF，``OpenProcessToken`` 会
    直接以 ERROR_INVALID_HANDLE 失败，于是整条特权通路在提不提权时都走不通。
    """
    if not IS_WINDOWS:
        return
    _advapi32.LookupPrivilegeValueW.argtypes = [
        wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.POINTER(_LUID)
    ]
    _advapi32.LookupPrivilegeValueW.restype = wintypes.BOOL
    _advapi32.OpenProcessToken.argtypes = [
        wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)
    ]
    _advapi32.OpenProcessToken.restype = wintypes.BOOL
    _advapi32.GetTokenInformation.argtypes = [
        wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
        wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
    ]
    _advapi32.GetTokenInformation.restype = wintypes.BOOL
    _advapi32.AdjustTokenPrivileges.argtypes = [
        wintypes.HANDLE, wintypes.BOOL, ctypes.POINTER(_TOKEN_PRIVILEGES),
        wintypes.DWORD, ctypes.POINTER(_TOKEN_PRIVILEGES),
        ctypes.POINTER(wintypes.DWORD),
    ]
    _advapi32.AdjustTokenPrivileges.restype = wintypes.BOOL
    _kernel32.GetCurrentProcess.argtypes = []
    _kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    _kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    _kernel32.CloseHandle.restype = wintypes.BOOL
    _kernel32.SetLastError.argtypes = [wintypes.DWORD]
    _kernel32.SetLastError.restype = None
    _shell32.ShellExecuteW.argtypes = [
        wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR,
        wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_int,
    ]
    _shell32.ShellExecuteW.restype = ctypes.c_void_p


_configure_prototypes()


def _current_user() -> str:
    """当前账号的 DOMAIN\\user 形式，供 icacls /grant 使用。"""
    domain = os.environ.get("USERDOMAIN", "")
    user = os.environ.get("USERNAME", "")
    if domain and user:
        return "%s\\%s" % (domain, user)
    try:
        import getpass

        return getpass.getuser()
    except Exception:  # noqa: BLE001
        return user or ""


def is_admin() -> bool:
    """当前进程是否已提权（拿到完整管理员令牌）。

    只看令牌的 TokenElevation，不看账号在不在 Administrators 组：UAC 下同一个
    管理员账号跑出的过滤令牌「属于管理员组但未提权」，用 IsUserAnAdmin 之类按组
    报的 API 会误判成已提权，从而把本该走的 UAC 重启这一步跳掉。
    """
    if not IS_WINDOWS:
        return os.geteuid() == 0 if hasattr(os, "geteuid") else False
    token = wintypes.HANDLE()
    try:
        if not _advapi32.OpenProcessToken(
            _kernel32.GetCurrentProcess(), TOKEN_QUERY, ctypes.byref(token)
        ):
            return False
        try:
            elevation = wintypes.DWORD()
            returned = wintypes.DWORD()
            ok = _advapi32.GetTokenInformation(
                token,
                TokenElevation,
                ctypes.byref(elevation),
                ctypes.sizeof(elevation),
                ctypes.byref(returned),
            )
            return bool(elevation.value) if ok else False
        finally:
            _kernel32.CloseHandle(token)
    except Exception:  # noqa: BLE001
        return False


def _set_privileges(names, enable: bool) -> List[str]:
    """在进程令牌上开启/关闭指定特权，返回实际生效的特权名。

    AdjustTokenPrivileges 即使有特权没分配成功也返回非零，真正的判据是错误码
    是不是 ERROR_NOT_ALL_ASSIGNED。逐个启用以便按名字归因。
    """
    if not IS_WINDOWS or not names:
        return []
    changed: List[str] = []
    token = wintypes.HANDLE()
    try:
        if not _advapi32.OpenProcessToken(
            _kernel32.GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
            ctypes.byref(token),
        ):
            return []
        for name in names:
            luid = _LUID()
            if not _advapi32.LookupPrivilegeValueW(None, name, ctypes.byref(luid)):
                continue
            state = _TOKEN_PRIVILEGES()
            state.PrivilegeCount = 1
            state.Privileges[0].Luid = luid
            state.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED if enable else 0
            _kernel32.SetLastError(0)
            ok = _advapi32.AdjustTokenPrivileges(
                token, False, ctypes.byref(state), 0, None, None
            )
            if ok and ctypes.get_last_error() != ERROR_NOT_ALL_ASSIGNED:
                changed.append(name)
    except Exception:  # noqa: BLE001
        return changed
    finally:
        if token:
            _kernel32.CloseHandle(token)
    return changed


def _enable_privileges(names=_REPAIR_PRIVILEGES) -> List[str]:
    return _set_privileges(names, True)


def _disable_privileges(names) -> None:
    """把借来的特权还回去，不留在进程令牌上。"""
    _set_privileges(names, False)


def _probe_write_direct(directory: Path) -> str:
    """直接建文件探测，返回错误描述（可写返回空串）。

    刻意不用 ``tempfile.mkstemp``：在「目录存在但拒绝写入」的情况下（Deny ACE、
    父目录继承限制），本机实测 mkstemp 会一直重试随机文件名而不返回，
    ``os.open(O_CREAT|O_EXCL)`` 同一目录 0.00s 就抛 PermissionError。
    探测本身不能有卡死的可能。
    """
    for _ in range(5):
        target = directory / (".write_probe_%s.tmp" % uuid.uuid4().hex[:12])
        try:
            handle = os.open(str(target), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            continue
        except OSError as exc:
            return "目录不可写: %s" % exc
        try:
            os.write(handle, b"probe")
        finally:
            os.close(handle)
        try:
            os.unlink(str(target))
        except OSError:
            pass
        return ""
    return "无法在目录内创建探测文件（文件名连续冲突）"


def probe_writable(directory: Path, timeout: float = 20.0) -> Optional[str]:
    """实测目录能否写入。可写返回 None，否则返回错误描述。

    只信实测：目录存在且 ACL 看着正常，不代表当前进程写得进去（令牌过滤、
    拒绝 ACE、父目录继承限制都会让「看起来能写」失效）。

    探测放在单独的线程里并设超时：文件系统层一旦卡住（网络盘、安全软件拦截），
    启动流程不能陪着一起卡死，超时按不可写处理，交给修权限流程去处理。
    """
    try:
        directory.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        return "目录不可创建: %s" % exc

    holder = {}

    def worker():
        try:
            holder["error"] = _probe_write_direct(directory)
        except Exception as exc:  # noqa: BLE001
            holder["error"] = "探测异常: %s: %s" % (type(exc).__name__, exc)

    thread = threading.Thread(target=worker, name="data-dir-probe", daemon=True)
    thread.start()
    thread.join(timeout)
    if thread.is_alive():
        return "目录写入探测超时(%.0fs)，按不可写处理" % timeout
    return holder.get("error") or None


def _run_tool(args: List[str], timeout: int = 120) -> Tuple[int, str]:
    try:
        res = subprocess.run(
            args, capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=timeout,
        )
        return res.returncode, ((res.stdout or "") + (res.stderr or "")).strip()
    except Exception as exc:  # noqa: BLE001
        return -1, "%s: %s" % (type(exc).__name__, exc)


def _scan_tree(root: Path, limit: int) -> Tuple[List[Path], List[str], bool]:
    """有界遍历目录树，不跟随重解析点。

    返回 (目录列表, 重解析点列表, 是否因触顶而截断)。用于在递归接管属主前先
    探明这棵树值不值得/能不能安全递归：有重解析点就绝不用 /R，树太大就只处理
    截断前的一部分。
    """
    dirs: List[Path] = []
    reparse: List[str] = []
    seen = set()
    stack = [root]
    truncated = False
    while stack:
        current = stack.pop()
        try:
            key = os.path.realpath(str(current))
        except OSError:
            key = str(current)
        if key in seen:
            continue
        seen.add(key)
        if len(dirs) >= limit:
            truncated = True
            break
        dirs.append(current)
        try:
            entries = list(os.scandir(str(current)))
        except OSError:
            continue
        for entry in entries:
            try:
                st = entry.stat(follow_symlinks=False)
                is_dir = entry.is_dir(follow_symlinks=False)
            except OSError:
                continue
            attrs = getattr(st, "st_file_attributes", 0)
            if attrs & _FILE_ATTRIBUTE_REPARSE_POINT:
                reparse.append(entry.path)
                continue
            if is_dir:
                stack.append(Path(entry.path))
    return dirs, reparse, truncated


def repair_acl(directory: Path) -> Tuple[bool, List[str]]:
    """尝试把目录的写权限修给当前账号。返回 (是否成功, 过程描述列表)。

    分三段，能停在越前面越好：

    1. 只动根目录：多数故障是根目录由别的账号创建，修根即可写；不递归，快且安全；
    2. 根目录修不动，接管根目录属主（仍不递归）；
    3. 仍不可写，才递归处理子项。递归前先有界扫描，遇到重解析点或目录数触顶
       就改用逐个目录显式授权，绝不跟随链接。
    """
    steps: List[str] = []
    if not IS_WINDOWS:
        return False, ["非 Windows 平台，跳过 ACL 修复"]

    enabled = _enable_privileges()
    steps.append("启用令牌特权: %s" % (", ".join(enabled) if enabled else "无（未提权或不可用）"))
    try:
        account = _current_user()
        if not account:
            return False, steps + ["取不到当前账号名，无法授权"]

        target = str(directory)
        grant = "%s:(OI)(CI)F" % account

        if not directory.exists():
            try:
                directory.mkdir(parents=True, exist_ok=True)
            except OSError as exc:
                return False, steps + ["根目录不可创建: %s" % exc]

        # 第 1 段：只修根目录，不递归
        code, _ = _run_tool(["icacls", target, "/grant", grant, "/Q"])
        steps.append("icacls 根目录授权 -> 返回码 %d" % code)
        if probe_writable(directory) is None:
            return True, steps

        # 第 2 段：接管根目录属主，仍不递归
        code, out = _run_tool(["takeown", "/F", target, "/D", "Y"])
        steps.append("takeown 根目录接管属主 -> 返回码 %d" % code)
        if code != 0 and out:
            steps.append("  takeown 输出: %s" % out.splitlines()[0][:200])
        code, _ = _run_tool(["icacls", target, "/grant", grant, "/Q"])
        steps.append("icacls 根目录接管后授权 -> 返回码 %d" % code)
        if probe_writable(directory) is None:
            return True, steps

        # 第 3 段：递归处理子项，先探明这棵树
        dirs, reparse, truncated = _scan_tree(directory, _MAX_SCAN_DIRS)
        if reparse:
            steps.append("发现 %d 个重解析点，已跳过（防止递归成环）: %s"
                         % (len(reparse), reparse[0]))
        if truncated:
            steps.append("目录数超过上限 %d，只处理已扫描到的部分" % _MAX_SCAN_DIRS)

        if not reparse and not truncated:
            code, out = _run_tool(["takeown", "/F", target, "/R", "/D", "Y"], timeout=300)
            steps.append("takeown 递归接管属主 -> 返回码 %d" % code)
            if code != 0 and out:
                steps.append("  takeown 输出: %s" % out.splitlines()[0][:200])
            code, _ = _run_tool(["icacls", target, "/reset", "/T", "/C", "/Q"], timeout=300)
            steps.append("icacls 递归重置继承 -> 返回码 %d" % code)
            code, _ = _run_tool(["icacls", target, "/grant", grant, "/T", "/C", "/Q"], timeout=300)
            steps.append("icacls 递归授权 -> 返回码 %d" % code)
        else:
            batch = 100
            done = 0
            for start in range(0, len(dirs), batch):
                chunk = [str(d) for d in dirs[start:start + batch]]
                _run_tool(["icacls", *chunk, "/grant", grant, "/C", "/Q"], timeout=300)
                done += len(chunk)
            steps.append("已对 %d 个目录逐个授权（未跟随重解析点）" % done)

        error = probe_writable(directory)
        if error is None:
            return True, steps
        steps.append("仍不可写: %s" % error)
        return False, steps
    finally:
        # 修完立即把借来的特权还回去
        _disable_privileges(enabled)


def _relaunch_argv(directory: Path, argv: Optional[List[str]]) -> List[str]:
    """构造提权重启用的参数：脚本路径转绝对、并把数据目录显式传给子进程。"""
    args = list(argv) if argv else list(sys.argv)
    if args:
        # argv[0] 转成绝对路径。提权进程的工作目录不一定是当前目录（runas 出来
        # 常落在 %SystemRoot%\\System32），相对脚本路径一进去就找不到文件。
        try:
            candidate = os.path.abspath(args[0])
            if os.path.isfile(candidate):
                args[0] = candidate
        except Exception:  # noqa: BLE001
            pass
    if DATA_DIR_ARG not in args:
        args += [DATA_DIR_ARG, str(directory)]
    return args


def elevated_relaunch_command(directory: Path, argv: Optional[List[str]]) -> str:
    """构造提权重启用的命令行（供测试与日志展示）。"""
    args = _relaunch_argv(directory, argv)
    return "%s %s" % (subprocess.list2cmdline([sys.executable]),
                      subprocess.list2cmdline(args))


def relaunch_elevated(directory: Path, argv: Optional[List[str]]) -> Tuple[bool, str]:
    """用 UAC 提权重启当前程序，并把目录钉给子进程。返回 (是否已启动, 说明)。

    钉目录是关键：提权后 %APPDATA% 可能指向提权账号的用户目录，不钉住就会把数据
    写到别的用户名下，正好违背「只能放到那个目录」。这里同时设环境变量和显式传
    --data-dir：runas 子进程不保证继承环境块，命令行参数才是确定的那条。
    工作目录也一并传进去，避免相对脚本路径在 System32 下解析失败。
    """
    if not IS_WINDOWS:
        return False, "非 Windows 平台，无法提权"

    args = _relaunch_argv(directory, argv)
    cwd = os.getcwd()
    os.environ[PIN_ENV] = str(directory)
    params = subprocess.list2cmdline(args)
    command = "%s %s" % (subprocess.list2cmdline([sys.executable]), params)
    try:
        result = _shell32.ShellExecuteW(
            None, "runas", sys.executable, params, cwd, 1,
        )
    except Exception as exc:  # noqa: BLE001
        return False, "提权调用失败: %s: %s" % (type(exc).__name__, exc)
    code = int(result or 0)
    if code > 32:
        return True, "已通过 UAC 提权重启: %s（工作目录 %s）" % (command, cwd)
    return False, "提权被拒绝或不可用（ShellExecuteW 返回 %s）: %s" % (code, command)


def guard_data_dir(directory: Path, argv: Optional[List[str]] = None) -> dict:
    """启动时的守卫流程，返回可写性、修复过程与后续动作的描述。

    调用方拿到 needs_relaunch 时应当结束当前进程，让提权实例接着跑。
    """
    result = {
        "directory": str(directory),
        "writable": False,
        "error": "",
        "repair_attempted": False,
        "repair_steps": [],
        "needs_relaunch": False,
        "message": "",
    }
    error = probe_writable(directory)
    if error is None:
        result["writable"] = True
        result["message"] = "数据目录可写"
        return result

    result["error"] = error
    result["repair_attempted"] = True
    ok, steps = repair_acl(directory)
    result["repair_steps"] = steps
    if ok:
        result["writable"] = True
        result["message"] = "数据目录原不可写，已在当前进程内修好权限"
        return result

    if is_admin():
        result["message"] = (
            "数据目录不可写，自动修复未成功（当前进程已提权，无法再提权重试）。"
            "目录属主可能是别的账号且没有可修改权限，请用管理员命令行执行 "
            "takeown / icacls 后重试。"
        )
        return result

    started, note = relaunch_elevated(directory, argv or sys.argv)
    result["relaunch_note"] = note
    if started:
        result["needs_relaunch"] = True
        result["message"] = "数据目录不可写，已请求 UAC 提权重启一次以修复权限"
    else:
        result["message"] = (
            "数据目录不可写，自动修复与提权均未成功：%s。"
            "请用管理员命令行执行 takeown / icacls 后重试。" % note
        )
    return result