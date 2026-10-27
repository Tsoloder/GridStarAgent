#!/usr/bin/env python3
r"""GridStarAgent 离线极简分发包：只带 server.py + app.py 跑起来所需的一切。

产物结构::

    dist/GridStarAgent_offline_<日期_时刻>/
    ├── runtime/            便携 CPython 3.12 + 全部 pip 依赖（目标机不用装 Python）
    ├── agent/              server.py  client.py  registry.py  tools/  prompt/  agent/
    ├── webui/              WebUI 静态资源（app.py 按 parents[2] 找它）
    ├── res/                skills 等资源
    ├── exports/            会话导出目录（包内可写）
    ├── output/             工具默认输出目录（包内可写）
    ├── 启动.bat            先起 MCP(5656)，再起 Web 服务(1231)，自动打开浏览器
    ├── 停止.bat            按端口结束这两个服务
    ├── 自检.bat + selftest.py   解释器/依赖/文件/数据目录/端口 + 真启动一次 app.py
    └── 部署说明.txt

为什么不用 PyInstaller
    本项目所有资源路径都是 ``Path(__file__).parents[2]`` 推出来的（app.py 的
    WEBUI_DIR、paths.py 的 APP_ROOT），冻结后 ``__file__`` 指向 _MEIPASS 会全部
    错位；而且打包真正贵的是站点依赖、不是源码保密。所以这里搬的是“一份完整
    CPython + 一份 venv 的 site-packages”，目录结构与仓库完全一致，业务代码一行不改。

为什么不是 --zip 才有 zip
    目录版方便现场改东西、看窗口报错；--zip 只是多压一层，给拷贝用。

用法::

    python package_offline.py                 # 出目录版，并做三项自检（含真启动一次）
    python package_offline.py --zip           # 额外压成 zip（拷到目标机解压即可）
    python package_offline.py --out D:\fast   # 换输出根目录（换到别的盘会快很多）
    python package_offline.py --no-verify     # 跳过自检与冒烟（只想快速出包时用）
    python package_offline.py --base-python C:\Python312\python.exe

不包含：voice_asr（语音，可选，缺失只影响 /asr）与 PartSeparation（部件分割，另有一个
GB 级包）。需要它们请用仓库里的 package_portable.py。
"""

from __future__ import annotations

import argparse
import datetime as _dt
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DEFAULT_OUT = ROOT / "dist"

# 只搬运行期真正需要的三层：agent 代码、webui 静态、res 资源
SOURCE_DIRS = ("agent", "webui", "res")
SOURCE_FILES = ("README.md", "LICENSE")

# 标准库/解释器目录里不进包的部分
LIB_SKIP = {
    "site-packages", "test", "idlelib", "ensurepip", "lib2to3",
    "turtledemo", "tkinter", "distutils", "tcl", "__pycache__",
}
PY_ROOT_FILES = ("python.exe", "pythonw.exe", "python3.dll",
                 "vcruntime140.dll", "vcruntime140_1.dll", "LICENSE.txt")
# 干净 Windows 上未必有 VC++ Redist，而这些 DLL 缺一个就是 "DLL load failed"
MSVC_DLLS = ("msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
             "vcruntime140.dll", "vcruntime140_1.dll", "concrt140.dll",
             "vcomp140.dll")
# 必须能导入，失败即中止（装出来就是废包）
CORE_IMPORTS = ("fastapi", "uvicorn", "fastmcp", "starlette", "sse_starlette",
                "httpx", "openai", "requests", "pydantic", "yaml", "sqlite3", "ssl")
# 缺失只降级对应工具组
SOFT_IMPORTS = ("mss", "uiautomation", "win32api", "comtypes", "PIL", "openpyxl", "pypdf")

COPY_IGNORE = shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo", ".pytest_cache")


# ------------------------------------------------------------------ 小工具
def human(n: float) -> str:
    return "%.1f MB" % (n / 1024 / 1024)


