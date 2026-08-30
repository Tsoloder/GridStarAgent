import os
from typing import Optional

import httpx

from ..types import ProviderConfig


class Provider:
    auth_header: Optional[str] = None
    auth_scheme: Optional[str] = None

    def __init__(self, config: ProviderConfig, *, transport=None):
        self.config = config
        self._transport = transport
        self._client: Optional[httpx.AsyncClient] = None

    def api_key(self) -> str:
        if self.config.api_key_env:
            value = os.getenv(self.config.api_key_env, "").strip()
            if value:
                return value
        return self.config.api_key

    def headers(self) -> dict[str, str]:
        headers = dict(self.config.headers)
        key = self.api_key()
        if key and self.auth_header:
            value = f"{self.auth_scheme} {key}" if self.auth_scheme else key
            headers[self.auth_header] = value
        return headers

    def client(self) -> httpx.AsyncClient:
        if self._client is None or self._client.is_closed:
            timeout = httpx.Timeout(self.config.timeout, connect=self.config.connect_timeout)
            self._client = httpx.AsyncClient(
                base_url=self.config.base_url.rstrip("/"), headers=self.headers(),
                timeout=timeout, verify=self.config.ssl_verify, transport=self._transport,
            )
        return self._client

    async def aclose(self) -> None:
        if self._client is not None:
            await self._client.aclose()

    async def discover_models(self) -> list[dict]:
        response = await self.client().get("/models")
        response.raise_for_status()
        return list(response.json().get("data", []))
