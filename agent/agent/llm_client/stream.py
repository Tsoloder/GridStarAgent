import json
import logging
import uuid

from .types import (
    Done, Error, Message, Start, TextBlock, TextDelta, TextEnd, TextStart,
    ThinkingBlock, ThinkingDelta, ThinkingEnd, ThinkingStart, ToolCallBlock,
    ToolCallDelta, ToolCallEnd, ToolCallStart, Usage,
)

logger = logging.getLogger(__name__)


def _json_complete(text: str) -> bool:
    """整段文本是否已经是一个完整 JSON 值（不允许多余尾巴）。"""
    if not text:
        return False
    try:
        json.loads(text)
        return True
    except (json.JSONDecodeError, ValueError):
        return False


def decode_first_value(text: str):
    """取开头第一个 JSON 值，返回 (value, 消耗的字符数)。

    兼容网关会把两段工具参数拼进同一个 index（并行调用共用 index，或整段
    参数被重发），于是 raw 长成 ``{...}{...}``。json.loads 对此一律报
    "Extra data"，而第一个对象本身是好的，用 raw_decode 就能拿回来。
    """
    stripped = (text or "").lstrip()
    if not stripped:
        return None, 0
    try:
        value, end = json.JSONDecoder().raw_decode(stripped)
    except (json.JSONDecodeError, ValueError):
        return None, 0
    return value, (len(text) - len(stripped)) + end


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
        self._tools[index] = {"id": call_id, "name": name, "raw": "", "tail": ""}
        return self._event(ToolCallStart, index=index, call_id=call_id, name=name)

    def arguments_raw(self, index):
        """已经收下的参数原文；适配器靠它判断一次调用是否已经闭合。"""
        state = self._tools.get(index)
        return state["raw"] if state else ""

    def arguments_complete(self, index):
        return _json_complete(self.arguments_raw(index))

    def tool_delta(self, index, delta):
        state = self._tools[index]
        if delta and state["raw"] and _json_complete(state["raw"]):
            if delta.strip() == state["raw"].strip():
                # 上游把整段参数又发了一遍：丢掉重复的一份，不能直接拼。
                delta = ""
            else:
                # 参数已闭合却又来了一段：并进 tail 留档，保持 raw 是合法的
                # 单个对象。适配器按 index 拆不干净时靠这一步兜底。
                state["tail"] += delta
                delta = ""
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
        tail = state.get("tail") or ""
        try:
            parsed = json.loads(raw or "{}")
            if not isinstance(parsed, dict):
                raise ValueError("tool arguments must be a JSON object")
            args = parsed
        except (json.JSONDecodeError, ValueError) as exc:
            # 兜底：raw 里第一个对象完整、后面还挂着内容时，取第一个继续走，
            # 尾巴单独留档。整轮不该为了一个多余片段直接失败。
            salvaged, consumed = decode_first_value(raw)
            if isinstance(salvaged, dict):
                args, parse_error = salvaged, None
                tail = (tail + raw[consumed:]).strip()
            else:
                parse_error = str(exc)
        if tail:
            logger.warning("[tool args] %s(%s) 丢弃多余片段 %d 字符: %s",
                           state["name"], state["id"], len(tail), tail[:200])
        self.blocks.append(ToolCallBlock(state["id"], state["name"], args, raw, parse_error, tail))
        return self._event(ToolCallEnd, index=index, call_id=state["id"], name=state["name"],
                           arguments=args, raw_arguments=raw, parse_error=parse_error,
                           discarded_arguments=tail)

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
