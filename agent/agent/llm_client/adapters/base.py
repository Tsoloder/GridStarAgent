from abc import ABC, abstractmethod
from typing import AsyncIterator

from ..types import Message, ModelConfig, StreamEvent


class Adapter(ABC):
    @abstractmethod
    def build_request(self, model: ModelConfig, messages: list[Message], tools: list) -> dict:
        raise NotImplementedError

    @abstractmethod
    async def stream(self, provider, model: ModelConfig, request: dict) -> AsyncIterator[StreamEvent]:
        raise NotImplementedError
