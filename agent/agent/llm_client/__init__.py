"""llm_client 包入口。

向后兼容：暴露 stream_chat() 函数，签名与原 llm_client.py 一致。
agent_loop.py 的 import llm_client; llm_client.stream_chat(...) 不用改。

v4 变更：
- 内部委托给 provider
- stream_with_retry 放在 registry 工厂层
- 支持 reasoning_chunk 事件透传
- 支持 done 事件（含 stop_reason）
"""
import logging
from typing import AsyncIterator

from .base import BaseProvider, ProviderConfig, RetryConfig
from .types import StreamDelta, ToolCallDelta
from .registry import create_provider, get_retry_config, PROVIDER_MAP
from .openai_provider import OpenAIProvider
from .anthropic_provider import AnthropicProvider

logger = logging.getLogger(__name__)


async def stream_chat(
    messages: list,
    system_prompt: str,
    config,
    tools: list,
    model_override: str = None,
) -> AsyncIterator[dict]:
    """流式调用 LLM。yield 统一事件。

    事件类型：
    {"type": "text_chunk", "delta": "..."}
    {"type": "reasoning_chunk", "delta": "..."}        # v4 新增
    {"type": "tool_call", "id": "...", "name": "...", "args": {...}}
    {"type": "usage", "input": N, "output": N, "total": N}
    {"type": "done", "stop_reason": "stop|length|tool_calls|content_filter"}  # v4 新增
    {"type": "error", "message": "...", "retryable": false}

    model_override: 可选的模型 ID 覆盖，优先于 config 中的默认模型。
    """
    provider = create_provider(config)
    effective_model_id = model_override if model_override else config.ResolveModelId()

    # v4: 使用 provider 的重试配置
    from retry import stream_with_retry
    retry_cfg = get_retry_config(config.api_type)

    async def factory():
        async for event in provider.stream_chat(
            messages, system_prompt, tools, effective_model_id
        ):
            yield event

    async for event in stream_with_retry(
        factory,
        max_retries=retry_cfg.max_retries,
        base_delay=retry_cfg.base_delay,
    ):
        yield event


# 向后兼容：暴露原有的工具转换函数
def to_openai_tools(mcp_tools: list) -> list:
    return OpenAIProvider.__new__(OpenAIProvider).to_tools(mcp_tools)


def to_anthropic_tools(mcp_tools: list) -> list:
    return AnthropicProvider.__new__(AnthropicProvider).to_tools(mcp_tools)


def to_anthropic_messages(messages: list, system_prompt: str):
    return AnthropicProvider.__new__(AnthropicProvider).to_messages(messages, system_prompt)