def dir_stats(path: Path) -> tuple[int, float]:
    count = 0
    total = 0
    for base, _dirs, files in os.walk(path):
        for name in files:
            try:
                total += os.path.getsize(os.path.join(base, name))
                count += 1
            except OSError:
                pass
    return count, float(total)


def say(msg: str = "") -> None:
    print(msg, flush=True)


def free_port() -> int:
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def port_busy(port: int) -> bool:
    sock = socket.socket()
    sock.settimeout(0.4)
    busy = sock.connect_ex(("127.0.0.1", port)) == 0
    sock.close()
    return busy


def blank_env(extra: dict | None = None) -> dict:
    """模拟一台什么都没装的目标机：PATH 只留系统目录。

    不清 PATH 的话，开发机的 conda / VS / CUDA 路径会偷偷补上包里缺的 DLL，
    “本机能跑”就证明不了“目标机能跑”。
    """
    env = {k: v for k, v in os.environ.items()
           if k.upper() not in ("PYTHONPATH", "PYTHONHOME", "PYTHONSTARTUP",
                                "VIRTUAL_ENV", "CONDA_PREFIX", "CONDA_DEFAULT_ENV",
                                "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY")}
    root = os.environ.get("SystemRoot", r"C:\Windows")
    env["PATH"] = root + r"\System32;" + root
    if extra:
        env.update(extra)
    return env


# ------------------------------------------------------------ 基础解释器定位
def find_base_python(explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit).expanduser()
        if path.is_file():
            return path.resolve()
        raise SystemExit("指定的基础解释器不存在: %s" % path)

    cfg = ROOT / ".venv" / "pyvenv.cfg"
    if cfg.is_file():
        text = cfg.read_text(encoding="utf-8", errors="replace")
        match = re.search(r"^home\s*=\s*(.+)$", text, re.MULTILINE)
        if match:
            cand = Path(match.group(1).strip()) / "python.exe"
            if cand.is_file():
                say("== 基础解释器（读自 .venv/pyvenv.cfg）: %s" % cand)
                return cand.resolve()

    for cmd in (["py", "-3.12", "-c", "import sys;print(sys.executable)"],
                ["py", "-3", "-c", "import sys;print(sys.executable)"],
                ["python", "-c", "import sys;print(sys.executable)"]):
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.SubprocessError):
            continue
        if out.returncode == 0 and out.stdout.strip():
            cand = Path(out.stdout.strip())
            if cand.is_file():
                say("== 基础解释器（自动探测）: %s" % cand)
                return cand.resolve()

    raise SystemExit(
        "找不到基础解释器。请用 --base-python 指定一个完整 CPython 3.12 安装里的 python.exe\n"
        "（形如 C:\\Python312\\python.exe；不能是 embeddable 精简包，也不能是还没装依赖的 venv）"
    )


def check_base(base_py: Path) -> Path:
    base = base_py.parent
    missing = [name for name in ("Lib", "DLLs") if not (base / name).is_dir()]
    if missing:
        raise SystemExit("%s 下缺少 %s，不像一个完整的 CPython 安装" % (base, ", ".join(missing)))
    probe = subprocess.run([str(base_py), "-c", "import sys;print(sys.version)"],
                           capture_output=True, text=True)
    if probe.returncode != 0:
        raise SystemExit("基础解释器无法运行: %s" % probe.stderr.strip())
    say("   版本: %s" % probe.stdout.strip())
    return base


