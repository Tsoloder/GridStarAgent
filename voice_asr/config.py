"""voice_asr 配置层。

所有配置项均支持环境变量覆盖，避免写死路径。
注意：本机 whisper.cpp 目录名实际为 ``whiseper_cpp``（含拼写），勿擅改默认值。
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

# 默认值（可被同名环境变量覆盖）
DEFAULT_CLI = r"D:\whiseper_cpp\build_cpu\bin\Release\whisper-cli.exe"
DEFAULT_MODEL = r"D:\whiseper_cpp\models\ggml-base.bin"
DEFAULT_LANG = "zh"
DEFAULT_TIMEOUT = 120
DEFAULT_MAX_BYTES = 15 * 1024 * 1024  # 15728640，约 15MB


def _env_int(name: str, default: int) -> int:
    """读取整型环境变量，非法值回退默认。"""
    raw = os.environ.get(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        value = int(raw)
    except ValueError:
        return default
    return value if value > 0 else default


@dataclass(frozen=True)
class Settings:
    """ASR 运行配置（不可变）。"""

    cli: str
    model: str
    lang: str
    timeout: int
    max_bytes: int

    def cli_exists(self) -> bool:
        return Path(self.cli).is_file()

    def model_exists(self) -> bool:
        return Path(self.model).is_file()

    def is_ready(self) -> bool:
        """CLI 与模型都存在时才视为就绪。"""
        return self.cli_exists() and self.model_exists()


def load_settings() -> Settings:
    """从环境变量加载配置（每次调用都重新读取，便于测试注入）。"""
    return Settings(
        cli=os.environ.get("WHISPER_CLI", DEFAULT_CLI),
        model=os.environ.get("WHISPER_MODEL", DEFAULT_MODEL),
        lang=os.environ.get("WHISPER_LANG", DEFAULT_LANG),
        timeout=_env_int("WHISPER_TIMEOUT", DEFAULT_TIMEOUT),
        max_bytes=_env_int("ASR_MAX_BYTES", DEFAULT_MAX_BYTES),
    )


# 模块级默认配置实例；测试可 monkeypatch 或直接调用 load_settings()。
settings = load_settings()
