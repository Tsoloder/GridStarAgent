from copy import deepcopy

from fastapi.testclient import TestClient

import app as server
from config import API_KEY_MASK, config_from_dict, config_revision


def _strip_seq(event):
    """去掉后台事件里重连回放用的内部序号 _seq，便于按业务字段断言。"""
    return {k: v for k, v in event.items() if k != "_seq"}


def config_data():
    return {
        "version": 1,
        "default_model": "alpha/model-b",
        "providers": [{
            "id": "alpha",
            "name": "Alpha",
            "base_url": "https://alpha.test/v1",
            "api_key": "secret-value",
            "headers": {},
            "discover_models": True,
            "discovery_api": "openai",
            "default_api": "openai-chat",
            "enabled": True,
        }],
        "models": [{
            "id": "model-b",
            "provider": "alpha",
            "name": "Model B",
            "capabilities": {"tools": True},
        }],
    }


def install_config(monkeypatch):
    config = config_from_dict(config_data())
    catalog, runtime = server._build_runtime(config)
    monkeypatch.setattr(server, "current_config", config)
    monkeypatch.setattr(server, "_model_catalog", catalog)
    monkeypatch.setattr(server, "_model_runtime", runtime)
    monkeypatch.setattr(server, "_config_lock", None)
    return config, runtime


def test_get_config_is_redacted_and_post_switches_atomically(monkeypatch):
    config, old_runtime = install_config(monkeypatch)
    saved = []
    closed = []
    monkeypatch.setattr(server, "save_config", lambda candidate: saved.append(candidate))

    async def close(runtime):
        closed.append(runtime)

    monkeypatch.setattr(server, "_close_runtime", close)
    client = TestClient(server.app)

    response = client.get("/config")
    assert response.status_code == 200
    assert response.json()["revision"] == config_revision(config)
    assert response.json()["config"]["providers"][0]["api_key"] == API_KEY_MASK
    assert "secret-value" not in response.text

    candidate = response.json()["config"]
    candidate["models"][0]["name"] = "Changed"
    updated = client.post("/config", json={"revision": response.json()["revision"], "config": candidate})
    assert updated.status_code == 200
    assert server.current_config.models[0].name == "Changed"
    assert server.current_config.providers[0].api_key == "secret-value"
    assert saved == [server.current_config]
    assert closed == [old_runtime]


def test_revision_conflict_and_invalid_config_do_not_switch(monkeypatch):
    config, runtime = install_config(monkeypatch)
    monkeypatch.setattr(server, "save_config", lambda candidate: None)
    client = TestClient(server.app)

    conflict = client.post("/config", json={"revision": "stale", "config": config_data()})
    assert conflict.status_code == 409
    assert conflict.json()["revision"] == config_revision(config)

    invalid = deepcopy(config_data())
    invalid["default_model"] = "alpha/missing"
    response = client.post("/config", json={"revision": config_revision(config), "config": invalid})
    assert response.status_code == 400
    assert server.current_config is config
    assert server._model_runtime is runtime


def test_provider_test_and_model_read_are_independent_sorted_and_closed(monkeypatch):
    install_config(monkeypatch)
    calls = []
    clients = []
    provider_keys = []

    class Response:
        status_code = 204

        def raise_for_status(self):
            return None

    class HttpClient:
        async def get(self, path):
            calls.append(("test", path))
            return Response()

    class ProviderClient:
        def __init__(self):
            self.closed = False
            clients.append(self)

        def client(self):
            return HttpClient()

        async def discover_models(self):
            calls.append(("models", "/models"))
            return [{"id": "z"}, {"id": "A", "name": "Alpha"}, {"id": "z", "name": "duplicate"}]

        async def aclose(self):
            self.closed = True

    def provider_client(provider):
        provider_keys.append(provider.api_key)
        return ProviderClient()

    monkeypatch.setattr(server, "_provider_client", provider_client)
    client = TestClient(server.app)
    provider = config_data()["providers"][0]
    provider["api_key"] = API_KEY_MASK

    tested = client.post("/config/providers/test", json={"provider": provider})
    assert tested.status_code == 200
    assert calls == [("test", "/models")]

    calls.clear()
    models = client.post("/config/providers/models", json={"provider": provider})
    assert models.status_code == 200
    assert [item["id"] for item in models.json()["models"]] == ["A", "z"]
    assert calls == [("models", "/models")]
    assert provider_keys == ["secret-value", "secret-value"]
    assert all(item.closed for item in clients)