# ---------------------------------------------------------------- 组装 runtime
def build_runtime(base: Path, venv_sp: Path, rt: Path) -> None:
    rt.mkdir(parents=True, exist_ok=True)

    copied = []
    for name in PY_ROOT_FILES:
        src = base / name
        if src.is_file():
            shutil.copy2(src, rt / name)
            copied.append(name)
    for dll in base.glob("python3*.dll"):
        if dll.name not in copied:
            shutil.copy2(dll, rt / dll.name)
            copied.append(dll.name)
    say("   解释器文件: %s" % ", ".join(sorted(copied)))

    # MSVC 运行时：base 里有就用 base 的，没有就从 system32 取。解释器所在目录
    # 在 DLL 搜索顺序里优先于 system32，放这里最稳。
    sys32 = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
    for name in MSVC_DLLS:
        if (rt / name).exists():
            continue
        for cand_dir in (base, sys32):
            src = cand_dir / name
            if src.is_file():
                shutil.copy2(src, rt / name)
                break

    shutil.copytree(base / "DLLs", rt / "DLLs", ignore=COPY_IGNORE, dirs_exist_ok=True)

    lib = rt / "Lib"
    lib.mkdir(exist_ok=True)
    for item in (base / "Lib").iterdir():
        if item.name in LIB_SKIP:
            continue
        dst = lib / item.name
        if item.is_dir():
            shutil.copytree(item, dst, ignore=COPY_IGNORE)
        else:
            shutil.copy2(item, dst)

    # 站点依赖：用 venv 的那一份（基础解释器自带的 site-packages 里没我们的依赖）
    target_sp = lib / "site-packages"
    target_sp.mkdir(exist_ok=True)
    for item in venv_sp.iterdir():
        name = item.name
        # pip 只在装新包时有用，运行期用不到，白占 10MB
        if name == "pip" or (name.startswith("pip-") and name.endswith(".dist-info")):
            continue
        if name == "__pycache__":
            continue
        dst = target_sp / name
        if item.is_dir():
            shutil.copytree(item, dst, ignore=COPY_IGNORE)
        else:
            shutil.copy2(item, dst)

    count, size = dir_stats(rt)
    say("   runtime 合计: %d 个文件, %s" % (count, human(size)))


# ---------------------------------------------------------------- 拷贝与自检
def copy_sources(stage: Path) -> None:
    say("== 拷贝源码与资产")
    for name in SOURCE_DIRS:
        src = ROOT / name
        if not src.is_dir():
            raise SystemExit("仓库里缺目录: %s" % src)
        shutil.copytree(src, stage / name, ignore=COPY_IGNORE)
        say("   %s/" % name)
    for name in SOURCE_FILES:
        src = ROOT / name
        if src.is_file():
            shutil.copy2(src, stage / name)
            say("   %s" % name)
    (stage / "output").mkdir(exist_ok=True)
    (stage / "output" / ".keep").write_text("工具默认输出目录\n", encoding="utf-8")
    (stage / "exports").mkdir(exist_ok=True)


def verify_runtime(py: Path, stage: Path) -> None:
    env = blank_env()
    probe = subprocess.run([str(py), "-c", "import sys;print(sys.version.split()[0]);print(sys.prefix)"],
                           capture_output=True, text=True, cwd=str(stage), env=env)
    if probe.returncode != 0:
        raise SystemExit("便携解释器无法启动:\n%s" % probe.stderr.strip())
    version, prefix = probe.stdout.strip().splitlines()[:2]
    say("== 自检 1/3 解释器: Python %s, prefix=%s" % (version, prefix))
    say("   （PATH 已清到只剩系统目录，等价于一台空白机）")

    for group, names, fatal in (("核心依赖", CORE_IMPORTS, True), ("可选依赖", SOFT_IMPORTS, False)):
        res = subprocess.run([str(py), "-c", "import %s" % ", ".join(names)],
                             capture_output=True, text=True, cwd=str(stage), env=env)
        if res.returncode == 0:
            say("   %s: 全部可导入" % group)
        elif fatal:
            tail = "\n".join(res.stderr.strip().splitlines()[-6:])
            raise SystemExit("%s 导入失败，装出来会是废包：\n%s" % (group, tail))
        else:
            say("   %s: 部分缺失（只降级对应工具组）" % group)


