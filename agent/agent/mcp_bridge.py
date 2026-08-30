import asyncio
import logging
import re

from fastmcp import Client

logger = logging.getLogger(__name__)


def _is_query_tool(name: str) -> bool:
    return bool(re.match(r"^(Get|Query|List|Find|Check|Read|Is|Has)", name, re.I))


class McpOperationStatusUnknown(RuntimeError):
    def __init__(self, name: str, cause: Exception):
        super().__init__(
            "MCP operation '%s' failed after dispatch; execution status is unknown: %s" %
            (name, cause)
        )
        self.tool_name = name
        self.__cause__ = cause


class McpBridge:
    def __init__(self, script: str):
        self._script = script
        self._lock = asyncio.Lock()
        self._entered = False
        self._cached_tools = []

    async def _list_tools_once(self):
        async with Client(self._script) as client:
            return await client.list_tools()

    async def _call_tool_once(self, name: str, args: dict) -> str:
        async with Client(self._script) as client:
            result = await client.call_tool(name, args)
            return result.content[0].text if result.content else ""

    async def connect(self):
        tools = await self.list_tools()
        self._entered = True
        logger.info(f"mcp connected, {len(tools)} tools available")

    async def disconnect(self):
        self._entered = False

    async def list_tools(self):
        async with self._lock:
            tools = await self._list_tools_once()
            self._cached_tools = tools
            return tools

    async def call_tool(self, name: str, args: dict) -> str:
        async with self._lock:
            try:
                return await self._call_tool_once(name, args)
            except Exception as e:
                if not _is_query_tool(name):
                    logger.warning(
                        "mcp operation '%s' failed after dispatch; not retrying because status is unknown: %s",
                        name, e,
                    )
                    raise McpOperationStatusUnknown(name, e) from e
                logger.warning("mcp query '%s' failed: %s, retrying once", name, e)
                return await self._call_tool_once(name, args)

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
