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
    """模拟 llm_client.stream_chat，按 fixture 顺序返回预设响应。"""

    def __init__(self, responses: list):
        self._responses = list(responses)
        self._call_index = 0

    async def stream_chat(self, **kwargs) -> AsyncIterator[dict]:
        if self._call_index >= len(self._responses):
            # 如果 LLM 被调用的次数超过预设，返回空回复
            yield {"type": "usage", "input": 0, "output": 0, "total": 0}
            return
        response = self._responses[self._call_index]
        self._call_index += 1
        for event in response.get("events", []):
            yield event


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

    async def compress(self, messages, session_id, config):
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