def verify_server_import(py: Path, stage: Path) -> None:
    """server.py 的模块级导入就是它的全部启动逻辑（端口在 __main__ 里才绑）。"""
    say("== 自检 2/3 MCP 服务导入（server.py 全部工具模块）")
    res = subprocess.run([str(py), "-c", "import server; print('mcp ok')"],
                         capture_output=True, text=True, cwd=str(stage / "agent"),
                         env=blank_env())
    if res.returncode != 0:
        tail = "\n".join(res.stderr.strip().splitlines()[-12:])
        raise SystemExit("server.py 导入失败：\n%s" % tail)
    say("   %s" % res.stdout.strip())


def verify_app_boot(py: Path, stage: Path, timeout: int = 150) -> None:
    """真把 app.py 起一次：空白环境、空闲端口、临时数据目录，然后请求 /ui/。"""
    say("== 自检 3/3 真启动一次 app.py（临时数据目录，不碰真实配置）")
    agent_dir = stage / "agent" / "agent"
    port = free_port()
    tmp = Path(tempfile.mkdtemp(prefix="gs_boot_"))
    env = blank_env({"GRIDSTAR_DATA_DIR_PINNED": str(tmp / "data")})
    log_path = tmp / "boot.log"
    log_file = open(log_path, "wb")
    proc = subprocess.Popen(
        [str(py), "app.py", "--host", "127.0.0.1", "--port", str(port)],
        cwd=str(agent_dir), env=env, stdout=log_file, stderr=subprocess.STDOUT,
        creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0),
    )
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    ok = False
    deadline = time.time() + timeout
    try:
        while time.time() < deadline:
            if proc.poll() is not None:
                break
            try:
                with opener.open("http://127.0.0.1:%d/ui/" % port, timeout=3) as resp:
                    if resp.status == 200:
                        ok = True
                        break
            except Exception:
                time.sleep(0.8)
    finally:
        log_file.close()
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       capture_output=True)
        try:
            proc.wait(timeout=15)
        except subprocess.SubprocessError:
            pass

    if not ok:
        text = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        tail = "\n".join(text[-25:]) if text else "(无输出)"
        shutil.rmtree(tmp, ignore_errors=True)
        raise SystemExit("app.py 起不来，包里是坏的：\n%s" % tail)

    say("   端口 %d 上 /ui/ 返回 200，Web 服务可用" % port)
    shutil.rmtree(tmp, ignore_errors=True)


# ------------------------------------------------------------------- 启动脚本
LAUNCH_BAT = r"""@echo off
setlocal EnableExtensions
title GridStarAgent 离线版 - 启动
cd /d "%~dp0"

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "PY=%ROOT%\runtime\python.exe"
set "AGENT_PORT=1231"
set "MCP_PORT=5656"
if not defined AGENT_HOST set "AGENT_HOST=127.0.0.1"

if not exist "%PY%" goto no_runtime

echo ============================================================
echo   GridStarAgent 离线版
echo   根目录 : %ROOT%
echo   解释器 : %PY%
echo ============================================================
echo.

rem ---------------- [1/2] MCP 工具服务 : 5656 ----------------
call :port_busy %MCP_PORT%
if not errorlevel 1 goto mcp_up
echo [1/2] 启动 MCP 工具服务（端口 %MCP_PORT%）...
start "GridStar MCP %MCP_PORT%" /D "%ROOT%\agent" cmd /k ""%PY%" server.py"
call :wait_port %MCP_PORT% 90
if errorlevel 1 goto mcp_timeout
echo       就绪
goto mcp_done
:mcp_up
echo [1/2] 端口 %MCP_PORT% 已在监听，跳过 MCP
goto mcp_done
:mcp_timeout
echo       [失败] 90 秒内端口 %MCP_PORT% 没起来，看 MCP 那个窗口的报错
echo.
echo 按任意键退出（已经起的窗口不会被关掉）
pause >nul
exit /b 1
:mcp_done
echo.

rem ---------------- [2/2] Web 服务 : 1231 -------------------
call :port_busy %AGENT_PORT%
if not errorlevel 1 goto app_up
echo [2/2] 启动 Web 服务（端口 %AGENT_PORT%）...
start "GridStar Agent %AGENT_PORT%" /D "%ROOT%\agent\agent" cmd /k ""%PY%" app.py --host %AGENT_HOST% --port %AGENT_PORT%"
call :wait_port %AGENT_PORT% 120
if errorlevel 1 goto app_timeout
echo       就绪
goto app_done
:app_up
echo [2/2] 端口 %AGENT_PORT% 已在监听，跳过 Web 服务
goto app_done
:app_timeout
echo       [失败] 120 秒内端口 %AGENT_PORT% 没起来，看 Agent 那个窗口的报错
echo.
echo 按任意键退出（已经起的窗口不会被关掉）
pause >nul
exit /b 1
:app_done
echo.

start "" http://%AGENT_HOST%:%AGENT_PORT%/ui/

echo ============================================================
echo   全部启动
echo   界面 : http://%AGENT_HOST%:%AGENT_PORT%/ui/
echo   MCP  : http://127.0.0.1:%MCP_PORT%/sse
echo.
echo   首次使用：在界面里填大模型的 API 地址与 Key（内网就填内网地址）。
echo   停止：双击 停止.bat，或直接关掉这两个控制台窗口。
echo ============================================================
echo.
pause
exit /b 0

:no_runtime
echo [失败] 找不到便携解释器: %PY%
echo        多半是解压不完整，重新解压一遍整个目录再试。
pause
exit /b 1

rem ------------------------- 子过程 --------------------------
:port_busy
netstat -ano | findstr /r /c:":%~1 .*LISTENING" >nul 2>nul
exit /b %errorlevel%

:wait_port
set "W=0"
:wait_loop
call :port_busy %~1
if not errorlevel 1 exit /b 0
set /a W+=1
if %W% geq %~2 exit /b 1
ping -n 2 127.0.0.1 >nul
goto wait_loop
"""