def test_masked_key_requires_matching_saved_provider(monkeypatch):
    install_config(monkeypatch)
    provider = config_data()["providers"][0]
    provider.update({"id": "new-provider", "api_key": API_KEY_MASK})

    response = TestClient(server.app).post("/config/providers/test", json={"provider": provider})

    assert response.status_code == 400
    assert response.json()["error"] == "masked API key has no saved credential"


def test_catalog_endpoints_and_health(monkeypatch):
    config, _ = install_config(monkeypatch)

    async def discover(provider):
        return [{"id": "model-b", "created": 10}, {"id": "model-a"}]

    server._model_catalog = server.DiscoveryCatalog(config, {"openai": discover})
    client = TestClient(server.app)
    refreshed = client.post("/config/models/refresh")

    assert refreshed.status_code == 200
    body = refreshed.json()
    assert body["default_model"] == "alpha/model-b"
    assert {item["key"] for item in body["models"]} == {"alpha/model-a", "alpha/model-b"}
    configured = next(item for item in body["models"] if item["key"] == "alpha/model-b")
    assert configured["api"] == "openai-chat"
    # 前端模型下拉分组标题用供应商名称展示，目录必须带上它
    assert configured["provider"] == "alpha"
    assert configured["provider_name"] == "Alpha"
    assert configured["capabilities"]["tools"] is True
    assert configured["status"] == "configured_and_discovered"

    health = client.get("/health").json()
    assert health["runtime_ready"] is True
    assert health["catalog_generation"] == 1
    assert health["catalog_models"] == 2


def test_mcp_tools_endpoint_returns_sorted_cached_catalog(monkeypatch):
    from types import SimpleNamespace

    class FakeMcp:
        connected = True

        def __init__(self):
            self.refreshed = 0
            self._tools = [
                SimpleNamespace(name="zeta_tool", description="Zeta 工具",
                                inputSchema={"type": "object",
                                             "properties": {"b": {"type": "string", "description": "参数 B"}},
                                             "required": ["b"]}),
                SimpleNamespace(name="alpha_tool", description="", inputSchema={}),
            ]

        async def list_tools(self):
            self.refreshed += 1
            return self._tools

        def available_tools(self):
            return self._tools

        def tool_schema(self, name):
            for tool in self._tools:
                if tool.name == name:
                    return tool.inputSchema
            return {}

    fake = FakeMcp()
    monkeypatch.setattr(server, "_mcp", fake)
    client = TestClient(server.app)

    cached = client.get("/mcp/tools")
    assert cached.status_code == 200
    body = cached.json()
    assert body["connected"] is True
    assert body["error"] == ""
    # 工具按名称排序，默认只读缓存不触发 list_tools
    assert [item["name"] for item in body["tools"]] == ["alpha_tool", "zeta_tool"]
    assert fake.refreshed == 0
    zeta = next(item for item in body["tools"] if item["name"] == "zeta_tool")
    assert zeta["description"] == "Zeta 工具"
    assert zeta["input_schema"]["required"] == ["b"]

    refreshed = client.get("/mcp/tools?refresh=1")
    assert refreshed.status_code == 200
    assert fake.refreshed == 1


def test_mcp_tools_endpoint_reports_unavailable_when_bridge_missing(monkeypatch):
    monkeypatch.setattr(server, "_mcp", None)
    client = TestClient(server.app)

    response = client.get("/mcp/tools")
    assert response.status_code == 503
    assert response.json()["connected"] is False
    assert response.json()["tools"] == []


def test_lifespan_closes_runtime_and_mcp(monkeypatch):
    monkeypatch.setattr(server, "current_config", None)
    events = []

    class Mcp:
        def __init__(self, url):
            pass

        async def connect(self):
            events.append("connect")

        async def disconnect(self):
            events.append("disconnect")

    monkeypatch.setattr(server, "McpBridge", Mcp)
    with TestClient(server.app):
        assert events == ["connect"]
    assert events == ["connect", "disconnect"]


