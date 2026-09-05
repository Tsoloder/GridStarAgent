import json

from .types import ImageBlock, Message, TextBlock, ThinkingBlock, ToolCallBlock, ToolResultBlock


class MessageTransformer:
    def from_legacy(self, messages: list[dict], system_prompt: str = "") -> list[Message]:
        result = []
        if system_prompt:
            result.append(Message("system", (TextBlock(system_prompt),)))
        for item in messages:
            role = item["role"]
            blocks = []
            content = item.get("content")
            if isinstance(content, list):
                # 多模态 content：[{"type":"text",...}, {"type":"image","media_type":...,"data":...}]
                for part in content:
                    if not isinstance(part, dict):
                        continue
                    if part.get("type") == "image" and part.get("data"):
                        blocks.append(ImageBlock(source=part["data"], media_type=part.get("media_type")))
                    elif part.get("text"):
                        blocks.append(TextBlock(str(part["text"])))
            elif content:
                blocks.append(TextBlock(str(content)))
            if role == "assistant" and item.get("reasoning_content"):
                blocks.append(ThinkingBlock(str(item["reasoning_content"])))
            if role == "assistant":
                for call in item.get("tool_calls", []):
                    function = call.get("function", {})
                    raw = function.get("arguments", "")
                    try:
                        args = json.loads(raw or "{}")
                        error = None if isinstance(args, dict) else "tool arguments must be a JSON object"
                        if error: args = None
                    except json.JSONDecodeError as exc:
                        args, error = None, str(exc)
                    blocks.append(ToolCallBlock(call.get("id", ""), function.get("name", ""), args, raw, error))
            elif role == "tool":
                blocks = [ToolResultBlock(item.get("tool_call_id", ""), item.get("content", ""),
                                          item.get("tool_name"), bool(item.get("is_error")))]
            result.append(Message(role, tuple(blocks), status=item.get("status", "complete")))
        return result

    def transform(self, messages: list[Message], model) -> list[Message]:
        return [message for message in messages
                if not (message.role == "assistant" and message.status in {"error", "aborted"})]
