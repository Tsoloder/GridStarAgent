"""模型元数据解析与保存时自动补全上限的测试。

覆盖两条链路：
- llm_client.model_metadata.resolve：按 (model_id, base_url) 解析真实上下文窗口/最大输出
- app._apply_model_limits：保存配置前，只为「未显式填写」的字段补上解析结果
"""
import app as server
from llm_client.model_metadata import _provider_hint, _variant_ids, resolve


def test_resolve_scoped_by_base_url_host():
    # api.openai.com 命中 openai，gpt-4o 登记为 128k 上下文 / 16384 输出
    limits = resolve("gpt-4o", "https://api.openai.com/v1")
    assert limits is not None
    assert limits.context_window == 128000
    assert limits.max_output_tokens == 16384
    assert limits.provider == "openai"


def test_resolve_strips_date_suffix_variant():
    # Anthropic 的带日期写法在数据源里登记为不带日期的形式
    limits = resolve("claude-sonnet-4-5-20250929", "https://api.anthropic.com")
    assert limits is not None
    assert limits.context_window == 1000000
    assert limits.max_output_tokens == 64000


def test_resolve_without_base_url_uses_shared_index():
    # 不传 base_url 时走跨供应商共享索引，仍能查到常见模型
    limits = resolve("gpt-4o")
    assert limits is not None
    assert limits.context_window >= 1


def test_resolve_unknown_model_returns_none():
    assert resolve("totally-unknown-model-xyz") is None
    assert resolve("") is None
    assert resolve("   ") is None


def test_provider_hint_strips_subdomains():
    # 逐级剥掉子域名匹配：foo.api.openai.com 也应命中 openai
    assert _provider_hint("https://foo.api.openai.com/v1") == "openai"
    assert _provider_hint("https://api.anthropic.com") == "anthropic"
    assert _provider_hint("http://localhost:11434") == ""
    assert _provider_hint("not a url") == ""
    assert _provider_hint("") == ""


def test_variant_ids_orders_by_confidence():
    variants = _variant_ids("claude-sonnet-4-5-20250929")
    assert variants[0] == "claude-sonnet-4-5-20250929"
    assert "claude-sonnet-4-5" in variants
    tagged = _variant_ids("qwen3:14b")
    assert tagged[0] == "qwen3:14b"
    assert "qwen3" in tagged


def test_apply_model_limits_fills_only_missing_fields():
    raw = {
        "providers": [
            {"id": "openai", "base_url": "https://api.openai.com/v1"},
            {"id": "local", "base_url": "http://localhost:11434"},
        ],
        "models": [
            # 缺两个字段：应被补全为 gpt-4o 的真实值
            {"id": "gpt-4o", "provider": "openai"},
            # 用户已显式指定：不应被覆盖
            {"id": "gpt-4o", "provider": "openai", "context_window": 8192, "max_output_tokens": 1024},
            # 未知模型 + 无主机提示：保持缺失，回落配置默认值
            {"id": "mystery", "provider": "local"},
        ],
    }
    result = server._apply_model_limits(raw)
    models = result["models"]
    assert models[0]["context_window"] == 128000
    assert models[0]["max_output_tokens"] == 16384
    # 显式值原样保留
    assert models[1]["context_window"] == 8192
    assert models[1]["max_output_tokens"] == 1024
    # 查不到就不写字段
    assert "context_window" not in models[2]
    assert "max_output_tokens" not in models[2]


def test_apply_model_limits_clamps_output_to_window():
    # 只填了 context_window（很小），max_output_tokens 缺失且解析值更大时，应被夹到窗口内
    raw = {
        "providers": [{"id": "openai", "base_url": "https://api.openai.com/v1"}],
        "models": [{"id": "gpt-4o", "provider": "openai", "context_window": 4096}],
    }
    result = server._apply_model_limits(raw)
    model = result["models"][0]
    assert model["context_window"] == 4096
    assert model["max_output_tokens"] <= 4096


def test_apply_model_limits_ignores_malformed_input():
    # 非 dict、缺 providers/models、模型缺 id 等情况都应原样返回、不抛异常
    assert server._apply_model_limits(None) is None
    assert server._apply_model_limits("nope") == "nope"
    passthrough = {"providers": "not-a-list", "models": []}
    assert server._apply_model_limits(passthrough) is passthrough
    raw = {"providers": [{"id": "openai", "base_url": "https://api.openai.com/v1"}],
           "models": [{"provider": "openai"}, "junk", {"id": "gpt-4o", "provider": "openai"}]}
    result = server._apply_model_limits(raw)
    assert result["models"][2]["context_window"] == 128000