def test_background_fallback_keeps_error_classification(monkeypatch):
    import asyncio
    from types import SimpleNamespace
    from llm_client.adapters.base import UpstreamError

    def exploding_loop(*args, **kwargs):
        async def gen():
            raise UpstreamError("connect timed out", category="network", retryable=True)
            yield
        return gen()

    monkeypatch.setattr(server, "run_agent_loop", exploding_loop)
    monkeypatch.setattr(server, "load_session",
                        lambda _: SimpleNamespace(model_id="test/test-model", messages=[],
                                                  append_trajectory=lambda *a, **k: None))
    monkeypatch.setattr(server, "TaskLedger", lambda _: SimpleNamespace(plan=None))

    async def main():
        bg = server.BackgroundSession("bg-1")
        await server._run_background_loop(
            bg, "bg-1", "hi", "base", [], [], "hi", "chat", ""
        )
        return [bg.queue.get_nowait() for _ in range(bg.queue.qsize())]

    queued = asyncio.run(main())
    # 队首是轨迹 user 事件，错误事件在其后
    failure = next(e for e in queued if e["type"] == "error")

    assert failure["type"] == "error" and failure["retryable"] is True
    assert failure["category"] == "network"
    assert "retry_after" not in failure
    # _seq 是重连回放用的内部序号，发给前端前会剥离，断言时同样忽略
    assert _strip_seq(queued[-1]) == {"type": "done"}  # 兜底之后仍然补终态事件


def test_background_loop_does_not_append_second_done(monkeypatch):
    """agent_loop 已发带统计的 done 时，finally 不能再补空 done。

    空 done 会被 SSE 的 drain 阶段发给前端，把已渲染的用量/用时按钮清空。
    """
    import asyncio
    import uuid
    from types import SimpleNamespace

    session_id = str(uuid.uuid4())
    done_event = {
        "type": "done", "session_id": session_id, "tokens": 120,
        "tokens_input": 100, "tokens_output": 20, "tokens_estimated": False,
        "cache_read_tokens": 0, "reasoning_tokens": 8, "model": "test/test-model",
        "elapsed_ms": 1500, "think_ms": 300, "ttft_ms": 400, "tps": 13.3,
    }

    def finishing_loop(*args, **kwargs):
        async def gen():
            yield {"type": "text_chunk", "delta": "ok"}
            yield dict(done_event)
        return gen()

    monkeypatch.setattr(server, "run_agent_loop", finishing_loop)
    monkeypatch.setattr(server, "load_session",
                        lambda _: SimpleNamespace(model_id="test/test-model", messages=[],
                                                  append_trajectory=lambda *a, **k: None))
    monkeypatch.setattr(server, "TaskLedger", lambda _: SimpleNamespace(plan=None))
    monkeypatch.setattr(server, "save_session", lambda *a, **k: None)
    monkeypatch.setattr(server, "update_index", lambda *a, **k: None)

    async def main():
        bg = server.BackgroundSession(session_id)
        await server._run_background_loop(
            bg, session_id, "hi", "base", [], [], "hi", "chat", ""
        )
        return [bg.queue.get_nowait() for _ in range(bg.queue.qsize())]

    queued = asyncio.run(main())
    assert [_strip_seq(e) for e in queued if e["type"] == "done"] == [done_event]


def test_background_status_reports_running_turn(monkeypatch):
    """前端切回会话靠这个接口判断后台是否还在跑、该接回哪一轮。"""
    import uuid
    from types import SimpleNamespace

    session_id = str(uuid.uuid4())
    client = TestClient(server.app)

    assert client.get("/sessions/%s/background" % session_id).json() == {
        "active": False, "last_message": None,
        "display_content": None, "turn_msg_count": None,
    }

    bg = server.BackgroundSession(session_id)
    bg.task = SimpleNamespace(done=lambda: False)  # 后台任务仍在运行
    bg.last_message = "hi"
    bg.last_display = "hi"
    bg.turn_msg_count = 3
    monkeypatch.setitem(server._background_sessions, session_id, bg)

    assert client.get("/sessions/%s/background" % session_id).json() == {
        "active": True, "last_message": "hi",
        "display_content": "hi", "turn_msg_count": 3,
    }

    bg.done_event.set()  # 本轮收尾后不再算活跃，前端不会重连
    assert client.get("/sessions/%s/background" % session_id).json()["active"] is False


