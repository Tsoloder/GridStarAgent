"""config 层测试：默认值、环境变量覆盖、就绪判定、非法整型回退。"""

from __future__ import annotations

from voice_asr import config


def test_defaults_when_no_env(monkeypatch):
    for key in ("WHISPER_CLI", "WHISPER_MODEL", "WHISPER_LANG", "WHISPER_TIMEOUT", "ASR_MAX_BYTES"):
        monkeypatch.delenv(key, raising=False)
    s = config.load_settings()
    assert s.cli == config.DEFAULT_CLI
    assert s.model == config.DEFAULT_MODEL
    assert s.lang == "zh"
    assert s.timeout == 120
    assert s.max_bytes == 15 * 1024 * 1024


def test_env_override(monkeypatch):
    monkeypatch.setenv("WHISPER_CLI", r"C:\x\whisper-cli.exe")
    monkeypatch.setenv("WHISPER_MODEL", r"C:\x\ggml-base.bin")
    monkeypatch.setenv("WHISPER_LANG", "en")
    monkeypatch.setenv("WHISPER_TIMEOUT", "30")
    monkeypatch.setenv("ASR_MAX_BYTES", "1024")
    s = config.load_settings()
    assert s.cli == r"C:\x\whisper-cli.exe"
    assert s.model == r"C:\x\ggml-base.bin"
    assert s.lang == "en"
    assert s.timeout == 30
    assert s.max_bytes == 1024


def test_invalid_int_falls_back_to_default(monkeypatch):
    monkeypatch.setenv("WHISPER_TIMEOUT", "abc")
    monkeypatch.setenv("ASR_MAX_BYTES", "-5")
    s = config.load_settings()
    assert s.timeout == config.DEFAULT_TIMEOUT
    assert s.max_bytes == config.DEFAULT_MAX_BYTES


def test_empty_int_env_falls_back_to_default(monkeypatch):
    monkeypatch.setenv("WHISPER_TIMEOUT", "   ")
    s = config.load_settings()
    assert s.timeout == config.DEFAULT_TIMEOUT


def test_existence_and_ready(tmp_path):
    cli = tmp_path / "whisper-cli.exe"
    model = tmp_path / "ggml-base.bin"
    cli.write_bytes(b"x")
    model.write_bytes(b"y")
    s = config.Settings(
        cli=str(cli), model=str(model), lang="zh", timeout=120, max_bytes=1024
    )
    assert s.cli_exists() is True
    assert s.model_exists() is True
    assert s.is_ready() is True


def test_not_ready_when_missing(tmp_path):
    s = config.Settings(
        cli=str(tmp_path / "nope.exe"),
        model=str(tmp_path / "nope.bin"),
        lang="zh",
        timeout=120,
        max_bytes=1024,
    )
    assert s.cli_exists() is False
    assert s.model_exists() is False
    assert s.is_ready() is False
