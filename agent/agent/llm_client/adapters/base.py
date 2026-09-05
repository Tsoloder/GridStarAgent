import asyncio
import json
import random
from abc import ABC, abstractmethod
from dataclasses import dataclass
from datetime import datetime, timezone
from email.utils import parsedate_to_datetime
from typing import Any, AsyncIterator, Optional

import httpx

from ..stream import StreamBuilder
from ..types import Message, ModelConfig, StreamEvent


# 瞬时故障：超时、并发冲突、过早请求、限流，以及全部 5xx。
RETRYABLE_STATUS = frozenset({408, 409, 425, 429, 500, 502, 503, 504})

# 部分兼容网关在 200 流里回 error 帧时不带状态码，只能靠错误码识别。
RETRYABLE_ERROR_MARKERS = ("throttl", "rate limit", "rate_limit", "too many requests",
                           "overload", "unavailable", "timeout", "server_error", "try again")


def is_retryable_status(status_code: Optional[int]) -> bool:
    return bool(status_code) and (status_code in RETRYABLE_STATUS or status_code >= 500)


def format_tool_result(content: Any) -> str:
    """工具结果转上游文本。

    字符串原样返回；dict/list 等结构化数据用 JSON 序列化。直接用 str() 会产出
    Python repr（单引号、True/None），模型读到的是无法解析的脏文本。
    """
    if isinstance(content, str):
        return content
    if content is None:
        return ""
    try:
        return json.dumps(content, ensure_ascii=False, default=str)
    except (TypeError, ValueError):
        return str(content)


def image_data_url(block: Any) -> str:
    """ImageBlock → data URL；source 本身已是 data:/http(s) URL 时原样透传。"""
    source = str(getattr(block, "source", "") or "")
    if source.startswith(("data:", "http://", "https://")):
        return source
    media_type = getattr(block, "media_type", "") or "image/png"
    return "data:%s;base64,%s" % (media_type, source)


def parse_retry_after(value: Any) -> Optional[float]:
    """解析 Retry-After 头，支持"秒数"和"HTTP-date"两种写法。"""
    text = str(value or "").strip()
    if not text:
        return None
    try:
        return max(0.0, float(text))
    except ValueError:
        pass
    try:
        when = parsedate_to_datetime(text)
    except (TypeError, ValueError):
        return None
    if when is None:
        return None
    if when.tzinfo is None:
        when = when.replace(tzinfo=timezone.utc)
    return max(0.0, (when - datetime.now(timezone.utc)).total_seconds())


class UpstreamError(RuntimeError):
    """上游错误，携带 Error 事件需要的分类字段（状态码、Retry-After、可否重试）。"""

    def __init__(self, message: str, *, category: str = "provider_error",
                 status_code: Optional[int] = None, retry_after: Optional[float] = None,
                 retryable: bool = False):
        super().__init__(message)
        self.category = category
        self.status_code = status_code
        self.retry_after = retry_after
        self.retryable = retryable


def stream_failure(data: Any) -> UpstreamError:
    """把塞在 200 流里的 `data: {"error": ...}` 帧翻译成可判重试的上游错误。

    兼容网关在流中途限流/过载时不走 HTTP 状态码，不识别就会被当成空回复吞掉。
    """
    error = data.get("error") if isinstance(data, dict) else None
    if not isinstance(error, dict):
        error = {"message": str(error or data)}
    detail = str(error.get("message") or error.get("code") or error)
    status = error.get("status") or error.get("http_code")
    status = status if isinstance(status, int) else None
    hints = " ".join(str(error.get(key, "")) for key in ("code", "type", "param")).lower()
    hints += " " + detail.lower()
    retryable = is_retryable_status(status) if status else any(m in hints for m in RETRYABLE_ERROR_MARKERS)
    return UpstreamError("Upstream stream error: %s" % detail[:500],
                         category="stream_error", status_code=status, retryable=retryable)


