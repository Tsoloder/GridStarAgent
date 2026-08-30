"""Protocol-neutral LLM providers, adapters, events, and runtime."""
from typing import AsyncIterator

from .adapters.openai_chat import _tools
from .anthropic_provider import AnthropicProvider as LegacyAnthropicProvider
from .openai_provider import OpenAIProvider as LegacyOpenAIProvider
from .registry import AdapterRegistry, ProviderRegistry
from .runtime import ModelCatalog, ModelRuntime
from .types import *


async def stream_chat(messages: list, system_prompt: str, config, tools: list,
                      model_override: str = None) -> AsyncIterator[dict]:
    """Legacy Agent Loop bridge; the new entry point is ModelRuntime.stream()."""
    from retry import stream_with_retry
    provider_cls = LegacyAnthropicProvider if config.api_type == "anthropic" else LegacyOpenAIProvider
    from .base import ProviderConfig
    provider = provider_cls(config, ProviderConfig())
    model_id = model_override or config.ResolveModelId()

    async def factory():
        async for event in provider.stream_chat(messages, system_prompt, tools, model_id):
            yield event

    async for event in stream_with_retry(factory, max_retries=3, base_delay=1.0):
        yield event


def to_openai_tools(mcp_tools: list) -> list:
    return _tools(mcp_tools)


def to_anthropic_tools(mcp_tools: list) -> list:
    return LegacyAnthropicProvider.__new__(LegacyAnthropicProvider).to_tools(mcp_tools)


def to_anthropic_messages(messages: list, system_prompt: str):
    return LegacyAnthropicProvider.__new__(LegacyAnthropicProvider).to_messages(messages, system_prompt)
