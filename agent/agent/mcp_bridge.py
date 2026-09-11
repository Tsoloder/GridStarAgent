import asyncio
import json
import logging
import re

from fastmcp import Client

logger = logging.getLogger(__name__)

# 服务端的工具发现入口，返回工具分组目录（见 agent/registry.py）。
TOOL_GROUPS_TOOL = "GetToolGroups"


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
        self._tool_groups = []
        self._tool_group_index = {}
        self._default_group_ids = []

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
            self._entered = True
            await self._load_tool_groups(tools)
            return tools

    async def _load_tool_groups(self, tools):
        """尽力拉取服务端工具分组目录。

        失败或服务端未提供 GetToolGroups（旧版本）时清空分组信息，
        调用方据此退回"全量暴露工具"的旧行为。
        """
        self._tool_groups = []
        self._tool_group_index = {}
        self._default_group_ids = []
        known = {getattr(tool, "name", "") for tool in tools}
        if TOOL_GROUPS_TOOL not in known:
            return
        try:
            raw = await self._call_tool_once(TOOL_GROUPS_TOOL, {})
            payload = json.loads(raw)
        except Exception as e:
            logger.warning("failed to load tool groups, exposing all tools instead: %s", e)
            return
        if not isinstance(payload, dict):
            return
        raw_groups = payload.get("groups")
        raw_defaults = payload.get("default_enabled")
        index = {}
        groups = []
        for group in raw_groups if isinstance(raw_groups, list) else []:
            if not isinstance(group, dict):
                continue
            group_id = group.get("id")
            names = group.get("tools")
            if not isinstance(group_id, str) or not group_id or not isinstance(names, list):
                continue
            # 只保留服务端确实注册了的工具名，避免分组目录与工具列表不一致。
            tool_names = [n for n in names if isinstance(n, str) and n in known]
            if not tool_names:
                continue
            groups.append({
                "id": group_id,
                "description": group.get("description") or "",
                "tools": tool_names,
            })
            for name in tool_names:
                index[name] = group_id
        self._tool_groups = groups
        self._tool_group_index = index
        self._default_group_ids = [
            gid for gid in (raw_defaults if isinstance(raw_defaults, list) else [])
            if isinstance(gid, str) and any(g["id"] == gid for g in groups)
        ]

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

    def tool_groups(self) -> list:
        """服务端工具分组目录；为空表示无分组信息（调用方应全量暴露工具）。"""
        return self._tool_groups

    def default_group_ids(self) -> list:
        """服务端声明的默认启用分组 id。"""
        return self._default_group_ids

    def group_for_tool(self, name: str):
        """按工具名反查分组 id，无分组信息或未命中返回 None。"""
        return self._tool_group_index.get(name)

    def tool_schema(self, name: str) -> dict:
        for tool in self._cached_tools:
            if tool.name == name:
                schema = getattr(tool, "inputSchema", None)
                return schema if isinstance(schema, dict) else {}
        return {}

    def tool_schemas(self) -> dict:
        return {tool.name: self.tool_schema(tool.name) for tool in self._cached_tools}
