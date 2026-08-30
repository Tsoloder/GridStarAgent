import json
import uuid

from .types import (
    Done, Error, Message, Start, TextBlock, TextDelta, TextEnd, TextStart,
    ThinkingBlock, ThinkingDelta, ThinkingEnd, ThinkingStart, ToolCallBlock,
    ToolCallDelta, ToolCallEnd, ToolCallStart, Usage,
)


class StreamBuilder:
    def __init__(self, response_id: str | None = None):
        self.response_id = response_id or str(uuid.uuid4())
        self.sequence = 0
        self.blocks = []
        self._text = {}
        self._thinking = {}
        self._tools = {}
        self.terminal = False

    def _event(self, cls, **kwargs):
        if self.terminal:
            raise RuntimeError("stream already terminated")
        event = cls(sequence=self.sequence, response_id=self.response_id, **kwargs)
        self.sequence += 1
        return event

    def start(self): return self._event(Start)

    def text_start(self, index=0):
        self._text[index] = ""
        return self._event(TextStart, index=index)

    def text_delta(self, delta, index=0):
        self._text[index] = self._text.get(index, "") + delta
        return self._event(TextDelta, delta=delta, index=index)

    def text_end(self, index=0):
        self.blocks.append(TextBlock(self._text.pop(index, "")))
        return self._event(TextEnd, index=index)

    def thinking_start(self, index=0):
        self._thinking[index] = ""
        return self._event(ThinkingStart, index=index)

    def thinking_delta(self, delta, index=0):
        self._thinking[index] = self._thinking.get(index, "") + delta
        return self._event(ThinkingDelta, delta=delta, index=index)

    def thinking_end(self, index=0, signature=None, redacted=False):
        self.blocks.append(ThinkingBlock(self._thinking.pop(index, ""), signature, redacted))
        return self._event(ThinkingEnd, index=index, signature=signature, redacted=redacted)

    def tool_start(self, index, call_id, name):
        if any(v["id"] == call_id for v in self._tools.values()):
            raise ValueError(f"duplicate tool call id: {call_id}")
        self._tools[index] = {"id": call_id, "name": name, "raw": ""}
        return self._event(ToolCallStart, index=index, call_id=call_id, name=name)

    def tool_delta(self, index, delta):
        state = self._tools[index]
        state["raw"] += delta
        partial = None
        try:
            value = json.loads(state["raw"])
            partial = value if isinstance(value, dict) else None
        except json.JSONDecodeError:
            pass
        return self._event(ToolCallDelta, index=index, arguments_delta=delta, partial_arguments=partial)

    def tool_end(self, index):
        state = self._tools.pop(index)
        raw, args, parse_error = state["raw"], None, None
        try:
            parsed = json.loads(raw or "{}")
            if not isinstance(parsed, dict):
                raise ValueError("tool arguments must be a JSON object")
            args = parsed
        except (json.JSONDecodeError, ValueError) as exc:
            parse_error = str(exc)
        self.blocks.append(ToolCallBlock(state["id"], state["name"], args, raw, parse_error))
        return self._event(ToolCallEnd, index=index, call_id=state["id"], name=state["name"],
                           arguments=args, raw_arguments=raw, parse_error=parse_error)

    def usage(self, **kwargs): return self._event(Usage, **kwargs)

    def done(self, stop_reason="stop"):
        self._close_open_blocks()
        event = self._event(Done, stop_reason=stop_reason,
                            message=Message("assistant", tuple(self.blocks)))
        self.terminal = True
        return event

    def error(self, category, message, **kwargs):
        self._close_open_blocks()
        event = self._event(Error, category=category, message=message, **kwargs)
        self.terminal = True
        return event

    def _close_open_blocks(self):
        for value in self._text.values(): self.blocks.append(TextBlock(value))
        for value in self._thinking.values(): self.blocks.append(ThinkingBlock(value))
        self._text.clear(); self._thinking.clear()
