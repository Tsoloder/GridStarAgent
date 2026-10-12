"""transcriber 测试：WAV 解析、float 转换、shim 调用与错误映射。

全部用假 shim（monkeypatch ``shim_backend.get_shim``），不加载真模型，
所以这套测试在任何机器上都能跑。
"""

from __future__ import annotations

import struct

import pytest

from voice_asr import config
from voice_asr import transcriber as T
from voice_asr.shim import ShimError, ShimResult


def make_wav(
    samples: list[int],
    *,
    rate: int = 16000,
    channels: int = 1,
    bits: int = 16,
    audio_format: int = 1,
) -> bytes:
    """造一段最小 WAV。"""
    pcm = struct.pack(f"<{len(samples)}h", *samples)
    body = b"fmt " + struct.pack(
        "<IHHIIHH",
        16,
        audio_format,
        channels,
        rate,
        rate * channels * bits // 8,
        channels * bits // 8,
        bits,
    )
    body += b"data" + struct.pack("<I", len(pcm)) + pcm
    return b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body


def _settings(tmp_path, lang: str = "zh") -> config.Settings:
    model = tmp_path / "ggml-base.bin"
    model.write_bytes(b"y")
    return config.Settings(model=str(model), lang=lang, threads=4, max_bytes=1024)


class FakeShim:
    """记录调用参数的假后端。"""

    def __init__(self, text: str = "你好世界", timestamps: str = "") -> None:
        self.text = text
        self.timestamps = timestamps
        self.calls: list[dict] = []
        self.boom: Exception | None = None

    def transcribe(self, samples, *, language="auto", translate=False, want_ts=False):
        self.calls.append(
            {
                "n": len(samples),
                "first": samples[0] if samples else None,
                "language": language,
                "want_ts": want_ts,
            }
        )
        if self.boom is not None:
            raise self.boom
        return ShimResult(
            text=self.text,
            language=language,
            duration_ms=1000,
            timestamps=self.timestamps,
        )


@pytest.fixture()
def fake(monkeypatch) -> FakeShim:
    f = FakeShim()
    monkeypatch.setattr(T.shim_backend, "get_shim", lambda: f)
    return f


# --- WAV 解析 ----------------------------------------------------------------


def test_parse_wav_basic():
    data = make_wav([0, 1000, -1000])
    pcm, rate, channels, bits = T._parse_wav(data)
    assert len(pcm) == 6
    assert (rate, channels, bits) == (16000, 1, 16)


def test_duration_ms():
    # 16000 个采样 @16k = 1000ms
    assert T.wav_duration_ms(make_wav([0] * 16000)) == 1000
    assert T.wav_duration_ms(make_wav([0] * 8000)) == 500


def test_to_float32_scaling():
    pcm = struct.pack("<3h", 0, 32767, -32768)
    out = T._to_float32(pcm)
    assert out[0] == 0.0
    assert out[1] == pytest.approx(32767 / 32768, abs=1e-6)
    assert out[2] == -1.0
    assert all(-1.0 <= v <= 1.0 for v in out)


def test_extra_chunks_are_skipped():
    """fmt 与 data 之间夹一个 LIST 块也要能解析。"""
    pcm = struct.pack("<2h", 1, 2)
    fmt = b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16)
    extra = b"LIST" + struct.pack("<I", 4) + b"INFO"
    body = fmt + extra + b"data" + struct.pack("<I", len(pcm)) + pcm
    data = b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body
    got, rate, _, _ = T._parse_wav(data)
    assert got == pcm
    assert rate == 16000


@pytest.mark.parametrize(
    "kw,needle",
    [
        ({"channels": 2}, "单声道"),
        ({"rate": 44100}, "16000Hz"),
        ({"bits": 8}, "16bit"),
        ({"audio_format": 3}, "PCM"),
    ],
)
def test_bad_format_rejected(kw, needle):
    with pytest.raises(T.TranscribeError) as ei:
        T.transcribe(make_wav([0] * 100, **kw))
    assert ei.value.kind == "bad_audio"
    assert needle in ei.value.detail


def test_non_riff_rejected():
    with pytest.raises(T.TranscribeError) as ei:
        T.transcribe(b"not a wav at all")
    assert ei.value.kind == "bad_audio"


def test_no_data_chunk_is_empty():
    fmt = b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16)
    body = fmt + b"data" + struct.pack("<I", 0)
    data = b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body
    with pytest.raises(T.TranscribeError) as ei:
        T.transcribe(data)
    assert ei.value.kind == "empty"


# --- 转写主流程 --------------------------------------------------------------


def test_transcribe_happy_path(fake, tmp_path):
    result = T.transcribe(make_wav([0] * 16000), settings=_settings(tmp_path))
    assert result.text == "你好世界"
    assert result.language == "zh"
    assert result.duration_ms == 1000
    assert result.backend_ms >= 0
    assert isinstance(result.backend_ms, int)
    assert len(fake.calls) == 1
    assert fake.calls[0]["n"] == 16000
    assert fake.calls[0]["language"] == "zh"
    assert fake.calls[0]["want_ts"] is False


def test_language_passed_through(fake, tmp_path):
    T.transcribe(make_wav([0] * 100), settings=_settings(tmp_path, lang="en"))
    assert fake.calls[0]["language"] == "en"


def test_timestamps_requested_when_asked(fake, tmp_path):
    fake.timestamps = "0.00:1.00"
    result = T.transcribe(
        make_wav([0] * 16000), settings=_settings(tmp_path), want_timestamps=True
    )
    assert fake.calls[0]["want_ts"] is True
    assert result.timestamps == "0.00:1.00"


def test_whitespace_only_text_is_empty(fake, tmp_path):
    fake.text = "   \n  "
    with pytest.raises(T.TranscribeError) as ei:
        T.transcribe(make_wav([0] * 100), settings=_settings(tmp_path))
    assert ei.value.kind == "empty"
    assert "再说一次" in ei.value.message


def test_empty_bytes_rejected(fake, tmp_path):
    with pytest.raises(T.TranscribeError) as ei:
        T.transcribe(b"", settings=_settings(tmp_path))
    assert ei.value.kind == "empty"


def test_samples_reach_backend_unscaled(fake, tmp_path):
    T.transcribe(make_wav([0, 16384, -16384]), settings=_settings(tmp_path))
    assert fake.calls[0]["n"] == 3
    assert fake.calls[0]["first"] == 0.0


# --- shim 错误映射 -----------------------------------------------------------


@pytest.mark.parametrize(
    "shim_kind", ["model_missing", "shim_missing", "model_load_failed", "empty"]
)
def test_shim_error_maps_to_transcribe_error(fake, tmp_path, shim_kind):
    fake.boom = ShimError(shim_kind, "原始消息", "原始 detail")
    with pytest.raises(T.TranscribeError) as ei:
        T.transcribe(make_wav([0] * 100), settings=_settings(tmp_path))
    assert ei.value.kind == shim_kind
    assert ei.value.message == "原始消息"
    assert ei.value.detail == "原始 detail"


def test_backend_info_does_not_load(fake, monkeypatch):
    """backend_info 只读状态，绝不能触发模型加载。"""
    called = {"load": False}

    def explode():
        called["load"] = True
        raise AssertionError("backend_info 不该调用 get_shim()")

    monkeypatch.setattr(T.shim_backend, "get_shim", explode)
    info = T.backend_info()
    assert called["load"] is False
    assert set(info) >= {"loaded", "backend", "error"}