"""voice_asr 的原生后端：通过 asr_shim.dll 驱动 GS 包内的 whisper.dll。

## 为什么要有这一层 C++ shim

Python 侧用 ctypes 手写 ``whisper_full_params`` 结构体是不可靠的：只要有一个字段
与真头文件对不上，ctypes 就会按错误尺寸回填，``whisper_full`` 随后读出乱码甚至
访问冲突，而且不一定报错（实测得到过 ``body's body's body's...`` 这类循环输出，
以及 exit code 0xC0000005）。排查成本极高，且每升一次 whisper.cpp 版本就要重查一遍。

所以布局交给 C++ 编译器按真头文件去算，对 Python 只暴露标量参数，不让任何
结构体跨越 DLL 边界。源码在 ``voice_asr/native/asr_shim.cpp``。

## 进程模型

whisper 上下文常驻内存（模型 147MB，加载一次约 0.24s）。转写在
``_LOCK`` 保护下串行执行；该锁是**不可重入**的 ``threading.Lock``，
它既是「上下文不是线程安全」的保险，也是「abort 回调靠全局标志」的必要条件。
"""

from __future__ import annotations

import ctypes
import os
import threading
from dataclasses import dataclass
from pathlib import Path

from . import config

# --- asr_shim.dll 导出的 C 接口（与 native/asr_shim.h 一一对应） ---------------

_INIT = ("asr_init", [ctypes.c_char_p, ctypes.c_int, ctypes.c_int], ctypes.c_int)
_TRANSCRIBE = (
    "asr_transcribe",
    [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,  # translate
        ctypes.c_int,  # want_ts
        ctypes.c_char_p,  # out_ts
        ctypes.c_int,  # out_ts_cap
    ],
    ctypes.c_int,
)
_LAST_TEXT = ("asr_last_text", [], ctypes.c_char_p)
_SET_INITIAL_PROMPT = ("asr_set_initial_prompt", [ctypes.c_char_p], ctypes.c_int)
_SET_PROMPT_FOR = ("asr_set_prompt_for", [ctypes.c_char_p, ctypes.c_char_p], ctypes.c_int)
_FREE = ("asr_free", [], None)
_BACKEND_VERSION = ("asr_backend_version", [], ctypes.c_char_p)
_EFFECTIVE_THREADS = ("asr_effective_threads", [], ctypes.c_int)

_TS_CAP = 8192


class ShimError(Exception):
    """原生后端不可用或调用失败。``kind`` 供上层映射 HTTP 状态码。"""

    def __init__(self, kind: str, message: str = "", detail: str = "") -> None:
        super().__init__(message or kind)
        self.kind = kind
        self.message = message or kind
        self.detail = detail


@dataclass(frozen=True)
class ShimResult:
    text: str
    language: str
    duration_ms: int
    timestamps: str = ""


def _decode(raw: bytes | None) -> str:
    if not raw:
        return ""
    return raw.decode("utf-8", errors="replace").strip()


