"""config 测试：默认值、环境变量覆盖、就绪判定。"""

from __future__ import annotations

import importlib

import pytest

from voice_asr import config


@pytest.fixture()
def clean_env(monkeypatch):
    """清掉所有相关环境变量，保证测的是默认路径。"""
    for name in (
        "WHISPER_MODEL",
        "WHISPER_DLL",
        "WHISPER_LANG",
        "WHISPER_THREADS",
        "WHISPER_TIMEOUT",
        "WHISPER_RUNTIME",
        "ASR_MAX_BYTES",
    ):
        monkeypatch.delenv(name, raising=False)
    return monkeypatch


def test_defaults(clean_env):
    s = config.load_settings()
    assert s.lang == "zh"
    assert s.timeout == 120
    assert s.max_bytes == 15 * 1024 * 1024
    assert 1 <= s.threads <= 8


def test_default_paths_live_inside_voice_asr(clean_env):
    """运行时资产默认落在 voice_asr/ 内，模块自包含，不依赖外部目录。"""
    module_dir = config.module_dir()
    assert config.model_path().is_relative_to(module_dir)
    assert config.whisper_dll_path().is_relative_to(module_dir)
    assert config.shim_dll_path().is_relative_to(module_dir)
    assert config.runtime_dir() == module_dir / "runtime"


def test_env_overrides(clean_env):
    clean_env.setenv("WHISPER_MODEL", r"D:\somewhere\ggml-small.bin")
    clean_env.setenv("WHISPER_DLL", r"D:\pack\whisper.dll")
    clean_env.setenv("WHISPER_LANG", "en")
    clean_env.setenv("WHISPER_THREADS", "3")
    clean_env.setenv("WHISPER_TIMEOUT", "45")
    clean_env.setenv("ASR_MAX_BYTES", "2048")
    s = config.load_settings()
    assert s.model == r"D:\somewhere\ggml-small.bin"
    assert config.whisper_dll_path().as_posix() == "D:/pack/whisper.dll"
    assert s.lang == "en"
    assert s.threads == 3
    assert s.timeout == 45
    assert s.max_bytes == 2048


def test_runtime_dir_override(clean_env):
    clean_env.setenv("WHISPER_RUNTIME", r"D:\runtime-elsewhere")
    assert config.runtime_dir().as_posix() == "D:/runtime-elsewhere"


@pytest.mark.parametrize("bad", ["abc", "", "  ", "3.5"])
def test_bad_int_env_falls_back(clean_env, bad):
    clean_env.setenv("WHISPER_THREADS", bad)
    assert config.load_settings().threads == config._default_threads()


def test_blank_lang_falls_back(clean_env):
    clean_env.setenv("WHISPER_LANG", "   ")
    assert config.load_settings().lang == "zh"


def test_settings_existence_probes(clean_env, tmp_path):
    model = tmp_path / "ggml-base.bin"
    model.write_bytes(b"y")
    s = config.Settings(model=str(model), lang="zh", threads=4, max_bytes=1024)
    assert s.model_exists() is True
    assert config.Settings(
        model=str(tmp_path / "nope.bin"), lang="zh", threads=4, max_bytes=1024
    ).model_exists() is False


def test_is_ready_requires_all_three(clean_env, tmp_path, monkeypatch):
    """就绪 = shim 组件 + 包内 whisper.dll + 模型，缺一不可。"""
    model = tmp_path / "ggml-base.bin"
    model.write_bytes(b"y")
    s = config.Settings(model=str(model), lang="zh", threads=4, max_bytes=1024)

    # native/bin/asr_shim.dll 在真实仓库里已构建，但测试不该依赖它
    monkeypatch.setattr(config, "shim_dll_path", lambda: tmp_path / "asr_shim.dll")
    monkeypatch.setattr(config, "whisper_dll_path", lambda: tmp_path / "whisper.dll")
    assert s.is_ready() is False  # 两个 DLL 都不存在

    (tmp_path / "whisper.dll").write_bytes(b"d")
    assert s.is_ready() is False  # 还差 shim
    (tmp_path / "asr_shim.dll").write_bytes(b"d")
    assert s.is_ready() is True
    assert s.shim_exists() is True


def test_module_level_settings_is_a_snapshot(clean_env):
    """模块级 settings 是导入时的快照；load_settings 才是实时的。"""
    assert isinstance(config.settings, config.Settings)
    clean_env.setenv("WHISPER_LANG", "ja")
    assert config.settings.lang == "zh"
    assert config.load_settings().lang == "ja"


def test_reload_picks_up_env(clean_env):
    clean_env.setenv("WHISPER_LANG", "fr")
    reloaded = importlib.reload(config)
    try:
        assert reloaded.settings.lang == "fr"
    finally:
        clean_env.delenv("WHISPER_LANG", raising=False)
        importlib.reload(config)