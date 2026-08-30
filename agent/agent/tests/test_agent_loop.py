"""回归测试：验证 agent_loop 产出的事件序列与 fixture 预期匹配。"""
import asyncio
from pathlib import Path

import pytest

from .conftest import (
    MockLLMClient,
    MockMcpBridge,
    MockSkillRegistry,
    MockContextManager,
    load_fixture,
)
from config import config_from_dict
from session import Session


def test_base_prompt_path_points_to_agent_prompt():
    import agent_loop

    assert agent_loop._PROMPT_DIR == Path(__file__).resolve().parents[2] / "prompt"
    assert "CFD" in agent_loop._load_base_prompt()


def _make_config():
    return config_from_dict({
        "version": 1,
        "default_model": "test/test-model",
        "providers": [{
            "id": "test", "name": "Test", "base_url": "http://localhost:11434",
            "api_key": "test-key", "default_api": "openai-chat",
        }],
        "models": [{"provider": "test", "id": "test-model", "context_window": 32000}],
    })


def _make_session(session_id="test-session"):
    import uuid
    sid = str(uuid.uuid5(uuid.NAMESPACE_URL, session_id)) if session_id else str(uuid.uuid4())
    now = "2026-08-14T10:00:00"
    return Session(id=sid, title="Test", created_at=now, updated_at=now)


def _patch_stream_runtime(monkeypatch, agent_loop, stream_chat):
    async def stream_runtime(runtime, model_key, messages, system_prompt, tools):
        async for event in stream_chat(
            messages=messages, system_prompt=system_prompt, tools=tools
        ):
            yield event

    monkeypatch.setattr(agent_loop, "_stream_runtime", stream_runtime)


async def _run_fixture(fixture_name: str) -> list:
    fixture = load_fixture(fixture_name)
    inp = fixture["input"]
    session = _make_session(inp["session_id"])
    mock_llm = MockLLMClient(fixture["mock_llm_responses"])
    mock_mcp = MockMcpBridge(
        fixture.get("mock_mcp_results", {}),
        tool_schemas=fixture.get("mock_tool_schemas", {}),
    )

    import agent_loop

    events = []
    stream = agent_loop.run_agent_loop(
        session=session,
        user_message=inp["message"],
        base_system_prompt=inp.get("system_prompt", ""),
        config=_make_config(),
        mcp=mock_mcp,
        ctx_mgr=MockContextManager(),
        skill_registry=MockSkillRegistry(),
        selected_skills=inp.get("selected_skills", []),
        request_tool_approval=None,
        attachments=inp.get("attachments"),
        display_content=inp.get("display_content", ""),
        interaction_mode=inp.get("interaction_mode", "manual"),
        model_override=inp.get("model_id"),
        model_runtime=mock_llm,
    )
    async for event in stream:
        events.append(event)
        if len(events) > 100:
            break
    return events


def _event_types(events):
    return [event["type"] for event in events]


def _full_text(events):
    return "".join(event.get("delta", "") for event in events if event["type"] == "text_chunk")


@pytest.mark.asyncio
async def test_simple_chat():
    events = await _run_fixture("simple_chat")
    types = _event_types(events)
    assert "heartbeat" in types
    assert "text_chunk" in types
    assert types[-1] == "done"
    assert "tool_call" not in types
    assert "你好" in _full_text(events)
    assert "GridStar" in _full_text(events)


@pytest.mark.asyncio
async def test_tool_call_flow():
    events = await _run_fixture("tool_call_flow")
    types = _event_types(events)
    assert types.index("tool_call") < types.index("tool_result")
    assert types[-1] == "done"
    assert "导入完成" in _full_text(events)
    tool_call = next(event for event in events if event["type"] == "tool_call")
    assert tool_call["name"] == "ImportCAD"
    assert tool_call["args"]["file"] == "NACA0012.txt"
    tool_result = next(event for event in events if event["type"] == "tool_result")
    assert "NACA0012" in tool_result["result"]


@pytest.mark.asyncio
async def test_auto_mode_flow():
    events = await _run_fixture("auto_mode_flow")
    types = _event_types(events)
    assert types.index("phase_plan") < types.index("tool_call")
    assert "tool_result" in types
    assert types[-1] == "done"


@pytest.mark.asyncio
async def test_format_retry_flow():
    events = await _run_fixture("format_retry_flow")
    chunks = [event for event in events if event["type"] == "text_chunk"]
    assert len(chunks) >= 2
    assert "options" not in chunks[0]["delta"]
    assert "options" in _full_text(events)
    assert events[-1]["type"] == "done"


@pytest.mark.asyncio
async def test_structured_continuation():
    events = await _run_fixture("structured_continuation")
    types = _event_types(events)
    assert "tool_call" in types
    assert "tool_result" in types
    assert types[-1] == "done"
    chunks = [event for event in events if event["type"] == "text_chunk"]
    assert len(chunks) >= 2
    assert "tool_params" in chunks[0]["delta"]
    assert "导入完成" in _full_text(events)


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("arguments", "parse_error"),
    [(None, "invalid JSON"), (["not", "object"], None), ("text", None)],
)
async def test_invalid_tool_arguments_are_not_executed(arguments, parse_error):
    from types import SimpleNamespace
    import agent_loop

    class InvalidRuntime:
        def context_window(self, model_key):
            return 32000

        async def stream(self, model_key, messages, tools=(), system_prompt=""):
            yield SimpleNamespace(
                type="tool_call_end", call_id="bad-1", name="ImportCAD",
                arguments=arguments, parse_error=parse_error,
            )

    class TrackingMcp(MockMcpBridge):
        def __init__(self):
            super().__init__({"ImportCAD": "unexpected"})
            self.calls = []

        async def call_tool(self, name, args):
            self.calls.append((name, args))
            return "unexpected"

    mcp = TrackingMcp()
    events = [event async for event in agent_loop.run_agent_loop(
        _make_session(None), "import", "base", _make_config(), mcp,
        MockContextManager(), MockSkillRegistry(), model_runtime=InvalidRuntime(),
    )]
    assert mcp.calls == []
    assert not any(event["type"] == "tool_call" for event in events)
    assert events[-1]["type"] == "error"
    assert events[-1]["retryable"] is False
