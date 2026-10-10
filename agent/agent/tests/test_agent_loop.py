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


def test_screenshot_gate_follows_capabilities_vision():
    """截图能不能送达模型，按配置里的 capabilities.vision 判定。"""
    import agent_loop

    config = config_from_dict({
        "version": 1,
        "default_model": "test/text-only",
        "providers": [{
            "id": "test", "name": "Test", "base_url": "http://localhost:11434",
            "api_key": "test-key", "default_api": "openai-chat",
        }],
        "models": [
            {"provider": "test", "id": "text-only", "context_window": 32000,
             "capabilities": {"vision": False}},
            {"provider": "test", "id": "sees-images", "context_window": 32000,
             "capabilities": {"vision": True}},
        ],
    })
    assert agent_loop._model_sees_images(config, "test/text-only") is False
    assert agent_loop._model_sees_images(config, "test/sees-images") is True
    # 配置里查不到这个模型时按「能看图」处理：宁可白发一张图，也不要谎报看不到
    assert agent_loop._model_sees_images(config, "test/absent") is True


def test_screenshot_note_tells_the_truth_about_vision():
    import agent_loop

    delivered = agent_loop._screenshot_note("test/sees-images", r"C:\uploads\a.png", True)
    assert "图片形式发送" in delivered

    blocked = agent_loop._screenshot_note("test/text-only", r"C:\uploads\a.png", False)
    assert "不会送达模型" in blocked
    assert "a.png" in blocked and "test/text-only" in blocked
    assert "不要重复截图" in blocked


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


def _text_events(text: str) -> list:
    return [
        SimpleNamespace(type="text_delta", delta=text),
        SimpleNamespace(type="usage", input_tokens=100, output_tokens=20, total_tokens=120,
                        cache_read_tokens=0, cache_write_tokens=0, reasoning_tokens=0),
    ]


def _tool_call_events(name: str, args: dict) -> list:
    return [
        SimpleNamespace(type="tool_call_end", call_id="call-1", name=name,
                        arguments=args, parse_error=None),
        SimpleNamespace(type="usage", input_tokens=100, output_tokens=20, total_tokens=120,
                        cache_read_tokens=0, cache_write_tokens=0, reasoning_tokens=0),
    ]


async def _run_direct(runtime, message, ledger=None, mcp=None, display_content="",
                      interaction_mode="manual"):
    import agent_loop

    return [event async for event in agent_loop.run_agent_loop(
        _make_session(None), message, "base", _make_config(), mcp or MockMcpBridge({}),
        MockContextManager(), MockSkillRegistry(), model_runtime=runtime, ledger=ledger,
        display_content=display_content, interaction_mode=interaction_mode,
    )]


@pytest.mark.asyncio
async def test_conversational_turn_skips_format_retry(tmp_path, monkeypatch):
    """寒暄轮没有决策点：不该为凑结构化输出再发一次请求（省一整轮前缀输入）。"""
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    runtime = _ScriptedRuntime([_text_events("好的，随时欢迎再来咨询。")])
    events = await _run_direct(runtime, "下次再说")
    assert len(runtime.exposed_tools) == 1
    assert _event_types(events)[-1] == "done"
    assert "随时欢迎" in _full_text(events)


@pytest.mark.asyncio
async def test_task_reply_without_structure_still_retries(tmp_path, monkeypatch):
    """真实请求仍受格式契约约束：首答复缺结构化块时照旧重试一次。"""
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    runtime = _ScriptedRuntime([
        _text_events("已为你加载当前模型。"),
        _text_events("```json\n{\"options\": [{\"label\": \"继续\", \"value\": \"go\"}]}\n```"),
    ])
    events = await _run_direct(runtime, "帮我查看当前模型")
    assert len(runtime.exposed_tools) == 2
    assert "options" in _full_text(events)


@pytest.mark.asyncio
async def test_tool_execution_blocks_format_exemption(tmp_path, monkeypatch):
    """本轮执行过工具就仍有决策点：寒暄豁免不生效，缺结构化块照旧重试。"""
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    runtime = _ScriptedRuntime([
        _tool_call_events("ImportCAD", {"file": "wing.step"}),
        _text_events("导入完成，共 128 个数模面。"),
        _text_events("```json\n{\"options\": [{\"label\": \"继续\", \"value\": \"go\"}]}\n```"),
    ])
    events = await _run_direct(runtime, "谢谢", mcp=MockMcpBridge({"ImportCAD": "ok"}))
    assert len(runtime.exposed_tools) == 3
    assert any(event["type"] == "tool_result" for event in events)


