from .base import Provider


class AnthropicProvider(Provider):
    auth_header = "x-api-key"

    def headers(self) -> dict[str, str]:
        headers = super().headers()
        headers.setdefault("anthropic-version", "2023-06-01")
        return headers

    async def discover_models(self) -> list[dict]:
        response = await self.client().get("/models")
        response.raise_for_status()
        return list(response.json().get("data", []))
