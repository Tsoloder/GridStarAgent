import json

from .types import Message, TextBlock, ToolCallBlock, ToolResultBlock


class MessageTransformer:
    def from_legacy(self, messages: list[dict], system_prompt: str = "") -> list[Message]:
        result = []
        if system_prompt:
            result.append(Message("system", (TextBlock(system_prompt),)))
        for item in messages:
            role = item["role"]
            blocks = []
            if item.get("content"):
                blocks.append(TextBlock(str(item["content"])))
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
