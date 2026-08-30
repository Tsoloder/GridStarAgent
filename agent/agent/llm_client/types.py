from dataclasses import dataclass, field
from typing import Any, ClassVar, Literal, Optional, Union


@dataclass(frozen=True)
class ModelCapabilities:
    tools: bool = False
    parallel_tools: bool = False
    reasoning: bool = False
    vision: bool = False
    stream_usage: bool = False


@dataclass(frozen=True)
class ProviderConfig:
    id: str
    name: str = ""
    base_url: str = ""
    api_key: str = ""
    api_key_env: str = ""
    headers: dict[str, str] = field(default_factory=dict)
    timeout: float = 120.0
    connect_timeout: float = 30.0
    ssl_verify: bool = True
    enabled: bool = True


@dataclass(frozen=True)
class ModelConfig:
    id: str
    provider: str
    api: str
    name: str = ""
    enabled: bool = True
    context_window: int = 32768
    max_output_tokens: int = 4096
    capabilities: ModelCapabilities = field(default_factory=ModelCapabilities)
    compat: dict[str, Any] = field(default_factory=dict)

    @property
    def key(self) -> str:
        return f"{self.provider}/{self.id}"


@dataclass(frozen=True)
class TextBlock:
    text: str
    type: Literal["text"] = "text"


@dataclass(frozen=True)
class ThinkingBlock:
    text: str = ""
    signature: Optional[str] = None
    redacted: bool = False
    type: Literal["thinking"] = "thinking"


@dataclass(frozen=True)
class ImageBlock:
    source: Any
    media_type: Optional[str] = None
    type: Literal["image"] = "image"


@dataclass(frozen=True)
class ToolCallBlock:
    id: str
    name: str
    arguments: Optional[dict[str, Any]]
    raw_arguments: Optional[str] = None
    parse_error: Optional[str] = None
    type: Literal["tool_call"] = "tool_call"


@dataclass(frozen=True)
class ToolResultBlock:
    tool_call_id: str
    content: Any
    name: Optional[str] = None
    is_error: bool = False
    type: Literal["tool_result"] = "tool_result"


ContentBlock = Union[TextBlock, ThinkingBlock, ImageBlock, ToolCallBlock, ToolResultBlock]


@dataclass(frozen=True)
class Message:
    role: str
    content: tuple[ContentBlock, ...]
    provider_metadata: dict[str, Any] = field(default_factory=dict)
    status: str = "complete"


@dataclass(frozen=True, kw_only=True)
class StreamEvent:
    sequence: int
    response_id: str
    type: ClassVar[str] = "event"


@dataclass(frozen=True, kw_only=True)
class Start(StreamEvent):
    type: Literal["start"] = "start"


@dataclass(frozen=True)
class TextStart(StreamEvent):
    index: int = 0
    type: Literal["text_start"] = "text_start"


@dataclass(frozen=True, kw_only=True)
class TextDelta(StreamEvent):
    delta: str
    index: int = 0
    type: Literal["text_delta"] = "text_delta"


@dataclass(frozen=True)
class TextEnd(StreamEvent):
    index: int = 0
    type: Literal["text_end"] = "text_end"


@dataclass(frozen=True)
class ThinkingStart(StreamEvent):
    index: int = 0
    type: Literal["thinking_start"] = "thinking_start"


@dataclass(frozen=True, kw_only=True)
class ThinkingDelta(StreamEvent):
    delta: str
    index: int = 0
    type: Literal["thinking_delta"] = "thinking_delta"


@dataclass(frozen=True)
class ThinkingEnd(StreamEvent):
    index: int = 0
    signature: Optional[str] = None
    redacted: bool = False
    type: Literal["thinking_end"] = "thinking_end"


@dataclass(frozen=True, kw_only=True)
class ToolCallStart(StreamEvent):
    index: int
    call_id: str
    name: str
    type: Literal["tool_call_start"] = "tool_call_start"


@dataclass(frozen=True, kw_only=True)
class ToolCallDelta(StreamEvent):
    index: int
    arguments_delta: str
    partial_arguments: Optional[dict[str, Any]] = None
    type: Literal["tool_call_delta"] = "tool_call_delta"


@dataclass(frozen=True, kw_only=True)
class ToolCallEnd(StreamEvent):
    index: int
    call_id: str
    name: str
    arguments: Optional[dict[str, Any]]
    raw_arguments: str
    parse_error: Optional[str] = None
    type: Literal["tool_call_end"] = "tool_call_end"


@dataclass(frozen=True)
class Usage(StreamEvent):
    input_tokens: Optional[int] = None
    output_tokens: Optional[int] = None
    reasoning_tokens: Optional[int] = None
    cache_read_tokens: Optional[int] = None
    cache_write_tokens: Optional[int] = None
    total_tokens: Optional[int] = None
    type: Literal["usage"] = "usage"


@dataclass(frozen=True, kw_only=True)
class Done(StreamEvent):
    stop_reason: str
    message: Message
    type: Literal["done"] = "done"


@dataclass(frozen=True, kw_only=True)
class Error(StreamEvent):
    category: str
    message: str
    retryable: bool = False
    status_code: Optional[int] = None
    retry_after: Optional[float] = None
    type: Literal["error"] = "error"


# Legacy extraction types retained for callers outside the new runtime.
@dataclass
class StreamDelta:
    text: Optional[str] = None
    tool_call_delta: Optional["LegacyToolCallDelta"] = None
    usage: Optional[dict] = None
    reasoning: Optional[str] = None


@dataclass
class LegacyToolCallDelta:
    index: int
    id: Optional[str] = None
    name: Optional[str] = None
    args_fragment: Optional[str] = None
