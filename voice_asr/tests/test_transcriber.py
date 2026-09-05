"""transcriber 测试：mock subprocess，不依赖真实模型/CLI。"""

from __future__ import annotations

import os
import subprocess

import pytest

from voice_asr import config, transcriber


def _settings(tmp_path) -> config.Settings:
    cli = tmp_path / "whisper-cli.exe"
    cli.write_bytes(b"x")
    model = tmp_path / "ggml-base.bin"
    model.write_bytes(b"y")
    return config.Settings(
        cli=str(cli), model=str(model), lang="zh", timeout=5, max_bytes=1024
    )


def test_success_strips_and_joins(monkeypatch, tmp_path):
    s = _settings(tmp_path)
    captured = {}

    def fake_run(cmd, **kwargs):
        captured["cmd"] = cmd
        return subprocess.CompletedProcess(
            cmd, 0, stdout="  你好世界 \n 再见 \n".encode("utf-8"), stderr=b""
        )

    monkeypatch.setattr(transcriber.subprocess, "run", fake_run)
    out = transcriber.transcribe(b"RIFFxxxx", settings=s)
    assert out == "你好世界再见"
    # 校验固定参数列表
    assert captured["cmd"][0] == s.cli
    assert captured["cmd"][1] == "-m"
    assert captured["cmd"][2] == s.model
    assert "-nt" in captured["cmd"]
    assert captured["cmd"][captured["cmd"].index("-l") + 1] == "zh"


def test_temp_file_cleaned_on_success(monkeypatch, tmp_path):
    s = _settings(tmp_path)
    seen = {}

    def fake_run(cmd, **kwargs):
        seen["path"] = cmd[cmd.index("-f") + 1]
        assert os.path.exists(seen["path"])  # 调用时临时文件存在
        return subprocess.CompletedProcess(cmd, 0, stdout=b"ok", stderr=b"")

    monkeypatch.setattr(transcriber.subprocess, "run", fake_run)
    transcriber.transcribe(b"RIFFxxxx", settings=s)
    assert not os.path.exists(seen["path"])  # 结束后清理


def test_temp_file_cleaned_on_error(monkeypatch, tmp_path):
    s = _settings(tmp_path)
    seen = {}

    def fake_run(cmd, **kwargs):
        seen["path"] = cmd[cmd.index("-f") + 1]
        return subprocess.CompletedProcess(cmd, 1, stdout=b"", stderr=b"boom")

    monkeypatch.setattr(transcriber.subprocess, "run", fake_run)
    with pytest.raises(transcriber.TranscribeError):
        transcriber.transcribe(b"RIFFxxxx", settings=s)
    assert not os.path.exists(seen["path"])


def test_timeout_mapping(monkeypatch, tmp_path):
    s = _settings(tmp_path)

    def fake_run(cmd, **kwargs):
        raise subprocess.TimeoutExpired(cmd, kwargs.get("timeout", 1))

    monkeypatch.setattr(transcriber.subprocess, "run", fake_run)
    with pytest.raises(transcriber.TranscribeError) as ei:
        transcriber.transcribe(b"RIFFxxxx", settings=s)
    assert ei.value.kind == "timeout"


def test_nonzero_exit_mapping(monkeypatch, tmp_path):
    s = _settings(tmp_path)

    def fake_run(cmd, **kwargs):
        return subprocess.CompletedProcess(cmd, 3, stdout=b"", stderr=b"boom error detail")

    monkeypatch.setattr(transcriber.subprocess, "run", fake_run)
    with pytest.raises(transcriber.TranscribeError) as ei:
        transcriber.transcribe(b"RIFFxxxx", settings=s)
    assert ei.value.kind == "nonzero_exit"
    assert "boom" in ei.value.detail


def test_cli_missing_mapping(monkeypatch, tmp_path):
    s = _settings(tmp_path)

    def fake_run(cmd, **kwargs):
        raise FileNotFoundError(cmd[0])

    monkeypatch.setattr(transcriber.subprocess, "run", fake_run)
    with pytest.raises(transcriber.TranscribeError) as ei:
        transcriber.transcribe(b"RIFFxxxx", settings=s)
    assert ei.value.kind == "cli_missing"


def test_empty_input(tmp_path):
    s = _settings(tmp_path)
    with pytest.raises(transcriber.TranscribeError) as ei:
        transcriber.transcribe(b"", settings=s)
    assert ei.value.kind == "empty"
