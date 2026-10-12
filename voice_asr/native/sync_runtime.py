"""把 GS 包里的 whisper 运行时资产同步到 voice_asr/runtime/。

为什么要这一步：模型 147MB 与 4 个 DLL 都不适合进 git，而
``package_intranet.py`` 只归档 git 跟踪的文件，所以运行时资产需要一个
显式的同步入口。

同步内容：
    voice_asr/runtime/whisper.dll          <- 包根
    voice_asr/runtime/ggml.dll             <- 包根
    voice_asr/runtime/ggml-base.dll        <- 包根
    voice_asr/runtime/ggml-cpu.dll         <- 包根
    voice_asr/runtime/models/ggml-base.bin <- res/whisperModels/
    voice_asr/runtime/include/*.h          <- 编译 shim 用的头文件

头文件来源按优先级依次尝试，任一成功即止：
    1. 已存在的 runtime/include/ 内容（重跑时不动）
    2. 上一步已铺好的 voice_asr/native/include/
    3. 本机 whisper.cpp 源码树（D:\\whiseper_cpp）
    4. 上游 GitHub v1.8.3（需要网络）

用法：
    python voice_asr/native/sync_runtime.py
    python voice_asr/native/sync_runtime.py --pack "D:\\path\\to\\GS_Pack"
"""

from __future__ import annotations

import argparse
import shutil
import sys
import time
import urllib.request
from pathlib import Path

# 已知的 GS 包位置：新包在前（同目录可能并存多个版本）
KNOWN_PACKS = [
    Path(r"D:\TRAE_project\GridStar-history-package\GS_Pack-20261010155413"),
    Path(r"D:\TRAE_project\GridStar-history-package\GS_Pack-20260924-112531"),
]

# 运行 whisper.dll 所必需的 DLL，缺任何一个都会加载失败。
#
# 后四个是 Microsoft VC++ 运行时，不是可选的美化项：dumpbin /dependents 显示
# whisper.dll / ggml*.dll 直接导入 MSVCP140.dll、VCRUNTIME140.dll、
# VCRUNTIME140_1.dll，ggml-cpu.dll 还导入 VCOMP140.DLL（它编译时开了 OpenMP，
# 系统信息里的 "OPENMP = 1" 就是它）。开发机上这些由 Visual Studio 装进
# System32，所以本机一切正常；换一台干净机器就会因缺 DLL 而加载失败。
# VCOMP140 尤其关键：它不在 Windows 自带组件里，也不随 Python 安装包提供。
# 目标机装过「VC++ 2015-2022 可再发行组件包」的话本来就有，但为了不把这个
# 前提当成默认事实，直接从 GS 包根目录一并分发（合计约 0.9MB）。
REQUIRED_DLLS = [
    "whisper.dll",
    "ggml.dll",
    "ggml-base.dll",
    "ggml-cpu.dll",
    "msvcp140.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "vcomp140.dll",
]

MODEL_REL = Path("res") / "whisperModels" / "ggml-base.bin"

# whisper.h 会 include 这几个；版本必须与 whisper.dll 同为 v1.8.3
GGML_HEADERS = ["ggml.h", "ggml-cpu.h", "ggml-backend.h", "ggml-alloc.h", "ggml-cpp.h"]

UPSTREAM_TAG = "v1.8.3"
UPSTREAM_BASE = f"https://raw.githubusercontent.com/ggml-org/whisper.cpp/{UPSTREAM_TAG}"

# 本机可能存在的 whisper.cpp 源码树（注意目录名拼写确实如此）
LOCAL_SOURCE_TREES = [
    Path(r"D:\whiseper_cpp"),
]

NATIVE_DIR = Path(__file__).resolve().parent
VOICE_ASR_DIR = NATIVE_DIR.parent
RUNTIME_DIR = VOICE_ASR_DIR / "runtime"
INCLUDE_DIR = RUNTIME_DIR / "include"
NATIVE_INCLUDE_DIR = NATIVE_DIR / "include"


def pick_pack(explicit: str | None) -> Path:
    """确定要同步的 GS 包目录。"""
    if explicit:
        pack = Path(explicit)
        if not (pack / MODEL_REL).is_file():
            raise SystemExit(f"指定的包里找不到 {MODEL_REL}: {pack}")
        return pack
    for pack in KNOWN_PACKS:
        if (pack / MODEL_REL).is_file():
            return pack
    raise SystemExit(
        "自动探测失败：没找到含 res/whisperModels/ggml-base.bin 的 GS 包。\n"
        "请用 --pack 显式指定。"
    )


def _copy(src: Path, dst: Path, label: str) -> bool:
    if not src.is_file():
        print(f"  [跳过] {label}: 源不存在 {src}")
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)

    # 已同步且大小一致就直接跳过：既让重跑幂等，也避开 Windows 上「目标文件
    # 正被占用」的问题（刚落地的大 DLL 常被 Defender 或索引服务短暂持有，
    # copy2 覆盖时会抛 WinError 32）。
    if dst.is_file() and dst.stat().st_size == src.stat().st_size:
        print(f"  [已有] {label}: {dst.name} 已同步，跳过 ({src.stat().st_size:,} bytes)")
        return True

    last_err: Exception | None = None
    for attempt in range(5):
        try:
            shutil.copy2(src, dst)
            print(f"  [ok]   {label}: {src.name} -> {dst.relative_to(RUNTIME_DIR.parent)} "
                  f"({src.stat().st_size:,} bytes)")
            return True
        except PermissionError as exc:  # WinError 32：目标被占用，稍等再试
            last_err = exc
            time.sleep(0.6 * (attempt + 1))
    raise SystemExit(
        f"复制 {src.name} 到 {dst} 失败：{last_err}\n"
        f"目标文件被占用。关掉正在运行的 GridStarAgent/python 进程，或先手工删除 {dst} 后重跑。"
    )


