"""内网部署自检：确认数据目录能不能写，不能写时给出可直接执行的修复命令。

用法（在跑 app.py 的那个 Python 环境、那个账号下执行）::

    python check_data_dir.py

脚本只读不写：唯一的写操作是在目标目录里建一个临时文件再删掉，用来实测权限，
不碰 config.json / sessions / logs 里已有的任何数据。
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

AGENT_DIR = Path(__file__).resolve().parent
if str(AGENT_DIR) not in sys.path:
    sys.path.insert(0, str(AGENT_DIR))


def line(title: str) -> None:
    print("\n" + "=" * 68)
    print(title)
    print("=" * 68)


def run(cmd: list[str], encoding: str = "") -> str:
    try:
        res = subprocess.run(
            cmd, capture_output=True, text=True, shell=False,
            encoding=encoding or None, errors="replace",
        )
        return ((res.stdout or "") + (res.stderr or "")).strip()
    except Exception as exc:  # noqa: BLE001
        return "<无法执行 %s: %s>" % (cmd, exc)


def run_ps(script: str) -> str:
    """跑一段 PowerShell，并强制按 UTF-8 取回结果。

    PowerShell 默认按控制台 OEM 代码页输出；即使把 [Console]::OutputEncoding
    设成 UTF-8，Python 这边也必须用 UTF-8 去解码管道，否则中文账号名（例如
    「汤汤\\84198」）会变成乱码，读的人会以为目录属主是别的东西。
    """
    wrapped = (
        "[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false; "
        "$OutputEncoding = [Console]::OutputEncoding; " + script
    )
    return run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command", wrapped],
        encoding="utf-8",
    )


def main() -> int:
    import getpass

    line("1. 运行身份")
    print("当前账号      : %s\\%s" % (os.environ.get("USERDOMAIN", "?"), getpass.getuser()))
    print("Python        : %s" % sys.executable)
    print("脚本所在目录  : %s" % AGENT_DIR)
    elevated = run_ps(
        "([Security.Principal.WindowsPrincipal]"
        "[Security.Principal.WindowsIdentity]::GetCurrent())"
        ".IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)"
    )
    print("是否提权运行  : %s" % elevated)
    print("注意：进程是不是管理员并不决定能否写自己的 AppData；")
    print("      决定权在目录 ACL 和目录属主，看第 3 节。")

    line("2. 数据目录解析结果")
    import paths

    print("CLINELIKECHAT_DATA_DIR : %r" % os.environ.get("CLINELIKECHAT_DATA_DIR", ""))
    print("APPDATA                : %r" % os.environ.get("APPDATA", ""))
    print("LOCALAPPDATA           : %r" % os.environ.get("LOCALAPPDATA", ""))
    print("TEMP                   : %r" % os.environ.get("TEMP", ""))
    print("-> DATA_DIR            : %s" % paths.DATA_DIR)
    print("-> LOG_DIR             : %s" % paths.LOG_DIR)
    print("-> SESSIONS_DIR        : %s" % paths.SESSIONS_DIR)
    if not os.environ.get("CLINELIKECHAT_DATA_DIR", "").strip():
        print("提示：未设置 CLINELIKECHAT_DATA_DIR 时，数据目录跟着「运行账号的 APPDATA」走。")
        print("      换个账号运行就换一个目录，这也是「同样的代码换台机器就报权限错」的常见原因。")

    line("3. 目录与属性")
    data_dir = Path(paths.DATA_DIR)
    print("DATA_DIR 存在  : %s" % data_dir.is_dir())
    for sub in ("logs", "sessions", "skills", "uploads"):
        p = data_dir / sub
        print("  %-9s 存在=%-5s 目录=%-5s" % (sub, p.exists(), p.is_dir()))
    print("只读属性       : %s" % run_ps(
        "(Get-Item -LiteralPath '%s' -Force).Attributes" % data_dir,
    ))
    print("目录属主       : %s" % run_ps(
        "(Get-Acl -LiteralPath '%s').Owner" % data_dir,
    ))
    print("ACL（全部）    :")
    print(run_ps(
        "Get-Acl -LiteralPath '%s' | Select-Object -ExpandProperty Access | "
        "ForEach-Object { '{0,-45} {1,-38} {2,-6} {3}' -f $_.IdentityReference, "
        "$_.FileSystemRights, $_.AccessControlType, "
        "$(if ($_.IsInherited) {'继承'} else {'显式'}) }" % data_dir,
    ))

    line("4. 逐目录实测写入")
    results = {}
    for sub in ("", "logs", "sessions", "skills", "uploads"):
        target = data_dir / sub if sub else data_dir
        created = False
        if not target.is_dir():
            try:
                target.mkdir(parents=True, exist_ok=True)
                print("  %-9s 已创建" % (sub or "."))
            except OSError as exc:
                print("  %-9s 建不出来: [%s] %s" % (sub or ".", type(exc).__name__, exc))
                results[sub or "."] = False
                continue
        probe = target / ("_write_test_%s.tmp" % uuid.uuid4().hex[:8])
        try:
            probe.write_text("x", encoding="utf-8")
            probe.unlink()
            created = True
            print("  %-9s 可写" % (sub or "."))
        except OSError as exc:
            print("  %-9s 写不了: [%s] %s" % (sub or ".", type(exc).__name__, exc))
        results[sub or "."] = created

    line("5. 日志文件是否被别的进程占着")
    log_file = Path(paths.LOG_PATH) if hasattr(paths, "LOG_PATH") else data_dir / "logs" / "agent.log"
    if log_file.exists():
        print("日志文件: %s (%d 字节)" % (log_file, log_file.stat().st_size))
        print("尝试以追加方式打开：")
        try:
            with open(log_file, "a", encoding="utf-8"):
                pass
            print("  可以打开")
        except OSError as exc:
            print("  打不开: [%s] %s" % (type(exc).__name__, exc))
    else:
        print("日志文件还不存在: %s" % log_file)
    print("先确认没有另一个 app.py 还在跑（旧实例会占着 agent.log，且它会继续用旧账号的目录）:")
    print(run_ps(
        "Get-CimInstance Win32_Process -Filter \"Name like '%python%'\" | "
        "ForEach-Object { '{0,-8} {1,-22} {2}' -f $_.ProcessId, "
        "$(try { ($_.GetOwner()).User } catch { '?' }), $_.CommandLine }"
    ) or "  <没有 python 进程>")

    line("6. 判定与修复")
    broken = [name for name, ok in results.items() if not ok]
    if not broken:
        print("数据目录可写。如果 app.py 仍然报 PermissionError，先看第 5 节：")
        print("多半是另一个账号启动的旧实例占着日志，或者你当前跑的代码不是这份。")
        return 0

    print("不可写的目录: %s" % ", ".join(broken))
    owner = run_ps("(Get-Acl -LiteralPath '%s').Owner" % data_dir)
    print("目录属主: %s" % owner)
    print()
    print("---- 方案 A（最快，不改权限，数据换到可写盘）----")
    print("  set CLINELIKECHAT_DATA_DIR=D:\\GridStarData")
    print("  python app.py --host 0.0.0.0 --port 1231")
    print("  说明：数据目录跟着环境变量走，config.json 与会话历史在新目录里重建；")
    print("        旧数据要保留的话，把原目录整个拷过去即可。")
    print()
    print("---- 方案 B（保留原目录，修 ACL）----")
    print("  在【管理员】命令提示符（cmd）里整段执行，%%USERNAME%% 就是当前登录账号：")
    print("    for /f \"delims=\" %%%%U in ('whoami') do set ME=%%%%U")
    print("    或直接用：icacls \"%s\" /grant \"%%USERNAME%%:(OI)(CI)F\" /T /C /Q" % data_dir)
    print("    必要时先接管属主（目录属主不是当前账号时）：")
    print("      takeown /F \"%s\" /R /D Y" % data_dir)
    print("      icacls \"%s\" /reset /T /C /Q" % data_dir)
    print("      icacls \"%s\" /grant \"%%USERNAME%%:(OI)(CI)F\" /T /C /Q" % data_dir)
    print()
    print("---- 方案 C（清掉旧状态，让程序自己按当前账号重建）----")
    print("  先确认没有 app.py 在跑，用改名而不是直接删，便于回退：")
    print("    ren \"%s\" ClineLikeChat_old" % data_dir)
    print("    python app.py --host 0.0.0.0 --port 1231")
    print()
    print("注意：目录若由别的账号（SYSTEM、另一个管理员、早先的服务账号）创建，")
    print("      即使你是本机管理员，未提权也未必写得进去；方案 B 是最直接的修法。")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())