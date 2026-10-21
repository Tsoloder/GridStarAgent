"""知识库工具：目录、检索、读原文。

工具只做三件事，刻意不做第四件事（比如「总结一下知识库」），因为检索的价值
在于把原文交给模型判断，而不是替模型预判。返回值按「定位 + 片段」给出，模型
必须再用 kb_read 取原文才能作答，提示词里也写死了这条规矩。

**库隔离**：当前会话用哪个库由上下文变量决定，取值有三种含义——
``None`` 表示没设过（按「全部库」处理）、``""`` 表示本轮明确不用知识库、
库 id 表示只看这个库、``*`` 表示跨库检索。隔离做在 kb_index 的 SQL 里，
这里只负责把「没启用」翻译成人话，不让模型去猜。
"""
from __future__ import annotations

import json
from contextvars import ContextVar
from typing import Any, Optional

import kb_index
from skill_runtime import RuntimeTool

KB_SEARCH_NAME = "kb_search"
KB_READ_NAME = "kb_read"
KB_CATALOG_NAME = "kb_catalog"

KB_TOOL_NAMES = {KB_SEARCH_NAME, KB_READ_NAME, KB_CATALOG_NAME}

# 当前会话选中的知识库。用 ContextVar 而不是模块全局变量：后台会话并发跑在
# 不同任务里，全局变量会让两个会话互相覆盖对方的库。
_ACTIVE_LIBRARY: ContextVar[Optional[str]] = ContextVar("kb_active_library", default=None)


def set_active_library(library_id: Optional[str]):
    return _ACTIVE_LIBRARY.set(library_id)


def reset_active_library(token) -> None:
    try:
        _ACTIVE_LIBRARY.reset(token)
    except (ValueError, RuntimeError):
        # token 来自别的上下文就重置不了，兜底把值清回未设置
        _ACTIVE_LIBRARY.set(None)


def active_library() -> Optional[str]:
    return _ACTIVE_LIBRARY.get()


def _scope() -> Optional[str]:
    """把上下文里的选择翻成 kb_index 认的过滤值。"""
    value = _ACTIVE_LIBRARY.get()
    if value is None:
        return kb_index.LIBRARY_ALL
    return value


def _disabled_message() -> str:
    return (
        "本轮没有启用知识库（输入框下方的知识库选择为「不使用」）。"
        "需要查手册时，请提示用户在输入框下方选一个知识库，或到设置中心的知识库页导入资料；"
        "不要用自身知识编造手册内容。"
    )


KB_SEARCH_TOOL = RuntimeTool(
    name=KB_SEARCH_NAME,
    description=(
        "在本地知识库（GridStar 用户手册、快速入门、CFD 资料、参数说明）中检索。"
        "只搜当前会话选中的那个知识库。返回命中片段的定位（文档名、章节路径、页码）"
        "和摘录，可能同时返回参数卡片。"
        "凡涉及软件操作步骤、功能含义、参数取值与默认值的问题，先调用本工具，"
        "不要凭记忆回答。命中后必须用 kb_read 取原文再作答；"
        "检索结果为空就明确告诉用户「知识库未收录」，不要用自身知识冒充手册内容。"
    ),
    inputSchema={
        "type": "object",
        "properties": {
            "query": {
                "type": "string",
                "description": "检索词。用手册里的说法，例如「附面层 首层高度」「设置边界 新建组」",
            },
            "top_k": {"type": "integer", "description": "返回块数，默认 5，最大 10"},
            "doc_id": {"type": "string", "description": "只在某份文档内检索，可省略"},
            "include_params": {
                "type": "boolean",
                "description": "是否附带参数表命中（默认 true）",
            },
        },
        "required": ["query"],
    },
)

KB_READ_TOOL = RuntimeTool(
    name=KB_READ_NAME,
    description=(
        "读取知识库中某一块的完整原文（可带前后各若干块）。"
        "kb_search 返回的 chunk_id 直接用；回答参数值、操作步骤、注意事项之前必须调用本工具，"
        "因为检索结果里的摘要可能截断在关键数字之后。"
    ),
    inputSchema={
        "type": "object",
        "properties": {
            "chunk_id": {"type": "integer", "description": "kb_search 返回的 chunk_id"},
            "context": {
                "type": "integer",
                "description": "额外读取前后各几块（默认 0，最大 2）",
            },
        },
        "required": ["chunk_id"],
    },
)