def sync_pack_assets(pack: Path) -> int:
    """复制 DLL 与模型。"""
    print(f"== 同步包资产 ==\n   包: {pack}")
    count = 0
    for name in REQUIRED_DLLS:
        if _copy(pack / name, RUNTIME_DIR / name, "DLL"):
            count += 1
    if _copy(pack / MODEL_REL, RUNTIME_DIR / "models" / "ggml-base.bin", "模型"):
        count += 1
    return count


def sync_headers() -> int:
    """准备编译 shim 需要的头文件。"""
    print("== 同步头文件 ==")
    INCLUDE_DIR.mkdir(parents=True, exist_ok=True)
    count = 0

    # 1) 已有内容就保留
    if (INCLUDE_DIR / "whisper.h").is_file() and all(
        (INCLUDE_DIR / h).is_file() for h in GGML_HEADERS
    ):
        print("  [ok]   runtime/include 已齐备，跳过")
        return 1

    # 2) 从 native/include 搬
    if (NATIVE_INCLUDE_DIR / "whisper.h").is_file():
        for h in ["whisper.h"] + GGML_HEADERS:
            if _copy(NATIVE_INCLUDE_DIR / h, INCLUDE_DIR / h, "头文件"):
                count += 1
        if (INCLUDE_DIR / "whisper.h").is_file():
            return count

    # 3) 从本机 whisper.cpp 源码树搬
    #    注意：本机 D:\whiseper_cpp 的 whisper.h 与上游 v1.8.3 逐行一致
    #    （仅换行符 CRLF/LF 不同），所以这份兜底是可用的。但仍如实提示来源，
    #    以免日后源码树被拉到别的版本而无人察觉。
    for tree in LOCAL_SOURCE_TREES:
        src_whisper = tree / "include" / "whisper.h"
        src_ggml_dir = tree / "ggml" / "include"
        if src_whisper.is_file() and (src_ggml_dir / "ggml.h").is_file():
            print(f"  [info] 命中本机源码树 {tree}")
            _copy(src_whisper, INCLUDE_DIR / "whisper.h", "whisper.h")
            for h in GGML_HEADERS:
                _copy(src_ggml_dir / h, INCLUDE_DIR / h, "头文件")
            print("  [warn] 头文件来自本机源码树（当前与 v1.8.3 逐行一致）；"
                  "若日后结构体报错请优先换上游 v1.8.3 头文件")
            return count + 1

    # 4) 从上游下载
    print(f"  [info] 尝试从上游下载 {UPSTREAM_TAG} 头文件")
    try:
        _download(UPSTREAM_BASE + "/include/whisper.h", INCLUDE_DIR / "whisper.h")
        for h in GGML_HEADERS:
            _download(UPSTREAM_BASE + f"/ggml/include/{h}", INCLUDE_DIR / h)
    except Exception as exc:  # noqa: BLE001 - 内网无网络是预期场景
        raise SystemExit(
            f"头文件准备失败：{exc}\n"
            "请在有网络的机器上跑一次本脚本，或手工把 whisper.h 与 ggml*.h 放进 "
            f"{INCLUDE_DIR}"
        ) from exc
    print("  [ok]   上游头文件已就位")
    return 1


def _download(url: str, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(url, timeout=60) as resp:  # noqa: S310 - 固定 https 域名
        data = resp.read()
    dst.write_bytes(data)
    print(f"  [ok]   {dst.name} ({len(data):,} bytes)")


def verify() -> bool:
    """同步结果自检。"""
    print("== 自检 ==")
    ok = True
    for name in REQUIRED_DLLS:
        p = RUNTIME_DIR / name
        exists = p.is_file()
        ok &= exists
        print(f"  {'[ok]  ' if exists else '[缺失]'} {name}"
              + (f" ({p.stat().st_size:,} bytes)" if exists else ""))
    model = RUNTIME_DIR / "models" / "ggml-base.bin"
    exists = model.is_file()
    ok &= exists
    print(f"  {'[ok]  ' if exists else '[缺失]'} models/ggml-base.bin"
          + (f" ({model.stat().st_size:,} bytes)" if exists else ""))
    shim = NATIVE_DIR / "bin" / "asr_shim.dll"
    has_shim = shim.is_file()
    ok &= has_shim
    print(f"  {'[ok]  ' if has_shim else '[缺失]'} native/bin/asr_shim.dll"
          + (f" ({shim.stat().st_size:,} bytes)" if has_shim else "  <- 跑 build_shim.bat"))
    inc = (INCLUDE_DIR / "whisper.h").is_file()
    ok &= inc
    print(f"  {'[ok]  ' if inc else '[缺失]'} include/whisper.h（仅编译 shim 需要）")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description="同步 whisper 运行时资产到 voice_asr/runtime/")
    ap.add_argument("--pack", default=None, help="GS 包根目录；缺省则自动探测")
    ap.add_argument("--skip-headers", action="store_true", help="只同步 DLL 与模型")
    args = ap.parse_args()

    pack = pick_pack(args.pack)
    sync_pack_assets(pack)
    if not args.skip_headers:
        sync_headers()
    ok = verify()
    print("\n" + ("== 全部就绪 ==" if ok else "== 尚有缺失，见上方 [缺失] 项 =="))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())