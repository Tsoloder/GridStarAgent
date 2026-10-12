"""shim 测试：用假 ctypes DLL 覆盖错误路径与返回值语义。

这里刻意的**不加载真 DLL**，因为需要验证的是「返回值怎么解释」，
而真 DLL 一旦跑起来就必然吃进 147MB 模型。返回值语义以 native/asr_shim.h
为唯一依据：asr_init 返回 1 成功 / 0 失败，asr_transcribe 返回文本字节数 / -1 失败。
"""

from __future__ import annotations

import ctypes

import pytest

from voice_asr import config, shim as S


class FakeFn:
    """记录调用参数的假导出函数。"""

    def __init__(self, ret=None) -> None:
        self.ret = ret
        self.argtypes = None
        self.restype = None
        self.calls: list[tuple] = []

    def __call__(self, *args):
        self.calls.append(args)
        return self.ret


class FakeDll:
    def __init__(self, init_ret=1, transcribe_ret=5, text=b"hi", version=b"1.8.3"):
        self.asr_init = FakeFn(init_ret)
        self.asr_transcribe = FakeFn(transcribe_ret)
        self.asr_last_text = FakeFn(text)
        self.asr_backend_version = FakeFn(version)
        self.asr_effective_threads = FakeFn(8)
        self.asr_free = FakeFn(None)
        self.asr_set_initial_prompt = FakeFn(1)
        self.asr_set_prompt_for = FakeFn(1)


@pytest.fixture()
def fake_dll(monkeypatch):
    """替换掉 ctypes.CDLL 与 add_dll_directory。"""
    dll = FakeDll()

    class Holder:
        pass

    def fake_cdll(path):
        Holder.path = path
        return dll

    monkeypatch.setattr(S.ctypes, "CDLL", fake_cdll)

    class FakeDir:
        path = ""

    monkeypatch.setattr(S.os, "add_dll_directory", lambda p: FakeDir(), raising=False)
    return dll


@pytest.fixture()
def assets(tmp_path):
    """造出 dll 与 model 两个假文件。"""
    dll = tmp_path / "asr_shim.dll"
    dll.write_bytes(b"d")
    model = tmp_path / "ggml-base.bin"
    model.write_bytes(b"m")
    return dll, tmp_path, model


@pytest.fixture(autouse=True)
def clean_singleton():
    S.reset()
    yield
    S.reset()


def _make(assets, fake_dll) -> S._Shim:
    dll, runtime, model = assets
    return S._Shim(dll, runtime, model)


# --- 构造期 ------------------------------------------------------------------


def test_missing_shim_dll(tmp_path):
    with pytest.raises(S.ShimError) as ei:
        S._Shim(tmp_path / "nope.dll", tmp_path, tmp_path / "m.bin")
    assert ei.value.kind == "shim_missing"
    assert "build_shim" in ei.value.detail


def test_missing_model(assets):
    dll, runtime, _ = assets
    with pytest.raises(S.ShimError) as ei:
        S._Shim(dll, runtime, runtime / "nope.bin")
    assert ei.value.kind == "model_missing"


def test_cdll_failure_is_reported(assets, monkeypatch):
    dll, runtime, model = assets

    def boom(path):
        raise OSError("不是有效的 Win32 应用程序")

    monkeypatch.setattr(S.ctypes, "CDLL", boom)
    with pytest.raises(S.ShimError) as ei:
        S._Shim(dll, runtime, model)
    assert ei.value.kind == "shim_load_failed"
    assert "Win32" in ei.value.detail


def test_missing_export_is_reported(assets, fake_dll):
    del fake_dll.asr_set_prompt_for
    dll, runtime, model = assets
    with pytest.raises(S.ShimError) as ei:
        S._Shim(dll, runtime, model)
    assert ei.value.kind == "shim_load_failed"
    assert "asr_set_prompt_for" in ei.value.detail


