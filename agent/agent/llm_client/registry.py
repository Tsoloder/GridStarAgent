from .adapters import AnthropicMessagesAdapter, OpenAIChatAdapter, OpenAIResponsesAdapter


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
