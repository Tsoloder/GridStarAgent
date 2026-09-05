import asyncio
import json
from datetime import datetime, timedelta, timezone
from email.utils import format_datetime

import httpx

from llm_client.adapters.base import RetryPolicy, parse_retry_after, stream_failure
from llm_client.adapters.openai_chat import OpenAIChatAdapter
from llm_client.providers.openai import OpenAIProvider
from llm_client.registry import AdapterRegistry, ProviderRegistry
from llm_client.runtime import ModelCatalog, ModelRuntime
from llm_client.stream import StreamBuilder
from llm_client.transform import MessageTransformer
from llm_client.types import (
    Message, ModelConfig, ProviderConfig, ToolCallEnd, ToolResultBlock,
)


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


def _chat_model():
    return ModelConfig("qwen-test", "vendor", "openai-chat")


def test_openai_chat_request_never_sends_null_or_empty_messages():
    """工具调用轮次的 assistant 消息没有正文，历史里存的是 null。

    严格兼容网关（DashScope 等）要求 content 必填，null 会让整轮请求 400。
    """
    unified = MessageTransformer().from_legacy(
        [
            {"role": "assistant", "content": None, "tool_calls": [
                {"id": "call_1", "type": "function",
                 "function": {"name": "GetLine", "arguments": '{"id":7}'}}]},
            {"role": "tool", "tool_call_id": "call_1", "content": "ok", "tool_name": "GetLine"},
            {"role": "assistant", "content": ""},
        ],
        "system prompt",
    )
    body = OpenAIChatAdapter().build_request(_chat_model(), unified, [])

    assert [item["role"] for item in body["messages"]] == ["system", "assistant", "tool"]
    assert all(item["content"] is not None for item in body["messages"])
    assistant = body["messages"][1]
    assert assistant["content"] == "" and assistant["tool_calls"][0]["id"] == "call_1"


def test_openai_chat_tool_result_uses_json_not_python_repr():
    message = Message("tool", (ToolResultBlock("call_1", {"ok": True, "名称": "网格"}, "GetLine"),))
    body = OpenAIChatAdapter().build_request(_chat_model(), [message], [])

    assert body["messages"][0]["content"] == '{"ok": true, "名称": "网格"}'


NO_WAIT = RetryPolicy(base_delay=0.0, max_delay=0.0)  # 退避归零，测试不真等


def _chat_events(handler, *, retry=NO_WAIT):
    """用假传输跑一遍 openai-chat 流，返回完整事件序列。"""
    provider = OpenAIProvider(
        ProviderConfig(id="vendor", base_url="https://example.test/v1", api_key="secret"),
        transport=httpx.MockTransport(handler),
    )

    async def run():
        adapter = OpenAIChatAdapter(retry=retry)
        request = adapter.build_request(_chat_model(), [], [])
        return [event async for event in adapter.stream(provider, _chat_model(), request)]

    return asyncio.run(run())


def test_openai_chat_stream_surfaces_in_stream_error():
    """200 流里夹带的 error 帧必须冒出来，否则会表现成"回复突然空了"。"""
    events = _chat_events(lambda request: httpx.Response(
        200,
        content=b'data: {"error": {"code": "Throttling", "message": "rate limit"}}\n\n',
        headers={"content-type": "text/event-stream"},
    ))
    assert events[-1].type == "error"
    assert "rate limit" in events[-1].message
    assert events[-1].retryable  # 限流是瞬时故障，不该标成永久失败


def test_rate_limited_http_error_is_retryable_with_retry_after():
    events = _chat_events(lambda request: httpx.Response(
        429, json={"error": {"message": "too many requests"}}, headers={"retry-after": "7.5"},
    ))
    failure = events[-1]
    assert failure.type == "error" and failure.retryable
    assert failure.category == "rate_limited"
    assert failure.status_code == 429 and failure.retry_after == 7.5


def test_server_errors_are_retryable_and_client_errors_are_not():
    for status, retryable in ((500, True), (503, True), (408, True), (400, False), (401, False)):
        events = _chat_events(lambda request, code=status: httpx.Response(
            code, json={"error": {"message": "boom"}}))
        failure = events[-1]
        assert failure.type == "error", status
        assert failure.retryable is retryable, status
        assert failure.status_code == status
        assert "boom" in failure.message  # 错误响应体不能被丢掉


