import asyncio

import httpx

from llm_client.providers.openai import OpenAIProvider
from llm_client.registry import AdapterRegistry, ProviderRegistry
from llm_client.runtime import ModelCatalog, ModelRuntime
from llm_client.stream import StreamBuilder
from llm_client.types import ModelConfig, ProviderConfig, ToolCallEnd


def test_provider_auth_precedence_client_reuse_and_close(monkeypatch):
    monkeypatch.setenv("TEST_LLM_KEY", "env-secret")
    seen = []

    def handler(request):
        seen.append(request.headers)
        return httpx.Response(200, json={"data": []})

    provider = OpenAIProvider(ProviderConfig(id="p", base_url="https://example.test/v1",
                              api_key="config-secret", api_key_env="TEST_LLM_KEY"),
                              transport=httpx.MockTransport(handler))

    async def run():
        first = provider.client()
        assert first is provider.client()
        await provider.discover_models()
        await provider.aclose()
        assert first.is_closed

    asyncio.run(run())
    assert seen[0]["authorization"] == "Bearer env-secret"


def test_stream_builder_parallel_tools_and_invalid_json_are_safe():
    builder = StreamBuilder("response")
    events = [builder.start(), builder.tool_start(0, "a", "one"),
              builder.tool_start(1, "b", "two"), builder.tool_delta(0, '{"x":1}'),
              builder.tool_delta(1, "{"), builder.tool_end(0), builder.tool_end(1),
              builder.done("tool_use")]
    assert [event.sequence for event in events] == list(range(len(events)))
    ends = [event for event in events if isinstance(event, ToolCallEnd)]
    assert ends[0].arguments == {"x": 1} and ends[0].parse_error is None
    assert ends[1].arguments is None and ends[1].parse_error
    assert events[-1].type == "done"


def test_runtime_routes_full_model_key_to_provider_and_adapter():
    class Adapter:
        def build_request(self, model, messages, tools):
            return {"model": model.id, "message_count": len(messages)}

        async def stream(self, provider, model, request):
            builder = StreamBuilder("r")
            yield builder.start()
            yield builder.done()

    providers = ProviderRegistry(); provider = object(); providers.register("vendor", provider)
    adapters = AdapterRegistry(); adapters.register("openai-chat", Adapter())
    runtime = ModelRuntime(ModelCatalog([ModelConfig("model", "vendor", "openai-chat")]), providers, adapters)

    async def run():
        return [event async for event in runtime.stream("vendor/model", [{"role": "user", "content": "hi"}])]

    assert [event.type for event in asyncio.run(run())] == ["start", "done"]
