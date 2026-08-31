import json

from .base import Adapter, ensure_response_ok
from .openai_chat import _tools
from ..stream import StreamBuilder
from ..types import TextBlock, ToolCallBlock, ToolResultBlock


class OpenAIResponsesAdapter(Adapter):
    def build_request(self, model, messages, tools):
        items = []
        for message in messages:
            text = "".join(b.text for b in message.content if isinstance(b, TextBlock))
            if text: items.append({"role": message.role, "content": text})
            for block in message.content:
                if isinstance(block, ToolCallBlock):
                    items.append({"type": "function_call", "call_id": block.id, "name": block.name,
                                  "arguments": block.raw_arguments or json.dumps(block.arguments)})
                elif isinstance(block, ToolResultBlock):
                    items.append({"type": "function_call_output", "call_id": block.tool_call_id,
                                  "output": str(block.content)})
        body = {"model": model.id, "input": items, "stream": True,
                "max_output_tokens": model.max_output_tokens}
        if tools:
            body["tools"] = [entry["function"] | {"type": "function"} for entry in _tools(tools)]
        return body

    async def stream(self, provider, model, request):
        builder = StreamBuilder(); yield builder.start()
        text_open = False; tools = set(); status = "stop"
        try:
            async with provider.client().stream("POST", "/responses", json=request) as response:
                await ensure_response_ok(response)
                async for line in response.aiter_lines():
                    if not line.startswith("data:"): continue
                    raw = line[5:].strip()
                    if raw == "[DONE]": break
                    event = json.loads(raw); kind = event.get("type", "")
                    if kind == "response.output_text.delta":
                        if not text_open: yield builder.text_start(); text_open = True
                        yield builder.text_delta(event.get("delta", ""))
                    elif kind == "response.output_item.added" and event.get("item", {}).get("type") == "function_call":
                        item = event["item"]; idx = event.get("output_index", 0)
                        yield builder.tool_start(idx, item.get("call_id", item.get("id", "")), item.get("name", "")); tools.add(idx)
                    elif kind == "response.function_call_arguments.delta":
                        yield builder.tool_delta(event.get("output_index", 0), event.get("delta", ""))
                    elif kind == "response.completed":
                        usage = event.get("response", {}).get("usage", {})
                        if usage: yield builder.usage(input_tokens=usage.get("input_tokens"), output_tokens=usage.get("output_tokens"), total_tokens=usage.get("total_tokens"))
                    elif kind in {"response.failed", "response.incomplete"}:
                        status = "length" if kind.endswith("incomplete") else "unknown"
            if text_open: yield builder.text_end()
            for idx in sorted(tools): yield builder.tool_end(idx)
            yield builder.done("tool_use" if tools else status)
        except Exception as exc:
            yield builder.error("provider_error", str(exc), retryable=False)
