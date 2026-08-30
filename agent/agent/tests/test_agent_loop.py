"""回归测试：验证 agent_loop 产出的事件序列与 fixture 预期匹配。

覆盖 5 条核心路径：
1. simple_chat — 纯文本问答
2. tool_call_flow — 工具调用流程
3. auto_mode_flow — auto 模式 phase_plan + 工具执行
4. format_retry_flow — 格式重试
5. structured_continuation — tool_params 确认后 LLM 重出拦截
"""
import asyncio
import json
import os
import sys
from pathlib import Path
from typing import AsyncIterator

import pytest

# conftest.py 已经把 AGENT_DIR 加入 sys.path

from .conftest import (
    MockLLMClient,
    MockMcpBridge,
    MockSkillRegistry,
    MockContextManager,
    load_fixture,
)
from config import ApiConfig, ModelEntry
from session import Session, create_session


def test_base_prompt_path_points_to_agent_prompt():
    import agent_loop

    assert agent_loop._PROMPT_DIR == Path(__file__).resolve().parents[2] / "prompt"
    assert "CFD" in agent_loop._load_base_prompt()


def _make_config():
    return ApiConfig(
        api_type="openai",
        api_url="http://localhost:11434",
        api_key="test-key",
        models=[ModelEntry(provider="test", model_id="test-model")],
        default_model_index=0,
    )


def _make_session(session_id="test-session"):
    import uuid
    sid = session_id or str(uuid.uuid4())
    now = "2026-08-14T10:00:00"
    return Session(id=sid, title="Test", created_at=now, updated_at=now)


async def _run_fixture(fixture_name: str) -> list:
    """加载 fixture 并运行 agent_loop，返回产出的事件列表。"""
    fixture = load_fixture(fixture_name)
    inp = fixture["input"]

    session = _make_session(inp["session_id"])
    config = _make_config()
    mock_llm = MockLLMClient(fixture["mock_llm_responses"])
    mock_mcp = MockMcpBridge(
        fixture.get("mock_mcp_results", {}),
        tool_schemas=fixture.get("mock_tool_schemas", {}),
    )
    mock_skills = MockSkillRegistry()
    mock_ctx = MockContextManager()

    # patch llm_client.stream_chat
    import agent_loop
    original_stream_chat = agent_loop.llm_client.stream_chat
    agent_loop.llm_client.stream_chat = mock_llm.stream_chat

    events = []
    try:
        async for event in agent_loop.run_agent_loop(
            session=session,
            user_message=inp["message"],
            base_system_prompt=inp.get("system_prompt", ""),
            config=config,
            mcp=mock_mcp,
            ctx_mgr=mock_ctx,
            skill_registry=mock_skills,
            selected_skills=inp.get("selected_skills", []),
            request_tool_approval=None,
            attachments=inp.get("attachments"),
            display_content=inp.get("display_content", ""),
            interaction_mode=inp.get("interaction_mode", "manual"),
            model_override=inp.get("model_id"),
        ):
            events.append(event)
            # 防止无限循环
            if len(events) > 100:
                break
    finally:
        agent_loop.llm_client.stream_chat = original_stream_chat

    return events


def _event_types(events: list) -> list:
    """提取事件类型列表（忽略 heartbeat 的具体次数，只保留存在性）。"""
    return [e["type"] for e in events]


def _full_text(events: list) -> str:
    """拼接所有 text_chunk 的 delta。"""
    return "".join(e.get("delta", "") for e in events if e["type"] == "text_chunk")


# ============================================================
# 测试用例
# ============================================================


@pytest.mark.asyncio
async def test_simple_chat():
    """纯文本问答：用户问→LLM答→done"""
    events = await _run_fixture("simple_chat")
    types = _event_types(events)

    # 必须有 heartbeat
    assert "heartbeat" in types, f"missing heartbeat in {types}"
    # 必须有 text_chunk
    assert "text_chunk" in types, f"missing text_chunk in {types}"
    # 必须以 done 结尾
    assert types[-1] == "done", f"expected done, got {types[-1]}"
    # 不能有 tool_call
    assert "tool_call" not in types, f"unexpected tool_call in {types}"
    # 文本内容正确
    text = _full_text(events)
    assert "你好" in text, f"text mismatch: {text}"
    assert "GridStar" in text, f"text mismatch: {text}"


