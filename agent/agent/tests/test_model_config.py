import json

import pytest

from config import (
    API_KEY_MASK,
    ConfigError,
    api_config_to_dict,
    config_from_dict,
    config_revision,
    load_config,
    preserve_masked_api_keys,
    redacted_config,
    save_config,
)


def valid_data():
    return {
        "version": 1,
        "default_model": "openai/gpt-test",
        "providers": [{
            "id": "openai",
            "name": "OpenAI",
            "base_url": "https://api.example.test/v1/",
            "api_key": "secret-value",
            "api_key_env": "OPENAI_API_KEY",
            "headers": {"X-Tenant": "test"},
            "discover_models": True,
            "discovery_api": "openai",
            "default_api": "openai-responses",
            "enabled": True,
        }],
        "models": [{
            "id": "gpt-test",
            "provider": "openai",
            "api": None,
            "name": "GPT Test",
            "enabled": True,
            "context_window": 32768,
            "max_output_tokens": 4096,
            "capabilities": {"tools": True},
            "compat": {"supports_strict_tools": True},
        }],
    }


def test_strict_config_and_full_model_key():
    config = config_from_dict(valid_data())
    assert config.default_model == "openai/gpt-test"
    assert config.model("openai/gpt-test").key == "openai/gpt-test"
    assert config.provider("openai").base_url == "https://api.example.test/v1"


@pytest.mark.parametrize("change", [
    lambda data: data.update(extra=True),
    lambda data: data["providers"][0].update(extra=True),
    lambda data: data["models"][0].update(extra=True),
    lambda data: data["providers"][0].update(base_url="file:///tmp/model"),
    lambda data: data["models"][0].update(api="unknown-adapter"),
    lambda data: data.update(default_model="openai/missing"),
])
def test_rejects_invalid_or_unknown_configuration(change):
    data = valid_data()
    change(data)
    with pytest.raises(ConfigError):
        config_from_dict(data)


def test_rejects_duplicate_provider_and_model_keys():
    data = valid_data()
    data["providers"].append(dict(data["providers"][0]))
    with pytest.raises(ConfigError, match="Provider|provider"):
        config_from_dict(data)
    data = valid_data()
    data["models"].append(dict(data["models"][0]))
    with pytest.raises(ConfigError, match="model keys"):
        config_from_dict(data)


def test_key_resolution_redaction_and_mask_preservation(monkeypatch):
    config = config_from_dict(valid_data())
    monkeypatch.setenv("OPENAI_API_KEY", "from-environment")
    assert config.provider("openai").resolved_api_key() == "from-environment"
    response = redacted_config(config)
    assert response["providers"][0]["api_key"] == API_KEY_MASK
    assert "secret-value" not in json.dumps(response)
    response["providers"][0]["api_key"] = API_KEY_MASK
    candidate = config_from_dict(response)
    preserved = preserve_masked_api_keys(candidate, config)
    assert preserved.provider("openai").api_key == "secret-value"
    response["providers"][0]["api_key"] = ""
    cleared = preserve_masked_api_keys(config_from_dict(response), config)
    assert cleared.provider("openai").api_key == ""


def test_revision_is_stable_and_changes_with_configuration():
    first = config_from_dict(valid_data())
    second_data = valid_data()
    second_data["models"][0]["name"] = "Changed"
    second = config_from_dict(second_data)
    assert config_revision(first) == config_revision(config_from_dict(valid_data()))
    assert config_revision(first) != config_revision(second)


def test_atomic_save_and_strict_load(tmp_path):
    path = tmp_path / "config.json"
    config = config_from_dict(valid_data())
    save_config(config, path)
    loaded = load_config(path)
    assert loaded == config
    assert json.loads(path.read_text(encoding="utf-8")) == api_config_to_dict(config)
