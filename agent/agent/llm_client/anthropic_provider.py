"""Anthropic Claude Provider。

支持 Claude 3.5 Sonnet / Opus / Haiku 等原生 Anthropic 协议的模型。
"""
import json
import logging
from typing import AsyncIterator, Optional

import httpx

from .base import BaseProvider, ProviderConfig, RetryConfig
from .types import StreamDelta, LegacyToolCallDelta as ToolCallDelta

logger = logging.getLogger(__name__)


# v4: Anthropic 的 stop_reason 归一化映射
_STOP_REASON_MAP = {
    "end_turn": "stop",
    "max_tokens": "length",
    "tool_use": "tool_calls",
    "stop_sequence": "stop",
}

# v4: Claude 模型上下文窗口
_MODEL_WINDOWS = {
    "claude-3-5-sonnet": 200000,
    "claude-3-opus": 200000,
    "claude-3-haiku": 200000,
    "claude-3.5-sonnet": 200000,
}


class AnthropicProvider(BaseProvider):
    """Anthropic 原生协议的 Provider。"""

    def to_tools(self, mcp_tools: list) -> list:
        return [
            {
                "name": tool.name,
                "description": tool.description or "",
                "input_schema": tool.inputSchema or {"type": "object", "properties": {}},
            }
            for tool in mcp_tools
        ]

    def to_messages(self, messages: list, system_prompt: str):
        """把 OpenAI 格式的 messages 转为 Anthropic 格式。
        返回 (system, messages) 元组。"""
        anthropic_messages = []
        for msg in messages:
            role = msg["role"]
            if role == "system":
                continue
            if role == "user":
                anthropic_messages.append(
                    {"role": "user", "content": [{"type": "text", "text": msg["content"]}]}
                )
            elif role == "assistant":
                content = []
                if msg.get("content"):
                    content.append({"type": "text", "text": msg["content"]})
                for tc in msg.get("tool_calls", []):
                    try:
                        input_dict = json.loads(tc["function"]["arguments"])
                    except (json.JSONDecodeError, KeyError):
                        input_dict = {}
                    content.append(
                        {
                            "type": "tool_use",
                            "id": tc["id"],
                            "name": tc["function"]["name"],
                            "input": input_dict,
                        }
                    )
                anthropic_messages.append({"role": "assistant", "content": content})
            elif role == "tool":
                anthropic_messages.append(
                    {
                        "role": "user",
                        "content": [
                            {
                                "type": "tool_result",
                                "tool_use_id": msg["tool_call_id"],
                                "content": msg["content"],
                            }
                        ],
                    }
                )
        system = system_prompt or None
        return system, anthropic_messages

    async def stream_chat(self, messages: list, system_prompt: str,
                          tools: list, model_id: str) -> AsyncIterator[dict]:
        system, anthropic_messages = self.to_messages(messages, system_prompt)
        anthropic_tools = self.to_tools(tools) if tools else None

        ssl_context = self._make_ssl_context()

        url = self._config.api_url.rstrip("/")
        if not url.endswith("/v1/messages"):
            if url.endswith("/v1"):
                url = url + "/messages"
            else:
                url = url + "/v1/messages"

        headers = {
            "x-api-key": self._config.api_key,
            "anthropic-version": "2023-06-01",
            "content-type": "application/json",
        }
        body = {
            "model": model_id,
            "messages": anthropic_messages,
            "max_tokens": 4096,
            "stream": True,
        }
        if system:
            body["system"] = system
        if anthropic_tools:
            body["tools"] = anthropic_tools

        tool_call_buffers = {}
        input_tokens = 0
        output_tokens = 0
        stop_reason = None

        async with httpx.AsyncClient(verify=ssl_context) as client:
            async with client.stream("POST", url, headers=headers, json=body,
                                     timeout=self._provider_config.timeout) as resp:
                if resp.status_code != 200:
                    text = await resp.aread()
                    logger.error(f"anthropic error {resp.status_code}: {text.decode()}")
                    yield {
                        "type": "error",
                        "message": f"Anthropic API error {resp.status_code}",
                        "retryable": False,
                    }
                    return

                event_type = None
                data_lines = []
                async for line in resp.aiter_lines():
                    line = line.strip()
                    if not line:
                        if event_type and data_lines:
                            data_str = "\n".join(data_lines)
                            try:
                                data = json.loads(data_str)
                            except json.JSONDecodeError:
                                data = {}
                            delta = _extract_anthropic_delta(event_type, data, tool_call_buffers)
                            if delta:
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
                                    if "input" in delta.usage:
                                        input_tokens = delta.usage["input"]
                                    if "output" in delta.usage:
                                        output_tokens = delta.usage["output"]
                            
                            # v4: 提取 stop_reason
                            if event_type == "message_delta":
                                sr = data.get("delta", {}).get("stop_reason")
                                if sr:
                                    stop_reason = sr
                            
                            event_type = None
                            data_lines = []
                            continue
                        if line.startswith("event: "):
                            event_type = line[7:]
                        elif line.startswith("data: "):
                            data_lines.append(line[6:])

        if input_tokens or output_tokens:
            yield {
                "type": "usage",
                "input": input_tokens,
                "output": output_tokens,
                "total": input_tokens + output_tokens,
            }

        # v4: 流结束时 yield done 事件（含归一化的 stop_reason）
        yield {"type": "done", "stop_reason": self._normalize_stop_reason(stop_reason or "stop")}

        for idx in sorted(tool_call_buffers.keys()):
            tc = tool_call_buffers[idx]
            try:
                args = json.loads(tc["args_str"]) if tc["args_str"] else {}
            except json.JSONDecodeError:
                logger.warning(f"anthropic tool_call args parse failed: {tc['args_str']}")
                args = {}
            yield {"type": "tool_call", "id": tc["id"], "name": tc["name"], "args": args}

    def model_name(self) -> str:
        return self._config.ResolveModelId()

    # v4: stop_reason 归一化
    def _normalize_stop_reason(self, raw: str) -> str:
        return _STOP_REASON_MAP.get(raw, "stop")

    # v4: 上下文窗口
    @property
    def context_window(self) -> int:
        model_id = self._config.ResolveModelId().lower()
        for k, v in _MODEL_WINDOWS.items():
            if k in model_id:
                return v
        return 200000

    # v4: 重试配置
    @property
    def retry_config(self) -> RetryConfig:
        return RetryConfig(max_retries=3, base_delay=1.0)


