import asyncio
import logging

from fastmcp import Client

logger = logging.getLogger(__name__)


class McpBridge:
    def __init__(self, script: str):
        self._script = script
        self._client = Client(script)
        self._lock = asyncio.Lock()
        self._entered = False
        self._cached_tools = []
        self._reconnect_attempts = 0
        self._max_reconnect_attempts = 3

    async def connect(self):
        await self._client.__aenter__()
        self._entered = True
        self._reconnect_attempts = 0
        tools = await self.list_tools()
        self._cached_tools = tools
        logger.info(f"mcp connected, {len(tools)} tools available")

    async def disconnect(self):
        if self._entered:
            try:
                await self._client.__aexit__(None, None, None)
            except Exception as e:
                logger.warning(f"mcp disconnect error: {e}")
            self._entered = False

    async def _reconnect(self) -> bool:
        """尝试重新连接 MCP server。

        在 call_tool 或 list_tools 失败时自动调用，
        避免因 MCP server 重启导致整个后端不可用。
        """
        if self._reconnect_attempts >= self._max_reconnect_attempts:
            logger.error("mcp reconnect failed: max attempts reached")
            return False

        self._reconnect_attempts += 1
        logger.info(f"mcp reconnecting (attempt {self._reconnect_attempts}/{self._max_reconnect_attempts})")

        # 先断开旧连接（忽略错误）
        if self._entered:
            try:
                await self._client.__aexit__(None, None, None)
            except Exception:
                pass
            self._entered = False

        try:
            # 创建新的 Client 实例（旧的可能已失效）
            self._client = Client(self._script)
            await self._client.__aenter__()
            self._entered = True
            self._reconnect_attempts = 0
            tools = await self._client.list_tools()
            self._cached_tools = tools
            logger.info(f"mcp reconnected, {len(tools)} tools available")
            return True
        except Exception as e:
            logger.warning(f"mcp reconnect failed: {e}")
            return False

    async def list_tools(self):
        async with self._lock:
            try:
                return await self._client.list_tools()
            except Exception as e:
                logger.warning(f"mcp list_tools failed: {e}, attempting reconnect")
                if await self._reconnect():
                    return await self._client.list_tools()
                raise

    async def call_tool(self, name: str, args: dict) -> str:
        async with self._lock:
            try:
                result = await self._client.call_tool(name, args)
                return result.content[0].text if result.content else ""
            except Exception as e:
                logger.warning(f"mcp call_tool '{name}' failed: {e}, attempting reconnect")
                if await self._reconnect():
                    result = await self._client.call_tool(name, args)
                    return result.content[0].text if result.content else ""
                raise

    @property
    def connected(self) -> bool:
        return self._entered

    def available_tools(self) -> list:
        return self._cached_tools

    def tool_schema(self, name: str) -> dict:
        for tool in self._cached_tools:
            if tool.name == name:
                schema = getattr(tool, "inputSchema", None)
                return schema if isinstance(schema, dict) else {}
        return {}

    def tool_schemas(self) -> dict:
        return {tool.name: self.tool_schema(tool.name) for tool in self._cached_tools}
