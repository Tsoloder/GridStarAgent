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
    result = {}
    for name, value in params.items():
        key = str(name)
        if not _rememberable(key, value):
            continue
        if properties and key not in properties:
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
    """Merge explicit args, confirmed memory, then schema defaults in that order."""
    result = dict(args or {})
    for name, value in get(tool, schema).items():
        result.setdefault(name, value)
    for name, property_schema in _properties(schema).items():
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


def clear(tool: str = "") -> None:
    data = _load()
    if tool:
        data.pop(tool, None)
    else:
        data = {}
    _save(data)
