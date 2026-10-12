"""语音识别路由：POST /asr 转写，GET /asr/health 探活。

挂载方式见 agent/agent/app.py —— 同一进程、同一端口，前端无需跨域配置。
"""

from __future__ import annotations

import logging
from typing import Optional

from fastapi import APIRouter, File, HTTPException, UploadFile
from starlette.concurrency import run_in_threadpool

from . import config
from .transcriber import TranscribeError, backend_info, transcribe

logger = logging.getLogger(__name__)

router = APIRouter(tags=["asr"])

# kind -> HTTP 状态码。未列出的 kind 一律 500。
_STATUS = {
    "bad_audio": 400,
    "empty": 400,
    "model_missing": 503,
    "shim_missing": 503,
    "shim_load_failed": 503,
    "model_load_failed": 503,
    "timeout": 504,
}


@router.get("/asr/health")
def health() -> dict:
    """探活。不触发模型加载（147MB），因此可以频繁调用。"""
    settings = config.load_settings()
    info = backend_info()
    return {
        "ready": settings.is_ready(),
        "shim": settings.shim_exists(),
        "model": settings.model_exists(),
        "language": settings.lang,
        "loaded": info["loaded"],
        "backend": info["backend"],
    }


@router.post("/asr")
async def asr(audio: Optional[UploadFile] = File(None)) -> dict:
    """接收一段 16kHz/单声道/16bit WAV，返回转写文本。"""
    settings = config.load_settings()
    if not settings.is_ready():
        raise HTTPException(
            status_code=503,
            detail="语音服务未就绪（缺少后端组件或模型）",
        )
    if audio is None:
        raise HTTPException(status_code=400, detail="缺少 audio 字段")

    # 先看声明的长度，避免把超大文件整个读进内存
    if audio.size is not None and audio.size > settings.max_bytes:
        raise HTTPException(status_code=413, detail="音频过大")

    data = await audio.read()
    if not data:
        raise HTTPException(status_code=400, detail="未录到声音")
    if len(data) > settings.max_bytes:
        raise HTTPException(status_code=413, detail="音频过大")

    try:
        result = await run_in_threadpool(transcribe, data, settings=settings)
    except TranscribeError as exc:
        status = _STATUS.get(exc.kind, 500)
        detail = exc.message if status != 500 else f"{exc.message}：{exc.detail}"
        raise HTTPException(status_code=status, detail=detail) from exc
    except Exception as exc:  # noqa: BLE001 - 兜底，避免 500 页面泄露堆栈
        logger.exception("asr 转写异常")
        raise HTTPException(status_code=500, detail=f"语音识别内部错误：{exc}") from exc

    return {
        "text": result.text,
        "language": result.language,
        "duration_ms": result.duration_ms,
        "backend_ms": result.backend_ms,
    }