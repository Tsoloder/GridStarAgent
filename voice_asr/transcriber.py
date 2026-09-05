"""转写核心：调用本机 whisper-cli 将 WAV 字节转写为文本。

阻塞式函数，由路由层用线程池 offload，避免阻塞事件循环。
子进程使用固定参数列表（非 shell=True），无命令注入面；临时文件 finally 清理。
"""

from __future__ import annotations

import os
import subprocess
import tempfile
from typing import Optional

from . import config

# stderr 摘要保留的最大字符数
_STDERR_TAIL = 500


class TranscribeError(Exception):
    """转写失败。``kind`` 供路由层映射 HTTP 状态码。

    kind 取值：``empty`` / ``cli_missing`` / ``timeout`` / ``nonzero_exit``。
    """

    def __init__(self, kind: str, message: str = "", detail: str = ""):
        super().__init__(message or kind)
        self.kind = kind
        self.detail = detail


def _parse_stdout(raw: bytes) -> str:
    """解析 whisper-cli 输出：逐行 strip、去空行后拼接（中文不留空格）。"""
    text = raw.decode("utf-8", "ignore")
    lines = [ln.strip() for ln in text.splitlines()]
    return "".join(ln for ln in lines if ln)


def transcribe(wav_bytes: bytes, *, settings: Optional[config.Settings] = None) -> str:
    """将 16kHz/16bit/单声道 WAV 字节转写为文本。

    失败时抛出 :class:`TranscribeError`。
    """
    if settings is None:
        settings = config.load_settings()
    if not wav_bytes:
        raise TranscribeError("empty", "音频为空")

    tmp_path: Optional[str] = None
    try:
        fd, tmp_path = tempfile.mkstemp(suffix=".wav")
        with os.fdopen(fd, "wb") as fh:
            fh.write(wav_bytes)

        cmd = [
            settings.cli,
            "-m", settings.model,
            "-f", tmp_path,
            "-l", settings.lang,
            "-nt",
        ]
        try:
            result = subprocess.run(cmd, capture_output=True, timeout=settings.timeout)
        except FileNotFoundError as exc:
            raise TranscribeError(
                "cli_missing", f"whisper-cli 不存在: {settings.cli}"
            ) from exc
        except subprocess.TimeoutExpired as exc:
            raise TranscribeError(
                "timeout", f"转写超时（>{settings.timeout}s）"
            ) from exc

        if result.returncode != 0:
            stderr = result.stderr.decode("utf-8", "ignore").strip()
            raise TranscribeError(
                "nonzero_exit",
                f"whisper-cli 退出码 {result.returncode}",
                detail=stderr[-_STDERR_TAIL:],
            )
        return _parse_stdout(result.stdout)
    finally:
        if tmp_path:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass
