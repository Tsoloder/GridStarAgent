"""voice_asr 配置：路径解析与环境变量覆盖。

运行时资产（whisper.dll / ggml*.dll / 模型）不进 git，它们放在
``voice_asr/runtime/``，由 ``native/sync_runtime.py`` 从 GS 包同步过来。
这样整个语音模块是自包含的：拷走 voice_asr/ 就能跑，不依赖 D:\\whiseper_cpp
这类开发机上的外部目录。

环境变量：
    WHISPER_MODEL     模型权重路径（默认 runtime/models/ggml-base.bin）
    WHISPER_DLL       包内 whisper.dll 路径（默认 runtime/whisper.dll）
    WHISPER_LANG      识别语言，填 auto 走自动检测，默认 zh
    WHISPER_THREADS   解码线程数，默认 min(8, 物理核数)
    ASR_MAX_BYTES     单次上传上限，默认 15MB
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

# --- 默认值 -------------------------------------------------------------------

DEFAULT_LANG = "zh"
DEFAULT_TIMEOUT = 120
DEFAULT_MAX_BYTES = 15 * 1024 * 1024

# 浏览器端录音统一编码成 16kHz 单声道 16bit WAV，后端按这个采样率转 float32
SAMPLE_RATE = 16000

_MODEL_REL = Path("runtime") / "models" / "ggml-base.bin"
_DLL_REL = Path("runtime") / "whisper.dll"
_SHIM_REL = Path("native") / "bin" / "asr_shim.dll"

# voice_asr/config.py -> voice_asr/ -> 项目根
_MODULE_DIR = Path(__file__).resolve().parent


def module_dir() -> Path:
    return _MODULE_DIR


def runtime_dir() -> Path:
    """运行时资产目录。默认取模块内的 runtime/，可用 WHISPER_RUNTIME 覆盖。"""
    override = os.environ.get("WHISPER_RUNTIME")
    if override:
        return Path(override)
    return _MODULE_DIR / "runtime"


def shim_dll_path() -> Path:
    return _MODULE_DIR / _SHIM_REL


def model_path() -> Path:
    override = os.environ.get("WHISPER_MODEL")
    if override:
        return Path(override)
    return _MODULE_DIR / _MODEL_REL


def whisper_dll_path() -> Path:
    override = os.environ.get("WHISPER_DLL")
    if override:
        return Path(override)
    return _MODULE_DIR / _DLL_REL


def _env_int(name: str, default: int) -> int:
    """读整数环境变量；缺失或非法一律回退默认值。"""
    raw = os.environ.get(name)
    if raw is None or not raw.strip():
        return default
    try:
        return int(raw.strip())
    except ValueError:
        return default


def _default_threads() -> int:
    """默认线程数：不超过 8，也不超过物理核数。"""
    return max(1, min(8, os.cpu_count() or 4))


@dataclass(frozen=True)
class Settings:
    model: str
    lang: str
    threads: int
    max_bytes: int
    timeout: int = DEFAULT_TIMEOUT

    def model_exists(self) -> bool:
        return Path(self.model).is_file()

    def shim_exists(self) -> bool:
        return shim_dll_path().is_file()

    def is_ready(self) -> bool:
        """三项齐备才算就绪：shim 组件、包内 whisper.dll、模型权重。"""
        return self.shim_exists() and whisper_dll_path().is_file() and self.model_exists()


def load_settings() -> Settings:
    """读取当前环境变量生成配置。每次调用都重新读，便于测试与热改。"""
    return Settings(
        model=str(model_path()),
        lang=os.environ.get("WHISPER_LANG", DEFAULT_LANG).strip() or DEFAULT_LANG,
        threads=_env_int("WHISPER_THREADS", _default_threads()),
        max_bytes=_env_int("ASR_MAX_BYTES", DEFAULT_MAX_BYTES),
        timeout=_env_int("WHISPER_TIMEOUT", DEFAULT_TIMEOUT),
    )


settings = load_settings()