STOP_BAT = r"""@echo off
setlocal EnableExtensions EnableDelayedExpansion
title GridStarAgent 离线版 - 停止
cd /d "%~dp0"

echo 正在结束 1231（Web）与 5656（MCP）上的进程...
for %%P in (1231 5656) do (
    for /f "tokens=5" %%p in ('netstat -ano ^| findstr /r /c:":%%P .*LISTENING"') do (
        echo   端口 %%P : 结束 PID %%p
        call :kill_one %%p
    )
)
echo 完成。
ping -n 3 127.0.0.1 >nul
exit /b 0

:kill_one
rem 先记下父进程（就是拉起服务的那个 cmd /k 窗口），杀完 python 再把它一并关掉。
rem 只杀 python 的话，cmd /k 会退回到提示符、窗口留在桌面上，看着像没停干净。
set "PARENT="
for /f "usebackq delims=" %%q in (`powershell -NoProfile -Command "$p=Get-CimInstance Win32_Process -Filter 'ProcessId=%~1'; if($p){ $pp=Get-CimInstance Win32_Process -Filter ('ProcessId='+$p.ParentProcessId); if($pp -and $pp.Name -eq 'cmd.exe'){ $pp.ProcessId } }"`) do set "PARENT=%%q"
taskkill /F /T /PID %~1 >nul 2>nul
if defined PARENT taskkill /F /T /PID !PARENT! >nul 2>nul
exit /b 0
"""

SELFTEST_BAT = r"""@echo off
chcp 65001 >nul
cd /d "%~dp0"
if not exist "%~dp0runtime\python.exe" (
    echo [失败] 找不到 runtime\python.exe，解压可能不完整
    pause
    exit /b 1
)
"%~dp0runtime\python.exe" "%~dp0selftest.py"
pause
"""

