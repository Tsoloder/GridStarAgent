"""数据目录可用性守卫：写不进去就当场修权限，修不动才请求提权。

内网 WinServer2019 上最常见的故障是：数据目录由别的账号（SYSTEM、早先的服务
账号、另一个管理员）创建过，当前账号没有写权限。程序要的效果是数据只能待在
那一个固定目录里，所以这里不做任何「换个地方写」的降级，只做三件事：

1. `probe_writable` 当场实测能不能写，不看 ACL 猜；
2. `repair_acl` 在当前进程内当场修权限。多数情况下目录属主就是当前账号，
   而属主天然持有 WRITE_DAC，启用 SeTakeOwnership/SeRestore 令牌特权后
   icacls 授权给自己即可生效，不需要重启进程、不弹 UAC；
3. 当场修不动且当前进程未提权时，`relaunch_elevated` 用 UAC 提权重启一次，
   并把解析好的目标目录通过 GRIDSTAR_DATA_DIR_PINNED 钉给子进程。提权后
   %APPDATA% 会变成提权账号的目录，不钉住就会落到别的用户目录下，与「只能
   放到那个目录」直接冲突。

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
from typing import List, Optional

IS_WINDOWS = os.name == "nt"

# 提权重启时钉住目标目录的变量。外部设置它会被当作「父进程已经解析好了」，
# 用于让提权后的子进程落回同一个目录，不作为对外提供的自定义路径开关。
PIN_ENV = "GRIDSTAR_DATA_DIR_PINNED"

TOKEN_ADJUST_PRIVILEGES = 0x0020
TOKEN_QUERY = 0x0008
SE_PRIVILEGE_ENABLED = 0x00000002

# 拿到属主身份、绕过 ACL 读取、修改 DACL 所需的三项特权
_REPAIR_PRIVILEGES = (
    "SeTakeOwnershipPrivilege",
    "SeRestorePrivilege",
    "SeBackupPrivilege",
)


class _LUID(ctypes.Structure):
    _fields_ = [("LowPart", wintypes.DWORD), ("HighPart", wintypes.LONG)]


class _LUID_AND_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Luid", _LUID), ("Attributes", wintypes.DWORD)]


class _TOKEN_PRIVILEGES(ctypes.Structure):
    _fields_ = [
        ("PrivilegeCount", wintypes.DWORD),
        ("Privileges", _LUID_AND_ATTRIBUTES * 1),
    ]


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
    """当前进程是否已提权（拿到完整管理员令牌）。"""
    if not IS_WINDOWS:
        return os.geteuid() == 0 if hasattr(os, "geteuid") else False
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:  # noqa: BLE001
        return False


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


def _enable_privileges(names=_REPAIR_PRIVILEGES) -> List[str]:
    """在当前进程令牌上启用指定特权，返回实际启用的特权名。

    未提权的进程即使名字在令牌里也启不动，这时返回值会是空列表，调用方据此
    判断「当场修不了」。
    """
    if not IS_WINDOWS:
        return []
    enabled: List[str] = []
    advapi32 = ctypes.windll.advapi32
    kernel32 = ctypes.windll.kernel32
    token = wintypes.HANDLE()
    try:
        if not advapi32.OpenProcessToken(
            kernel32.GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
            ctypes.byref(token),
        ):
            return []
        for name in names:
            luid = _LUID()
            if not advapi32.LookupPrivilegeValueW(None, name, ctypes.byref(luid)):
                continue
            state = _TOKEN_PRIVILEGES()
            state.PrivilegeCount = 1
            state.Privileges[0].Luid = luid
            state.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED
            if advapi32.AdjustTokenPrivileges(
                token, False, ctypes.byref(state), 0, None, None
            ) and kernel32.GetLastError() == 0:
                enabled.append(name)
    except Exception:  # noqa: BLE001
        return enabled
    finally:
        if token:
            kernel32.CloseHandle(token)
    return enabled


def _run_tool(args: List[str]) -> tuple:
    try:
        res = subprocess.run(
            args, capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=120,
        )
        return res.returncode, ((res.stdout or "") + (res.stderr or "")).strip()
    except Exception as exc:  # noqa: BLE001
        return -1, "%s: %s" % (type(exc).__name__, exc)


def repair_acl(directory: Path) -> tuple:
    """尝试把目录的写权限修给当前账号。返回 (是否成功, 过程描述列表)。

    分两级：先直接授权（属主本来就有改 DACL 的权利）；不行再接管属主后重试。
    接管属主会改变目录归属，所以放在第二步，只在必要时做。
    """
    steps: List[str] = []
    if not IS_WINDOWS:
        return False, ["非 Windows 平台，跳过 ACL 修复"]

    enabled = _enable_privileges()
    steps.append("启用令牌特权: %s" % (", ".join(enabled) if enabled else "无（未提权）"))

    account = _current_user()
    if not account:
        return False, steps + ["取不到当前账号名，无法授权"]

    target = str(directory)
    if directory.exists():
        code, _ = _run_tool(["icacls", target, "/grant", "%s:(OI)(CI)F" % account, "/T", "/C", "/Q"])
        steps.append("icacls /grant 直接授权 -> 返回码 %d" % code)
        if probe_writable(directory) is None:
            return True, steps

    code, out = _run_tool(["takeown", "/F", target, "/R", "/D", "Y"])
    steps.append("takeown 接管属主 -> 返回码 %d" % code)
    if code != 0 and out:
        steps.append("  takeown 输出: %s" % out.splitlines()[0][:200])

    code, _ = _run_tool(["icacls", target, "/reset", "/T", "/C", "/Q"])
    steps.append("icacls /reset 重置继承 -> 返回码 %d" % code)
    code, _ = _run_tool(["icacls", target, "/grant", "%s:(OI)(CI)F" % account, "/T", "/C", "/Q"])
    steps.append("icacls /grant 接管后授权 -> 返回码 %d" % code)

    error = probe_writable(directory)
    if error is None:
        return True, steps
    steps.append("仍不可写: %s" % error)
    return False, steps


def elevated_relaunch_command(directory: Path, argv: List[str]) -> str:
    """构造提权重启用的命令行（供测试与日志展示）。"""
    interpreter = sys.executable
    arguments = " ".join('"%s"' % item for item in argv)
    return '"%s" %s' % (interpreter, arguments)


def relaunch_elevated(directory: Path, argv: List[str]) -> tuple:
    """用 UAC 提权重启当前程序，并把目录钉给子进程。返回 (是否已启动, 说明)。

    钉目录是关键：提权后 %APPDATA% 指向提权账号的用户目录，不钉住就会把数据
    写到别的用户名下，正好违背「只能放到那个目录」。
    """
    if not IS_WINDOWS:
        return False, "非 Windows 平台，无法提权"
    command = elevated_relaunch_command(directory, argv)
    env_ok = os.environ.get(PIN_ENV, "")
    if env_ok != str(directory):
        os.environ[PIN_ENV] = str(directory)
    try:
        result = ctypes.windll.shell32.ShellExecuteW(
            None, "runas", sys.executable, " ".join('"%s"' % item for item in argv),
            None, 1,
        )
    except Exception as exc:  # noqa: BLE001
        return False, "提权调用失败: %s: %s" % (type(exc).__name__, exc)
    if result > 32:
        return True, "已通过 UAC 提权重启: %s" % command
    return False, "提权被拒绝或不可用（ShellExecuteW 返回 %s）: %s" % (result, command)


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
            "数据目录不可写，且当前已是提权进程，自动修复未成功。"
            "目录属主可能是别的账号且没有可修权限，请人工执行 takeown / icacls。"
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