@pytest.mark.asyncio
async def test_active_plan_blocks_format_exemption(tmp_path, monkeypatch):
    """台账里还有未收尾阶段时，寒暄轮也要给结构化出口。"""
    import session as session_mod

    import uuid

    from task_ledger import TaskLedger

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    ledger = TaskLedger(str(uuid.uuid4()))
    ledger.update_plan("cad-mesh", "CAD 到 CFD 网格生成",
                       [{"id": "a", "title": "阶段A", "status": "in_progress"}])
    runtime = _ScriptedRuntime([
        _text_events("好的。"),
        _text_events("```json\n{\"options\": [{\"label\": \"继续\", \"value\": \"go\"}]}\n```"),
    ])
    events = await _run_direct(runtime, "谢谢", ledger=ledger)
    assert len(runtime.exposed_tools) == 2
    assert _event_types(events)[-1] == "done"


@pytest.mark.asyncio
async def test_card_answer_blocks_format_exemption(tmp_path, monkeypatch):
    """卡片作答即使文本落在白名单里也不能豁免：那是一次决策答复。

    前端把选项的 value 当消息、label 当 display_content 发过来，所以
    display_content 非空即"这是卡片作答"，必须继续给结构化出口。
    """
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    runtime = _ScriptedRuntime([
        _text_events("好的。"),
        _text_events("```json\n{\"options\": [{\"label\": \"继续\", \"value\": \"go\"}]}\n```"),
    ])
    events = await _run_direct(runtime, "谢谢", display_content="谢谢你")
    assert len(runtime.exposed_tools) == 2
    assert _event_types(events)[-1] == "done"


def test_conversational_message_matching():
    """豁免判定必须窄：真实请求、应答词、稍长的寒暄扩展句都不能命中。"""
    import agent_loop

    assert agent_loop._is_conversational_message("你好")
    assert agent_loop._is_conversational_message("  谢谢！ ")
    assert agent_loop._is_conversational_message("下次再说。")
    assert not agent_loop._is_conversational_message("导入模型")
    assert not agent_loop._is_conversational_message("你好，帮我导入模型")
    assert not agent_loop._is_conversational_message("")
    assert not agent_loop._is_conversational_message("你好" * 10)
    # 应答词可能是放行确认，不进白名单
    for ack in ("好的", "好", "收到", "OK", "明白了"):
        assert not agent_loop._is_conversational_message(ack)


def test_plan_in_flight_detection():
    import agent_loop

    assert not agent_loop._plan_in_flight(None)
    assert not agent_loop._plan_in_flight(SimpleNamespace(plan=None))
    assert not agent_loop._plan_in_flight(SimpleNamespace(
        plan={"phases": [{"status": "done"}, {"status": "skipped"}]}))
    assert agent_loop._plan_in_flight(SimpleNamespace(
        plan={"phases": [{"status": "in_progress"}]}))
    # 缺省 status 视为 pending，同样算在途
    assert agent_loop._plan_in_flight(SimpleNamespace(plan={"phases": [{"id": "x"}]}))


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


def test_parse_structured_interaction_confirmed_flag():
    """取消与确认必须解析成不同状态；旧 XML 形态无 confirmed 字段时按已确认处理。"""
    import agent_loop

    confirmed = agent_loop.parse_structured_interaction(
        '<structured_interaction>{"type":"tool_params_confirmed","tool":"ImportCAD",'
        '"confirmed":true,"params":{"file":"wing.txt"}}</structured_interaction>'
    )
    assert confirmed["confirmed"] is True
    assert confirmed["cancelled"] is False
    assert confirmed["tool"] == "ImportCAD"

    cancelled = agent_loop.parse_structured_interaction(
        '<structured_interaction>{"type":"tool_params_confirmed","tool":"ImportCAD",'
        '"confirmed":false,"params":{"file":"wing.txt"}}</structured_interaction>'
    )
    assert cancelled["confirmed"] is False
    assert cancelled["cancelled"] is True

    legacy = agent_loop.parse_structured_interaction(
        '<structured_interaction><tool_params_confirmed tool="ImportCAD">'
        '<params>{"file":"wing.txt"}</params></tool_params_confirmed></structured_interaction>'
    )
    assert legacy["cancelled"] is False
    assert legacy["tool"] == "ImportCAD"

    assert agent_loop.parse_structured_interaction("导入模型并生成网格") == {}