class _Shim:
    """asr_shim.dll 的薄封装。生命周期由模块级 ``_instance()`` 管理。"""

    def __init__(self, dll_path: Path, runtime_dir: Path, model_path: Path) -> None:
        self._dll_path = dll_path
        self._model_path = model_path
        self._lock = threading.Lock()
        self._bound = False
        self._backend_version = "unknown"

        if not dll_path.is_file():
            raise ShimError(
                "shim_missing",
                "语音后端组件缺失",
                f"缺少 {dll_path}；请运行 voice_asr/native/build_shim.bat 构建",
            )
        if not model_path.is_file():
            raise ShimError(
                "model_missing", "语音模型缺失", f"缺少 {model_path}"
            )

        # Python 3.8+ 起，DLL 搜索路径默认不含被测 DLL 所在目录。
        # 必须先把包根加进去，whisper.dll 才找得到同目录的 ggml*.dll
        # （旧式 os.environ["PATH"] 追加对 ctypes 已无效）。
        if hasattr(os, "add_dll_directory"):
            self._dll_dir_handle = os.add_dll_directory(str(runtime_dir))
        else:  # pragma: no cover - 仅非 Windows 平台
            self._dll_dir_handle = None

        try:
            self._lib = ctypes.CDLL(str(dll_path))
        except OSError as exc:
            raise ShimError(
                "shim_load_failed", "语音后端组件无法加载", str(exc)
            ) from exc

        for name, argtypes, restype in (
            _INIT,
            _TRANSCRIBE,
            _LAST_TEXT,
            _SET_INITIAL_PROMPT,
            _SET_PROMPT_FOR,
            _FREE,
            _BACKEND_VERSION,
            _EFFECTIVE_THREADS,
        ):
            try:
                fn = getattr(self._lib, name)
            except AttributeError as exc:
                raise ShimError(
                    "shim_load_failed",
                    "语音后端组件不完整",
                    f"导出符号 {name} 不存在，DLL 可能构建失败或版本不匹配",
                ) from exc
            fn.argtypes = argtypes
            fn.restype = restype

    # --- 进程内单例的初始化：只在第一次转写时付这个代价 -----------------------

    def ensure_loaded(self) -> None:
        """加载模型（幂等）。首次调用约 0.24s。"""
        with self._lock:
            if self._bound:
                return
            settings = config.load_settings()
            # forward slashes：Windows API 同样接受，免掉反斜杠转义的歧义
            model_arg = str(self._model_path).replace("\\", "/").encode("utf-8")
            # asr_init 返回 1 成功 / 0 失败（见 native/asr_shim.h）
            rc = self._lib.asr_init(model_arg, 0, settings.threads)
            if rc != 1:
                raise ShimError(
                    "model_load_failed",
                    "语音模型加载失败",
                    f"asr_init 返回 {rc}（1=成功）",
                )
            self._backend_version = _decode(self._lib.asr_backend_version()) or "unknown"
            self._bound = True

    # --- 转写 -----------------------------------------------------------------

    def transcribe(
        self,
        samples,
        *,
        language: str = "auto",
        translate: bool = False,
        want_ts: bool = False,
    ) -> ShimResult:
        """转写一段 16kHz 单声道 float32 采样。

        与 ``asr_init`` 共用同一把不可重入锁：原生侧只有一个全局上下文，
        并发进入会让回调状态互相踩踏。
        """
        self.ensure_loaded()
        n_samples = len(samples)
        if n_samples <= 0:
            raise ShimError("empty", "未录到声音")

        buf = (ctypes.c_float * n_samples)(*samples)
        lang_arg = None if language == "auto" else language.encode("utf-8")
        ts_buf = ctypes.create_string_buffer(_TS_CAP)
        samples_ms = int(n_samples * 1000 / config.SAMPLE_RATE)

        with self._lock:
            # 返回写出的 UTF-8 文本字节数；-1 失败（见 native/asr_shim.h）
            rc = self._lib.asr_transcribe(
                buf,
                n_samples,
                lang_arg,
                1 if translate else 0,
                1 if want_ts else 0,
                ts_buf,
                _TS_CAP,
            )
            # asr_last_text() 的指针在下次 asr_transcribe 前有效，故在锁内取走
            text = _decode(self._lib.asr_last_text())

        if rc < 0:
            raise ShimError(
                "failed",
                "语音识别失败",
                f"asr_transcribe 返回 {rc}；文本={text!r}",
            )

        return ShimResult(
            text=text,
            language=language,
            # 原生返回的是文本字节数，不是耗时，所以这里按音频时长如实上报
            duration_ms=samples_ms,
            timestamps=ts_buf.value.decode("utf-8", errors="replace").strip(),
        )

    def set_initial_prompt(self, prompt: str) -> bool:
        """设置全局默认提示词。传空串即清空。"""
        self.ensure_loaded()
        # 返回 1 成功 / 0 失败（见 native/asr_shim.h）
        rc = self._lib.asr_set_initial_prompt(prompt.encode("utf-8"))
        return rc == 1

    def set_prompt_for(self, language: str, prompt: str) -> bool:
        """按语言设提示词（zh 走简体约束，其他语言走通用提示词）。"""
        # 返回 1 成功 / 0 失败；language 为 NULL 时失败
        rc = self._lib.asr_set_prompt_for(
            language.encode("utf-8"), prompt.encode("utf-8")
        )
        return rc == 1

    def free(self) -> None:
        with self._lock:
            if getattr(self, "_bound", False):
                self._lib.asr_free()
                self._bound = False

    @property
    def backend_version(self) -> str:
        return self._backend_version

    @property
    def is_bound(self) -> bool:
        """模型是否已经加载进内存。"""
        return self._bound

    @property
    def effective_threads(self) -> int:
        return int(self._lib.asr_effective_threads())


_lock = threading.Lock()
_instance: _Shim | None = None
_error: ShimError | None = None


def _paths() -> tuple[Path, Path, Path]:
    runtime = config.runtime_dir()
    return (
        config.shim_dll_path(),
        runtime,
        config.model_path(),
    )


def get_shim() -> _Shim:
    """取得（并在首次调用时创建）进程内唯一的 shim 实例。

    构造失败会被记住：不重复尝试加载一个已知缺失的 DLL。
    """
    global _instance, _error
    with _lock:
        if _instance is not None:
            return _instance
        if _error is not None:
            raise _error
        dll_path, runtime_dir, model_path = _paths()
        try:
            _instance = _Shim(dll_path, runtime_dir, model_path)
        except ShimError as exc:
            _error = exc
            raise
        return _instance


def loaded_info() -> dict:
    """已经加载过才返回的信息。绝不触发模型加载 —— ``/asr/health`` 会频繁调用。"""
    with _lock:
        inst = _instance
        err = _error
    if inst is None:
        return {
            "loaded": False,
            "backend": "",
            "error": err.message if err is not None else "",
        }
    return {
        "loaded": inst.is_bound,
        "backend": inst.backend_version,
        "threads": inst.effective_threads if inst.is_bound else 0,
        "error": "",
    }


def reset() -> None:
    """丢弃已缓存的实例与错误（测试用；也可用于运行时重新加载）。"""
    global _instance, _error
    with _lock:
        if _instance is not None:
            try:
                _instance.free()
            except Exception:  # noqa: BLE001 - 卸载失败不阻断重置
                pass
        _instance = None
        _error = None