DEPLOY_NOTE = """GridStarAgent 离线版（生成于 {date}）

一、怎么用
   1. 把整个目录拷到目标机（任意路径都行，路径里尽量别有空格和中文以外的问题字符），
      或者解压本目录同名的 zip。
   2. 双击 自检.bat。全部 PASS 就说明这台机器能跑；有 FAIL 会直接写原因。
   3. 双击 启动.bat。会依次拉起两个服务并自动打开界面：
        MCP 工具服务  127.0.0.1:5656    （agent\\server.py）
        Web 服务      127.0.0.1:1231    （agent\\agent\\app.py）
      浏览 http://127.0.0.1:1231/ui/
   4. 双击 停止.bat 结束这两个服务。

二、目标机需要什么
   不需要 Python、不需要 pip、不需要联网、不需要装 VC++ 运行库。
   包里是一份完整的 CPython {pyver} + 全部依赖，双击就能用。

三、第一次启动要配什么
   界面里填大模型的 API 地址与 Key。内网就填内网地址（vLLM / Ollama / one-api 都行）。
   配置存在 %USERPROFILE%\\AppData\\Roaming\\GridstarAgent.data\\config.json
   换包不丢数据；换 Windows 账号会看不到旧数据。

四、这个包里有什么、没有什么
   有：MCP 工具服务、Agent 后端、WebUI 前端、skills 资源。
   没有：voice_asr（语音转文字，缺失只影响 /asr 接口）、
         PartSeparation（部件分割服务，那是另一个 GB 级包）。
   部件分割相关的 ProcessWithServer 需要另配服务地址，单独部署。

五、排障
   每个服务一个控制台窗口，报错直接打在窗口里，别急着关。
   端口被占：先 停止.bat；还不行就 netstat -ano ^| findstr 1231 看 PID，再 taskkill /PID <pid> /F
   界面打不开：确认 1231 在监听，浏览器地址带 /ui/ 后缀
   自检报 DLL 缺失：说明解压不完整（zip 没解完），重新解压
"""


def write_launchers(stage: Path, py_version: str) -> None:
    # .bat 里的中文按 Windows ANSI 码页（zh-CN 即 GBK）落盘。用 UTF-8 写 bat 会被
    # cmd 的解析偏移弄丢紧跟中文 echo 的那一行（实测），GBK 与系统 ANSI 码页一致、
    # 不需要 chcp，是传统 Windows 脚本的正确选择。
    say("== 生成启动脚本（bat 编码: GBK）")

    def write(name: str, text: str) -> None:
        body = text.replace("\r\n", "\n").replace("\n", "\r\n")
        (stage / name).write_text(body, encoding="gbk", newline="")
        say("   %s" % name)

    write("启动.bat", LAUNCH_BAT)
    write("停止.bat", STOP_BAT)
    write("自检.bat", SELFTEST_BAT)
    note = DEPLOY_NOTE.replace("{date}", _dt.datetime.now().strftime("%Y-%m-%d %H:%M"))
    note = note.replace("{pyver}", py_version)
    (stage / "部署说明.txt").write_text(note.replace("\n", "\r\n"), encoding="gbk", newline="")
    say("   部署说明.txt")


