import logging
import os
from typing import Optional, Union

import httpx

from ..types import ProviderConfig

logger = logging.getLogger(__name__)


def _resolve_ssl_verify(ssl_verify: Union[bool, str]) -> Union[bool, str]:
    """将 ssl_verify 配置值转换为 httpx verify 参数。

    - True: 使用系统默认 CA 验证
    - False: 跳过验证（仅内网测试）
    - str: CA 证书文件路径
    """
    if isinstance(ssl_verify, bool):
        if not ssl_verify:
            logger.warning("SSL certificate verification is DISABLED for provider")
        return ssl_verify
    # 字符串：CA 证书路径
    path = ssl_verify.strip()
    if not os.path.isfile(path):
        logger.error("ssl_verify CA cert file not found: %s — falling back to default verification", path)
        return True
    logger.info("Using custom CA certificate for SSL verification: %s", path)
    return path


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
            verify = _resolve_ssl_verify(self.config.ssl_verify)
            self._client = httpx.AsyncClient(
                base_url=self.config.base_url.rstrip("/"), headers=self.headers(),
                timeout=timeout, verify=verify, transport=self._transport,
            )
        return self._client

    async def aclose(self) -> None:
        if self._client is not None:
            await self._client.aclose()

    async def discover_models(self) -> list[dict]:
        response = await self.client().get("/models")
        response.raise_for_status()
        return list(response.json().get("data", []))