@pytest.mark.asyncio
async def test_tool_params_cancelled_blocks_tool_execution(tmp_path, monkeypatch):
    """用户点「取消」后模型仍调用同一工具：必须被拦截，不能落到 MCP 执行。"""
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    events = await _run_fixture("tool_params_cancelled")
    types = _event_types(events)
    assert types == load_fixture("tool_params_cancelled")["expected_event_types"]

    tool_result = next(event for event in events if event["type"] == "tool_result")
    assert "cancelled by user" in tool_result["result"]
    assert "成功导入" not in tool_result["result"]
    assert types[-1] == "done"


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


@pytest.mark.asyncio
async def test_cancel_persists_partial_reply_as_interrupted(tmp_path, monkeypatch):
    """停止本轮对话：已生成的部分文本要落盘并标 interrupted，否则刷新后凭空消失。

    CancelledError 继承自 BaseException，agent_loop 里原有的 except Exception 接不到，
    必须单独兜底，否则 text_acc 随协程栈一起丢掉、轨迹也停在 running。
    """
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    import agent_loop

    reached = asyncio.Event()

    async def stream_runtime(runtime, model_key, messages, system_prompt, tools):
        yield {"type": "text_chunk", "delta": "前一半"}
        reached.set()
        await asyncio.Event().wait()   # 模型还在生成，此时用户点了停止
        yield {"type": "text_chunk", "delta": "后一半"}

    monkeypatch.setattr(agent_loop, "_stream_runtime", stream_runtime)

    session = _make_session("cancel-mid-stream")
    stream = agent_loop.run_agent_loop(
        session=session, user_message="你好", base_system_prompt="base",
        config=_make_config(), mcp=MockMcpBridge({}), ctx_mgr=MockContextManager(),
        skill_registry=MockSkillRegistry(), model_runtime=MockLLMClient([]),
    )

    async def consume():
        async for _ in stream:
            pass

    task = asyncio.create_task(consume())
    await reached.wait()
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task

    assistants = [m for m in session.messages if m["role"] == "assistant"]
    assert [m["content"] for m in assistants] == ["前一半"]
    assert assistants[0].get("interrupted") is True

    ends = [e for e in session.read_trajectory() if e["type"] == "traj_request_end"]
    assert [e["status"] for e in ends] == ["interrupted"]
    assert ends[0]["content"] == "前一半"


@pytest.mark.asyncio
async def test_cancel_before_first_token_writes_no_empty_reply(tmp_path, monkeypatch):
    """一个字都没生成就停止：不留空气泡，轨迹仍要收尾。"""
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    import agent_loop

    reached = asyncio.Event()

    async def stream_runtime(runtime, model_key, messages, system_prompt, tools):
        reached.set()
        await asyncio.Event().wait()
        yield {"type": "text_chunk", "delta": "never"}

    monkeypatch.setattr(agent_loop, "_stream_runtime", stream_runtime)

    session = _make_session("cancel-before-token")
    stream = agent_loop.run_agent_loop(
        session=session, user_message="你好", base_system_prompt="base",
        config=_make_config(), mcp=MockMcpBridge({}), ctx_mgr=MockContextManager(),
        skill_registry=MockSkillRegistry(), model_runtime=MockLLMClient([]),
    )

    async def consume():
        async for _ in stream:
            pass

    task = asyncio.create_task(consume())
    await reached.wait()
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task

    assert [m for m in session.messages if m["role"] == "assistant"] == []
    ends = [e for e in session.read_trajectory() if e["type"] == "traj_request_end"]
    assert [e["status"] for e in ends] == ["interrupted"]


