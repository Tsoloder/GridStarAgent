import json

from .base import Adapter, ensure_response_ok, format_tool_result, image_data_url, stream_failure
from ..types import TextBlock, ThinkingBlock, ImageBlock, ToolCallBlock, ToolResultBlock


def _tools(tools):
    return [{"type": "function", "function": {"name": t.name, "description": t.description or "",
             "parameters": t.inputSchema or {"type": "object", "properties": {}}}} for t in tools]


class OpenAIChatAdapter(Adapter):
    def build_request(self, model, messages, tools):
        output = []
        for message in messages:
            text = "".join(b.text for b in message.content if isinstance(b, TextBlock))
            reasoning = "".join(b.text for b in message.content if isinstance(b, ThinkingBlock))
            item = {"role": message.role, "content": text}
            if message.role == "assistant" and reasoning:
                item[model.compat.get("reasoning_field", "reasoning_content")] = reasoning
            calls = [b for b in message.content if isinstance(b, ToolCallBlock)]
            results = [b for b in message.content if isinstance(b, ToolResultBlock)]
            images = [b for b in message.content if isinstance(b, ImageBlock) and b.source]
            if calls:
                item["tool_calls"] = [{"id": b.id, "type": "function", "function": {
                    "name": b.name, "arguments": b.raw_arguments or json.dumps(b.arguments)}} for b in calls]
            if results:
                for block in results:
                    output.append({"role": "tool", "tool_call_id": block.tool_call_id,
                                   "content": format_tool_result(block.content)})
                continue
            # 图片附件走多模态 content 数组，纯文本消息保持字符串（兼容严格网关）
            if images:
                parts = [{"type": "text", "text": text}] if text else []
                parts.extend({"type": "image_url", "image_url": {"url": image_data_url(b)}} for b in images)
                item["content"] = parts
            if text or calls or images:
                output.append(item)
        body = {"model": model.id, "messages": output, "stream": True}
        if tools: body["tools"] = _tools(tools)
        # 思考强度：默认什么都不下发。不认这些字段的网关会因为多出来的键直接 400，
        # 所以必须由 compat 显式开启。实测（阿里 compatible-mode +
        # deepseek-v4.1-flash）：thinking=disabled 真能把 reasoning 归零；
        # reasoning_effort 被接受但不改变思考量（minimal/low/baseline 都在 119~159
        # token 波动），留着给认这条的网关用。
        thinking = str(model.compat.get("thinking", "") or "").strip().lower()
        if thinking == "disabled":
            body["thinking"] = {"type": "disabled"}
        elif thinking == "enabled":
            body["thinking"] = {"type": "enabled"}
        effort = str(model.compat.get("reasoning_effort", "") or "").strip()
        if effort:
            body["reasoning_effort"] = effort
        if model.capabilities.stream_usage and model.compat.get("stream_options", True):
            body["stream_options"] = {"include_usage": True}
        field = model.compat.get("max_tokens_field", "max_tokens")
        body[field] = model.max_output_tokens
        return body

    async def attempt(self, provider, model, request, builder, state):
        text_open = thinking_open = False
        open_tools = set()
        # 上游 index 不等于一次调用：并行调用时部分网关把两次调用的增量都挂在
        # index=0 上，另一些还会在流末把整段参数重发一遍。slots 记录每个上游
        # index 当前对应的槽位，opened 记录槽位对应的 id/name，next_slot 用来给
        # 后续冒出来的调用（同 index）另开一个槽位，保证每段 arguments 各归各的。
        slots = {}      # 上游 index → 槽位
        call_slots = {}  # 上游 call id → 槽位
        opened = {}     # 槽位 → {"id", "name"}
        next_slot = 0
        finish = "stop"
        async with provider.client().stream("POST", "/chat/completions", json=request) as response:
            await ensure_response_ok(response)
            async for line in response.aiter_lines():
                if not line.startswith("data:"): continue
                payload = line[5:].strip()
                if payload == "[DONE]": break
                data = json.loads(payload)
                if isinstance(data, dict) and data.get("error"):
                    raise stream_failure(data)
                usage = data.get("usage")
                if usage:
                    # 缓存与推理明细在 *_tokens_details 里，供前端用量面板展示
                    prompt_details = usage.get("prompt_tokens_details") or {}
                    completion_details = usage.get("completion_tokens_details") or {}
                    yield builder.usage(input_tokens=usage.get("prompt_tokens"),
                                        output_tokens=usage.get("completion_tokens"),
                                        total_tokens=usage.get("total_tokens"),
                                        cache_read_tokens=prompt_details.get("cached_tokens"),
                                        reasoning_tokens=completion_details.get("reasoning_tokens"))
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
                        index = call.get("index", 0)
                        idx = index if isinstance(index, int) else 0
                        call_id = str(call.get("id") or "").strip()
                        function = call.get("function") or {}
                        chunk = function.get("arguments")
                        slot = call_slots.get(call_id) if call_id else None
                        if slot is None:
                            current = slots.get(idx)
                            if current is not None:
                                # 同一 index 上出现新的 id，说明网关又把并行调用的
                                # 增量并过来了；没有 id 时靠"参数已闭合却又以 { 开头"
                                # 判断是另起一段。
                                restarted = (
                                    isinstance(chunk, str)
                                    and chunk.lstrip().startswith("{")
                                    and builder.arguments_complete(current)
                                )
                                if call_id and call_id != opened[current]["id"]:
                                    slot = None
                                elif restarted and not call_id:
                                    slot = None
                                else:
                                    slot = current
                        if slot is None:
                            slot = next_slot; next_slot += 1
                            name = str(function.get("name") or "")
                            if not name and idx in slots:
                                # 续上来的第二段参数不带 name，沿用同 index 上的旧名
                                name = opened[slots[idx]]["name"]
                            new_id = call_id or f"call_{slot}"
                            if new_id in call_slots:  # 同一 id 重发整段参数
                                new_id = "%s_%d" % (new_id, slot)
                            call_slots[new_id] = slot
                            slots[idx] = slot
                            opened[slot] = {"id": new_id, "name": name}
                            open_tools.add(slot)
                            yield builder.tool_start(slot, new_id, name)
                        if isinstance(chunk, str) and chunk:
                            yield builder.tool_delta(slot, chunk)
                    if choice.get("finish_reason"):
                        finish = {"tool_calls": "tool_use", "max_tokens": "length"}.get(choice["finish_reason"], choice["finish_reason"])
        if thinking_open: yield builder.thinking_end()
        if text_open: yield builder.text_end()
        for idx in sorted(open_tools): yield builder.tool_end(idx)
        state.stop_reason = finish if finish in {"stop", "length", "tool_use", "content_filter"} else "unknown"
