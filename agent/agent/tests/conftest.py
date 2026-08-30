"""pytest 配置：mock 掉 llm_client.stream_chat 和 mcp.call_tool。

测试策略：
- mock 掉 llm_client.stream_chat，按 fixture 预设返回事件序列
- mock 掉 mcp.call_tool，按 fixture 预设返回结果
- mock 掉 mcp.available_tools / mcp.tool_schema / mcp.tool_schemas
- 验证 agent_loop 产出的事件序列与 expected_event_types 匹配
"""
import json
import os
import sys
from types import SimpleNamespace
from typing import AsyncIterator
from unittest.mock import MagicMock, AsyncMock, patch

import pytest

# 把 agent 目录加入 sys.path
AGENT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if AGENT_DIR not in sys.path:
    sys.path.insert(0, AGENT_DIR)

FIXTURES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")


def load_fixture(name: str) -> dict:
    path = os.path.join(FIXTURES_DIR, f"{name}.json")
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


class MockLLMResponse:
    """模拟 LLM 的一次完整响应。"""

    def __init__(self, events: list):
        self._events = events
        self._index = 0

    async def __aiter__(self):
        for event in self._events:
            yield event


class MockLLMClient:
    """模拟 ModelRuntime，按 fixture 顺序返回统一流事件。"""

    def __init__(self, responses: list):
        self._responses = list(responses)
        self._call_index = 0

    def context_window(self, model_key):
        return 32000

    async def stream(self, model_key, messages, tools=(), system_prompt=""):
        if self._call_index >= len(self._responses):
            yield SimpleNamespace(type="usage", input_tokens=0, output_tokens=0,
                                  total_tokens=0)
            return
        response = self._responses[self._call_index]
        self._call_index += 1
        for event in response.get("events", []):
            event_type = event["type"]
            if event_type == "text_chunk":
                yield SimpleNamespace(type="text_delta", delta=event["delta"])
            elif event_type == "reasoning_chunk":
                yield SimpleNamespace(type="thinking_delta", delta=event["delta"])
            elif event_type == "tool_call":
                yield SimpleNamespace(type="tool_call_end", call_id=event["id"],
                                      name=event["name"], arguments=event.get("args"),
                                      parse_error=event.get("parse_error"))
            elif event_type == "usage":
                yield SimpleNamespace(type="usage", input_tokens=event.get("input"),
                                      output_tokens=event.get("output"),
                                      total_tokens=event.get("total"))
            elif event_type == "done":
                yield SimpleNamespace(type="done", stop_reason=event.get("stop_reason", "stop"))
            elif event_type == "error":
                yield SimpleNamespace(type="error", message=event["message"],
                                      retryable=event.get("retryable", False))


class MockMcpBridge:
    """模拟 McpBridge，按 fixture 预设返回工具结果。"""

    def __init__(self, results: dict, tool_schemas: dict = None):
        self._results = results
        self._tool_schemas = tool_schemas or {}
        self._tools = []
        for name in results:
            tool = MagicMock()
            tool.name = name
            tool.description = f"Mock tool: {name}"
            tool.inputSchema = self._tool_schemas.get(name, {"type": "object", "properties": {}})
            self._tools.append(tool)

    async def call_tool(self, name: str, args: dict) -> str:
        return self._results.get(name, f"Mock result for {name}")

    def available_tools(self) -> list:
        return self._tools

    def tool_schema(self, name: str) -> dict:
        return self._tool_schemas.get(name, {})

    def tool_schemas(self) -> dict:
        return {name: self.tool_schema(name) for name in self._results}


class MockSkillRegistry:
    """模拟 SkillRegistry，返回空技能列表。"""

    def __init__(self):
        self._skills = []

    def all(self):
        return self._skills

    def get(self, skill_id):
        raise ValueError(f"unknown skill: {skill_id}")

    def reload(self):
        pass

    def set_roots(self, roots, writable_root=None):
        pass

    @staticmethod
    def default_roots():
        return []

    def catalog_prompt(self, selected_ids=None):
        return "<available_skills></available_skills>"

    def read_skill(self, skill_id):
        return ""

    def read_resource(self, skill_id, relative_path):
        return ""

    def create_skill(self, skill_id, files, overwrite=False):
        return ""

    def internal_tools(self):
        return []

    def shadowed(self, skill_id):
        return []


class MockContextManager:
    """模拟 ContextManager，不做压缩。"""

    def __init__(self):
        from context import TokenCounter
        self.counter = TokenCounter()

    async def compress(self, messages, session_id, config, model_key="", runtime=None):
        return messages

    def persist_large_result(self, result, session_id):
        return result


@pytest.fixture
def fixture_loader():
    return load_fixture


@pytest.fixture
def mock_llm_client():
    def create(responses):
        return MockLLMClient(responses)
    return create


@pytest.fixture
def mock_mcp():
    def create(results, tool_schemas=None):
        return MockMcpBridge(results, tool_schemas)
    return create


@pytest.fixture
def mock_skill_registry():
    return MockSkillRegistry()


@pytest.fixture
def mock_ctx_mgr():
    return MockContextManager()