async def ensure_response_ok(response):
    """非 2xx 时读取上游响应体再抛出，保留 4xx/5xx 的具体错误原因。

    直接使用 response.raise_for_status() 会丢弃错误响应体，
    导致 400 等错误在日志中只有状态码、无法定位原因。
    """
    if response.status_code < 400:
        return
    body = (await response.aread()).decode("utf-8", "replace").strip()
    status = response.status_code
    raise UpstreamError(
        "Upstream HTTP %s: %s" % (status, body[:800]),
        category="rate_limited" if status == 429 else "upstream_http",
        status_code=status,
        retry_after=parse_retry_after(response.headers.get("retry-after")),
        retryable=is_retryable_status(status),
    )


def is_retryable(exc: Exception) -> bool:
    """异常是否属于"重发同一个请求有机会成功"的瞬时故障。"""
    if isinstance(exc, UpstreamError):
        return exc.retryable
    return isinstance(exc, httpx.TransportError) and not isinstance(exc, httpx.UnsupportedProtocol)


@dataclass(frozen=True)
class RetryPolicy:
    """流式请求的重试预算；退避优先服从上游的 Retry-After。"""
    max_attempts: int = 3
    base_delay: float = 1.0
    max_delay: float = 30.0

    def delay(self, attempt: int, retry_after: Optional[float] = None) -> float:
        if retry_after is not None:
            return min(max(retry_after, 0.0), self.max_delay)
        backoff = self.base_delay * (2 ** attempt)
        return min(backoff + random.uniform(0, 0.25 * self.base_delay), self.max_delay)


DEFAULT_RETRY_POLICY = RetryPolicy()


class StreamState:
    """单次尝试回传给重试骨架的信息（stop_reason 需要由 attempt 填写）。"""

    def __init__(self):
        self.stop_reason = "stop"


def error_event(builder, exc: Exception):
    """把流式过程中的异常转成带完整分类字段的 Error 事件。

    此前所有异常都被笼统标成 retryable=False，429/5xx/网络抖动这些本该重试的
    错误也被当成永久失败。
    """
    if isinstance(exc, UpstreamError):
        category, status, retry_after = exc.category, exc.status_code, exc.retry_after
    elif is_retryable(exc):
        category, status, retry_after = "network", None, None
    else:
        category, status, retry_after = "provider_error", None, None
    return builder.error(category, str(exc) or type(exc).__name__,
                         retryable=is_retryable(exc), status_code=status, retry_after=retry_after)


class Adapter(ABC):
    """上游适配器基类。

    子类只实现 attempt()（单次流式尝试），start/done/error 事件与瞬时故障重试
    统一由 stream() 负责，三个协议共享同一套重试语义。
    """

    def __init__(self, retry: RetryPolicy = DEFAULT_RETRY_POLICY):
        self.retry = retry

    @abstractmethod
    def build_request(self, model: ModelConfig, messages: list[Message], tools: list) -> dict:
        raise NotImplementedError

    @abstractmethod
    async def attempt(self, provider, model: ModelConfig, request: dict,
                      builder, state: StreamState) -> AsyncIterator[StreamEvent]:
        """跑一次上游流，只产出内容事件；结束前把 stop_reason 写进 state。"""
        raise NotImplementedError

    async def stream(self, provider, model: ModelConfig, request: dict) -> AsyncIterator[StreamEvent]:
        builder = StreamBuilder()
        yield builder.start()
        last_attempt = self.retry.max_attempts - 1
        for attempt in range(self.retry.max_attempts):
            state = StreamState()
            emitted = False
            failure = None
            try:
                async for event in self.attempt(provider, model, request, builder, state):
                    emitted = True
                    yield event
            except Exception as exc:
                failure = exc
            if failure is None:
                yield builder.done(state.stop_reason)
                return
            # 已经吐出内容就不能重放，否则用户会看到重复文本；
            # 预算用尽或错误本身不可恢复时，把真实分类交给下游。
            if emitted or attempt == last_attempt or not is_retryable(failure):
                yield error_event(builder, failure)
                return
            retry_after = failure.retry_after if isinstance(failure, UpstreamError) else None
            await asyncio.sleep(self.retry.delay(attempt, retry_after))
