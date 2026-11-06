#!/usr/bin/env python3
"""内网离线分发包打包脚本。

产物结构::

    dist/GridStarAgent_intranet_<日期>.zip
    ├── GridStarAgent_intranet_<日期>/
    │   ├── agent/  QtChartWidget/  webui/  voice_asr/  res/  docs/ ...  (源码)
    │   ├── wheels/                     (离线依赖, pip --no-index 安装)
    │   ├── install_offline.bat         (内网一键安装)
    │   └── requirements.txt            (指向 agent/agent/requirements.txt 的副本)

语音识别（voice_asr）的二进制资产不在 git 里（见 .gitignore），由本脚本单独复制：
``voice_asr/runtime/``（whisper.dll、ggml*.dll、models/ggml-base.bin 147MB）与
``voice_asr/native/bin/asr_shim.dll``。没有它们内网机器上 /asr 会一直未就绪。

用法::

    python package_intranet.py                      # 源码 + 语音资产 + 离线依赖 (当前平台)
    python package_intranet.py --no-wheels          # 源码 + 语音资产
    python package_intranet.py --no-asr             # 不带语音资产（省约 145MB）
    python package_intranet.py --python-version 311 --platform win_amd64
"""

from __future__ import annotations

import argparse
import datetime as _dt
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DIST = ROOT / "dist"
REQUIREMENTS = ROOT / "agent" / "agent" / "requirements.txt"

# 语音模块的源码目录：--no-voice 时整块不带（app.py 里是 try/except 挂载，缺了只留一条 warning）
VOICE_DIR = "voice_asr"
# 本机临时目录（截图/对照），不应进分发包
EXCLUDE_TOP_DIRS = {"_qtrefined"}

INSTALL_BAT = """@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
set "ROOT=%~dp0"
if not defined PYTHON set "PYTHON=python"

echo ==== [1/3] create venv ====
"%PYTHON%" -m venv "%ROOT%venv"
if errorlevel 1 goto :fail
call "%ROOT%venv\\Scripts\\activate.bat"

echo ==== [2/3] install offline wheels ====
python -m pip install --no-index --find-links "%ROOT%wheels" --upgrade pip setuptools wheel
python -m pip install --no-index --find-links "%ROOT%wheels" -r "%ROOT%requirements.txt"
if errorlevel 1 goto :fail

echo ==== [3/3] done ====
echo.
echo venv : %ROOT%venv
echo.
echo 启动 Web 服务 ^(默认 http://127.0.0.1:1231^)：
echo   call "%ROOT%venv\\Scripts\\activate.bat"
echo   cd /d "%ROOT%agent\\agent"
echo   python app.py --host 0.0.0.0 --port 1231
echo.
echo 启动 MCP 工具服务 ^(可选, SSE 127.0.0.1:5656^)：
echo   cd /d "%ROOT%agent"  ^&^&  python server.py
echo.
goto :eof

:fail
echo.
echo [ERROR] 离线安装失败，请检查上方日志。
exit /b 1
"""


def _run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=str(ROOT), text=True, capture_output=True, **kw)


def collect_source_files() -> list[Path]:
    """收集需要归档的源码文件：git 跟踪文件 + 未被忽略的未跟踪文件。

    用 ``-z`` 取 NUL 分隔的原始路径：git 默认会把非 ASCII 路径做 C 引用
    （``"_qt_webui_diff/report/Qt\\344\\270\\216..."``），直接拼路径会让这些文件在
    ``is_file()`` 处被静默丢弃——本仓库有中文命名的文档。
    """
    res = _run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
               encoding="utf-8")
    if res.returncode != 0:
        raise SystemExit(f"git ls-files 失败: {res.stderr.strip()}")
    files = [ROOT / line for line in res.stdout.split("\0") if line]
    present = [f for f in files if f.is_file()]
    if len(present) != len(files):
        # 已跟踪但在磁盘上不存在（例如已暂存删除）时给出提示，避免再次静默丢文件
        gone = [f for f in files if not f.is_file()]
        print(f"== 警告: {len(gone)} 个清单文件在磁盘上不存在，已跳过 ==")
        for f in gone[:10]:
            print("   " + f.relative_to(ROOT).as_posix())
    return present


