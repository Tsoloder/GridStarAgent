"""router 测试：TestClient + mock transcribe/load_settings，覆盖各状态码与 health。"""

from __future__ import annotations

import importlib

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from voice_asr import config
from voice_asr.transcriber import TranscribeError

# 注意：voice_asr/__init__.py 导出的 router 对象会遮蔽同名子模块，
# 故用 importlib 取得 router 子模块本身（便于 patch 其内部的 transcribe）。
router_module = importlib.import_module("voice_asr.router")

_MAX = 15 * 1024 * 1024


def _ready_settings(tmp_path, max_bytes=_MAX) -> config.Settings:
    cli = tmp_path / "whisper-cli.exe"
    cli.write_bytes(b"x")
    model = tmp_path / "ggml-base.bin"
    model.write_bytes(b"y")
    return config.Settings(
        cli=str(cli), model=str(model), lang="zh", timeout=5, max_bytes=max_bytes
    )


def _not_ready_settings(tmp_path) -> config.Settings:
    return config.Settings(
        cli=str(tmp_path / "nope.exe"),
        model=str(tmp_path / "nope.bin"),
        lang="zh",
        timeout=5,
        max_bytes=1024,
    )


@pytest.fixture()
def client() -> TestClient:
    app = FastAPI()
    app.include_router(router_module.router)
    return TestClient(app)


def _wav(content: bytes = b"RIFFxxxx"):
    return {"audio": ("a.wav", content, "audio/wav")}


def test_health_ready(client, monkeypatch, tmp_path):
    monkeypatch.setattr(config, "load_settings", lambda: _ready_settings(tmp_path))
    r = client.get("/asr/health")
    assert r.status_code == 200
    assert r.json() == {"ready": True, "cli": True, "model": True, "language": "zh"}


def test_health_not_ready(client, monkeypatch, tmp_path):
    monkeypatch.setattr(config, "load_settings", lambda: _not_ready_settings(tmp_path))
    r = client.get("/asr/health")
    assert r.status_code == 200
    body = r.json()
    assert body["ready"] is False
    assert body["cli"] is False
    assert body["model"] is False


def test_asr_success(client, monkeypatch, tmp_path):
    monkeypatch.setattr(config, "load_settings", lambda: _ready_settings(tmp_path))
    monkeypatch.setattr(router_module, "transcribe", lambda data, settings=None: "你好世界")
    r = client.post("/asr", files=_wav())
    assert r.status_code == 200
    body = r.json()
    assert body["text"] == "你好世界"
    assert body["language"] == "zh"
    assert isinstance(body["duration_ms"], int)


def test_asr_missing_field(client, monkeypatch, tmp_path):
    monkeypatch.setattr(config, "load_settings", lambda: _ready_settings(tmp_path))
    r = client.post("/asr", data={})
    assert r.status_code == 400


def test_asr_empty_audio(client, monkeypatch, tmp_path):
    monkeypatch.setattr(config, "load_settings", lambda: _ready_settings(tmp_path))
    r = client.post("/asr", files=_wav(b""))
    assert r.status_code == 400


def test_asr_too_large(client, monkeypatch, tmp_path):
    monkeypatch.setattr(config, "load_settings", lambda: _ready_settings(tmp_path, max_bytes=4))
    r = client.post("/asr", files=_wav(b"RIFFxxxxxxxx"))
    assert r.status_code == 413


def test_asr_not_ready(client, monkeypatch, tmp_path):
    monkeypatch.setattr(config, "load_settings", lambda: _not_ready_settings(tmp_path))
    r = client.post("/asr", files=_wav())
    assert r.status_code == 503


def test_asr_timeout(client, monkeypatch, tmp_path):
    monkeypatch.setattr(config, "load_settings", lambda: _ready_settings(tmp_path))

    def boom(data, settings=None):
        raise TranscribeError("timeout", "转写超时")

    monkeypatch.setattr(router_module, "transcribe", boom)
    r = client.post("/asr", files=_wav())
    assert r.status_code == 504


def test_asr_nonzero_exit(client, monkeypatch, tmp_path):
    monkeypatch.setattr(config, "load_settings", lambda: _ready_settings(tmp_path))

    def boom(data, settings=None):
        raise TranscribeError("nonzero_exit", "退出码 1", detail="boom detail")

    monkeypatch.setattr(router_module, "transcribe", boom)
    r = client.post("/asr", files=_wav())
    assert r.status_code == 500
    assert "boom detail" in r.json()["detail"]
