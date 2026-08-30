import asyncio
import json
from pathlib import Path
from types import SimpleNamespace

import pytest

import mcp_bridge
from mcp_bridge import McpBridge, McpOperationStatusUnknown
from workflow_runner import run_workflow


SERVER_PATH = str(Path(__file__).resolve().parents[2] / "server.py")


class ClientFactory:
    def __init__(self, call_results=None, tools=None):
        self.call_results = iter(call_results or [])
        self.tools = tools or []
        self.instances = []
        self.exit_count = 0

    def __call__(self, script):
        client = FakeClient(self, script)
        self.instances.append(client)
        return client


class FakeClient:
    def __init__(self, factory, script):
        self.factory = factory
        self.script = script
        self.call_count = 0
        self.list_count = 0

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, traceback):
        self.factory.exit_count += 1

    async def list_tools(self):
        self.list_count += 1
        return self.factory.tools

    async def call_tool(self, name, args):
        self.call_count += 1
        result = next(self.factory.call_results)
        if isinstance(result, Exception):
            raise result
        return result


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


def test_mcp_connect_caches_tools_and_closes_client(monkeypatch):
    async def run():
        tools = [SimpleNamespace(name="QueryState")]
        factory = ClientFactory(tools=tools)
        monkeypatch.setattr(mcp_bridge, "Client", factory)
        bridge = McpBridge(SERVER_PATH)

        await bridge.connect()

        assert bridge.connected
        assert bridge.available_tools() == tools
        assert len(factory.instances) == 1
        assert factory.instances[0].list_count == 1
        assert factory.exit_count == 1

        await bridge.disconnect()
        assert not bridge.connected

    asyncio.run(run())


def test_mcp_each_successful_call_uses_a_short_lived_client(monkeypatch):
    async def run():
        result = SimpleNamespace(content=[SimpleNamespace(text="ok")])
        factory = ClientFactory(call_results=[result, result])
        monkeypatch.setattr(mcp_bridge, "Client", factory)
        bridge = McpBridge(SERVER_PATH)

        assert await bridge.call_tool("QueryState", {}) == "ok"
        assert await bridge.call_tool("QueryState", {}) == "ok"
        assert len(factory.instances) == 2
        assert [client.call_count for client in factory.instances] == [1, 1]
        assert factory.exit_count == 2

    asyncio.run(run())


def test_mcp_query_failure_retries_once_with_a_new_client(monkeypatch):
    async def run():
        factory = ClientFactory(call_results=[
            RuntimeError("connection lost"),
            SimpleNamespace(content=[SimpleNamespace(text="ok")]),
        ])
        monkeypatch.setattr(mcp_bridge, "Client", factory)
        bridge = McpBridge(SERVER_PATH)

        assert await bridge.call_tool("QueryState", {}) == "ok"
        assert len(factory.instances) == 2
        assert [client.call_count for client in factory.instances] == [1, 1]
        assert factory.exit_count == 2

    asyncio.run(run())


def test_mcp_query_is_not_retried_more_than_once(monkeypatch):
    async def run():
        factory = ClientFactory(call_results=[
            RuntimeError("connection lost"),
            RuntimeError("still unavailable"),
        ])
        monkeypatch.setattr(mcp_bridge, "Client", factory)
        bridge = McpBridge(SERVER_PATH)

        with pytest.raises(RuntimeError, match="still unavailable"):
            await bridge.call_tool("QueryState", {})

        assert len(factory.instances) == 2
        assert factory.exit_count == 2

    asyncio.run(run())


def test_mcp_operation_failure_is_not_retried_and_status_is_unknown(monkeypatch):
    async def run():
        factory = ClientFactory(call_results=[RuntimeError("connection lost")])
        monkeypatch.setattr(mcp_bridge, "Client", factory)
        bridge = McpBridge(SERVER_PATH)

        with pytest.raises(McpOperationStatusUnknown, match="status is unknown"):
            await bridge.call_tool("CreateMesh", {})

        assert len(factory.instances) == 1
        assert factory.instances[0].call_count == 1
        assert factory.exit_count == 1

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
