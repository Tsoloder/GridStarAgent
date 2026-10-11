"""Persistent user-confirmed defaults for MCP tool parameters."""
import html
import json
import re
from datetime import datetime
from pathlib import Path
from typing import Dict, Mapping, Optional

from paths import DATA_DIR
from session import atomic_write

MEMORY_PATH = DATA_DIR / "tool_parameter_memory.json"
_TRANSIENT = re.compile(
    r"(path|file|filename|dirname|ids?$|id$|coord|point|endpoint|startpoint|outids|"
    r"api[_-]?key|token|password|passwd|secret|credential)", re.I
)


def _load() -> dict:
    if not MEMORY_PATH.exists():
        return {}
    try:
        data = json.loads(MEMORY_PATH.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def _save(data: dict) -> None:
    atomic_write(str(MEMORY_PATH), json.dumps(data, ensure_ascii=False, indent=2))


def _rememberable(name: str, value) -> bool:
    if not name or _TRANSIENT.search(name):
        return False
    return isinstance(value, (str, int, float, bool))


def _properties(schema: Optional[Mapping[str, object]]) -> dict:
    if not isinstance(schema, Mapping):
        return {}
    props = schema.get("properties", {})
    return props if isinstance(props, Mapping) else {}


def _declares_parameters(schema: Optional[Mapping[str, object]]) -> bool:
    """schema 是否是一份权威的对象声明。

    零参数工具的 schema 长这样：{"type": "object", "properties": {}}。
    properties 为空但依然是权威声明：任何不在 properties 里的键都不该被记住、
    更不该被注回调用——服务端的参数校验会以 Unexpected keyword argument 直接
    拒绝整次调用（CaptureGridStarWindow 就这样被拦下过四次）。
    只有 schema 缺失或形状不像对象声明时，才当作"信息不足、全部保留"。
    """
    if not isinstance(schema, Mapping):
        return False
    return schema.get("type") == "object" and isinstance(schema.get("properties"), Mapping)


def _coerce(value, schema: Mapping[str, object]):
    kind = schema.get("type")
    try:
        if kind == "integer" and isinstance(value, str):
            return int(value.strip())
        if kind == "number" and isinstance(value, str):
            return float(value.strip())
        if kind == "boolean" and isinstance(value, str):
            lowered = value.strip().lower()
            if lowered in {"true", "1", "yes", "on"}:
                return True
            if lowered in {"false", "0", "no", "off"}:
                return False
    except (TypeError, ValueError):
        return value
    return value


def _filter_params(params: Mapping[str, object], schema: Optional[Mapping[str, object]]) -> dict:
    properties = _properties(schema)
    authoritative = _declares_parameters(schema)
    result = {}
    for name, value in params.items():
        key = str(name)
        if not _rememberable(key, value):
            continue
        if authoritative and key not in properties:
            # Tool schemas can change between application versions. Silently
            # discard stale remembered fields rather than breaking the Agent.
            continue
        result[key] = _coerce(value, properties.get(key, {}))
    return result


def remember(tool: str, params: Mapping[str, object], schema: Optional[Mapping[str, object]] = None) -> Dict[str, object]:
    tool = str(tool or "").strip()
    if not tool:
        return {}
    data = _load()
    entry = data.get(tool, {}) if isinstance(data.get(tool), dict) else {}
    existing = entry.get("params", {}) if isinstance(entry.get("params"), dict) else {}
    values = _filter_params(existing, schema)
    values.update(_filter_params(params, schema))
    if values:
        data[tool] = {"params": values, "updated_at": datetime.now().isoformat()}
        _save(data)
    elif tool in data:
        # 记忆里的字段已被 schema 淘汰（例如工具改成零参数）：顺手清掉，
        # 免得留着一条永远用不上的陈旧记录。
        data.pop(tool, None)
        _save(data)
    return values


def get(tool: str, schema: Optional[Mapping[str, object]] = None) -> Dict[str, object]:
    entry = _load().get(str(tool or ""), {})
    values = entry.get("params", {}) if isinstance(entry, dict) else {}
    if not isinstance(values, dict):
        return {}
    return _filter_params(values, schema)


def normalize(tool: str, args: Mapping[str, object], schema: Optional[Mapping[str, object]] = None) -> dict:
    """Keep all current call args while coercing schema-declared scalar values."""
    properties = _properties(schema)
    result = {}
    for name, value in dict(args or {}).items():
        key = str(name)
        result[key] = _coerce(value, properties.get(key, {}))
    return result


def merge(tool: str, args: Mapping[str, object], schema: Optional[Mapping[str, object]] = None) -> dict:
    """Merge explicit args, confirmed memory, then schema defaults in that order.

    schema 是权威对象声明时，显式传入但未声明的键同样丢弃：多余的键会让服务端
    以 Unexpected keyword argument 拒绝整次调用，宁可在本地先扔掉（记忆、审批
    回填、模型自由发挥都可能塞进这种键）。schema 缺失时保持原样放行。
    """
    properties = _properties(schema)
    authoritative = _declares_parameters(schema)
    result = {}
    for name, value in dict(args or {}).items():
        key = str(name)
        if authoritative and key not in properties:
            continue
        result[key] = _coerce(value, properties.get(key, {}))
    for name, value in get(tool, schema).items():
        result.setdefault(name, value)
    for name, property_schema in properties.items():
        if isinstance(property_schema, Mapping) and "default" in property_schema:
            result.setdefault(name, property_schema["default"])
    return result


def catalog(schemas: Optional[Mapping[str, Mapping[str, object]]] = None) -> str:
    data = _load()
    schemas = schemas or {}
    lines = []
    for tool, entry in sorted(data.items()):
        values = get(tool, schemas.get(tool))
        if values:
            payload = html.escape(
                json.dumps(values, ensure_ascii=False, sort_keys=True), quote=False
            )
            lines.append("<tool name=\"%s\">%s</tool>" % (
                html.escape(tool, quote=True), payload))
    if not lines:
        return ""
    return "<tool_parameter_memory>\n%s\n</tool_parameter_memory>" % "\n".join(lines)


def catalog_all() -> Dict[str, object]:
    """给 API 层用的结构化读取。

    与上面的 ``catalog(schemas)`` 是两回事：那个渲染成 ``<tool_parameter_memory>``
    给提示词用，返回字符串；这个返回 dict 给设置页用。同名会让后来者覆盖前者，
    ``agent_loop`` 就会拿到一个 dict 去拼字符串——所以两个名字必须分开。
    """
    data = _load()
    tools = []
    field_count = 0
    for name, entry in sorted(data.items()):
        values = entry.get("params") if isinstance(entry, dict) else None
        if not isinstance(values, dict):
            continue
        fields = {
            str(key): value
            for key, value in sorted(values.items())
            if isinstance(value, (str, int, float, bool))
        }
        if not fields:
            continue
        field_count += len(fields)
        tools.append({
            "tool": str(name),
            "updated_at": str(entry.get("updated_at", "")),
            "fields": fields,
        })
    return {"tools": tools, "tool_count": len(tools), "field_count": field_count}


def delete_fields(tool: str, fields) -> int:
    """删除某工具下的若干字段，返回真正删掉的数量。字段删空则工具条目整体消失。"""
    tool = str(tool or "").strip()
    if not tool:
        return 0
    wanted = {str(name) for name in fields if str(name).strip()}
    if not wanted:
        return 0
    data = _load()
    entry = data.get(tool)
    values = entry.get("params") if isinstance(entry, dict) else None
    if not isinstance(values, dict):
        return 0
    removed = 0
    for name in wanted:
        if name in values:
            values.pop(name)
            removed += 1
    if removed:
        if values:
            entry["params"] = values
            entry["updated_at"] = datetime.now().isoformat()
            data[tool] = entry
        else:
            data.pop(tool, None)
        _save(data)
    return removed


def counts() -> Dict[str, int]:
    info = catalog_all()
    return {"tools": int(info["tool_count"]), "fields": int(info["field_count"])}


def clear(tool: str = "") -> int:
    """清空指定工具或全部记忆，返回被删除的字段数（旧调用方忽略返回值即可）。"""
    data = _load()
    if tool:
        entry = data.get(tool)
        values = entry.get("params") if isinstance(entry, dict) else None
        removed = len(values) if isinstance(values, dict) else 0
        data.pop(tool, None)
    else:
        removed = 0
        for entry in data.values():
            if isinstance(entry, dict) and isinstance(entry.get("params"), dict):
                removed += len(entry["params"])
        data = {}
    _save(data)
    return removed
