from abc import ABC, abstractmethod
from typing import AsyncIterator

from ..types import Message, ModelConfig, StreamEvent


async def ensure_response_ok(response):
    """非 2xx 时读取上游响应体再抛出，保留 4xx/5xx 的具体错误原因。

    直接使用 response.raise_for_status() 会丢弃错误响应体，
    导致 400 等错误在日志中只有状态码、无法定位原因。
    """
    if response.status_code < 400:
        return
    body = (await response.aread()).decode("utf-8", "replace").strip()
    raise RuntimeError("Upstream HTTP %s: %s" % (response.status_code, body[:800]))


class Adapter(ABC):
    @abstractmethod
    def build_request(self, model: ModelConfig, messages: list[Message], tools: list) -> dict:
        raise NotImplementedError

    @abstractmethod
    async def stream(self, provider, model: ModelConfig, request: dict) -> AsyncIterator[StreamEvent]:
        raise NotImplementedError
