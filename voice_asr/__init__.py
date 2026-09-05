"""语音转文字（ASR）后端模块。

复用本机 whisper.cpp（CPU 构建）与 Whisper base 模型，
以 FastAPI APIRouter 形式挂载进主应用，提供 POST /asr 与 GET /asr/health。
"""

from .router import router

__all__ = ["router"]
