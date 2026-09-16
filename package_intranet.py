#!/usr/bin/env python3
"""内网离线分发包打包脚本。

产物结构::

    dist/GridStarAgent_intranet_<日期>.zip
    ├── GridStarAgent_intranet_<日期>/
    │   ├── agent/  QtChartWidget/  webui/  voice_asr/  res/  docs/ ...  (源码)
    │   ├── wheels/                     (离线依赖, pip --no-index 安装)
    │   ├── install_offline.bat         (内网一键安装)
    │   └── requirements.txt            (指向 agent/agent/requirements.txt 的副本)

用法::

    python package_intranet.py                      # 源码 + 离线依赖 (当前平台)
    python package_intranet.py --no-wheels          # 只打包源码
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
    """收集需要归档的源码文件：git 跟踪文件 + 未被忽略的未跟踪文件。"""
    res = _run(["git", "ls-files", "--cached", "--others", "--exclude-standard"])
    if res.returncode != 0:
        raise SystemExit(f"git ls-files 失败: {res.stderr.strip()}")
    files = [ROOT / line.strip() for line in res.stdout.splitlines() if line.strip()]
    return [f for f in files if f.is_file()]


def copy_sources(stage: Path) -> int:
    files = collect_source_files()
    skip = {ROOT / "package_intranet.py"}
    count = 0
    for src in files:
        if src in skip:
            continue
        rel = src.relative_to(ROOT)
        dst = stage / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        count += 1
    return count


def download_wheels(wheel_dir: Path, pyver: str, platform: str) -> int:
    wheel_dir.mkdir(parents=True, exist_ok=True)
    base = [
        sys.executable, "-m", "pip", "download",
        "-r", str(REQUIREMENTS),
        "-d", str(wheel_dir),
        "--only-binary=:all:",
        "--python-version", pyver,
        "--implementation", "cp",
        "--abi", f"cp{pyver}",
        "--platform", platform,
    ]
    print("== 下载离线依赖 wheel ==")
    print("   " + " ".join(base[1:]))
    res = subprocess.run(base, cwd=str(ROOT))
    if res.returncode != 0:
        raise SystemExit("pip download 失败，请检查网络或改用 --no-wheels")
    # pip / setuptools / wheel 自身也带上，便于内网升级
    for extra in ("pip", "setuptools", "wheel"):
        subprocess.run(
            [sys.executable, "-m", "pip", "download", extra, "-d", str(wheel_dir),
             "--only-binary=:all:", "--python-version", pyver, "--implementation", "cp",
             "--abi", f"cp{pyver}", "--platform", platform],
            cwd=str(ROOT),
        )
    return len(list(wheel_dir.glob("*.whl")))


def build_zip(stage: Path, zip_base: Path) -> Path:
    shutil.make_archive(str(zip_base), "zip", root_dir=str(stage.parent), base_dir=stage.name)
    return Path(f"{zip_base}.zip")


def main() -> None:
    ap = argparse.ArgumentParser(description="生成内网离线分发包")
    ap.add_argument("--no-wheels", action="store_true", help="只打包源码，不下载依赖")
    ap.add_argument("--python-version", default="313", help="目标 Python 版本，如 311 / 313")
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

    n_src = copy_sources(stage)
    print(f"== 源码归档: {n_src} 个文件 -> {stage}")

    n_wheel = 0
    if not args.no_wheels:
        n_wheel = download_wheels(stage / "wheels", args.python_version, args.platform)
        shutil.copy2(REQUIREMENTS, stage / "requirements.txt")
        (stage / "install_offline.bat").write_text(INSTALL_BAT, encoding="utf-8", newline="\r\n")
        print(f"== 离线依赖: {n_wheel} 个 wheel")

    zip_path = Path(args.out) if args.out else build_zip(stage, DIST / name)
    size_mb = zip_path.stat().st_size / 1024 / 1024
    print("\n==== 打包完成 ====")
    print(f"  文件: {zip_path}")
    print(f"  大小: {size_mb:.1f} MB")
    print(f"  源码: {n_src} 文件" + (f" | 依赖: {n_wheel} wheel" if n_wheel else " | 依赖: 未包含"))


if __name__ == "__main__":
    main()