def test_argtypes_are_declared(assets, fake_dll):
    """必须显式声明 argtypes，否则 64 位下指针会被截成 int。"""
    _make(assets, fake_dll)
    assert fake_dll.asr_init.argtypes == [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    assert fake_dll.asr_transcribe.restype is ctypes.c_int
    assert fake_dll.asr_last_text.restype is ctypes.c_char_p


# --- 加载 --------------------------------------------------------------------


def test_init_success_is_one(assets, fake_dll, monkeypatch, tmp_path):
    monkeypatch.setattr(
        config,
        "load_settings",
        lambda: config.Settings(
            model=str(tmp_path / "m.bin"), lang="zh", threads=4, max_bytes=1024
        ),
    )
    s = _make(assets, fake_dll)
    s.ensure_loaded()
    assert s.is_bound is True
    assert s.backend_version == "1.8.3"
    assert s.effective_threads == 8
    # 模型路径用正斜杠、UTF-8 编码传给 C 侧
    (passed,) = fake_dll.asr_init.calls[0][:1]
    assert isinstance(passed, bytes)
    assert b"\\" not in passed
    assert fake_dll.asr_init.calls[0][1:] == (0, 4)  # use_gpu=0, n_threads=4


def test_init_failure_zero_is_error(assets, fake_dll, monkeypatch, tmp_path):
    """asr_init 返回 0 表示失败（不是成功），这条语义曾经写反过。"""
    fake_dll.asr_init.ret = 0
    monkeypatch.setattr(
        config,
        "load_settings",
        lambda: config.Settings(
            model=str(tmp_path / "m.bin"), lang="zh", threads=4, max_bytes=1024
        ),
    )
    s = _make(assets, fake_dll)
    with pytest.raises(S.ShimError) as ei:
        s.ensure_loaded()
    assert ei.value.kind == "model_load_failed"
    assert s.is_bound is False


def test_ensure_loaded_is_idempotent(assets, fake_dll, monkeypatch, tmp_path):
    monkeypatch.setattr(
        config,
        "load_settings",
        lambda: config.Settings(
            model=str(tmp_path / "m.bin"), lang="zh", threads=4, max_bytes=1024
        ),
    )
    s = _make(assets, fake_dll)
    s.ensure_loaded()
    s.ensure_loaded()
    assert len(fake_dll.asr_init.calls) == 1


# --- 转写 --------------------------------------------------------------------


@pytest.fixture()
def bound(assets, fake_dll, monkeypatch, tmp_path):
    monkeypatch.setattr(
        config,
        "load_settings",
        lambda: config.Settings(
            model=str(tmp_path / "m.bin"), lang="zh", threads=4, max_bytes=1024
        ),
    )
    s = _make(assets, fake_dll)
    s.ensure_loaded()
    return s


def test_transcribe_happy_path(bound, fake_dll):
    fake_dll.asr_last_text.ret = "你好".encode("utf-8")
    r = bound.transcribe([0.0] * 16000, language="zh")
    assert r.text == "你好"
    assert r.duration_ms == 1000
    assert r.language == "zh"
    # 参数顺序：samples, n, language, translate, want_ts, ts_buf, ts_cap
    args = fake_dll.asr_transcribe.calls[0]
    assert args[1] == 16000
    assert args[2] == b"zh"
    assert args[3] == 0
    assert args[4] == 0
    assert args[6] == S._TS_CAP


def test_auto_language_passes_null(bound, fake_dll):
    bound.transcribe([0.0] * 10, language="auto")
    assert fake_dll.asr_transcribe.calls[0][2] is None


def test_timestamps_flag_and_buffer(bound, fake_dll):
    bound.transcribe([0.0] * 10, language="zh", want_ts=True)
    assert fake_dll.asr_transcribe.calls[0][4] == 1


def test_negative_return_is_error(bound, fake_dll):
    fake_dll.asr_transcribe.ret = -1
    fake_dll.asr_last_text.ret = b"partial"
    with pytest.raises(S.ShimError) as ei:
        bound.transcribe([0.0] * 10, language="zh")
    assert ei.value.kind == "failed"
    assert "-1" in ei.value.detail
    assert "partial" in ei.value.detail


def test_zero_samples_is_empty(bound):
    with pytest.raises(S.ShimError) as ei:
        bound.transcribe([], language="zh")
    assert ei.value.kind == "empty"


def test_byte_count_is_not_reported_as_duration(bound, fake_dll):
    """asr_transcribe 返回的是文本字节数，不能当耗时上报。"""
    fake_dll.asr_transcribe.ret = 999999
    r = bound.transcribe([0.0] * 8000, language="zh")
    assert r.duration_ms == 500


# --- 提示词与单例 -------------------------------------------------------------


def test_prompt_setting(bound, fake_dll):
    assert bound.set_initial_prompt("以下是普通话") is True
    assert fake_dll.asr_set_initial_prompt.calls[0][0] == "以下是普通话".encode("utf-8")
    assert bound.set_prompt_for("zh", "简体") is True
    assert fake_dll.asr_set_prompt_for.calls[0] == (
        b"zh",
        "简体".encode("utf-8"),
    )


def test_prompt_setting_reports_failure(bound, fake_dll):
    fake_dll.asr_set_prompt_for.ret = 0
    assert bound.set_prompt_for("zh", "简体") is False


def test_free_releases(bound, fake_dll):
    bound.free()
    assert bound.is_bound is False
    assert len(fake_dll.asr_free.calls) == 1


def test_singleton_is_cached(monkeypatch, assets, fake_dll):
    """同一个进程里只构造一次 shim（模型 147MB，不能反复加载）。"""
    dll, runtime, model = assets
    monkeypatch.setattr(config, "shim_dll_path", lambda: dll)
    monkeypatch.setattr(config, "runtime_dir", lambda: runtime)
    monkeypatch.setattr(config, "model_path", lambda: model)
    a = S.get_shim()
    assert S.get_shim() is a
    assert isinstance(a, S._Shim)


def test_get_shim_remembers_failure(monkeypatch, tmp_path):
    """构造失败要被记住：不反复尝试加载一个已知缺失的 DLL。"""
    monkeypatch.setattr(config, "shim_dll_path", lambda: tmp_path / "nope.dll")
    monkeypatch.setattr(config, "runtime_dir", lambda: tmp_path)
    monkeypatch.setattr(config, "model_path", lambda: tmp_path / "m.bin")
    with pytest.raises(S.ShimError):
        S.get_shim()
    info = S.loaded_info()
    assert info["loaded"] is False
    assert info["error"]  # 记住了失败原因
    with pytest.raises(S.ShimError):
        S.get_shim()