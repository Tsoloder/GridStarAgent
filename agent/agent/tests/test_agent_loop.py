"""回归测试：验证 agent_loop 产出的事件序列与 fixture 预期匹配。"""
import asyncio
from pathlib import Path
from types import SimpleNamespace

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
                  and e["name"] == "GenerateSurMesh"]
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
    assert "GenerateSurMesh" in tools_called
    assert tools_called.count("GenerateSurMesh") == 1
    assert all(call["ok"] for call in ledger.calls)


@pytest.mark.asyncio
async def test_auto_mode_readonly_query_exempt_from_plan_gate(tmp_path, monkeypatch):
    """单步只读查询豁免建计划门禁：直接执行，不暂缓、不建计划、不多烧一次请求。"""
    import json
    import uuid
    import session as session_mod
    from task_ledger import TaskLedger

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    sid = str(uuid.uuid5(uuid.NAMESPACE_URL, "test-auto-003"))
    ledger = TaskLedger(sid)

    events = await _run_fixture("auto_mode_readonly_query_exempt", ledger=ledger)
    types = _event_types(events)

    # 门禁未触发：查询工具首轮就真正执行，没有 [deferred] 占位结果
    query_call = next(e for e in events if e["type"] == "tool_call")
    assert query_call["name"] == "ListMeshFaces"
    query_result = next(e for e in events if e["type"] == "tool_result")
    assert "[deferred]" not in query_result["result"]
    assert "face_1" in query_result["result"]

    # 只读查询不需要阶段计划，也不该为补计划再花一次请求
    assert "plan_updated" not in types
    assert ledger.plan is None
    assert types.count("traj_request_start") == 2
    assert types[-1] == "done"

    # 台账照常记账（无活动阶段时归属为空）
    assert [call["tool"] for call in ledger.calls] == ["ListMeshFaces"]
    assert ledger.calls[0]["phase"] == ""

    # 缓存用量透传到 done 事件并落盘，前端才能显示真实命中率
    assert events[-1]["cache_read_tokens"] == 21504 * 2
    stored = [json.loads(line) for line in
              (tmp_path / sid / "messages.jsonl").read_text(encoding="utf-8").splitlines()]
    usage = next(m["usage"] for m in stored
                 if m.get("role") == "assistant" and m.get("usage"))
    assert usage["cache_read"] == 21504 * 2
    assert usage["model"] == "test-model"


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


@pytest.mark.asyncio
async def test_upstream_error_classification_is_forwarded():
    from types import SimpleNamespace
    import agent_loop

    class RateLimitedRuntime:
        def context_window(self, model_key):
            return 32000

        async def stream(self, model_key, messages, tools=(), system_prompt=""):
            yield SimpleNamespace(type="error", category="rate_limited",
                                  message="Upstream HTTP 429", retryable=True,
                                  status_code=429, retry_after=7.5)

    events = [event async for event in agent_loop._stream_runtime(
        RateLimitedRuntime(), "test/test-model", [], "base", []
    )]

    assert events == [{
        "type": "error", "category": "rate_limited", "message": "Upstream HTTP 429",
        "retryable": True, "status_code": 429, "retry_after": 7.5,
    }]


@pytest.mark.asyncio
async def test_permanent_errors_do_not_advertise_a_retry_window():
    from types import SimpleNamespace
    import agent_loop

    class BadRequestRuntime:
        def context_window(self, model_key):
            return 32000

        async def stream(self, model_key, messages, tools=(), system_prompt=""):
            yield SimpleNamespace(type="error", category="upstream_http",
                                  message="Upstream HTTP 400", retryable=False,
                                  status_code=400, retry_after=None)

    events = [event async for event in agent_loop._stream_runtime(
        BadRequestRuntime(), "test/test-model", [], "base", []
    )]

    assert events[-1]["retryable"] is False
    assert events[-1]["status_code"] == 400
    assert "retry_after" not in events[-1]


@pytest.mark.asyncio
async def test_transport_error_escapes_as_retryable():
    import httpx
    import agent_loop

    class BrokenRuntime:
        def context_window(self, model_key):
            return 32000

        async def stream(self, model_key, messages, tools=(), system_prompt=""):
            raise httpx.ConnectError("connection refused")
            yield  # 让 stream 保持异步生成器，异常在首次迭代时抛出

    events = [event async for event in agent_loop.run_agent_loop(
        _make_session(None), "hi", "base", _make_config(), MockMcpBridge({}),
        MockContextManager(), MockSkillRegistry(), model_runtime=BrokenRuntime(),
    )]

    assert events[-1]["type"] == "error"
    assert events[-1]["retryable"] is True  # 网络抖动不该被说成永久失败


# --------------------------------------------------------------------------
# 工具分组按需暴露（GetToolGroups + enable_tool_group + 直呼自动解锁）
# --------------------------------------------------------------------------