KB_CATALOG_TOOL = RuntimeTool(
    name=KB_CATALOG_NAME,
    description=(
        "列出知识库收录了哪些文档、每份文档有哪些章节（只有标题与页码，不含正文）。"
        "不知道资料里有没有相关内容、或想按主题找入口时先用本工具看全景。"
    ),
    inputSchema={
        "type": "object",
        "properties": {
            "prefix": {"type": "string", "description": "只看包含该关键词的文档或章节，可省略"},
        },
    },
)


def _lib_name(library_id: str) -> str:
    try:
        item = kb_index.get_library(library_id)
    except Exception:  # noqa: BLE001
        item = None
    return item["name"] if item else ""


def _fmt_param(row: dict[str, Any]) -> str:
    head = "[参数] %s" % row["name"]
    meta = " · ".join(item for item in (row.get("domain") or "", row.get("module") or "") if item)
    tail = "%s · P%s" % (row.get("doc") or "", row.get("page") or "?")
    value = []
    if row.get("default"):
        value.append("默认 %s" % row["default"])
    if row.get("min") is not None or row.get("max") is not None:
        low = "" if row.get("min") is None else ("%g" % row["min"])
        high = "" if row.get("max") is None else ("%g" % row["max"])
        value.append("范围 %s~%s" % (low, high))
    if row.get("unit"):
        value.append(row["unit"])
    lines = [head + ("（%s）" % meta if meta else "") + "  " + tail]
    if value:
        lines.append("    " + " / ".join(value))
    lines.append("    " + (row.get("description") or "")[:200])
    return "\n".join(lines)


def _fmt_hit(index: int, total: int, hit: dict[str, Any]) -> str:
    heading = (hit.get("heading_path") or "").replace(" / ", " > ")
    library = hit.get("library") or ""
    origin = "%s · %s · P%s" % (hit["doc"], heading, hit["page"])
    if library:
        origin = "［%s］%s" % (library, origin)
    lines = [
        "[命中 %d/%d] chunk_id=%s  score=%.2f  %s"
        % (index, total, hit["chunk_id"], hit["score"], origin),
        "    " + (hit.get("snippet") or "").replace("\n", " "),
    ]
    return "\n".join(lines)


def search_tool(args: dict[str, Any]) -> str:
    scope = _scope()
    if scope == "":
        return _disabled_message()

    query = str(args.get("query", "") or "").strip()
    if not query:
        return "缺少 query 参数。"
    top_k = args.get("top_k")
    try:
        top_k = max(1, min(10, int(top_k))) if top_k is not None else 5
    except (TypeError, ValueError):
        top_k = 5
    doc_id = str(args.get("doc_id", "") or "").strip() or None
    include_params = args.get("include_params", True)
    if isinstance(include_params, str):
        include_params = include_params.strip().lower() not in {"0", "false", "no"}

    try:
        hits = kb_index.search(query, top_k=top_k, doc_id=doc_id, library_id=scope)
    except Exception as exc:  # noqa: BLE001 - 知识库故障不该打断主任务
        return "知识库检索失败（%s: %s）。当前无法依据知识库回答，请如实告知用户。" % (
            type(exc).__name__, exc)

    blocks: list[str] = []
    if include_params:
        try:
            params = kb_index.find_params(query, limit=3, library_id=scope)
        except Exception:  # noqa: BLE001
            params = []
        for row in params:
            blocks.append(_fmt_param(row))

    if hits:
        scope_label = ""
        if scope and scope != kb_index.LIBRARY_ALL:
            scope_label = "（知识库：%s）" % (_lib_name(scope) or scope)
        blocks.append("检索到 %d 个相关片段%s（原文用 kb_read 取）：" % (len(hits), scope_label))
        for order, hit in enumerate(hits, start=1):
            blocks.append(_fmt_hit(order, len(hits), hit))
    elif not blocks:
        where = ""
        if scope and scope != kb_index.LIBRARY_ALL:
            where = "当前知识库「%s」" % (_lib_name(scope) or scope)
        else:
            where = "全部知识库"
        return (
            "%s未收录与「%s」相关的内容。请告诉用户这条资料没有收录，"
            "不要用自身知识编造手册内容。可用 kb_catalog 查看收录范围。" % (where, query)
        )
    return "\n".join(blocks)


