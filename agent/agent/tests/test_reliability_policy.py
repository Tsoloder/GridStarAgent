import asyncio
import json
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from mcp_bridge import McpBridge, McpOperationStatusUnknown
from workflow_runner import run_workflow


SERVER_PATH = str(Path(__file__).resolve().parents[2] / "server.py")


class WorkflowSession:
    id = "test-session"

    def __init__(self):
        self.runs = []

    def begin_workflow_run(self, run_id, steps):
        self.runs.append((run_id, list(steps), "running", ""))

    def update_workflow_run(self, run_id, steps, status, message):
        self.runs.append((run_id, steps, status, message))


class WorkflowMcp:
    def __init__(self, results):
        self.results = iter(results)
        self.calls = []

    def available_tools(self):
        return [SimpleNamespace(name="First"), SimpleNamespace(name="Second")]

    def tool_schema(self, name):
        return {}

    async def call_tool(self, name, args):
        self.calls.append(name)
        return next(self.results)


def test_mcp_query_failure_reconnects_and_retries(monkeypatch):
    async def run():
        bridge = McpBridge(SERVER_PATH)
        bridge._client = SimpleNamespace(call_tool=AsyncMock(side_effect=[
            RuntimeError("connection lost"),
            SimpleNamespace(content=[SimpleNamespace(text="ok")]),
        ]))
        reconnect = AsyncMock(return_value=True)
        monkeypatch.setattr(bridge, "_reconnect", reconnect)

        assert await bridge.call_tool("QueryState", {}) == "ok"
        reconnect.assert_awaited_once()
        assert bridge._client.call_tool.await_count == 2

    asyncio.run(run())


def test_mcp_operation_failure_is_not_retried_and_status_is_unknown(monkeypatch):
    async def run():
        bridge = McpBridge(SERVER_PATH)
        bridge._client = SimpleNamespace(call_tool=AsyncMock(side_effect=RuntimeError("connection lost")))
        reconnect = AsyncMock(return_value=True)
        monkeypatch.setattr(bridge, "_reconnect", reconnect)

        with pytest.raises(McpOperationStatusUnknown, match="status is unknown"):
            await bridge.call_tool("CreateMesh", {})

        reconnect.assert_not_awaited()
        bridge._client.call_tool.assert_awaited_once()

    asyncio.run(run())


@pytest.mark.parametrize("failure", [
    {"status": "error", "message": "failed"},
    {"result": False, "message": "failed"},
    {"result": "  FaLsE  ", "message": "failed"},
    False,
])
def test_workflow_stops_on_failure_payload(failure):
    async def run():
        session = WorkflowSession()
        mcp = WorkflowMcp([json.dumps(failure), json.dumps({"result": True})])
        events = [event async for event in run_workflow(
            session,
            [{"tool": "First", "params": {}}, {"tool": "Second", "params": {}}],
            mcp,
            request_tool_approval=None,
        )]
        return mcp, events

    mcp, events = asyncio.run(run())
    assert mcp.calls == ["First"]
    steps = [event for event in events if event["type"] == "workflow_step"]
    assert any(step["tool"] == "First" and step["status"] == "failed" for step in steps)
    assert any(step["tool"] == "Second" and step["status"] == "cancelled" for step in steps)
    assert events[-1]["type"] == "workflow_done"
    assert events[-1]["status"] == "failed"
