"""会话任务台账（Task Ledger）

长会话中消息历史会被压缩，模型会丢失"干过什么、计划到哪了"。台账独立于消息历史：

- plan: 阶段级计划，模型通过 update_plan 工具显式维护（全量替换语义）
- calls: 工具执行账本，后端在每次工具调用后自动记录（ID 列表全量保留）

落盘于 sessions/<session_id>/ledger.json；每轮请求前渲染为 <task_progress>
快照注入上下文（只进请求，不写回 session.messages）。
"""
import json
import logging
import os
import re
from datetime import datetime

from session import atomic_write, session_dir

logger = logging.getLogger(__name__)

PLAN_STATUSES = ("pending", "in_progress", "done", "failed", "skipped")

# ID 列表单参数上限，超过截断（极端情况保护）
ARGS_LIST_CAP = 500
# 长文本参数截断长度（路径等）
ARGS_TEXT_LIMIT = 100
# 结果文本截断长度
RESULT_TEXT_LIMIT = 200
# 注入快照中保留的最近调用明细条数
RECENT_CALLS_IN_SNAPSHOT = 8
# 注入快照字符预算（约 800 token）
SNAPSHOT_CHAR_BUDGET = 2400

_STATUS_ICONS = {
    "done": "✅",
    "in_progress": "▶",
    "pending": "⬜",
    "failed": "❌",
    "skipped": "⏭",
}
_STATUS_LABELS = {
    "done": "完成",
    "in_progress": "进行中",
    "pending": "待开始",
    "failed": "失败",
    "skipped": "已跳过",
}

_CAMEL_SPLIT_RE = re.compile(r"[^0-9a-zA-Z]+|(?<=[a-z])(?=[A-Z])")


def _is_id_key(key) -> bool:
    """键的最后一个词素是 id/ids 时视为 ID 字段。

    匹配 "id" / "surface_ids" / "objectId"；不误伤 "valid" / "solid"。
    """
    tokens = [t for t in _CAMEL_SPLIT_RE.split(str(key)) if t]
    return bool(tokens) and tokens[-1].lower() in ("id", "ids")


def _truncate(text: str, limit: int) -> str:
    text = str(text)
    if len(text) <= limit:
        return text
    return text[:limit] + "…"


def digest_value(value):
    """参数值摘要：字符串/数字数组视为 ID 列表全量保留（超上限截断），
    长字符串截断，数值/布尔原样保留。"""
    if isinstance(value, (bool, int, float)) or value is None:
        return value
    if isinstance(value, str):
        return _truncate(value, ARGS_TEXT_LIMIT)
    if isinstance(value, list):
        if all(isinstance(item, (str, int, float)) and not isinstance(item, bool) for item in value):
            if len(value) <= ARGS_LIST_CAP:
                return list(value)
            return list(value[:ARGS_LIST_CAP]) + ["...+%d 项已截断" % (len(value) - ARGS_LIST_CAP)]
        return "[%d 项]" % len(value)
    if isinstance(value, dict):
        return {key: digest_value(item) for key, item in value.items()}
    return _truncate(str(value), ARGS_TEXT_LIMIT)


def digest_args(args) -> dict:
    if not isinstance(args, dict):
        return {}
    return {key: digest_value(value) for key, value in args.items()}


def digest_result(result) -> str:
    """结果摘要：JSON 结果优先保留 ID 类字段，其次取文本字段；纯文本取头部。"""
    text = str(result or "").strip()
    if not text:
        return ""
    try:
        obj = json.loads(text)
    except (json.JSONDecodeError, ValueError, TypeError):
        return _truncate(text, RESULT_TEXT_LIMIT)
    if isinstance(obj, dict):
        id_fields = {key: digest_value(value) for key, value in obj.items() if _is_id_key(key)}
        if id_fields:
            return _truncate(json.dumps(id_fields, ensure_ascii=False), RESULT_TEXT_LIMIT)
        for key in ("message", "msg", "status", "result", "content"):
            if key in obj:
                return _truncate(str(obj[key]), RESULT_TEXT_LIMIT)
    return _truncate(text, RESULT_TEXT_LIMIT)