# ------------------------------------------------------------------ 包内自检
SELFTEST_PY = r'''# -*- coding: utf-8 -*-
"""离线包自检：解释器、依赖、程序文件、数据目录、端口，最后真启动一次 app.py。

退出码 0 表示没有 FAIL（WARN 不影响启动）。双击「自检.bat」运行。
"""
import importlib
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PY = ROOT / "runtime" / "python.exe"
FAILS, WARNS = [], []


def ok(msg):
    print("   [PASS] " + msg)


def bad(msg):
    print("   [FAIL] " + msg)
    FAILS.append(msg)


def warn(msg):
    print("   [WARN] " + msg)
    WARNS.append(msg)


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def port_busy(port):
    sock = socket.socket()
    sock.settimeout(0.4)
    busy = sock.connect_ex(("127.0.0.1", port)) == 0
    sock.close()
    return busy


print("== 1. 解释器 ==")
print("   exe : %s" % sys.executable)
print("   ver : %s" % sys.version.split()[0])
if str(Path(sys.executable)).lower().startswith(str(ROOT).lower()):
    ok("用的是包内 runtime，目标机不需要装 Python")
else:
    warn("解释器不在包内，可能误用了系统 Python")

print("== 2. 依赖 ==")
for name in ("fastapi", "uvicorn", "fastmcp", "starlette", "sse_starlette",
             "httpx", "openai", "requests", "pydantic", "yaml", "sqlite3", "ssl"):
    try:
        importlib.import_module(name)
        ok(name)
    except Exception as exc:
        bad("%s: %s" % (name, exc))
for name in ("mss", "uiautomation", "win32api", "comtypes", "PIL", "openpyxl", "pypdf"):
    try:
        importlib.import_module(name)
        ok(name + "（可选）")
    except Exception as exc:
        warn("%s 未就绪，只影响对应工具组: %s" % (name, exc))

print("== 3. 程序文件 ==")
for rel in ("agent/server.py", "agent/client.py", "agent/registry.py", "agent/tools",
            "agent/prompt/base_prompt.md", "agent/agent/app.py",
            "webui/index.html", "webui/app.js", "res/skills"):
    if (ROOT / rel).exists():
        ok(rel)
    else:
        bad("缺少 " + rel)

print("== 4. 数据目录 ==")
try:
    sys.path.insert(0, str(ROOT / "agent" / "agent"))
    import paths
    data_dir = Path(paths.DATA_DIR)
    data_dir.mkdir(parents=True, exist_ok=True)
    probe = data_dir / ".write_probe"
    probe.write_text("ok", encoding="utf-8")
    probe.unlink()
    ok("可写: %s" % data_dir)
except Exception as exc:
    bad("数据目录不可用: %s" % exc)

print("== 5. 端口 ==")
for port, name in ((5656, "MCP 工具服务"), (1231, "Web 服务"), (1313, "格星软件")):
    if port_busy(port):
        print("   [占用] %s 端口 %d 已在监听" % (name, port))
    elif port == 1313:
        warn("格星软件未在 1313 监听，工具调用会失败（启动前请先打开它）")
    else:
        print("   [空闲] %s 端口 %d 尚未启动" % (name, port))

print("== 6. 真启动一次 app.py（临时数据目录，不碰你的配置）==")
if not PY.is_file():
    bad("找不到 %s" % PY)
else:
    port = free_port()
    tmp = Path(tempfile.mkdtemp(prefix="gs_selftest_"))
    env = dict(os.environ)
    env["GRIDSTAR_DATA_DIR_PINNED"] = str(tmp / "data")
    for key in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"):
        env.pop(key, None)
    log_path = tmp / "boot.log"
    with open(log_path, "wb") as log_file:
        proc = subprocess.Popen(
            [str(PY), "app.py", "--host", "127.0.0.1", "--port", str(port)],
            cwd=str(ROOT / "agent" / "agent"), env=env,
            stdout=log_file, stderr=subprocess.STDOUT,
        )
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    started = False
    deadline = time.time() + 150
    try:
        while time.time() < deadline:
            if proc.poll() is not None:
                break
            try:
                with opener.open("http://127.0.0.1:%d/ui/" % port, timeout=3) as resp:
                    if resp.status == 200:
                        started = True
                        break
            except Exception:
                time.sleep(0.8)
    finally:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       capture_output=True)
        try:
            proc.wait(timeout=15)
        except Exception:
            pass
    if started:
        ok("app.py 在端口 %d 上响应 /ui/，Web 服务能跑" % port)
    else:
        bad("app.py 起不来，见下面日志")
        lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        for line in lines[-20:]:
            print("        " + line)
    import shutil as _shutil
    _shutil.rmtree(tmp, ignore_errors=True)

print()
if FAILS:
    print("自检结果: %d 项 FAIL，%d 项 WARN" % (len(FAILS), len(WARNS)))
    for item in FAILS:
        print("  FAIL  " + item)
    sys.exit(1)
print("自检结果: 全部 PASS" + ("，%d 项 WARN" % len(WARNS) if WARNS else ""))
sys.exit(0)
'''


