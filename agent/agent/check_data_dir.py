"""内网部署自检：确认固定数据目录能不能写，不能写就说明修没修好。

用法（在跑 app.py 的那个 Python 环境、那个账号下执行）::

    python check_data_dir.py

脚本只读不写业务数据：唯一的写操作是在数据目录里建一个临时文件再删掉，用来实测
权限。--repair 会真正调用与启动流程相同的修权限逻辑。
"""
from __future__ import annotations

import argparse
import getpass
import os
import subprocess
import sys
import uuid
from pathlib import Path

AGENT_DIR = Path(__file__).resolve().parent
if str(AGENT_DIR) not in sys.path:
    sys.path.insert(0, str(AGENT_DIR))


def line(title: str) -> None:
    print("\n" + "=" * 68)
    print(title)
    print("=" * 68)


def run(cmd: list, encoding: str = "") -> str:
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
    parser = argparse.ArgumentParser(description="数据目录自检")
    parser.add_argument("--repair", action="store_true",
                        help="当场执行与启动流程相同的修权限逻辑")
    args = parser.parse_args()

    line("1. 运行身份")
    print("当前账号      : %s\\%s" % (os.environ.get("USERDOMAIN", "?"), getpass.getuser()))
    print("Python        : %s" % sys.executable)
    print("脚本所在目录  : %s" % AGENT_DIR)
    import data_dir_guard as guard

    print("是否提权运行  : %s" % guard.is_admin())
    print("注意：进程是不是管理员并不决定能否写自己的数据目录；")
    print("      决定权在目录 ACL 和目录属主，见第 3 节。")

    line("2. 数据目录解析结果")
    import paths

    print("数据目录固定为当前用户数据目录下的 %s，不提供自定义开关。" % paths.DATA_DIR_NAME)
    print("USERPROFILE   : %r" % os.environ.get("USERPROFILE", ""))
    print("APPDATA       : %r" % os.environ.get("APPDATA", ""))
    print("-> DATA_DIR   : %s" % paths.DATA_DIR)
    print("-> LOG_DIR    : %s" % paths.LOG_DIR)
    print("-> SESSIONS   : %s" % paths.SESSIONS_DIR)
    for notice in paths.startup_notices():
        print("启动提示      : %s" % notice)

    line("3. 目录与属性")
    data_dir = Path(paths.DATA_DIR)
    print("DATA_DIR 存在  : %s" % data_dir.is_dir())
    for sub in ("logs", "sessions", "skills", "uploads"):
        p = data_dir / sub
        print("  %-9s 存在=%-5s 目录=%-5s" % (sub, p.exists(), p.is_dir()))
    print("目录属主       : %s" % run_ps(
        "(Get-Acl -LiteralPath '%s').Owner" % data_dir))
    print("ACL（全部）    :")
    print(run_ps(
        "Get-Acl -LiteralPath '%s' | Select-Object -ExpandProperty Access | "
        "ForEach-Object { '{0,-45} {1,-38} {2,-6} {3}' -f $_.IdentityReference, "
        "$_.FileSystemRights, $_.AccessControlType, "
        "$(if ($_.IsInherited) {'继承'} else {'显式'}) }" % data_dir))

    line("4. 逐目录实测写入")
    results = {}
    for sub in ("", "logs", "sessions", "skills", "uploads"):
        target = data_dir / sub if sub else data_dir
        if not target.is_dir():
            try:
                target.mkdir(parents=True, exist_ok=True)
            except OSError as exc:
                print("  %-9s 建不出来: [%s] %s" % (sub or ".", type(exc).__name__, exc))
                results[sub or "."] = False
                continue
        probe = target / ("_write_test_%s.tmp" % uuid.uuid4().hex[:8])
        try:
            probe.write_text("x", encoding="utf-8")
            probe.unlink()
            print("  %-9s 可写" % (sub or "."))
            results[sub or "."] = True
        except OSError as exc:
            print("  %-9s 写不了: [%s] %s" % (sub or ".", type(exc).__name__, exc))
            results[sub or "."] = False

    line("5. 日志文件是否被别的进程占着")
    log_file = data_dir / "logs" / "agent.log"
    if log_file.exists():
        print("日志文件: %s (%d 字节)" % (log_file, log_file.stat().st_size))
        try:
            with open(log_file, "a", encoding="utf-8"):
                pass
            print("  可以打开")
        except OSError as exc:
            print("  打不开: [%s] %s" % (type(exc).__name__, exc))
    else:
        print("日志文件还不存在: %s" % log_file)
    print("先确认没有另一个 app.py 还在跑（旧实例占着 agent.log，并继续用旧目录）:")
    print(run_ps(
        "Get-CimInstance Win32_Process -Filter \"Name like '%python%'\" | "
        "ForEach-Object { '{0,-8} {1,-22} {2}' -f $_.ProcessId, "
        "$(try { ($_.GetOwner()).User } catch { '?' }), $_.CommandLine }"
    ) or "  <没有 python 进程>")

    line("6. 判定")
    if all(results.values()):
        print("数据目录可写，无需处理。")
        return 0

    broken = [name for name, ok in results.items() if not ok]
    print("不可写的目录: %s" % ", ".join(broken))
    print("目录属主    : %s" % run_ps("(Get-Acl -LiteralPath '%s').Owner" % data_dir))

    if args.repair:
        line("7. 当场执行修权限（与启动流程同一套逻辑）")
        ok, steps = guard.repair_acl(data_dir)
        for step in steps:
            print("  %s" % step)
        print("结果: %s" % ("已修好" if ok else "仍不可写"))
        if ok:
            return 0
    else:
        line("7. 下一步")
        print("直接跑一次: python app.py --host 0.0.0.0 --port 1231")
        print("  启动流程会自动尝试修权限；当场修不动且当前进程未提权时，")
        print("  会用 UAC 提权重启一次并把数据目录钉住。")
        print("想在这里先试修一次: python check_data_dir.py --repair")

    print()
    print("---- 人工兜底（管理员 cmd，%%USERNAME%% 换成本机实际账号）----")
    print("  takeown /F \"%s\" /R /D Y" % data_dir)
    print("  icacls \"%s\" /reset /T /C /Q" % data_dir)
    print("  icacls \"%s\" /grant \"%%USERNAME%%:(OI)(CI)F\" /T /C /Q" % data_dir)
    print()
    print("注意：目录若由别的账号（SYSTEM、早先的服务账号、另一个管理员）创建，")
    print("      即使你是本机管理员，未提权也未必写得进去。")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())