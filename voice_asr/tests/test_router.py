"""router 测试：TestClient + 假 settings/transcribe，覆盖各状态码与 health。

注意：voice_asr/__init__.py 导出的 router 对象会遮蔽同名子模块，
故用 importlib 取得 router 子模块本身（便于 patch 其内部的 transcribe）。
"""

from __future__ import annotations

import importlib
from pathlib import Path as P

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from voice_asr import config
from voice_asr.transcriber import TranscribeError, TranscribeResult
from voice_asr.tests.test_transcriber import make_wav

router_module = importlib.import_module("voice_asr.router")

_MAX = 15 * 1024 * 1024


def _ready_settings(tmp_path, max_bytes=_MAX) -> config.Settings:
    model = tmp_path / "ggml-base.bin"
    model.write_bytes(b"y")
    return config.Settings(
        model=str(model), lang="zh", threads=4, max_bytes=max_bytes, timeout=5
    )


def _not_ready_settings(tmp_path) -> config.Settings:
    """模型路径指向不存在的文件 -> is_ready() 为 False。"""
    return config.Settings(
        model=str(tmp_path / "nope.bin"), lang="zh", threads=4, max_bytes=1024
    )


@pytest.fixture()
def client() -> TestClient:
    app = FastAPI()
    app.include_router(router_module.router)
    return TestClient(app)


@pytest.fixture()
def ready(monkeypatch, tmp_path):
    """就绪环境：settings 就绪 + 假 transcribe + 假后端信息。"""
    monkeypatch.setattr(config, "load_settings", lambda: _ready_settings(tmp_path))

    def fake_transcribe(data, settings=None, want_timestamps=False):
        return TranscribeResult(
            text="你好世界",
            language="zh",
            duration_ms=1234,
            backend_ms=56,
        )

    monkeypatch.setattr(router_module, "transcribe", fake_transcribe)
    monkeypatch.setattr(
        router_module,
        "backend_info",
        lambda: {"loaded": True, "backend": "1.8.3", "error": ""},
    )


def _wav(content: bytes | None = None):
    return {"audio": ("a.wav", content if content is not None else make_wav([0] * 160), "audio/wav")}


# --- health ------------------------------------------------------------------


def test_health_ready(client, monkeypatch, tmp_path):
    monkeypatch.setattr(config, "load_settings", lambda: _ready_settings(tmp_path))
    monkeypatch.setattr(
        router_module,
        "backend_info",
        lambda: {"loaded": True, "backend": "1.8.3", "error": ""},
    )
    r = client.get("/asr/health")
    assert r.status_code == 200
    assert r.json() == {
        "ready": True,
        "shim": True,
        "model": True,
        "language": "zh",
        "loaded": True,
        "backend": "1.8.3",
    }


def test_health_not_ready(client, monkeypatch, tmp_path):
    monkeypatch.setattr(config, "load_settings", lambda: _not_ready_settings(tmp_path))
    monkeypatch.setattr(
        router_module,
        "backend_info",
        lambda: {"loaded": False, "backend": "", "error": "语音模型缺失"},
    )
    r = client.get("/asr/health")
    assert r.status_code == 200
    body = r.json()
    assert body["ready"] is False
    assert body["model"] is False
    assert body["backend"] == ""


def test_health_does_not_load_model(client, monkeypatch, tmp_path):
    """health 必须是廉价的：不允许触发 147MB 模型加载。"""
    monkeypatch.setattr(config, "load_settings", lambda: _ready_settings(tmp_path))
    monkeypatch.setattr(
        router_module,
        "backend_info",
        lambda: {"loaded": False, "backend": "", "error": ""},
    )
    r = client.get("/asr/health")
    assert r.json()["ready"] is True
    assert r.json()["loaded"] is False


# --- POST /asr 成功路径 --------------------------------------------------------


def test_asr_success(client, ready):
    r = client.post("/asr", files=_wav())
    assert r.status_code == 200
    body = r.json()
    assert body["text"] == "你好世界"
    assert body["language"] == "zh"
    assert body["duration_ms"] == 1234
    assert body["backend_ms"] == 56
    assert isinstance(body["duration_ms"], int)


def test_asr_accepts_declared_size_within_limit(client, ready):
    r = client.post("/asr", files=_wav(make_wav([0] * 8000)))
    assert r.status_code == 200


# --- 错误路径 -----------------------------------------------------------------


def test_asr_missing_field(client, ready):
    r = client.post("/asr", data={})
    assert r.status_code == 400
    assert "audio" in r.json()["detail"]


def test_asr_empty_audio(client, ready):
    r = client.post("/asr", files=_wav(b""))
    assert r.status_code == 400


def test_asr_too_large_by_declared_size(client, monkeypatch, tmp_path):
    monkeypatch.setattr(
        config, "load_settings", lambda: _ready_settings(tmp_path, max_bytes=4)
    )
    r = client.post("/asr", files=_wav(b"RIFFxxxxxxxx"))
    assert r.status_code == 413


def test_asr_not_ready(client, monkeypatch, tmp_path):
    monkeypatch.setattr(config, "load_settings", lambda: _not_ready_settings(tmp_path))
    r = client.post("/asr", files=_wav())
    assert r.status_code == 503


@pytest.mark.parametrize(
    "kind,status",
    [
        ("bad_audio", 400),
        ("empty", 400),
        ("model_missing", 503),
        ("shim_missing", 503),
        ("shim_load_failed", 503),
        ("model_load_failed", 503),
        ("timeout", 504),
    ],
)
def test_asr_error_kind_to_status(client, monkeypatch, tmp_path, kind, status):
    monkeypatch.setattr(config, "load_settings", lambda: _ready_settings(tmp_path))

    def boom(data, settings=None, want_timestamps=False):
        raise TranscribeError(kind, "出错了")

    monkeypatch.setattr(router_module, "transcribe", boom)
    r = client.post("/asr", files=_wav())
    assert r.status_code == status
    assert r.json()["detail"] == "出错了"


def test_asr_unknown_kind_is_500_with_detail(client, monkeypatch, tmp_path):
    monkeypatch.setattr(config, "load_settings", lambda: _ready_settings(tmp_path))

    def boom(data, settings=None, want_timestamps=False):
        raise TranscribeError("failed", "识别失败", detail="boom detail")

    monkeypatch.setattr(router_module, "transcribe", boom)
    r = client.post("/asr", files=_wav())
    assert r.status_code == 500
    assert "boom detail" in r.json()["detail"]


def test_asr_unexpected_exception_is_500_not_traceback(client, monkeypatch, tmp_path):
    """任何未预期异常都要变成 500 JSON，不能把堆栈抛给前端。"""
    monkeypatch.setattr(config, "load_settings", lambda: _ready_settings(tmp_path))

    def boom(data, settings=None, want_timestamps=False):
        raise RuntimeError("段错误风险")

    monkeypatch.setattr(router_module, "transcribe", boom)
    r = client.post("/asr", files=_wav())
    assert r.status_code == 500
    assert "语音识别内部错误" in r.json()["detail"]