def _extract_anthropic_delta(event_type: str, data: dict, buffers: dict) -> Optional[StreamDelta]:
    """从 Anthropic SSE 事件中提取增量数据。"""
    delta_obj = StreamDelta()

    if event_type == "message_start":
        usage = data.get("message", {}).get("usage", {})
        if usage:
            delta_obj.usage = {"input": usage.get("input_tokens", 0)}

    elif event_type == "content_block_start":
        block = data.get("content_block", {})
        idx = data.get("index", 0)
        if block.get("type") == "tool_use":
            delta_obj.tool_call_delta = ToolCallDelta(
                index=idx,
                id=block.get("id"),
                name=block.get("name"),
            )

    elif event_type == "content_block_delta":
        delta_data = data.get("delta", {})
        idx = data.get("index", 0)
        if delta_data.get("type") == "text_delta":
            delta_obj.text = delta_data.get("text", "")
        elif delta_data.get("type") == "input_json_delta":
            delta_obj.tool_call_delta = ToolCallDelta(
                index=idx,
                args_fragment=delta_data.get("partial", ""),
            )

    elif event_type == "message_delta":
        usage = data.get("usage", {})
        if usage:
            delta_obj.usage = {"output": usage.get("output_tokens", 0)}

    return delta_obj if (delta_obj.text or delta_obj.tool_call_delta or delta_obj.usage) else None