def test_ask_user_tool_contract():
    """询问工具与 update_plan 同类：校验参数、限制选项数量、非法输入回错误而不抛异常。"""
    import agent_loop

    assert agent_loop.ASK_USER_TOOL_NAME == "ask_user_question"
    assert agent_loop.ASK_USER_TOOL_NAME in agent_loop._NON_EXEC_TOOLS

    payload, error = agent_loop.normalize_ask_user_args({
        "title": "下一步",
        "question": "接下来做什么？",
        "options": [
            {"label": "跑测试", "value": "run_tests", "description": "执行 WebUI 测试"},
            {"label": "提交改动", "value": "commit", "style": "primary"},
        ],
    })
    assert error is None
    assert payload["title"] == "下一步"
    assert payload["question"] == "接下来做什么？"
    assert payload["options"][0] == {"label": "跑测试", "description": "执行 WebUI 测试",
                                     "value": "run_tests", "style": ""}
    assert payload["options"][1]["style"] == "primary"

    # 标题缺省时回落默认值，选项超过上限按 6 项截断
    payload, error = agent_loop.normalize_ask_user_args({
        "question": "选一个",
        "options": [{"label": "选项%d" % index, "value": "v%d" % index} for index in range(8)],
    })
    assert error is None
    assert payload["title"] == "请选择下一步"
    assert len(payload["options"]) == 6

    # 非法参数只回错误文案，交给模型自纠
    for bad in (None, {},
                {"question": "选一个", "options": []},
                {"question": "选一个", "options": [{"label": "只有一个", "value": "a"}]},
                {"question": "选一个", "options": [{"label": "a", "value": "a"}, {"value": "b"}]},
                {"options": [{"label": "a", "value": "a"}, {"label": "b", "value": "b"}]}):
        payload, error = agent_loop.normalize_ask_user_args(bad)
        assert payload is None and error


def test_base_prompt_puts_ask_user_tool_first():
    """主提示词必须把询问工具立为主路径，正文 options 只作降级。

    这两条是"少花一半输入"的前提：工具优先才能让格式重试保持沉默。
    """
    import agent_loop

    prompt = agent_loop._load_base_prompt()
    assert prompt.count("ask_user_question") >= 3
    assert "优先调用 `ask_user_question`" in prompt
    assert "降级路径" in prompt
    # 降级的两个正当用途必须在文中写清，否则模型会随意退回正文 JSON
    assert "tool_params" in prompt and "工具不可用" in prompt


@pytest.mark.asyncio
async def test_injected_format_reminder_requires_tool_first(tmp_path, monkeypatch):
    """每轮注入的格式提醒必须与主提示词同口径：先工具，失败才兜底。"""
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    runtime = _ScriptedRuntime([_text_events("请确认下一步。")])
    await _run_direct(runtime, "帮我查看当前模型")
    system_prompt = runtime.system_prompts[0]
    assert "必须调用 ask_user_question 工具" in system_prompt
    assert "调用失败或被拒绝" in system_prompt
    # 旧的"未使用该工具时…兜底"措辞会把降级说成常规路径，必须消失
    assert "未使用该工具时" not in system_prompt


@pytest.mark.asyncio
async def test_ask_user_tool_ends_turn_with_single_request(tmp_path, monkeypatch):
    """模型走工具路径询问时：一次请求收尾，产出 options_offered 供前端渲染卡片。"""
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    runtime = _ScriptedRuntime([_tool_call_events("ask_user_question", {
        "question": "后缘面处理已完成，接下来做什么？",
        "title": "下一步",
        "options": [
            {"label": "继续体网格块创建", "value": "continue_volume"},
            {"label": "导出当前网格", "value": "export", "style": "primary"},
        ],
    })])
    events = await _run_direct(runtime, "继续")
    types = _event_types(events)
    assert len(runtime.exposed_tools) == 1          # 工具路径不该再多发一次请求
    assert "options_offered" in types
    assert types[-1] == "done"
    offered = next(event for event in events if event["type"] == "options_offered")
    assert offered["question"] == "后缘面处理已完成，接下来做什么？"
    assert [item["label"] for item in offered["options"]] == ["继续体网格块创建", "导出当前网格"]
    tool_result = next(event for event in events if event["type"] == "tool_result")
    assert "已向用户发出选择" in tool_result["result"]