def test_chat_stream_replays_history_on_reconnect(monkeypatch):
    """SSE 断开重连：断开前的事件按序补发，内部序号 _seq 不外泄。"""
    import asyncio
    import json
    import uuid
    from types import SimpleNamespace

    install_config(monkeypatch)
    monkeypatch.setattr(server, "_mcp", object())
    monkeypatch.setattr(server, "skill_registry",
                        SimpleNamespace(reload=lambda: None,
                                        set_roots=lambda *a, **k: None))

    session_id = str(uuid.uuid4())
    bg = server.BackgroundSession(session_id)
    bg.task = SimpleNamespace(done=lambda: False)  # 任务在跑，重连不该再起一轮
    bg.last_message = "hi"

    async def seed():
        await server._bg_put(bg, {"type": "user", "turn": 1, "content": "hi"})
        await server._bg_put(bg, {"type": "text_chunk", "delta": "你好"})
        await server._bg_put(bg, {"type": "done", "tokens": 5})

    asyncio.run(seed())
    monkeypatch.setitem(server._background_sessions, session_id, bg)

    response = TestClient(server.app).post(
        "/chat/stream", json={"session_id": session_id, "message": "hi"}
    )
    assert response.status_code == 200
    assert "_seq" not in response.text

    frames = []
    for frame in response.text.split("\n\n"):
        if not frame.strip():
            continue
        name, data = None, None
        for line in frame.split("\n"):
            if line.startswith("event:"):
                name = line[len("event:"):].strip()
            elif line.startswith("data:"):
                data = json.loads(line[len("data:"):].strip())
        frames.append((name, data))
    assert frames == [
        ("user", {"type": "user", "turn": 1, "content": "hi"}),
        ("text_chunk", {"type": "text_chunk", "delta": "你好"}),
        ("done", {"type": "done", "tokens": 5}),
    ]


def test_cancel_background_stops_running_task(monkeypatch):
    """前端「停止」按钮：除了 abort SSE，还要取消后台 agent_loop，避免继续消耗 token。"""
    import uuid
    from types import SimpleNamespace

    session_id = str(uuid.uuid4())
    client = TestClient(server.app)

    # 没有后台任务时幂等返回 cancelled=False
    assert client.post("/sessions/%s/cancel" % session_id).json() == {"cancelled": False}

    cancelled = []
    bg = server.BackgroundSession(session_id)
    bg.task = SimpleNamespace(
        done=lambda: False, cancel=lambda: cancelled.append(session_id)
    )
    monkeypatch.setitem(server._background_sessions, session_id, bg)

    assert client.post("/sessions/%s/cancel" % session_id).json() == {"cancelled": True}
    assert cancelled == [session_id]

    # 任务已结束时不再重复取消
    bg.task = SimpleNamespace(done=lambda: True, cancel=lambda: cancelled.append("late"))
    assert client.post("/sessions/%s/cancel" % session_id).json() == {"cancelled": False}
    assert cancelled == [session_id]


def test_get_sessions_marks_active_background(monkeypatch):
    """会话列表注入 active/waiting 字段：刷新页面后前端仍能在列表里标出「进行中/待确认」。"""
    import uuid
    from types import SimpleNamespace

    active_id = str(uuid.uuid4())
    idle_id = str(uuid.uuid4())
    monkeypatch.setattr(server, "list_sessions", lambda query="", archived=False: [
        {"id": active_id, "title": "跑着的"},
        {"id": idle_id, "title": "空闲的"},
    ])

    bg = server.BackgroundSession(active_id)
    bg.task = SimpleNamespace(done=lambda: False)
    monkeypatch.setitem(server._background_sessions, active_id, bg)
    # 挂起的工具审批 → waiting 字段（键格式 "session_id:call_id"）
    monkeypatch.setitem(server._pending_approvals, active_id + ":call-1", None)

    client = TestClient(server.app)
    sessions = client.get("/sessions").json()["sessions"]
    assert [item["active"] for item in sessions] == [True, False]
    assert [item["waiting"] for item in sessions] == [True, False]

    bg.done_event.set()  # 本轮收尾后不再算活跃，前端刷新后不会误标
    sessions = client.get("/sessions").json()["sessions"]
    assert sessions[0]["active"] is False
    # 审批解决后 waiting 复位（_request_tool_approval 的 finally 会 pop 键）
    del server._pending_approvals[active_id + ":call-1"]
    sessions = client.get("/sessions").json()["sessions"]
    assert sessions[0]["waiting"] is False
