from .adapters import AnthropicMessagesAdapter, OpenAIChatAdapter, OpenAIResponsesAdapter
from .providers import AnthropicProvider, OpenAICompatibleProvider, OpenAIProvider


class Registry:
    def __init__(self):
        self._items = {}

    def register(self, key, value):
        if key in self._items:
            raise ValueError(f"duplicate registration: {key}")
        self._items[key] = value

    def get(self, key):
        try:
            return self._items[key]
        except KeyError:
            raise ValueError(f"unknown registration: {key}") from None


class ProviderRegistry(Registry):
    async def aclose(self):
        for provider in self._items.values():
            await provider.aclose()


class AdapterRegistry(Registry):
    pass


def default_adapter_registry():
    registry = AdapterRegistry()
    registry.register("openai-chat", OpenAIChatAdapter())
    registry.register("openai-responses", OpenAIResponsesAdapter())
    registry.register("anthropic-messages", AnthropicMessagesAdapter())
    return registry


def create_provider(config):
    """Legacy factory retained for context.py until its scheduled migration."""
    provider_cls = AnthropicProvider if config.api_type == "anthropic" else OpenAIProvider
    from .types import ProviderConfig
    return provider_cls(ProviderConfig(id=config.api_type, base_url=config.api_url, api_key=config.api_key,
                                       ssl_verify=False))


def provider_class(kind):
    classes = {"openai": OpenAIProvider, "anthropic": AnthropicProvider,
               "openai-compatible": OpenAICompatibleProvider}
    if kind not in classes: raise ValueError(f"unknown provider type: {kind}")
    return classes[kind]