@pytest.mark.asyncio
async def test_tool_call_flow():
    """工具调用流程：用户问→tool_call→tool_result→LLM答→done"""
    events = await _run_fixture("tool_call_flow")
    types = _event_types(events)

    # 必须有 tool_call 和 tool_result
    assert "tool_call" in types, f"missing tool_call in {types}"
    assert "tool_result" in types, f"missing tool_result in {types}"
    # tool_call 在 tool_result 之前
    tc_idx = types.index("tool_call")
    tr_idx = types.index("tool_result")
    assert tc_idx < tr_idx, f"tool_call({tc_idx}) should come before tool_result({tr_idx})"
    # 必须以 done 结尾
    assert types[-1] == "done", f"expected done, got {types[-1]}"
    # 最终文本包含"导入完成"
    text = _full_text(events)
    assert "导入完成" in text, f"text mismatch: {text}"

    # 验证 tool_call 事件内容
    tool_call_event = next(e for e in events if e["type"] == "tool_call")
    assert tool_call_event["name"] == "ImportCAD"
    assert tool_call_event["args"]["file"] == "NACA0012.txt"

    # 验证 tool_result 事件内容
    tool_result_event = next(e for e in events if e["type"] == "tool_result")
    assert "NACA0012" in tool_result_event["result"]


@pytest.mark.asyncio
async def test_auto_mode_flow():
    """auto 模式：phase_plan + 工具执行"""
    events = await _run_fixture("auto_mode_flow")
    types = _event_types(events)

    # 必须有 phase_plan 事件
    assert "phase_plan" in types, f"missing phase_plan in {types}"
    # 必须有 tool_call 和 tool_result
    assert "tool_call" in types, f"missing tool_call in {types}"
    assert "tool_result" in types, f"missing tool_result in {types}"
    # phase_plan 在 tool_call 之前
    pp_idx = types.index("phase_plan")
    tc_idx = types.index("tool_call")
    assert pp_idx < tc_idx, f"phase_plan({pp_idx}) should come before tool_call({tc_idx})"
    # 必须以 done 结尾
    assert types[-1] == "done", f"expected done, got {types[-1]}"


@pytest.mark.asyncio
async def test_format_retry_flow():
    """格式重试：LLM 首次无 JSON→reminder→补 JSON→done"""
    events = await _run_fixture("format_retry_flow")
    types = _event_types(events)

    # 必须有至少两轮 text_chunk（第一次无 JSON，第二次有 JSON）
    text_chunks = [e for e in events if e["type"] == "text_chunk"]
    assert len(text_chunks) >= 2, f"expected at least 2 text_chunks, got {len(text_chunks)}"
    # 第一次文本不含 options
    assert "options" not in text_chunks[0]["delta"], f"first response should not have JSON: {text_chunks[0]}"
    # 最终文本包含 options
    full = _full_text(events)
    assert "options" in full, f"final text should contain options: {full}"
    # 必须以 done 结尾
    assert types[-1] == "done", f"expected done, got {types[-1]}"


@pytest.mark.asyncio
async def test_structured_continuation():
    """tool_params 确认后 LLM 重出 tool_params→拦截重试→工具执行"""
    events = await _run_fixture("structured_continuation")
    types = _event_types(events)

    # 必须有 tool_call（最终 LLM 调了工具）
    assert "tool_call" in types, f"missing tool_call in {types}"
    assert "tool_result" in types, f"missing tool_result in {types}"
    # 必须以 done 结尾
    assert types[-1] == "done", f"expected done, got {types[-1]}"
    # LLM 被调用至少 2 次（第一次重出 tool_params 被拦截，第二次才调工具）
    # 通过 text_chunk 轮数推断：第一轮有 text_chunk，第二轮有 text_chunk + tool_call
    text_chunks = [e for e in events if e["type"] == "text_chunk"]
    assert len(text_chunks) >= 2, f"expected at least 2 rounds of text, got {len(text_chunks)}"
    # 第一次文本包含 tool_params（LLM 重出了）
    assert "tool_params" in text_chunks[0]["delta"], f"first response should have tool_params: {text_chunks[0]}"
    # 最终文本包含"导入完成"
    full = _full_text(events)
    assert "导入完成" in full, f"final text should contain 导入完成: {full}"
