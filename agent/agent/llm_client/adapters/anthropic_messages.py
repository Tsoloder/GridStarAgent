import json

from .base import Adapter, ensure_response_ok, format_tool_result, stream_failure
from ..types import TextBlock, ThinkingBlock, ImageBlock, ToolCallBlock, ToolResultBlock

# /messages 的提示词缓存必须手动打 cache_control 断点，不打就完全不缓存。
# 断点上限 4 个，这里用 3 个覆盖逐轮增长的稳定前缀：system → tools → 末条消息。
_EPHEMERAL = {"type": "ephemeral"}


def _cache_breakpoints(model) -> bool:
    """是否给请求打显式缓存断点。

    anthropic-messages 适配器只对接 Anthropic 系接口，提示词缓存是其原生能力，
    默认开启；若网关不认 cache_control 字段，用 compat.prompt_caching=false 关闭。
    """
    return model.compat.get("prompt_caching", True) is not False


class AnthropicMessagesAdapter(Adapter):
    def build_request(self, model, messages, tools):
        system, output = [], []
        for message in messages:
            content = []
            for block in message.content:
                if isinstance(block, TextBlock): content.append({"type": "text", "text": block.text})
                elif isinstance(block, ImageBlock) and block.source: content.append({"type": "image", "source": {"type": "base64", "media_type": block.media_type or "image/png", "data": block.source}})
                elif isinstance(block, ThinkingBlock) and block.text: content.append({"type": "text", "text": block.text})
                elif isinstance(block, ToolCallBlock): content.append({"type": "tool_use", "id": block.id, "name": block.name, "input": block.arguments or {}})
                elif isinstance(block, ToolResultBlock): content.append({"type": "tool_result", "tool_use_id": block.tool_call_id, "content": format_tool_result(block.content), "is_error": block.is_error})
            if message.role == "system": system.extend(b["text"] for b in content if b["type"] == "text")
            elif content: output.append({"role": "user" if message.role == "tool" else message.role, "content": content})
        cache = _cache_breakpoints(model)
        body = {"model": model.id, "messages": output, "max_tokens": model.max_output_tokens, "stream": True}
        if system:
            text = "\n".join(system)
            body["system"] = [{"type": "text", "text": text, "cache_control": dict(_EPHEMERAL)}] if cache else text
        if tools:
            body["tools"] = [{"name": t.name, "description": t.description or "", "input_schema": t.inputSchema or {"type": "object", "properties": {}}} for t in tools]
            if cache: body["tools"][-1]["cache_control"] = dict(_EPHEMERAL)
        # 末条消息再打一个断点，让本轮内多次请求之间不断变长的历史也进缓存
        if cache and output: output[-1]["content"][-1]["cache_control"] = dict(_EPHEMERAL)
        return body

    async def attempt(self, provider, model, request, builder, state):
        event_name = None; data_lines = []; blocks = {}
        prompt_tokens = None; completion_tokens = None; cache_read = 0; cache_write = 0
        async def events(response):
            nonlocal event_name, data_lines
            async for line in response.aiter_lines():
                if not line:
                    if event_name and data_lines: yield event_name, json.loads("\n".join(data_lines))
                    event_name, data_lines = None, []; continue
                if line.startswith("event:"): event_name = line[6:].strip()
                elif line.startswith("data:"): data_lines.append(line[5:].strip())
            if event_name and data_lines: yield event_name, json.loads("\n".join(data_lines))
        stop = "stop"
        async with provider.client().stream("POST", "/messages", json=request) as response:
            await ensure_response_ok(response)
            async for kind, data in events(response):
                idx = data.get("index", 0); block = data.get("content_block", {}); delta = data.get("delta", {})
                if kind == "error":
                    raise stream_failure(data)
                if kind == "content_block_start":
                    blocks[idx] = block.get("type")
                    if block.get("type") == "text": yield builder.text_start(idx)
                    elif block.get("type") in {"thinking", "redacted_thinking"}: yield builder.thinking_start(idx)
                    elif block.get("type") == "tool_use": yield builder.tool_start(idx, block.get("id", ""), block.get("name", ""))
                elif kind == "content_block_delta":
                    if delta.get("type") == "text_delta": yield builder.text_delta(delta.get("text", ""), idx)
                    elif delta.get("type") == "thinking_delta": yield builder.thinking_delta(delta.get("thinking", ""), idx)
                    elif delta.get("type") == "input_json_delta": yield builder.tool_delta(idx, delta.get("partial_json", ""))
                elif kind == "content_block_stop":
                    if blocks.get(idx) == "text": yield builder.text_end(idx)
                    elif blocks.get(idx) in {"thinking", "redacted_thinking"}: yield builder.thinking_end(idx, redacted=blocks[idx] == "redacted_thinking")
                    elif blocks.get(idx) == "tool_use": yield builder.tool_end(idx)
                elif kind == "message_delta": stop = {"end_turn": "stop", "max_tokens": "length", "tool_use": "tool_use"}.get(delta.get("stop_reason"), "unknown")
                usage = data.get("usage") or data.get("message", {}).get("usage")
                if usage:
                    # message_start 给输入侧与缓存明细，message_delta 给最终输出数，
                    # 合并后在流结束时上报一条，避免分段 usage 被重复累加。
                    if usage.get("input_tokens") is not None: prompt_tokens = usage["input_tokens"]
                    if usage.get("output_tokens") is not None: completion_tokens = usage["output_tokens"]
                    cache_read = usage.get("cache_read_input_tokens") or cache_read
                    cache_write = usage.get("cache_creation_input_tokens") or cache_write
        # Anthropic 的 input_tokens 不含缓存量，并回去与 OpenAI 口径对齐
        # （前端按 input 计算缓存命中率、按 input - cache_read 算未缓存输入）。
        prompt = (prompt_tokens or 0) + cache_read + cache_write
        completion = completion_tokens or 0
        yield builder.usage(input_tokens=prompt, output_tokens=completion,
                            total_tokens=prompt + completion,
                            cache_read_tokens=cache_read or None,
                            cache_write_tokens=cache_write or None)
        state.stop_reason = stop

