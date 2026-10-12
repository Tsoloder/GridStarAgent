"""转写入口：WAV 字节 -> 文本。

与旧实现的区别：以前是每次转写都 ``subprocess`` 起一个 whisper-cli.exe
（每次冷启动都要重读 147MB 模型，且依赖一个外部 exe）；现在走进程内的
``asr_shim.dll``，模型常驻内存，只加载一次。

音频格式的约束沿用前端约定：浏览器端用 Web Audio 编好 16kHz / 单声道 /
16bit PCM 的 WAV 再上传，所以后端不需要引入 ffmpeg/pydub/soundfile 之类的
解码依赖，自己做协议解析即可。
"""

from __future__ import annotations

import array
import struct
from dataclasses import dataclass
from time import perf_counter

from . import config, shim as shim_backend

# WAV 解析只认这一种格式，前端保证了它就是这种
_EXPECTED_BITS = 16


class TranscribeError(Exception):
    """转写失败。``kind`` 用于映射 HTTP 状态码。"""

    def __init__(self, kind: str, message: str = "", detail: str = "") -> None:
        super().__init__(message or kind)
        self.kind = kind
        self.message = message or kind
        self.detail = detail


@dataclass(frozen=True)
class TranscribeResult:
    text: str
    language: str
    duration_ms: int  # 音频时长
    backend_ms: int  # 后端转写耗时
    timestamps: str = ""  # 形如 "0.00:2.34,2.34:5.01"，未请求时为空


def _parse_wav(data: bytes) -> tuple[bytes, int, int, int]:
    """极简 WAV 解析：返回 (PCM 字节, 采样率, 声道数, 位深)。

    只处理 RIFF/WAVE + fmt + data 的标准布局，够用且无依赖。
    """
    if len(data) < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise TranscribeError("bad_audio", "音频格式不正确", "不是 RIFF/WAVE 文件")

    fmt: tuple[int, int, int] | None = None
    pcm: bytes | None = None
    pos = 12
    total = len(data)

    while pos + 8 <= total:
        chunk_id = data[pos : pos + 4]
        (size,) = struct.unpack_from("<I", data, pos + 4)
        body = pos + 8
        if body + size > total:
            # 浏览器可能没回填长度，按剩余全部处理
            size = total - body

        if chunk_id == b"fmt " and size >= 16:
            audio_format, channels, rate, _, _, bits = struct.unpack_from(
                "<HHIIHH", data, body
            )
            if audio_format != 1:
                raise TranscribeError(
                    "bad_audio",
                    "音频格式不正确",
                    f"只支持 PCM，收到 audio_format={audio_format}",
                )
            fmt = (rate, channels, bits)
        elif chunk_id == b"data":
            pcm = data[body : body + size]

        pos = body + size + (size & 1)  # chunk 按偶数字节对齐
        if fmt is not None and pcm is not None:
            break

    if fmt is None:
        raise TranscribeError("bad_audio", "音频格式不正确", "缺少 fmt 块")
    if pcm is None or not pcm:
        raise TranscribeError("empty", "未录到声音")

    rate, channels, bits = fmt
    if bits != _EXPECTED_BITS:
        raise TranscribeError(
            "bad_audio", "音频格式不正确", f"需要 {_EXPECTED_BITS}bit，收到 {bits}bit"
        )
    if rate != config.SAMPLE_RATE:
        raise TranscribeError(
            "bad_audio",
            "音频格式不正确",
            f"需要 {config.SAMPLE_RATE}Hz，收到 {rate}Hz",
        )
    if channels != 1:
        raise TranscribeError(
            "bad_audio", "音频格式不正确", f"需要单声道，收到 {channels} 声道"
        )
    return pcm, rate, channels, bits


def _to_float32(pcm: bytes) -> list[float]:
    """16bit 小端 PCM 转 [-1, 1] 的 float32 采样。"""
    count = len(pcm) // 2
    # array('h') 比 struct.unpack 逐个快得多，且原生小端
    samples = array.array("h")
    samples.frombytes(pcm[: count * 2])
    return [s / 32768.0 for s in samples]


def wav_duration_ms(data: bytes) -> int:
    """音频时长（毫秒）。解析失败返回 0。"""
    try:
        pcm, rate, _, _ = _parse_wav(data)
    except TranscribeError:
        return 0
    return int(len(pcm) / 2 * 1000 / rate)


def transcribe(
    wav_bytes: bytes,
    *,
    settings: config.Settings | None = None,
    want_timestamps: bool = False,
) -> TranscribeResult:
    """把一段 WAV 字节转写成文本。阻塞调用，由路由层丢进线程池。"""
    if not wav_bytes:
        raise TranscribeError("empty", "未录到声音")

    settings = settings or config.load_settings()
    pcm, _, _, _ = _parse_wav(wav_bytes)
    samples = _to_float32(pcm)
    if not samples:
        raise TranscribeError("empty", "未录到声音")

    try:
        backend = shim_backend.get_shim()
        started = perf_counter()
        result = backend.transcribe(
            samples,
            language=settings.lang,
            want_ts=want_timestamps,
        )
        elapsed = int((perf_counter() - started) * 1000)
    except shim_backend.ShimError as exc:
        raise TranscribeError(exc.kind, exc.message, exc.detail) from exc

    text = result.text.strip()
    if not text:
        # 模型确实没识别出内容，让前端提示用户重录，而不是回一个空字符串
        raise TranscribeError("empty", "没听清，请再说一次")

    return TranscribeResult(
        text=text,
        language=result.language,
        duration_ms=wav_duration_ms(wav_bytes) or result.duration_ms,
        backend_ms=elapsed,
        timestamps=result.timestamps,
    )


def backend_info() -> dict:
    """后端的惰性状态：只读取已经发生过的加载结果，绝不触发 147MB 模型加载。

    ``/asr/health`` 靠它报告版本，所以这里不能调 ``get_shim().ensure_loaded()``。
    """
    return shim_backend.loaded_info()


def reset() -> None:
    """释放常驻的模型上下文（测试或需要换模型时用）。"""
    shim_backend.reset()