def copy_sources(stage: Path, skip_voice: bool = False) -> int:
    files = collect_source_files()
    skip = {ROOT / "package_intranet.py"}
    count = 0
    for src in files:
        if src in skip:
            continue
        rel = src.relative_to(ROOT)
        # 本机验收用的截图目录不进包（git 里没有忽略规则，这里显式排掉）
        if rel.parts and rel.parts[0] in EXCLUDE_TOP_DIRS:
            continue
        # --no-voice：语音模块（源码 + 资产）整块不带
        if skip_voice and rel.parts and rel.parts[0] == VOICE_DIR:
            continue
        dst = stage / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        count += 1
    return count


# --- 语音识别（voice_asr）二进制资产 ---
# 这些文件被 .gitignore 排除（147MB 模型 + 上游 DLL + 本机构建的 shim），
# 走 git ls-files 收不到，必须单独复制进离线包，否则内网机器上 /asr 永远未就绪。
ASR_ASSET_DIRS = (
    ROOT / "voice_asr" / "runtime",      # whisper.dll / ggml*.dll / VC 运行时 / models/ggml-base.bin
    ROOT / "voice_asr" / "native" / "bin",  # asr_shim.dll
)
# 模型缺失时不必让打包失败，但必须显式告警：这是「装完发现语音不能用」的根因。
# VC 运行时四项同样列入必查：本机因为有 Visual Studio 而一切正常，干净机器上
# 缺 MSVCP140/VCRUNTIME140/VCRUNTIME140_1/VCOMP140 会直接加载失败（VCOMP140 还得
# 归因于 ggml-cpu.dll 开了 OpenMP）。它们不随 Python 提供，也不在 Windows 自带组件里。
ASR_REQUIRED = (
    ROOT / "voice_asr" / "runtime" / "models" / "ggml-base.bin",
    ROOT / "voice_asr" / "runtime" / "whisper.dll",
    ROOT / "voice_asr" / "runtime" / "msvcp140.dll",
    ROOT / "voice_asr" / "runtime" / "vcruntime140.dll",
    ROOT / "voice_asr" / "runtime" / "vcruntime140_1.dll",
    ROOT / "voice_asr" / "runtime" / "vcomp140.dll",
    ROOT / "voice_asr" / "native" / "bin" / "asr_shim.dll",
)


def copy_asr_assets(stage: Path) -> tuple[int, int]:
    """复制 voice_asr 的原生资产。返回 (文件数, 总字节数)。"""
    count = 0
    total = 0
    for base in ASR_ASSET_DIRS:
        if not base.is_dir():
            continue
        for src in sorted(base.rglob("*")):
            if not src.is_file():
                continue
            rel = src.relative_to(ROOT)
            dst = stage / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            count += 1
            total += src.stat().st_size
    return count, total


def check_asr_assets() -> list[Path]:
    """返回缺失的关键语音资产（空列表 = 齐全）。"""
    return [p for p in ASR_REQUIRED if not p.is_file()]


# PyPI 上只有源码包（sdist）、没有 wheel 的依赖。pywebview -> proxy_tools 就是这种：
# 带 --platform/--abi 的下载被 pip 强制要求 --only-binary=:all:，主下载会直接报
# "No matching distribution found for proxy_tools"，整个内网包就卡在这一步。
# 处理顺序见 download_wheels：先取 setuptools/wheel，把这类包就地构建成 wheel
# （构建依赖走本地 wheel 目录，不联网），再让主下载用 --find-links 指回这个目录。
SDIST_ONLY = ("proxy_tools",)


def _name_prefixes(pkg: str) -> tuple[str, ...]:
    """sdist/wheel 文件名里的包名可能把 - 换成 _，两种都认。"""
    alt = pkg.replace("-", "_")
    return (pkg, alt) if alt != pkg else (pkg,)


def download_wheels(wheel_dir: Path, pyver: str, platform: str) -> int:
    wheel_dir.mkdir(parents=True, exist_ok=True)
    tags = ["--python-version", pyver, "--implementation", "cp",
            "--abi", f"cp{pyver}", "--platform", platform]

    # pip / setuptools / wheel 自身也带上：内网升级要用，下面构建 sdist 时也要用
    for extra in ("pip", "setuptools", "wheel"):
        subprocess.run(
            [sys.executable, "-m", "pip", "download", extra, "-d", str(wheel_dir),
             "--only-binary=:all:", *tags],
            cwd=str(ROOT),
        )

    # sdist-only 的包：下载源码包并构建成 wheel，已构建过的跳过（重复跑不必再来一次）
    for pkg in SDIST_ONLY:
        prefixes = _name_prefixes(pkg)
        hits = [w.name for p in prefixes for w in wheel_dir.glob(f"{p}-*.whl")]
        if hits:
            print(f"   sdist-only: {pkg} 已有 wheel（{', '.join(hits)}），跳过")
            continue
        print(f"   sdist-only: {pkg} 取源码包并就地构建 wheel")
        got = subprocess.run(
            [sys.executable, "-m", "pip", "download", pkg, "--no-deps",
             "--no-binary=:all:", "-d", str(wheel_dir)],
            cwd=str(ROOT),
        )
        # 这一步失败必须当场报错：静默跳过会让后面的主下载报一句
        # "proxy_tools 无匹配发行版"，把真正的原因埋在几百行回溯日志里。
        if got.returncode != 0:
            raise SystemExit(f"下载 {pkg} 源码包失败（缺它主下载会被 --only-binary 卡死）")
        sources = [f for p in prefixes
                   for pat in (f"{p}*.tar.gz", f"{p}*.zip")
                   for f in wheel_dir.glob(pat)]
        if not sources:
            raise SystemExit(f"{pkg}: 源码包没落到 {wheel_dir}，无法构建 wheel")
        for sdist in sources:
            built = subprocess.run(
                [sys.executable, "-m", "pip", "wheel", str(sdist), "--no-deps",
                 "--no-index", "--find-links", str(wheel_dir), "-w", str(wheel_dir)],
                cwd=str(ROOT),
            )
            if built.returncode != 0:
                raise SystemExit(f"构建 {sdist.name} 失败，内网侧将无法离线安装 {pkg}")
            sdist.unlink()   # 留源码包会让内网 pip 试图现场构建，反而更容易失败

    base = [
        sys.executable, "-m", "pip", "download",
        "-r", str(REQUIREMENTS),
        "-d", str(wheel_dir),
        "--only-binary=:all:",
        "--find-links", str(wheel_dir),
        *tags,
    ]
    print("== 下载离线依赖 wheel ==")
    print("   " + " ".join(base[1:]))
    res = subprocess.run(base, cwd=str(ROOT))
    if res.returncode != 0:
        raise SystemExit("pip download 失败，请检查网络或改用 --no-wheels")
    return len(list(wheel_dir.glob("*.whl")))


def _deploy_doc(stamp: str, with_wheels: bool, with_voice: bool) -> str:
    """内网部署说明：包里有什么、怎么装、怎么起，不依赖看的人懂仓库结构。"""
    voice_line = ("voice_asr/  语音转写模块（切换进包内）" if with_voice
                  else "voice_asr/  已整块剔除：/asr 接口不会挂载，其它功能不受影响")
    wheel_line = ("wheels/     离线依赖（install_offline.bat 一键装）" if with_wheels
                  else "wheels/     未内置离线依赖，按内网源或离线 wheel 自行安装")
    install = ("双击 install_offline.bat（建 venv + 离线装依赖）" if with_wheels
               else "python -m pip install -r requirements.txt")
    return f"""GridStarAgent 内网分发包  {stamp}

一、包里有什么
  agent/          Python 后端（FastAPI）：WebUI 服务与 MCP 工具服务
  webui/          前端（浏览器直接打开，也可由 Qt 内嵌）
  QtChartWidget/  Qt 界面库源码（需要时在内网自行编译）
  res/ docs/      资源与文档
  requirements.txt 依赖清单
  {wheel_line}

二、包里没有什么
  {voice_line}

三、安装依赖
  {install}
  注意：Python 版本要符合 requirements.txt 的要求，且先装完依赖再启动服务。

四、启动
  1) Web / Agent 服务（必需）
     cd agent\\agent
     python app.py --host 0.0.0.0 --port 1231
     浏览器打开 http://127.0.0.1:1231/ui/
  2) MCP 工具服务（可选，默认 SSE 127.0.0.1:5656）
     cd agent
     python server.py
  3) 桌面窗口（可选，需要 pywebview）
     python desktop.py

五、Qt 界面（QtChartWidget，可选）
  内网需 Qt 5.12.2 + VS2022。在 QtChartWidget\\ 下把 build.bat 里的 Qt / vcvars 路径改成本机后执行：
    build.bat            编库 + demo + 示例 + 回归测试
    build.bat shot       额外出一张离屏截图
  产物是 QtChartWidget\\bin\\QtChartWidget.dll，由宿主程序链接；demo.exe 可单独预览。
  只用 WebUI 时可完全忽略这一节。

六、配置与数据
  首次启动会在 %USERPROFILE%\\GridstarAgent.data 下建配置与数据目录，
  在设置页填入模型 API 地址与 Key 即可。同一台机器的不同 Windows 账号各自一份数据。

七、排障
  端口被占：换 --port，或 netstat -ano | findstr 1231 找到 PID 后 taskkill /PID <pid> /F
  页面空白：确认访问的是 /ui/ 后缀，且 1231 端口在监听
  语音按钮无效：本包未包含语音模块，属于预期行为
"""