# ------------------------------------------------------------------ 主流程
def main() -> None:
    parser = argparse.ArgumentParser(description="生成 GridStarAgent 离线极简分发包")
    parser.add_argument("--out", default=None, help="输出根目录，默认 dist")
    parser.add_argument("--zip", action="store_true", help="额外压成 zip")
    parser.add_argument("--base-python", default=None, help="基础解释器 python.exe 的完整路径")
    parser.add_argument("--no-verify", action="store_true", help="跳过自检与冒烟启动")
    parser.add_argument("--name", default=None, help="包目录名（默认带日期时间，不覆盖旧包）")
    args = parser.parse_args()

    started = time.time()
    out_root = Path(args.out).resolve() if args.out else DEFAULT_OUT
    if not out_root.is_absolute():
        out_root = (ROOT / out_root).resolve()
    out_root.mkdir(parents=True, exist_ok=True)

    name = args.name or ("GridStarAgent_offline_%s" % _dt.datetime.now().strftime("%Y%m%d_%H%M"))
    stage = out_root / name

    venv_sp = ROOT / ".venv" / "Lib" / "site-packages"
    if not venv_sp.is_dir():
        alt = ROOT / "venv" / "Lib" / "site-packages"
        if alt.is_dir():
            venv_sp = alt
        else:
            raise SystemExit("找不到 .venv\\Lib\\site-packages，先跑一次 一键部署.bat 把依赖装齐")

    say("=" * 62)
    say("GridStarAgent 离线打包")
    say("  来源 : %s" % ROOT)
    say("  目标 : %s" % stage)
    say("  含    : agent/ webui/ res/ + 便携 runtime（不含语音与部件分割）")
    say("=" * 62)
    say()

    dep_count, dep_size = dir_stats(venv_sp)
    say("== 站点依赖: %d 个文件, %s" % (dep_count, human(dep_size)))

    base_py = find_base_python(args.base_python)
    base = check_base(base_py)

    if stage.exists():
        if not args.name:
            raise SystemExit("目标目录已存在: %s（本不该发生，请检查系统时间）" % stage)
        say("== 清理同名旧产物: %s" % stage)
        try:
            shutil.rmtree(stage)
        except OSError as exc:
            raise SystemExit(
                "清理旧产物失败: %s\n"
                "多半是旧包里的服务还在跑（当前目录被占用）。先双击旧包的 停止.bat，"
                "或换一个输出目录: --out D:\\gs_offline" % exc
            )
    stage.mkdir(parents=True)

    say()
    say("== 组装便携 runtime（CPython + 依赖，约 200MB，等几十秒）")
    py = stage / "runtime" / "python.exe"
    build_runtime(base, venv_sp, stage / "runtime")

    say()
    copy_sources(stage)

    version = subprocess.run([str(py), "-c", "import sys;print('%d.%d' % sys.version_info[:2])"],
                             capture_output=True, text=True).stdout.strip() or "3.12"

    say()
    write_launchers(stage, version)
    (stage / "selftest.py").write_text(SELFTEST_PY, encoding="utf-8", newline="\r\n")
    say("   selftest.py")

    if not args.no_verify:
        say()
        verify_runtime(py, stage)
        say()
        verify_server_import(py, stage)
        say()
        verify_app_boot(py, stage)

    count, size = dir_stats(stage)
    say()
    say("=" * 62)
    say("打包完成")
    say("  目录 : %s" % stage)
    say("  规模 : %d 个文件, %s" % (count, human(size)))
    say("  耗时 : %.1f 秒" % (time.time() - started))

    if args.zip:
        say()
        say("== 生成 zip")
        zip_path = shutil.make_archive(str(stage), "zip",
                                       root_dir=str(stage.parent), base_dir=stage.name)
        say("  zip  : %s  (%s)" % (zip_path, human(os.path.getsize(zip_path))))

    say()
    say("下一步：拷到目标机 → 双击 自检.bat → 双击 启动.bat")
    say("=" * 62)


if __name__ == "__main__":
    main()