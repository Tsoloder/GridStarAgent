"""voice_asr 路由层。

提供：
- ``POST /asr``：接收 multipart 字段 ``audio``（WAV），转写为文本。
- ``GET /asr/health``：报告 CLI/模型就绪状态。

由主应用 ``app.py`` 通过 ``include_router`` 挂载（同源、无跨域）。
"""

from __future__ import annotations

import time
from typing import Optional

from fastapi import APIRouter, File, HTTPException, UploadFile
from fastapi.concurrency import run_in_threadpool

from . import config
from .transcriber import TranscribeError, transcribe

router = APIRouter(tags=["asr"])


@router.get("/asr/health")
def asr_health() -> dict:
    """就绪探针：前端启动时调用，未就绪则禁用语音按钮。"""
    s = config.load_settings()
    return {
        "ready": s.is_ready(),
        "cli": s.cli_exists(),
        "model": s.model_exists(),
        "language": s.lang,
    }


@router.post("/asr")
async def asr(audio: Optional[UploadFile] = File(None)) -> dict:
    """将上传的 WAV 转写为文本。

    错误码：400 空音频/无字段、413 过大、503 未就绪、504 超时、500 CLI 非零退出。
    """
    settings = config.load_settings()

    # 未就绪（CLI 或模型缺失）优先返回 503，避免无谓读取上传体。
    if not settings.is_ready():
        raise HTTPException(status_code=503, detail="语音服务未就绪（CLI 或模型缺失）")

    if audio is None:
        raise HTTPException(status_code=400, detail="缺少 audio 字段")

    # 先用 multipart 报告的 size 提前拦截超大上传，避免整体读入内存。
    if audio.size is not None and audio.size > settings.max_bytes:
        raise HTTPException(status_code=413, detail="音频过大")

    data = await audio.read()
    if len(data) > settings.max_bytes:
        raise HTTPException(status_code=413, detail="音频过大")
    if not data:
        raise HTTPException(status_code=400, detail="未录到声音")

    started = time.perf_counter()
    try:
        text = await run_in_threadpool(transcribe, data, settings=settings)
    except TranscribeError as exc:
        if exc.kind == "empty":
            raise HTTPException(status_code=400, detail="未录到声音") from exc
        if exc.kind == "timeout":
            raise HTTPException(status_code=504, detail=str(exc)) from exc
        if exc.kind == "cli_missing":
            raise HTTPException(status_code=503, detail="whisper-cli 缺失") from exc
        raise HTTPException(
            status_code=500, detail=f"转写失败: {exc.detail or 'whisper-cli 非零退出'}"
        ) from exc
    duration_ms = int((time.perf_counter() - started) * 1000)

    return {"text": text, "language": settings.lang, "duration_ms": duration_ms}
