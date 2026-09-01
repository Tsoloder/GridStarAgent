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


async def _run_fixture(fixture_name: str, ledger=None) -> list:
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
        ledger=ledger,
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
async def test_auto_mode_flow(tmp_path, monkeypatch):
    import uuid
    import session as session_mod
    from task_ledger import TaskLedger

    # 隔离会话数据目录，避免污染真实台账/消息文件
    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    sid = str(uuid.uuid5(uuid.NAMESPACE_URL, "test-auto-001"))
    ledger = TaskLedger(sid)

    events = await _run_fixture("auto_mode_flow", ledger=ledger)
    types = _event_types(events)

    # update_plan 是首个执行的工具（内置、不走 MCP/审批）
    first_tool = next(e for e in events if e["type"] == "tool_call")
    assert first_tool["name"] == "update_plan"
    assert first_tool.get("internal") is True
    # plan_updated 在 update_plan 的 tool_result 前发出，且早于外部工具执行
    plan_events = [e for e in events if e["type"] == "plan_updated"]
    assert len(plan_events) == 2
    first_update_idx = types.index("plan_updated")
    assert types.index("tool_call") < first_update_idx
    assert plan_events[0]["plan"]["id"] == "mesh-main"
    assert plan_events[0]["plan"]["phases"][0]["status"] == "in_progress"
    # 全量替换：第二次更新后所有阶段完成
    assert all(p["status"] == "done" for p in plan_events[1]["plan"]["phases"])
    assert plan_events[1]["plan"]["phases"][0]["note"] == "模型树获取完成"

    assert "tool_result" in types
    assert types[-1] == "done"
    assert "模型树获取完成" in _full_text(events)

    # 台账自动记账：update_plan 与外部工具均入账且全部成功
    tools_called = [call["tool"] for call in ledger.calls]
    assert "update_plan" in tools_called
    assert "GetModelTree" in tools_called
    assert all(call["ok"] for call in ledger.calls)
    # 外部调用归属活动阶段
    get_tree = next(c for c in ledger.calls if c["tool"] == "GetModelTree")
    assert get_tree["phase"] == "CAD导入"


@pytest.mark.asyncio
async def test_auto_mode_defer_internal_tools(tmp_path, monkeypatch):
    """门禁暂缓执行类工具时，同批次的内部工具（read_skill_resource）应放行执行。"""
    import uuid
    import session as session_mod
    from task_ledger import TaskLedger

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    sid = str(uuid.uuid5(uuid.NAMESPACE_URL, "test-auto-002"))
    ledger = TaskLedger(sid)

    events = await _run_fixture("auto_mode_defer_internal", ledger=ledger)
    types = _event_types(events)
    assert types == load_fixture("auto_mode_defer_internal")["expected_event_types"]

    # 第一轮：内部工具放行并正常执行
    first_tool = next(e for e in events if e["type"] == "tool_call")
    assert first_tool["name"] == "read_skill_resource"
    assert first_tool.get("internal") is True

    # 执行类工具全程只出现一次（首轮被暂缓，第二轮建计划后才真正执行）
    exec_calls = [i for i, e in enumerate(events)
                  if e["type"] == "tool_call"
                  and e["name"] == "GetGenerateSurMeshDefaultParam"]
    assert len(exec_calls) == 1
    assert types.index("plan_updated") < exec_calls[0]

    # 计划事件正常发出
    plan_events = [e for e in events if e["type"] == "plan_updated"]
    assert len(plan_events) == 1
    assert plan_events[0]["plan"]["id"] == "surmesh-main"

    assert types[-1] == "done"

    # 台账：内部工具与被暂缓后重发的执行类工具均入账，被暂缓的首次调用不入账
    tools_called = [call["tool"] for call in ledger.calls]
    assert "read_skill_resource" in tools_called
    assert "GetGenerateSurMeshDefaultParam" in tools_called
    assert tools_called.count("GetGenerateSurMeshDefaultParam") == 1
    assert all(call["ok"] for call in ledger.calls)


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