_REPLY = '已完成。\n```json\n{"options": [{"label": "继续", "value": "go"}]}\n```'

_GROUPS = [
    {"id": "query", "description": "只读查询", "tools": ["GetPointCount"]},
    {"id": "project", "description": "工程文件管理", "tools": ["OpenSpdFile"]},
    {"id": "generation", "description": "网格生成", "tools": ["UGSur"]},
]

_MCP_RESULTS = {
    # GetToolGroups 不属于任何分组，必须始终暴露（发现入口）
    "GetToolGroups": '{"groups": [], "default_enabled": ["query", "project"]}',
    "GetPointCount": "point_count=42",
    "OpenSpdFile": "opened",
    "UGSur": "sur mesh generated",
}


def _text(delta):
    return SimpleNamespace(type="text_delta", delta=delta)


def _tool(call_id, name, args):
    return SimpleNamespace(type="tool_call_end", call_id=call_id, name=name,
                           arguments=args, parse_error=None)


def _usage():
    return SimpleNamespace(type="usage", input_tokens=10, output_tokens=5,
                           total_tokens=15)


def _done():
    return SimpleNamespace(type="done", stop_reason="stop")


class _ScriptedRuntime:
    """按脚本逐轮产出事件，并记录每轮实际暴露给模型的工具名单。"""

    def __init__(self, script):
        self._script = list(script)
        self.exposed_tools = []
        self.system_prompts = []
        self.messages = []

    def context_window(self, model_key):
        return 32000

    async def stream(self, model_key, messages, tools=(), system_prompt=""):
        self.exposed_tools.append([getattr(t, "name", "") for t in tools])
        self.system_prompts.append(system_prompt)
        self.messages.append(messages)
        index = len(self.exposed_tools) - 1
        for event in (self._script[index] if index < len(self._script) else []):
            yield event


def _grouped_mcp(mock_mcp):
    return mock_mcp(_MCP_RESULTS, tool_groups=_GROUPS,
                    default_group_ids=["query", "project"])


async def _run_grouped(mcp, runtime, tmp_path, monkeypatch, message="生成面网格"):
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    import agent_loop

    return [event async for event in agent_loop.run_agent_loop(
        _make_session(None), message, "base", _make_config(), mcp,
        MockContextManager(), MockSkillRegistry(), model_runtime=runtime,
    )]


@pytest.mark.asyncio
async def test_tool_groups_default_exposure(tmp_path, monkeypatch, mock_mcp):
    """默认只暴露 query/project 两组 + 发现入口，其他分组的工具不进 schema。"""
    runtime = _ScriptedRuntime([[_text(_REPLY), _usage(), _done()]])
    events = await _run_grouped(_grouped_mcp(mock_mcp), runtime, tmp_path, monkeypatch)

    exposed = runtime.exposed_tools[0]
    assert "GetToolGroups" in exposed          # 不属于任何分组，始终暴露
    assert "enable_tool_group" in exposed      # 内部启用工具
    assert "GetPointCount" in exposed          # query（默认组）
    assert "OpenSpdFile" in exposed            # project（默认组）
    assert "UGSur" not in exposed              # generation 未启用
    assert "read_skill" in exposed             # 技能内部工具不受分组影响
    # system prompt 里给出固定的工具发现说明（内容不随已启用分组变化，保护前缀缓存）
    assert "<tool_discovery>" in runtime.system_prompts[0]
    assert events[-1]["type"] == "done"


@pytest.mark.asyncio
async def test_enable_tool_group_exposes_group_next_turn(tmp_path, monkeypatch, mock_mcp):
    """enable_tool_group 是内置工具，启用后下一轮起该分组工具可直接调用。"""
    runtime = _ScriptedRuntime([
        [_tool("c1", "enable_tool_group", {"group_id": "generation"}), _usage(), _done()],
        [_tool("c2", "UGSur", {}), _usage(), _done()],
        [_text(_REPLY), _usage(), _done()],
    ])
    events = await _run_grouped(_grouped_mcp(mock_mcp), runtime, tmp_path, monkeypatch)

    call = next(e for e in events if e["type"] == "tool_call")
    assert call["name"] == "enable_tool_group"
    assert call.get("internal") is True
    result = next(e for e in events if e["type"] == "tool_result")
    assert "已启用工具分组 generation" in result["result"]
    assert "UGSur" in result["result"]

    # 第一轮不含 UGSur，启用后的每一轮都含（本次消息内持续有效）
    assert "UGSur" not in runtime.exposed_tools[0]
    assert "UGSur" in runtime.exposed_tools[1]
    assert "UGSur" in runtime.exposed_tools[2]
    # 启用后直呼成功执行，没有被判为无效工具名
    ugsur = next(e for e in events if e["type"] == "tool_result" and e["name"] == "UGSur")
    assert ugsur["result"] == "sur mesh generated"
    assert not any(e["type"] == "error" for e in events)


