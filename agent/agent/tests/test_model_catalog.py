import asyncio

import pytest

from config import config_from_dict
from llm_client.catalog import ModelCatalog


def config_data():
    return {
        "version": 1,
        "default_model": "alpha/shared",
        "providers": [
            {"id": "alpha", "name": "Alpha", "base_url": "https://alpha.test/v1", "default_api": "openai-chat", "discovery_api": "openai"},
            {"id": "beta", "name": "Beta", "base_url": "https://beta.test", "default_api": "anthropic-messages", "discovery_api": "anthropic"},
        ],
        "models": [
            {"id": "shared", "provider": "alpha", "name": "Manual Name", "capabilities": {"tools": True, "vision": False}, "compat": {"manual": True}},
            {"id": "manual-only", "provider": "alpha"},
            {"id": "shared", "provider": "beta"},
        ],
    }


def test_discovery_merge_status_defaults_and_same_model_ids():
    async def scenario():
        async def openai(provider):
            return [
                {"id": "shared", "name": "Remote Name", "created": 123, "owned_by": "org", "defaults": {"context_window": 65536, "capabilities": {"vision": True}, "compat": {"remote": True}}},
                {"id": "remote-only"},
            ]

        async def anthropic(provider):
            return [{"id": "shared"}]

        catalog = ModelCatalog(config_from_dict(config_data()), {"openai": openai, "anthropic": anthropic})
        snapshot = await catalog.refresh()
        assert set(snapshot.models) == {"alpha/shared", "alpha/manual-only", "alpha/remote-only", "beta/shared"}
        merged = snapshot.get("alpha/shared")
        assert merged.status == "configured_and_discovered"
        assert merged.config.name == "Manual Name"
        assert merged.config.context_window == 65536
        assert merged.config.capabilities.tools is True
        assert merged.config.capabilities.vision is False
        assert dict(merged.config.compat) == {"remote": True, "manual": True}
        assert snapshot.get("alpha/manual-only").status == "configured"
        discovered = snapshot.get("alpha/remote-only")
        assert discovered.status == "discovered"
        assert discovered.config.context_window == 32768
        assert discovered.config.max_output_tokens == 4096
        assert not any(vars(discovered.config.capabilities).values())

    asyncio.run(scenario())


def test_provider_failure_is_isolated_and_manual_models_remain():
    async def scenario():
        async def failed(provider):
            raise RuntimeError("temporary discovery failure")

        async def working(provider):
            return [{"id": "shared"}]

        catalog = ModelCatalog(config_from_dict(config_data()), {"openai": failed, "anthropic": working})
        snapshot = await catalog.refresh()
        assert snapshot.get("alpha/shared").status == "unavailable"
        assert snapshot.get("alpha/manual-only").status == "unavailable"
        assert snapshot.get("beta/shared").status == "configured_and_discovered"
        assert "temporary discovery failure" in snapshot.errors["alpha"]

    asyncio.run(scenario())


def test_concurrent_refresh_reuses_provider_task_and_old_generation_cannot_overwrite():
    async def scenario():
        started = asyncio.Event()
        release = asyncio.Event()
        calls = 0

        async def discover(provider):
            nonlocal calls
            calls += 1
            started.set()
            await release.wait()
            return [{"id": f"generation-{calls}"}]

        async def anthropic(provider):
            return []

        catalog = ModelCatalog(config_from_dict(config_data()), {"openai": discover, "anthropic": anthropic})
        first = asyncio.create_task(catalog.refresh())
        await started.wait()
        second = asyncio.create_task(catalog.refresh())
        await asyncio.sleep(0)
        release.set()
        await asyncio.gather(first, second)
        assert calls == 1
        assert catalog.snapshot.generation == 2
        assert "alpha/generation-1" in catalog.snapshot.models
        with pytest.raises(TypeError):
            catalog.snapshot.models["new"] = None

    asyncio.run(scenario())
