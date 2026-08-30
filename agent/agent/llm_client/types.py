"""LLM 流式响应的数据类型定义。

v4 修复：StreamDelta 增加 reasoning 字段（原 llm_client.py 漏定义）。
"""
from dataclasses import dataclass
from typing import Optional


@dataclass
class StreamDelta:
    """LLM 流式响应的单帧增量。"""
    text: Optional[str] = None
    tool_call_delta: Optional["ToolCallDelta"] = None
    usage: Optional[dict] = None
    reasoning: Optional[str] = None    # v4 修复：DeepSeek R1 推理内容


@dataclass
class ToolCallDelta:
    """工具调用的流式增量片段。"""
    index: int
    id: Optional[str] = None
    name: Optional[str] = None
    args_fragment: Optional[str] = None