def build_zip(stage: Path, zip_base: Path) -> Path:
    shutil.make_archive(str(zip_base), "zip", root_dir=str(stage.parent), base_dir=stage.name)
    return Path(f"{zip_base}.zip")


def main() -> None:
    ap = argparse.ArgumentParser(description="生成内网离线分发包")
    ap.add_argument("--no-wheels", action="store_true", help="只打包源码，不下载依赖")
    ap.add_argument("--no-asr", action="store_true",
                    help="不打包语音识别资产（whisper DLL 与 147MB 模型）")
    ap.add_argument("--no-voice", action="store_true",
                    help="完全不带语音模块：voice_asr 源码与原生资产都排除")
    ap.add_argument("--python-version", default="312",
                    help="目标 Python 版本，如 311 / 312 / 313（须与内网机器一致）")
    ap.add_argument("--platform", default="win_amd64", help="目标平台，如 win_amd64 / manylinux2014_x86_64")
    ap.add_argument("--out", default=None, help="输出 zip 路径")
    args = ap.parse_args()

    if not REQUIREMENTS.is_file():
        raise SystemExit(f"未找到依赖清单: {REQUIREMENTS}")

    stamp = _dt.datetime.now().strftime("%Y%m%d")
    name = f"GridStarAgent_intranet_{stamp}"
    stage = DIST / name

    if DIST.exists():
        shutil.rmtree(DIST)
    stage.mkdir(parents=True)

    n_src = copy_sources(stage, skip_voice=args.no_voice)
    print(f"== 源码归档: {n_src} 个文件 -> {stage}")
    (stage / "内网部署说明.txt").write_text(
        _deploy_doc(stamp, with_wheels=not args.no_wheels, with_voice=not args.no_voice),
        encoding="utf-8-sig", newline="\r\n")
    if args.no_voice:
        print(f"== 语音模块: 已按 --no-voice 整块排除（{VOICE_DIR}/ 不打包，/asr 路由不会挂载）==")

    n_asr = 0
    asr_mb = 0.0
    if args.no_voice:
        pass
    elif args.no_asr:
        print("== 语音识别资产: 已按 --no-asr 跳过（内网将无法使用语音输入）==")
    else:
        missing = check_asr_assets()
        if missing:
            print("== 警告: 语音识别关键资产缺失，内网装好后 /asr 将一直未就绪 ==")
            for p in missing:
                print(f"   缺: {p.relative_to(ROOT).as_posix()}")
            print("   补齐办法: python voice_asr/native/sync_runtime.py"
                  "（从 GS 安装包同步）后重新打包")
        n_asr, asr_bytes = copy_asr_assets(stage)
        asr_mb = asr_bytes / 1024 / 1024
        print(f"== 语音识别资产: {n_asr} 个文件, {asr_mb:.1f} MB")

    n_wheel = 0
    # 依赖清单总是给一份：内网有自己的源时直接 pip install -r 即可
    shutil.copy2(REQUIREMENTS, stage / "requirements.txt")
    if not args.no_wheels:
        n_wheel = download_wheels(stage / "wheels", args.python_version, args.platform)
        (stage / "install_offline.bat").write_text(INSTALL_BAT, encoding="utf-8", newline="\r\n")
        print(f"== 离线依赖: {n_wheel} 个 wheel")
    else:
        print("== 离线依赖: 已按 --no-wheels 跳过（只带 requirements.txt）==")

    zip_path = Path(args.out) if args.out else build_zip(stage, DIST / name)
    size_mb = zip_path.stat().st_size / 1024 / 1024
    print("\n==== 打包完成 ====")
    print(f"  文件: {zip_path}")
    print(f"  大小: {size_mb:.1f} MB")
    print(f"  源码: {n_src} 文件"
          + (f" | 语音: {n_asr} 文件 ({asr_mb:.1f} MB)" if n_asr else " | 语音: 未包含")
          + (f" | 依赖: {n_wheel} wheel" if n_wheel else " | 依赖: 未包含"))


if __name__ == "__main__":
    main()
