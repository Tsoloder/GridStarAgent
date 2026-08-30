import asyncio
from dataclasses import dataclass, field, replace
from types import MappingProxyType
from typing import Any, Awaitable, Callable, Mapping, Optional

from config import ApiConfig, ModelCapabilities, ModelConfig, ProviderConfig

Discover = Callable[[ProviderConfig], Awaitable[list[Mapping[str, Any]]]]


@dataclass(frozen=True)
class CatalogModel:
    config: ModelConfig
    status: str
    created: Optional[int] = None
    owned_by: Optional[str] = None

    @property
    def key(self) -> str:
        return self.config.key


@dataclass(frozen=True)
class CatalogSnapshot:
    generation: int
    models: Mapping[str, CatalogModel] = field(default_factory=lambda: MappingProxyType({}))
    errors: Mapping[str, str] = field(default_factory=lambda: MappingProxyType({}))

    def get(self, model_key: str) -> CatalogModel:
        return self.models[model_key]


def _capabilities(value: Any) -> ModelCapabilities:
    if not isinstance(value, Mapping):
        return ModelCapabilities()
    allowed = ModelCapabilities.__dataclass_fields__
    return ModelCapabilities(**{
        key: item for key, item in value.items()
        if key in allowed and type(item) is bool
    })


def _discovered_model(provider: ProviderConfig, raw: Mapping[str, Any]) -> ModelConfig:
    model_id = str(raw.get("id", "")).strip()
    if not model_id:
        raise ValueError("discovered model is missing id")
    defaults = raw.get("defaults", {})
    if not isinstance(defaults, Mapping):
        defaults = {}
    capabilities = _capabilities(defaults.get("capabilities", {}))
    compat = defaults.get("compat", {})
    if not isinstance(compat, Mapping):
        compat = {}
    return ModelConfig(
        id=model_id,
        provider=provider.id,
        api=defaults.get("api"),
        name=str(raw.get("name") or model_id),
        context_window=int(defaults.get("context_window", 32768)),
        max_output_tokens=int(defaults.get("max_output_tokens", 4096)),
        capabilities=capabilities,
        compat=MappingProxyType(dict(compat)),
    )


def _merge_model(discovered: ModelConfig, configured: ModelConfig) -> ModelConfig:
    capabilities = ModelCapabilities(**{
        name: getattr(configured.capabilities, name)
        if name in configured._specified_capabilities
        else getattr(discovered.capabilities, name)
        for name in ModelCapabilities.__dataclass_fields__
    })
    compat = dict(discovered.compat)
    compat.update(configured.compat)
    specified = configured._specified_fields
    return replace(
        configured,
        api=configured.api if "api" in specified else discovered.api,
        name=configured.name if "name" in specified else discovered.name,
        context_window=(configured.context_window if "context_window" in specified else discovered.context_window),
        max_output_tokens=(configured.max_output_tokens if "max_output_tokens" in specified else discovered.max_output_tokens),
        capabilities=capabilities,
        compat=MappingProxyType(compat),
    )


class ModelCatalog:
    def __init__(self, config: ApiConfig, discoverers: Mapping[str, Discover]):
        self._config = config
        self._discoverers = dict(discoverers)
        self._generation = 0
        self._tasks: dict[str, asyncio.Task] = {}
        self._snapshot = self._configured_snapshot(0)

    @property
    def snapshot(self) -> CatalogSnapshot:
        return self._snapshot

    def _configured_snapshot(self, generation: int) -> CatalogSnapshot:
        models = {
            model.key: CatalogModel(model, "unavailable")
            for model in self._config.models
        }
        return CatalogSnapshot(generation, MappingProxyType(models), MappingProxyType({}))

    async def refresh(self) -> CatalogSnapshot:
        self._generation += 1
        generation = self._generation
        providers = [
            provider for provider in self._config.providers
            if provider.enabled and provider.discover_models and provider.discovery_api != "none"
        ]
        results = await asyncio.gather(
            *(self._refresh_provider(provider, generation) for provider in providers),
            return_exceptions=False,
        )
        if generation != self._generation:
            return self._snapshot
        discovered: dict[str, Mapping[str, Any]] = {}
        errors: dict[str, str] = {}
        for provider, models, error in results:
            if error:
                errors[provider.id] = error
            for item in models:
                model_id = str(item.get("id", "")).strip()
                if model_id:
                    discovered[f"{provider.id}/{model_id}"] = item
        merged: dict[str, CatalogModel] = {}
        configured = {model.key: model for model in self._config.models}
        providers_by_id = {provider.id: provider for provider in self._config.providers}
        for key in sorted(set(configured) | set(discovered)):
            manual = configured.get(key)
            remote = discovered.get(key)
            provider = providers_by_id[key.split("/", 1)[0]]
            if remote is not None:
                found = _discovered_model(provider, remote)
                model = _merge_model(found, manual) if manual is not None else found
                status = "configured_and_discovered" if manual is not None else "discovered"
                merged[key] = CatalogModel(model, status, remote.get("created"), remote.get("owned_by"))
            else:
                merged[key] = CatalogModel(manual, "unavailable" if provider.id in errors else "configured")
        self._snapshot = CatalogSnapshot(
            generation,
            MappingProxyType(merged),
            MappingProxyType(errors),
        )
        return self._snapshot

    async def _refresh_provider(self, provider: ProviderConfig, generation: int):
        existing = self._tasks.get(provider.id)
        if existing is not None and not existing.done():
            return await existing

        async def run():
            discover = self._discoverers.get(provider.discovery_api)
            if discover is None:
                return provider, [], f"unknown discovery API: {provider.discovery_api}"
            try:
                return provider, await discover(provider), ""
            except Exception as exc:
                return provider, [], str(exc)

        task = asyncio.create_task(run())
        self._tasks[provider.id] = task
        try:
            return await task
        finally:
            if self._tasks.get(provider.id) is task:
                self._tasks.pop(provider.id, None)
