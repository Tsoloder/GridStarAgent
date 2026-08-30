import hashlib
import json
import logging
import os
from dataclasses import dataclass, field, fields, replace
from pathlib import Path
from types import MappingProxyType
from typing import Any, Mapping, Optional
from urllib.parse import urlparse

from paths import CONFIG_PATH

logger = logging.getLogger(__name__)

API_KEY_MASK = "********"
SUPPORTED_APIS = frozenset({"openai-chat", "openai-responses", "anthropic-messages"})
SUPPORTED_DISCOVERY_APIS = frozenset({"openai", "anthropic", "none"})


class ConfigError(ValueError):
    pass


@dataclass(frozen=True)
class ModelCapabilities:
    tools: bool = False
    parallel_tools: bool = False
    reasoning: bool = False
    vision: bool = False
    stream_usage: bool = False


@dataclass(frozen=True)
class ProviderConfig:
    id: str
    name: str
    base_url: str
    api_key: str = field(default="", repr=False)
    api_key_env: str = ""
    headers: Mapping[str, str] = field(default_factory=lambda: MappingProxyType({}))
    discover_models: bool = True
    discovery_api: str = "openai"
    default_api: str = "openai-chat"
    enabled: bool = True

    def resolved_api_key(self, environ: Optional[Mapping[str, str]] = None) -> str:
        env = os.environ if environ is None else environ
        if self.api_key_env:
            value = env.get(self.api_key_env, "")
            if value:
                return value
        return self.api_key


@dataclass(frozen=True)
class ModelConfig:
    id: str
    provider: str
    api: Optional[str] = None
    name: str = ""
    enabled: bool = True
    context_window: int = 32768
    max_output_tokens: int = 4096
    capabilities: ModelCapabilities = field(default_factory=ModelCapabilities)
    compat: Mapping[str, Any] = field(default_factory=lambda: MappingProxyType({}))
    _specified_fields: frozenset[str] = field(default_factory=frozenset, repr=False, compare=False)
    _specified_capabilities: frozenset[str] = field(default_factory=frozenset, repr=False, compare=False)

    @property
    def key(self) -> str:
        return f"{self.provider}/{self.id}"


@dataclass(frozen=True)
class ApiConfig:
    version: int
    default_model: str
    providers: tuple[ProviderConfig, ...]
    models: tuple[ModelConfig, ...]

    def provider(self, provider_id: str) -> ProviderConfig:
        for item in self.providers:
            if item.id == provider_id:
                return item
        raise KeyError(provider_id)

    def model(self, model_key: str) -> ModelConfig:
        for item in self.models:
            if item.key == model_key:
                return item
        raise KeyError(model_key)


_PROVIDER_FIELDS = frozenset({
    "id", "name", "base_url", "api_key", "api_key_env", "headers",
    "discover_models", "discovery_api", "default_api", "enabled",
})
_MODEL_FIELDS = frozenset({
    "id", "provider", "api", "name", "enabled", "context_window",
    "max_output_tokens", "capabilities", "compat",
})
_CAPABILITY_FIELDS = frozenset(ModelCapabilities.__dataclass_fields__)
_ROOT_FIELDS = frozenset({"version", "default_model", "providers", "models"})


def _require_fields(data: Mapping[str, Any], allowed: frozenset[str], required: set[str], path: str) -> None:
    unknown = set(data) - allowed
    missing = required - set(data)
    if unknown:
        raise ConfigError(f"{path}: unknown fields: {', '.join(sorted(unknown))}")
    if missing:
        raise ConfigError(f"{path}: missing fields: {', '.join(sorted(missing))}")


def _string(value: Any, path: str, *, nonempty: bool = False) -> str:
    if not isinstance(value, str):
        raise ConfigError(f"{path} must be a string")
    value = value.strip()
    if nonempty and not value:
        raise ConfigError(f"{path} must not be empty")
    return value