@pytest.mark.asyncio
async def test_ask_user_invalid_args_reach_the_model(tmp_path, monkeypatch):
    """参数非法时错误文案必须回到模型手里，不能被一次 MCP 调用结果覆盖。

    回归守卫：询问工具是内置工具，失败分支此前会继续落进 MCP 执行链，
    模型拿到的是 "Mock result / 未知工具" 而不是"options 至少需要 2 项"。
    """
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    runtime = _ScriptedRuntime([
        _tool_call_events("ask_user_question", {
            "question": "选一个",
            "options": [{"label": "只有一个", "value": "a"}],
        }),
        _text_events("```json\n{\"options\": [{\"label\": \"重试\", \"value\": \"retry\"}]}\n```"),
    ])
    events = await _run_direct(runtime, "继续", mcp=MockMcpBridge({}))
    tool_result = next(event for event in events if event["type"] == "tool_result")
    assert "options 至少需要 2 项" in tool_result["result"]
    assert "Mock result" not in tool_result["result"]
    assert "options_offered" not in _event_types(events)   # 没发出卡片，模型可以自纠
    assert _event_types(events)[-1] == "done"
    assert len(runtime.exposed_tools) == 2                 # 模型有机会重来一次


@pytest.mark.asyncio
async def test_auto_mode_blocks_ask_user_while_plan_open(tmp_path, monkeypatch):
    """auto 模式计划未收尾时不得停下问用户：工具必须被拒绝而不是挂起等待。"""
    import session as session_mod

    import uuid

    from task_ledger import TaskLedger

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    ledger = TaskLedger(str(uuid.uuid4()))
    ledger.update_plan("cad-mesh", "CAD 到 CFD 网格生成",
                       [{"id": "a", "title": "阶段A", "status": "in_progress"}])
    runtime = _ScriptedRuntime([_tool_call_events("ask_user_question", {
        "question": "继续吗？",
        "options": [{"label": "继续", "value": "go"}, {"label": "停止", "value": "stop"}],
    })])
    events = await _run_direct(runtime, "继续", ledger=ledger, interaction_mode="auto")
    refused = next(event for event in events if event["type"] == "tool_result")
    assert "ask_user_question 不可用" in refused["result"]
    assert "options_offered" not in _event_types(events)


@pytest.mark.asyncio
async def test_set_active_category_notifies_the_frontend(tmp_path, monkeypatch):
    """模型改了会话分类必须推 session_meta：否则 WebUI 的下拉框停在旧值。

    回归守卫两层：
    1. set_active_category 必须推 session_meta，否则界面读自己的 state 永不更新，
       下一轮请求还会带着旧分类把服务端刚设好的值覆盖回去；
    2. 分类只落在 Session 上。meta.json 的唯一写者是 save_session，而它每轮都用
       自己那份 meta 全量覆盖文件；agent_loop 另写一遍非但多余，还会在同一个回合
       里被抹掉。
    """
    import json

    import agent_loop
    import paths as paths_mod
    import session as session_mod

    monkeypatch.setattr(paths_mod, "SESSIONS_DIR", tmp_path)
    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)

    async def run(category, tag):
        session = _make_session(tag)
        (tmp_path / session.id).mkdir(parents=True, exist_ok=True)
        runtime = _ScriptedRuntime([
            _tool_call_events("set_active_category", {"category": category}),
            _text_events("好。"),
        ])
        return session, [event async for event in agent_loop.run_agent_loop(
            session, "这是导弹", "base", _make_config(), MockMcpBridge({}),
            MockContextManager(), MockSkillRegistry(), model_runtime=runtime,
            interaction_mode="manual",
        )]

    session, events = await run("missile", "category-meta")
    kinds = _event_types(events)
    assert "session_meta" in kinds
    # 事件要排在对应的工具结果之后，前端拿到时状态已经改好了
    assert kinds.index("session_meta") > kinds.index("tool_result")
    assert next(e for e in events if e["type"] == "session_meta")["category"] == "missile"
    # agent_loop 只改内存里的 Session，落盘交给唯一的写者 save_session
    # （app.py 在 tool_result 事件处调用它，写的是同一个 Session 对象）
    assert session.category == "missile"
    session_mod.save_session(session)
    saved = json.loads((tmp_path / session.id / "meta.json").read_text(encoding="utf-8"))
    assert saved["category"] == "missile"
    # 刷新页面/切会话时 GET /sessions/{id} 从 meta.json 恢复，必须能读回来
    assert session_mod.load_session(session.id).category == "missile"

    # 非法分类只把错误文案交给模型，不能推事件、也不能改会话分类
    invalid, refused = await run("submarine", "category-invalid")
    assert "session_meta" not in _event_types(refused)
    assert invalid.category == ""
    assert not (tmp_path / invalid.id / "meta.json").exists()