def read_tool(args: dict[str, Any]) -> str:
    scope = _scope()
    if scope == "":
        return _disabled_message()

    raw = args.get("chunk_id")
    try:
        chunk_id = int(raw)
    except (TypeError, ValueError):
        return "chunk_id 必须是整数（取 kb_search 返回的 chunk_id）。"
    try:
        context = int(args.get("context") or 0)
    except (TypeError, ValueError):
        context = 0
    context = max(0, min(2, context))

    try:
        payload = kb_index.read_chunk(chunk_id, context=context, library_id=scope)
    except Exception as exc:  # noqa: BLE001
        return "读取失败（%s: %s）。" % (type(exc).__name__, exc)
    if payload is None:
        return (
            "未找到 chunk_id=%s 的片段：可能知识库已重建，或它属于另一个知识库。"
            "请重新 kb_search。" % chunk_id
        )

    heading = (payload.get("heading_path") or "").replace(" / ", " > ")
    lines = [
        "《%s》 %s P%s–%s  [chunk_id=%s]"
        % (payload["doc"], heading, payload["page"],
           payload.get("page_end") or payload["page"], payload["chunk_id"]),
        "",
        payload["text"],
    ]
    for key, label in (("context_before", "前一块"), ("context_after", "后一块")):
        for item in payload.get(key) or []:
            lines.append("")
            lines.append("【%s chunk_id=%s】%s" % (label, item["chunk_id"], item["text"]))
    lines.append("")
    lines.append("引用时请带上《文档名》章节与页码。")
    return "\n".join(lines)


def catalog_tool(args: dict[str, Any]) -> str:
    scope = _scope()
    if scope == "":
        return _disabled_message()

    prefix = str(args.get("prefix", "") or "").strip()
    try:
        data = kb_index.catalog(prefix, library_id=scope)
    except Exception as exc:  # noqa: BLE001
        return "读取知识库目录失败（%s: %s）。" % (type(exc).__name__, exc)

    documents = data.get("documents") or []
    if not documents:
        if scope and scope != kb_index.LIBRARY_ALL:
            return "知识库「%s」还是空的，或没有匹配「%s」的文档。" % (
                _lib_name(scope) or scope, prefix or "全部")
        return "还没有任何知识库收录资料，或没有匹配「%s」的文档。" % (prefix or "全部")

    lines = ["共 %d 份文档：" % data.get("total", len(documents))]
    for doc in documents:
        lines.append("")
        origin = "《%s》 doc_id=%s  %d 页 / %d 块" % (
            doc["doc"], doc["doc_id"], doc.get("pages") or 0, doc.get("chunks") or 0)
        if doc.get("library"):
            origin = "［%s］%s" % (doc["library"], origin)
        lines.append(origin)
        if doc.get("folder"):
            lines.append("   归档：%s" % doc["folder"])
        for section in doc.get("sections") or []:
            lines.append("   %s  (P%s)" % (section["heading"], section["page"]))
        if not doc.get("sections"):
            lines.append("   （无章节结构）")
    lines.append("")
    lines.append("用 kb_search 检索具体内容，用 doc_id 限定在某份文档内。")
    return "\n".join(lines)


_EXECUTORS = {
    KB_SEARCH_NAME: search_tool,
    KB_READ_NAME: read_tool,
    KB_CATALOG_NAME: catalog_tool,
}


def is_kb_tool(name: str) -> bool:
    return name in KB_TOOL_NAMES


def execute(name: str, args: dict[str, Any]) -> str:
    handler = _EXECUTORS.get(name)
    if handler is None:
        return "未知的知识库工具：%s" % name
    if not isinstance(args, dict):
        try:
            args = json.loads(args or "{}")
        except (TypeError, ValueError):
            args = {}
    return handler(args)


def tools() -> list[RuntimeTool]:
    return [KB_SEARCH_TOOL, KB_READ_TOOL, KB_CATALOG_TOOL]