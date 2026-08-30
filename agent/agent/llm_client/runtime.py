from types import MappingProxyType

from .transform import MessageTransformer


class ModelCatalog:
    def __init__(self, models):
        self._models = MappingProxyType({model.key: model for model in models})

    def get(self, key):
        try:
            return self._models[key]
        except KeyError:
            raise ValueError(f"unknown model: {key}") from None

    def snapshot(self):
        return self._models


class ModelRuntime:
    def __init__(self, catalog, providers, adapters, transformer=None):
        self.catalog = catalog
        self.providers = providers
        self.adapters = adapters
        self.transformer = transformer or MessageTransformer()

    def model(self, model_key):
        return self.catalog.get(model_key)

    def context_window(self, model_key):
        return self.model(model_key).context_window

    async def stream(self, model_key, messages, tools=(), system_prompt=""):
        model = self.model(model_key)
        if not model.enabled:
            raise ValueError(f"model is disabled: {model_key}")
        provider = self.providers.get(model.provider)
        adapter = self.adapters.get(model.api)
        unified = self.transformer.from_legacy(messages, system_prompt) if messages and isinstance(messages[0], dict) else list(messages)
        normalized = self.transformer.transform(unified, model)
        request = adapter.build_request(model, normalized, list(tools))
        async for event in adapter.stream(provider, model, request):
            yield event

    async def aclose(self):
        await self.providers.aclose()