@pytest.mark.asyncio
async def test_enable_unknown_tool_group_points_to_catalog(tmp_path, monkeypatch, mock_mcp):
    """未知 group_id 不报错，返回可选分组并提示先查 GetToolGroups。"""
    runtime = _ScriptedRuntime([
        [_tool("c1", "enable_tool_group", {"group_id": "nope"}), _usage(), _done()],
        [_text(_REPLY), _usage(), _done()],
    ])
    events = await _run_grouped(_grouped_mcp(mock_mcp), runtime, tmp_path, monkeypatch)

    result = next(e for e in events if e["type"] == "tool_result")
    assert "未知分组 id" in result["result"]
    assert "GetToolGroups" in result["result"]
    assert "generation" in result["result"]
    # 未启用的分组仍然不暴露
    assert "UGSur" not in runtime.exposed_tools[-1]


@pytest.mark.asyncio
async def test_direct_call_of_hidden_tool_auto_unlocks_group(tmp_path, monkeypatch, mock_mcp):
    """模型不查目录直接直呼未暴露工具：自动启用其分组并放行执行。"""
    runtime = _ScriptedRuntime([
        [_tool("c1", "UGSur", {}), _usage(), _done()],
        [_text(_REPLY), _usage(), _done()],
    ])
    events = await _run_grouped(_grouped_mcp(mock_mcp), runtime, tmp_path, monkeypatch)

    result = next(e for e in events if e["type"] == "tool_result")
    assert result["name"] == "UGSur"
    assert result["result"] == "sur mesh generated"
    assert not any(e["type"] == "error" for e in events)
    # 没有触发"无效工具名"重选提醒
    assert "invalid_tool_name_reminder" not in str(runtime.messages[-1])
    # 解锁的分组在后续请求里保持暴露
    assert "UGSur" in runtime.exposed_tools[1]


@pytest.mark.asyncio
async def test_no_group_info_falls_back_to_full_exposure(tmp_path, monkeypatch, mock_mcp):
    """旧服务端没有分组信息时全量暴露，且不注入 enable_tool_group / 说明块。"""
    runtime = _ScriptedRuntime([[_text(_REPLY), _usage(), _done()]])
    events = await _run_grouped(mock_mcp(dict(_MCP_RESULTS)), runtime, tmp_path, monkeypatch)

    exposed = runtime.exposed_tools[0]
    assert "UGSur" in exposed
    assert "GetPointCount" in exposed
    assert "enable_tool_group" not in exposed
    assert "<tool_discovery>" not in runtime.system_prompts[0]
    assert events[-1]["type"] == "done"


@pytest.mark.asyncio
async def test_bridge_caches_tool_groups_from_server():
    """bridge 缓存服务端分组目录，并剔除目录里实际未注册的名字。"""
    import json as _json
    from mcp_bridge import McpBridge

    bridge = McpBridge("python server.py")
    tools = [SimpleNamespace(name=n)
             for n in ("GetToolGroups", "GetPointCount", "UGSur")]

    async def fake_call(name, args):
        assert name == "GetToolGroups"
        return _json.dumps({
            "groups": [
                {"id": "query", "description": "只读查询",
                 "tools": ["GetPointCount", "GhostTool"]},
                {"id": "generation", "description": "网格生成", "tools": ["UGSur"]},
            ],
            "default_enabled": ["query", "nope"],
        })

    bridge._call_tool_once = fake_call
    await bridge._load_tool_groups(tools)

    assert [g["id"] for g in bridge.tool_groups()] == ["query", "generation"]
    assert bridge.tool_groups()[0]["tools"] == ["GetPointCount"]  # GhostTool 未注册
    assert bridge.default_group_ids() == ["query"]                # nope 不存在
    assert bridge.group_for_tool("UGSur") == "generation"
    assert bridge.group_for_tool("GetToolGroups") is None


@pytest.mark.asyncio
async def test_bridge_without_group_info_stays_empty():
    """旧服务端没有 GetToolGroups 或拉取失败时，分组信息为空（调用方全量暴露）。"""
    from mcp_bridge import McpBridge

    bridge = McpBridge("python server.py")

    async def boom(name, args):
        raise RuntimeError("server has no GetToolGroups")

    bridge._call_tool_once = boom
    await bridge._load_tool_groups([SimpleNamespace(name="UGSur")])
    assert bridge.tool_groups() == []
    assert bridge.default_group_ids() == []

    await bridge._load_tool_groups([SimpleNamespace(name="GetToolGroups"),
                                    SimpleNamespace(name="UGSur")])
    assert bridge.tool_groups() == []
    assert bridge.group_for_tool("UGSur") is None

