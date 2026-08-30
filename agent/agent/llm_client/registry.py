"""Provider 注册表和工厂函数。

v4 新增：
- RETRY_CONFIGS：per-provider 可配置重试策略
- create_provider_with_retry：创建带重试的 provider
"""
import logging
from typing import Dict

from .base import BaseProvider, ProviderConfig, RetryConfig
from .openai_provider import OpenAIProvider
from .anthropic_provider import AnthropicProvider

logger = logging.getLogger(__name__)


# Provider 注册表
PROVIDER_MAP: Dict[str, type[BaseProvider]] = {
    "openai": OpenAIProvider,
    "anthropic": AnthropicProvider,
    # 后续扩展：
    # "deepseek": DeepSeekProvider,
    # "ollama": OllamaProvider,
}

# v4: per-provider 重试配置
RETRY_CONFIGS: Dict[str, RetryConfig] = {
    "openai": RetryConfig(max_retries=3, base_delay=1.0),
    "anthropic": RetryConfig(max_retries=3, base_delay=1.0),
    "deepseek": RetryConfig(max_retries=5, base_delay=2.0),  # DeepSeek 限制更严
    "ollama": RetryConfig(max_retries=0, base_delay=0.0),    # 本地不重试
}


def create_provider(config: "ApiConfig") -> BaseProvider:
    """工厂函数：根据 config.api_type 创建 provider。

    自动包装 retry 策略（通过 stream_with_retry 在 __init__.py 中实现）。
    """
    provider_cls = PROVIDER_MAP.get(config.api_type, OpenAIProvider)
    provider = provider_cls(config, ProviderConfig())
    return provider


def get_retry_config(api_type: str) -> RetryConfig:
    """获取指定 api_type 的重试配置。"""
    return RETRY_CONFIGS.get(api_type, RetryConfig())
