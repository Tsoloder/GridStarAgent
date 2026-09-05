from copy import deepcopy

from fastapi.testclient import TestClient

import app as server
from config import API_KEY_MASK, config_from_dict, config_revision


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
    assert calls == [("test", "")]

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
                        lambda _: SimpleNamespace(model_id="test/test-model", messages=[]))
    monkeypatch.setattr(server, "TaskLedger", lambda _: SimpleNamespace(plan=None))

    async def main():
        bg = server.BackgroundSession("bg-1")
        await server._run_background_loop(
            bg, "bg-1", "hi", "base", [], [], "hi", "chat", ""
        )
        return [bg.queue.get_nowait() for _ in range(bg.queue.qsize())]

    queued = asyncio.run(main())
    failure = queued[0]

    assert failure["type"] == "error" and failure["retryable"] is True
    assert failure["category"] == "network"
    assert "retry_after" not in failure
    assert queued[-1] == {"type": "done"}  # 兜底之后仍然补终态事件
