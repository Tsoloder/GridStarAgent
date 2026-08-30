"""OpenAI 兼容 Provider。

支持所有兼容 OpenAI Chat Completions API 的模型：
- GPT-4o / GPT-4 Turbo
- DeepSeek V3 / R1（含 reasoning_content）
- 通义千问 / 智谱 GLM / Moonshot Kimi / 零一万物 Yi
- 本地 vLLM / Ollama
"""
import json
import logging
from typing import AsyncIterator, Optional

import httpx
from openai import AsyncOpenAI

from .base import BaseProvider, ProviderConfig, RetryConfig
from .types import StreamDelta, LegacyToolCallDelta as ToolCallDelta

logger = logging.getLogger(__name__)


# v4: 模型上下文窗口大小（从 context.py 迁移）
_MODEL_WINDOWS = {
    "gpt-4": 8192,
    "gpt-4-turbo": 128000,
    "gpt-4o": 128000,
    "deepseek-chat": 64000,
    "deepseek-reasoner": 64000,
    "qwen-max": 32768,
    "glm-4": 128000,
    "mimo": 32000,
}


class OpenAIProvider(BaseProvider):
    """OpenAI 兼容协议的 Provider。"""

    def to_tools(self, mcp_tools: list) -> list:
        return [
            {
                "type": "function",
                "function": {
                    "name": tool.name,
                    "description": tool.description or "",
                    "parameters": tool.inputSchema or {"type": "object", "properties": {}},
                },
            }
            for tool in mcp_tools
        ]

    def to_messages(self, messages: list, system_prompt: str):
        """OpenAI 协议直接使用原格式，system 作为第一条消息。"""
        full_messages = []
        if system_prompt:
            full_messages.append({"role": "system", "content": system_prompt})
        full_messages.extend(messages)
        return system_prompt, full_messages

    async def stream_chat(self, messages: list, system_prompt: str,
                          tools: list, model_id: str) -> AsyncIterator[dict]:
        _, full_messages = self.to_messages(messages, system_prompt)
        openai_tools = self.to_tools(tools) if tools else None

        base_url = self._config.api_url.rstrip("/")
        if base_url.endswith("/chat/completions"):
            base_url = base_url[: -len("/chat/completions")]
        if not base_url.endswith("/v1"):
            base_url += "/v1"

        ssl_context = self._make_ssl_context()
        http_client = httpx.AsyncClient(
            verify=ssl_context,
            timeout=httpx.Timeout(self._provider_config.timeout,
                                  connect=self._provider_config.connect_timeout),
        )
        client = AsyncOpenAI(
            base_url=base_url,
            api_key=self._config.api_key,
            http_client=http_client,
            timeout=httpx.Timeout(self._provider_config.timeout,
                                  connect=self._provider_config.connect_timeout),
        )

        response = await client.chat.completions.create(
            model=model_id,
            messages=full_messages,
            tools=openai_tools,
            stream=True,
            stream_options={"include_usage": True} if openai_tools else None,
        )

        tool_call_buffers = {}
        _thinking_started = False
        _thinking_closed = False
        finish_reason = None

        async for chunk in response:
            delta = _extract_openai_delta(chunk)

            # v4: reasoning_content 作为独立事件
            if delta.reasoning:
                yield {"type": "reasoning_chunk", "delta": delta.reasoning}

            if delta.text:
                yield {"type": "text_chunk", "delta": delta.text}

            if delta.tool_call_delta:
                idx = delta.tool_call_delta.index
                if idx not in tool_call_buffers:
                    tool_call_buffers[idx] = {
                        "id": delta.tool_call_delta.id or "",
                        "name": delta.tool_call_delta.name or "",
                        "args_str": "",
                    }
                if delta.tool_call_delta.id:
                    tool_call_buffers[idx]["id"] = delta.tool_call_delta.id
                if delta.tool_call_delta.name:
                    tool_call_buffers[idx]["name"] = delta.tool_call_delta.name
                if delta.tool_call_delta.args_fragment:
                    tool_call_buffers[idx]["args_str"] += delta.tool_call_delta.args_fragment

            if delta.usage:
                yield delta.usage

            # 提取 finish_reason
            if hasattr(chunk, "choices") and chunk.choices:
                fr = chunk.choices[0].finish_reason
                if fr:
                    finish_reason = fr

        # v4: 流结束时 yield done 事件（含归一化的 stop_reason）
        yield {"type": "done", "stop_reason": self._normalize_stop_reason(finish_reason or "stop")}

        # yield tool_call 事件
        processed = self._post_process_tool_calls(
            [{"id": tc["id"], "name": tc["name"], "args_str": tc["args_str"]}
             for tc in [tool_call_buffers[k] for k in sorted(tool_call_buffers.keys())]]
        )
        for tc in processed:
            try:
                args = json.loads(tc["args_str"]) if tc.get("args_str") else {}
            except json.JSONDecodeError:
                logger.warning(f"tool_call args parse failed: {tc.get('args_str', '')}")
                args = {}
            yield {"type": "tool_call", "id": tc["id"], "name": tc["name"], "args": args}

    def model_name(self) -> str:
        return self._config.ResolveModelId()

    # v4: stop_reason 归一化（OpenAI 协议本身就是标准值）
    def _normalize_stop_reason(self, raw: str) -> str:
        return raw or "stop"

    # v4: 上下文窗口
    @property
    def context_window(self) -> int:
        model_id = self._config.ResolveModelId().lower()
        for k, v in _MODEL_WINDOWS.items():
            if k in model_id:
                return v
        return 32000

    # v4: 重试配置
    @property
    def retry_config(self) -> RetryConfig:
        return RetryConfig(max_retries=3, base_delay=1.0)


def _extract_openai_delta(chunk) -> StreamDelta:
    """从 OpenAI chunk 中提取增量数据。"""
    delta_obj = StreamDelta()
    if not chunk.choices:
        if hasattr(chunk, "usage") and chunk.usage:
            delta_obj.usage = {
                "type": "usage",
                "input": chunk.usage.prompt_tokens or 0,
                "output": chunk.usage.completion_tokens or 0,
                "total": chunk.usage.total_tokens or 0,
            }
        return delta_obj

    choice = chunk.choices[0]
    delta = choice.delta

    if delta and delta.content:
        delta_obj.text = delta.content

    # DeepSeek 推理模型通过 reasoning_content 返回思考内容
    reasoning_content = getattr(delta, "reasoning_content", None)
    if reasoning_content:
        delta_obj.reasoning = reasoning_content

    if delta and delta.tool_calls:
        for tc_delta in delta.tool_calls:
            delta_obj.tool_call_delta = ToolCallDelta(
                index=tc_delta.index,
                id=tc_delta.id,
                name=tc_delta.function.name if tc_delta.function else None,
                args_fragment=tc_delta.function.arguments if tc_delta.function else None,
            )

    return delta_obj
