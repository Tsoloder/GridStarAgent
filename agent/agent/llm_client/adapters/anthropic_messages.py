import json

from .base import Adapter
from ..stream import StreamBuilder
from ..types import TextBlock, ThinkingBlock, ToolCallBlock, ToolResultBlock


class AnthropicMessagesAdapter(Adapter):
    def build_request(self, model, messages, tools):
        system, output = [], []
        for message in messages:
            content = []
            for block in message.content:
                if isinstance(block, TextBlock): content.append({"type": "text", "text": block.text})
                elif isinstance(block, ThinkingBlock) and block.text: content.append({"type": "text", "text": block.text})
                elif isinstance(block, ToolCallBlock): content.append({"type": "tool_use", "id": block.id, "name": block.name, "input": block.arguments or {}})
                elif isinstance(block, ToolResultBlock): content.append({"type": "tool_result", "tool_use_id": block.tool_call_id, "content": str(block.content), "is_error": block.is_error})
            if message.role == "system": system.extend(b["text"] for b in content if b["type"] == "text")
            elif content: output.append({"role": "user" if message.role == "tool" else message.role, "content": content})
        body = {"model": model.id, "messages": output, "max_tokens": model.max_output_tokens, "stream": True}
        if system: body["system"] = "\n".join(system)
        if tools: body["tools"] = [{"name": t.name, "description": t.description or "", "input_schema": t.inputSchema or {"type": "object", "properties": {}}} for t in tools]
        return body

    async def stream(self, provider, model, request):
        builder = StreamBuilder(); yield builder.start()
        event_name = None; data_lines = []; blocks = {}; stop = "stop"
        async def events(response):
            nonlocal event_name, data_lines
            async for line in response.aiter_lines():
                if not line:
                    if event_name and data_lines: yield event_name, json.loads("\n".join(data_lines))
                    event_name, data_lines = None, []; continue
                if line.startswith("event:"): event_name = line[6:].strip()
                elif line.startswith("data:"): data_lines.append(line[5:].strip())
            if event_name and data_lines: yield event_name, json.loads("\n".join(data_lines))
        try:
            async with provider.client().stream("POST", "/messages", json=request) as response:
                response.raise_for_status()
                async for kind, data in events(response):
                    idx = data.get("index", 0); block = data.get("content_block", {}); delta = data.get("delta", {})
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
                    if usage: yield builder.usage(input_tokens=usage.get("input_tokens"), output_tokens=usage.get("output_tokens"))
            yield builder.done(stop)
        except Exception as exc:
            yield builder.error("provider_error", str(exc), retryable=False)