def _boolean(value: Any, path: str) -> bool:
    if type(value) is not bool:
        raise ConfigError(f"{path} must be a boolean")
    return value


def _positive_int(value: Any, path: str) -> int:
    if type(value) is not int or value <= 0:
        raise ConfigError(f"{path} must be a positive integer")
    return value


def _mapping(value: Any, path: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise ConfigError(f"{path} must be an object")
    return value


def _parse_provider(raw: Any, index: int) -> ProviderConfig:
    path = f"providers[{index}]"
    data = _mapping(raw, path)
    _require_fields(data, _PROVIDER_FIELDS, {"id", "name", "base_url", "default_api"}, path)
    base_url = _string(data["base_url"], f"{path}.base_url", nonempty=True).rstrip("/")
    parsed = urlparse(base_url)
    if parsed.scheme not in ("http", "https") or not parsed.netloc:
        raise ConfigError(f"{path}.base_url must be an HTTP(S) URL")
    default_api = _string(data["default_api"], f"{path}.default_api", nonempty=True)
    if default_api not in SUPPORTED_APIS:
        raise ConfigError(f"{path}.default_api references unknown adapter: {default_api}")
    discovery_api = _string(data.get("discovery_api", "openai"), f"{path}.discovery_api", nonempty=True)
    if discovery_api not in SUPPORTED_DISCOVERY_APIS:
        raise ConfigError(f"{path}.discovery_api is invalid: {discovery_api}")
    headers_raw = _mapping(data.get("headers", {}), f"{path}.headers")
    headers: dict[str, str] = {}
    for key, value in headers_raw.items():
        if not isinstance(key, str) or not isinstance(value, str):
            raise ConfigError(f"{path}.headers must contain string keys and values")
        headers[key] = value
    return ProviderConfig(
        id=_string(data["id"], f"{path}.id", nonempty=True),
        name=_string(data["name"], f"{path}.name", nonempty=True),
        base_url=base_url,
        api_key=_string(data.get("api_key", ""), f"{path}.api_key"),
        api_key_env=_string(data.get("api_key_env", ""), f"{path}.api_key_env"),
        headers=MappingProxyType(headers),
        discover_models=_boolean(data.get("discover_models", True), f"{path}.discover_models"),
        discovery_api=discovery_api,
        default_api=default_api,
        enabled=_boolean(data.get("enabled", True), f"{path}.enabled"),
    )


def _parse_model(raw: Any, index: int) -> ModelConfig:
    path = f"models[{index}]"
    data = _mapping(raw, path)
    _require_fields(data, _MODEL_FIELDS, {"id", "provider"}, path)
    api = data.get("api")
    if api is not None:
        api = _string(api, f"{path}.api", nonempty=True)
        if api not in SUPPORTED_APIS:
            raise ConfigError(f"{path}.api references unknown adapter: {api}")
    capabilities_raw = _mapping(data.get("capabilities", {}), f"{path}.capabilities")
    _require_fields(capabilities_raw, _CAPABILITY_FIELDS, set(), f"{path}.capabilities")
    capabilities = ModelCapabilities(**{
        key: _boolean(value, f"{path}.capabilities.{key}")
        for key, value in capabilities_raw.items()
    })
    compat_raw = _mapping(data.get("compat", {}), f"{path}.compat")
    return ModelConfig(
        id=_string(data["id"], f"{path}.id", nonempty=True),
        provider=_string(data["provider"], f"{path}.provider", nonempty=True),
        api=api,
        name=_string(data.get("name", ""), f"{path}.name"),
        enabled=_boolean(data.get("enabled", True), f"{path}.enabled"),
        context_window=_positive_int(data.get("context_window", 32768), f"{path}.context_window"),
        max_output_tokens=_positive_int(data.get("max_output_tokens", 4096), f"{path}.max_output_tokens"),
        capabilities=capabilities,
        compat=MappingProxyType(dict(compat_raw)),
        _specified_fields=frozenset(data),
        _specified_capabilities=frozenset(capabilities_raw),
    )


def config_from_dict(data: Mapping[str, Any]) -> ApiConfig:
    data = _mapping(data, "config")
    _require_fields(data, _ROOT_FIELDS, {"version", "default_model", "providers", "models"}, "config")
    if data["version"] != 1:
        raise ConfigError("config.version must be 1")
    if not isinstance(data["providers"], list) or not isinstance(data["models"], list):
        raise ConfigError("config.providers and config.models must be arrays")
    providers = tuple(_parse_provider(item, i) for i, item in enumerate(data["providers"]))
    models = tuple(_parse_model(item, i) for i, item in enumerate(data["models"]))
    provider_ids = [item.id for item in providers]
    if len(provider_ids) != len(set(provider_ids)):
        raise ConfigError("provider IDs must be unique")
    model_keys = [item.key for item in models]
    if len(model_keys) != len(set(model_keys)):
        raise ConfigError("model keys must be unique")
    providers_by_id = {item.id: item for item in providers}
    for model in models:
        provider = providers_by_id.get(model.provider)
        if provider is None:
            raise ConfigError(f"model {model.key} references unknown provider")
        if model.context_window < model.max_output_tokens:
            raise ConfigError(f"model {model.key} context_window must be >= max_output_tokens")
        effective_api = model.api or provider.default_api
        if provider.discovery_api == "anthropic" and effective_api != "anthropic-messages":
            raise ConfigError(f"model {model.key} uses an incompatible adapter")
        if provider.discovery_api != "anthropic" and effective_api == "anthropic-messages":
            raise ConfigError(f"model {model.key} uses an incompatible adapter")
    default_model = _string(data["default_model"], "config.default_model", nonempty=True)
    enabled_models = {
        model.key for model in models
        if model.enabled and providers_by_id[model.provider].enabled
    }
    if default_model not in enabled_models:
        raise ConfigError("default_model must reference an enabled model and provider")
    return ApiConfig(1, default_model, providers, models)


def _plain(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {key: _plain(item) for key, item in value.items()}
    if isinstance(value, tuple):
        return [_plain(item) for item in value]
    if hasattr(value, "__dataclass_fields__"):
        return {
            item.name: _plain(getattr(value, item.name))
            for item in fields(value) if not item.name.startswith("_")
        }
    return value


def api_config_to_dict(config: ApiConfig, *, redact: bool = False) -> dict[str, Any]:
    result = _plain(config)
    if redact:
        for provider in result["providers"]:
            provider["api_key"] = API_KEY_MASK if provider["api_key"] else ""
    return result


def redacted_config(config: ApiConfig) -> dict[str, Any]:
    return api_config_to_dict(config, redact=True)


def preserve_masked_api_keys(candidate: ApiConfig, current: ApiConfig) -> ApiConfig:
    current_keys = {provider.id: provider.api_key for provider in current.providers}
    providers = tuple(
        replace(provider, api_key=current_keys.get(provider.id, ""))
        if provider.api_key == API_KEY_MASK else provider
        for provider in candidate.providers
    )
    return replace(candidate, providers=providers)


def config_revision(config: ApiConfig) -> str:
    payload = json.dumps(api_config_to_dict(config), sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def load_config(path: Path = CONFIG_PATH) -> Optional[ApiConfig]:
    if not path.exists():
        return None
    try:
        return config_from_dict(json.loads(path.read_text(encoding="utf-8")))
    except (OSError, json.JSONDecodeError, ConfigError) as exc:
        logger.warning("load_config failed: %s", exc)
        return None


def save_config(config: ApiConfig, path: Path = CONFIG_PATH) -> None:
    from session import atomic_write
    atomic_write(str(path), json.dumps(api_config_to_dict(config), ensure_ascii=False, indent=2))
    logger.info("config saved")
