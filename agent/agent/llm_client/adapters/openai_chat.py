import json

from .base import Adapter, ensure_response_ok
from ..stream import StreamBuilder
from ..types import TextBlock, ThinkingBlock, ToolCallBlock, ToolResultBlock


def _tools(tools):
    return [{"type": "function", "function": {"name": t.name, "description": t.description or "",
             "parameters": t.inputSchema or {"type": "object", "properties": {}}}} for t in tools]


class OpenAIChatAdapter(Adapter):
    def build_request(self, model, messages, tools):
        output = []
        for message in messages:
            text = "".join(b.text for b in message.content if isinstance(b, TextBlock))
            reasoning = "".join(b.text for b in message.content if isinstance(b, ThinkingBlock))
            item = {"role": message.role, "content": text or None}
            if message.role == "assistant" and reasoning:
                item[model.compat.get("reasoning_field", "reasoning_content")] = reasoning
            calls = [b for b in message.content if isinstance(b, ToolCallBlock)]
            results = [b for b in message.content if isinstance(b, ToolResultBlock)]
            if calls:
                item["tool_calls"] = [{"id": b.id, "type": "function", "function": {
                    "name": b.name, "arguments": b.raw_arguments or json.dumps(b.arguments)}} for b in calls]
            if results:
                for block in results:
                    output.append({"role": "tool", "tool_call_id": block.tool_call_id,
                                   "content": str(block.content)})
                continue
            output.append(item)
        body = {"model": model.id, "messages": output, "stream": True}
        if tools: body["tools"] = _tools(tools)
        if model.capabilities.stream_usage and model.compat.get("stream_options", True):
            body["stream_options"] = {"include_usage": True}
        field = model.compat.get("max_tokens_field", "max_tokens")
        body[field] = model.max_output_tokens
        return body

    async def stream(self, provider, model, request):
        builder = StreamBuilder()
        yield builder.start()
        text_open = thinking_open = False
        open_tools = set()
        finish = "stop"
        try:
            async with provider.client().stream("POST", "/chat/completions", json=request) as response:
                await ensure_response_ok(response)
                async for line in response.aiter_lines():
                    if not line.startswith("data:"): continue
                    payload = line[5:].strip()
                    if payload == "[DONE]": break
                    data = json.loads(payload)
                    usage = data.get("usage")
                    if usage:
                        yield builder.usage(input_tokens=usage.get("prompt_tokens"),
                                            output_tokens=usage.get("completion_tokens"),
                                            total_tokens=usage.get("total_tokens"))
                    for choice in data.get("choices", []):
                        delta = choice.get("delta", {})
                        reasoning = delta.get(model.compat.get("reasoning_field", "reasoning_content"))
                        if reasoning:
                            if not thinking_open: yield builder.thinking_start(); thinking_open = True
                            yield builder.thinking_delta(reasoning)
                        if delta.get("content"):
                            if thinking_open: yield builder.thinking_end(); thinking_open = False
                            if not text_open: yield builder.text_start(); text_open = True
                            yield builder.text_delta(delta["content"])
                        for call in delta.get("tool_calls", []):
                            idx = call.get("index", 0); function = call.get("function", {})
                            if idx not in open_tools:
                                yield builder.tool_start(idx, call.get("id", f"call_{idx}"), function.get("name", "")); open_tools.add(idx)
                            if function.get("arguments") is not None:
                                yield builder.tool_delta(idx, function["arguments"])
                        if choice.get("finish_reason"):
                            finish = {"tool_calls": "tool_use", "max_tokens": "length"}.get(choice["finish_reason"], choice["finish_reason"])
            if thinking_open: yield builder.thinking_end()
            if text_open: yield builder.text_end()
            for idx in sorted(open_tools): yield builder.tool_end(idx)
            yield builder.done(finish if finish in {"stop", "length", "tool_use", "content_filter"} else "unknown")
        except Exception as exc:
            if not builder.terminal:
                yield builder.error("provider_error", str(exc), retryable=False)