def result_failed(result) -> bool:
    """工具结果是否语义失败：与 workflow_runner._tool_result_failed 同规则。

    JSON 结果为 false、status=error、result=false（布尔或字符串）时视为失败；
    非 JSON 文本不判定（异常路径由调用方显式传 ok=False）。
    """
    try:
        payload = json.loads(result)
    except (TypeError, json.JSONDecodeError, ValueError):
        return False
    if payload is False:
        return True
    if not isinstance(payload, dict):
        return False
    inner = payload.get("result")
    return (
        str(payload.get("status", "")).lower() == "error"
        or inner is False
        or isinstance(inner, str) and inner.strip().lower() == "false"
    )


def normalize_phase(item) -> dict:
    if not isinstance(item, dict):
        raise ValueError("phase must be an object")
    phase_id = str(item.get("id", "")).strip()
    title = str(item.get("title", "")).strip()
    if not phase_id or not title:
        raise ValueError("phase requires non-empty id and title")
    status = str(item.get("status", "pending")).strip()
    if status not in PLAN_STATUSES:
        status = "pending"
    return {
        "id": phase_id,
        "title": title,
        "status": status,
        "note": str(item.get("note", "")).strip(),
    }


class TaskLedger:
    """会话级任务台账：计划状态 + 工具执行账本，独立落盘、每轮注入。"""

    def __init__(self, session_id: str):
        self.session_id = session_id
        self.plan = None  # {"id", "title", "phases": [...]} | None
        self.calls = []   # [{ts, phase, tool, args_digest, ok, result_digest, file_ref}]
        self._pending_flush = 0
        self._load()

    # ---------- 持久化 ----------

    @property
    def path(self) -> str:
        return str(session_dir(self.session_id) / "ledger.json")

    def _load(self):
        if not os.path.exists(self.path):
            return
        try:
            with open(self.path, "r", encoding="utf-8") as f:
                data = json.load(f)
            if isinstance(data.get("plan"), dict):
                self.plan = data["plan"]
            calls = data.get("calls", [])
            if isinstance(calls, list):
                self.calls = [item for item in calls if isinstance(item, dict)]
        except Exception as e:
            logger.warning("ledger load failed (%s): %s", self.session_id, e)

    def flush(self):
        try:
            atomic_write(self.path, json.dumps(
                {"plan": self.plan, "calls": self.calls}, ensure_ascii=False, indent=2,
            ))
            self._pending_flush = 0
        except Exception as e:
            logger.warning("ledger flush failed (%s): %s", self.session_id, e)

    def clear(self):
        self.plan = None
        self.calls = []
        try:
            if os.path.exists(self.path):
                os.unlink(self.path)
        except OSError as e:
            logger.warning("ledger clear failed (%s): %s", self.session_id, e)

    # ---------- 计划 ----------

    def update_plan(self, plan_id: str, title: str, phases: list) -> str:
        """全量替换计划，返回给模型的确认文本。"""
        if not str(plan_id or "").strip():
            raise ValueError("update_plan requires non-empty id")
        if not str(title or "").strip():
            raise ValueError("update_plan requires non-empty title")
        if not isinstance(phases, list) or not phases:
            raise ValueError("update_plan requires non-empty phases")
        self.plan = {
            "id": str(plan_id).strip(),
            "title": str(title).strip(),
            "phases": [normalize_phase(item) for item in phases],
        }
        self.flush()
        total = len(self.plan["phases"])
        done = sum(1 for item in self.plan["phases"] if item["status"] in ("done", "skipped"))
        active = self.active_phase()
        progress = "当前阶段：%s" % active["title"] if active else "无进行中阶段"
        return "计划已更新：%s（%s），%d/%d 阶段完成，%s" % (
            self.plan["id"], self.plan["title"], done, total, progress,
        )

    def active_phase(self):
        """第一个 in_progress 阶段；无计划或无进行中阶段时返回 None。"""
        if not self.plan:
            return None
        for item in self.plan.get("phases", []):
            if item.get("status") == "in_progress":
                return item
        return None

    # ---------- 工具执行账本 ----------

    def record_call(self, tool: str, args, result="", ok=True, file_ref=""):
        self.calls.append({
            "ts": datetime.now().isoformat(timespec="seconds"),
            "phase": (self.active_phase() or {}).get("title", ""),
            "tool": str(tool),
            "args_digest": digest_args(args),
            "ok": bool(ok),
            "result_digest": digest_result(result),
            "file_ref": str(file_ref or ""),
        })
        # 节流落盘：失败必刷，成功每 5 条刷一次
        self._pending_flush += 1
        if not ok or self._pending_flush >= 5:
            self.flush()

    def is_empty(self) -> bool:
        return self.plan is None and not self.calls

    # ---------- 每轮注入快照 ----------

    def render_task_progress(self) -> str:
        if self.is_empty():
            return ""
        lines = ["<task_progress>"]
        if self.plan:
            lines.extend(self._render_plan())
        if self.calls:
            lines.extend(self._render_calls())
        gap = self._render_gap()
        if gap:
            lines.append("【差距】" + gap)
        lines.append("</task_progress>")
        return "\n".join(lines)

    def _render_plan(self) -> list:
        phases = self.plan.get("phases", [])
        total = len(phases)
        done = sum(1 for item in phases if item.get("status") in ("done", "skipped"))
        lines = ["【当前计划】%s（%d/%d 完成）" % (self.plan.get("title", self.plan.get("id", "")), done, total)]
        for item in phases:
            icon = _STATUS_ICONS.get(item.get("status", "pending"), "⬜")
            label = _STATUS_LABELS.get(item.get("status", "pending"), item.get("status", ""))
            entry = "  %s %s" % (icon, item.get("title", item.get("id", "")))
            if item.get("status") == "in_progress":
                entry += "（进行中）"
            note = str(item.get("note", "")).strip()
            if note:
                entry += " — " + _truncate(note, RESULT_TEXT_LIMIT)
            elif item.get("status") not in ("done", "in_progress"):
                entry += " — " + label
            lines.append(entry)
        return lines

    def _format_call(self, call: dict) -> str:
        icon = "✅" if call.get("ok") else "❌"
        args = call.get("args_digest", {})
        if isinstance(args, dict) and args:
            args_text = json.dumps(args, ensure_ascii=False)
            args_text = _truncate(args_text, RESULT_TEXT_LIMIT)
        else:
            args_text = ""
        result = str(call.get("result_digest", ""))
        if call.get("file_ref"):
            result = (result + " " if result else "") + "[完整结果: %s]" % call["file_ref"]
        phase = " [%s]" % call["phase"] if call.get("phase") else ""
        return "  %s %s(%s) → %s%s" % (icon, call.get("tool", "?"), args_text, _truncate(result, RESULT_TEXT_LIMIT) or "-", phase)

    def _render_calls(self) -> list:
        recent = self.calls[-RECENT_CALLS_IN_SNAPSHOT:]
        lines = ["【执行记录】最近 %d 条：" % len(recent)]
        lines.extend(self._format_call(item) for item in recent)
        older = self.calls[:-RECENT_CALLS_IN_SNAPSHOT]
        if older:
            ok_count = sum(1 for item in older if item.get("ok"))
            lines.append("  （另有 %d 条调用：%d 成功 / %d 失败）" % (
                len(older), ok_count, len(older) - ok_count,
            ))
        return lines

    def _render_gap(self) -> str:
        parts = []
        if self.plan:
            remaining = [
                "%s(%s)" % (item.get("title", item.get("id", "")), _STATUS_LABELS.get(item.get("status", ""), item.get("status", "")))
                for item in self.plan.get("phases", [])
                if item.get("status") in ("pending", "in_progress")
            ]
            parts.append("未完成：%s" % "、".join(remaining) if remaining else "所有阶段已完成")  # % 优先级高于三元，行为正确
        failed = [item for item in self.calls if not item.get("ok")]
        if failed:
            last = failed[-1]
            parts.append("上次失败：%s" % last.get("tool", "?"))
        return "；".join(parts)