def test_network_failures_are_retryable():
    def handler(request):
        raise httpx.ConnectError("connection refused")

    failure = _chat_events(handler)[-1]
    assert failure.type == "error" and failure.category == "network"
    assert failure.retryable and failure.status_code is None


def test_in_stream_error_markers_decide_retryability():
    assert stream_failure({"error": {"code": "invalid_parameter_error",
                                     "message": "The content field is a required field."}}).retryable is False
    assert stream_failure({"error": {"code": "ServiceUnavailable",
                                     "message": "server is overloaded"}}).retryable is True
    assert stream_failure({"error": {"status": 503}}).retryable is True


def test_retry_after_accepts_seconds_and_http_date():
    assert parse_retry_after("120") == 120
    assert parse_retry_after(None) is None
    assert parse_retry_after("garbage") is None
    later = format_datetime(datetime.now(timezone.utc) + timedelta(seconds=60), usegmt=True)
    assert 0 < parse_retry_after(later) <= 60


def _sse(text):
    return httpx.Response(
        200,
        content=('data: %s\n\ndata: [DONE]\n\n' % json.dumps(
            {"choices": [{"delta": {"content": text}}]})).encode(),
        headers={"content-type": "text/event-stream"},
    )


def test_rate_limited_stream_is_retried_until_it_succeeds():
    """429 属于瞬时故障：应重发请求，直到拿到内容后正常收尾。"""
    calls = []

    def handler(request):
        calls.append(request)
        if len(calls) < 3:
            return httpx.Response(429, json={"error": {"message": "slow down"}})
        return _sse("网格")

    events = _chat_events(handler)
    assert len(calls) == 3
    assert events[0].type == "start" and events[-1].type == "done"
    assert [e.type for e in events].count("start") == 1  # 重试不得重复开场
    assert "".join(e.delta for e in events if e.type == "text_delta") == "网格"
    assert not [e for e in events if e.type == "error"]


def test_budget_exhausted_still_reports_the_real_error():
    calls = []

    def handler(request):
        calls.append(request)
        return httpx.Response(429, json={"error": {"message": "slow down"}})

    failure = _chat_events(handler, retry=RetryPolicy(max_attempts=2, base_delay=0.0))[-1]
    assert len(calls) == 2  # 用尽预算就停手，不能无限重试
    assert failure.type == "error" and failure.retryable and failure.category == "rate_limited"


def test_client_error_is_never_retried():
    """400 这类请求本身有问题，重发只会浪费一轮配额。"""
    calls = []

    def handler(request):
        calls.append(request)
        return httpx.Response(400, json={"error": {"message": "The content field is a required field."}})

    failure = _chat_events(handler)[-1]
    assert len(calls) == 1
    assert failure.type == "error" and not failure.retryable


def test_emitted_content_is_never_replayed():
    """吐出过内容之后再失败只能报错，重放会让用户看到重复文本。"""
    calls = []

    def handler(request):
        calls.append(request)
        return httpx.Response(
            200,
            content=b'data: {"choices": [{"delta": {"content": "\xe7\xbd\x91\xe6\xa0\xbc"}}]}\n\n'
                    b'data: {"error": {"code": "Throttling", "message": "rate limit"}}\n\n',
            headers={"content-type": "text/event-stream"},
        )

    events = _chat_events(handler)
    assert len(calls) == 1
    assert [e.type for e in events].count("text_start") == 1
    assert events[-1].type == "error" and events[-1].retryable


def test_retry_delay_prefers_retry_after_and_respects_the_ceiling():
    policy = RetryPolicy(base_delay=1.0, max_delay=20.0)
    assert policy.delay(0, retry_after=3.5) == 3.5  # 上游说了等多久就等多久
    assert policy.delay(0, retry_after=999) == 20.0
    assert policy.delay(0, retry_after=0) == 0.0
    assert 1.0 <= policy.delay(0) <= 1.25  # 没有 Retry-After 时指数退避 + jitter
    assert 2.0 <= policy.delay(1) <= 2.25
    assert RetryPolicy(max_delay=5.0).delay(6) == 5.0
