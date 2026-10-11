"""记忆子系统的统一门面。

上层的 ``app.py`` 路由、下层的经验库/参数记忆，都只跟这一层打交道：

- 经验库（``experience_memory``）与工具参数记忆（``tool_memory``）读写入口统一在这里，
  避免出现两套写入逻辑；
- 提炼队列、维护任务、开关状态一并收口，路由只做参数校验和错误码转换；
- 所有异常在这里翻译成带 HTTP 语义的 ``MemoryFault``，路由层不再猜异常类型。

**P0a 阶段（当前）只采集不注入**：本模块不提供任何把记忆送回模型请求的入口，
``experience_inject`` / ``fact_inject`` 两个开关先落盘待用。这样影子期攒下来的候选
先被人看过一遍，再决定要不要让它们进提示词。
"""
from __future__ import annotations

import asyncio
import logging
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping, Optional, Sequence

import tool_memory
from config import MemoryConfig
from experience_extractor import (
    TRIGGER_MANUAL,
    ExperienceExtractor,
    ExtractionQueue,
    detect_trigger,
    load_state,
)
from experience_memory import (
    BULK_ACTIONS,
    MemoryStore,
    RecordNotFound,
    StateConflict,
    StoreUnavailable,
    utcnow,
)
from paths import SESSIONS_DIR
from task_ledger import TaskLedger

logger = logging.getLogger(__name__)

MAINTENANCE_KINDS = ("extract", "governance")
DEFAULT_EXTRACT_SESSIONS = 3
MAX_BULK_IDS = 100

#: P0a 影子期：候选只进审核队列，不进模型请求。
STAGE = "shadow"
STAGE_NOTICE = (
    "当前为影子采集阶段：新提炼的经验只进入候选区，不会自动注入模型请求。"
    "审核通过后才具备注入资格。"
)

#: 库出错时给用户看的话，白名单生成，绝不透传异常原文（异常里常带路径）。
_STORE_MESSAGES = {
    "unavailable": "经验库暂时不可读",
    "internal": "经验库出现内部错误",
}


class MemoryFault(Exception):
    """带 HTTP 语义的记忆子系统错误。"""

    def __init__(self, code: str, message: str, status: int = 400) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status


def _fault(code: str, message: str, status: int) -> MemoryFault:
    return MemoryFault(code, message, status)


def _guard(call: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
    """把存储层异常翻译成路由层能直接用的错误码。"""
    try:
        return call(*args, **kwargs)
    except RecordNotFound as exc:
        raise _fault("not_found", "该记录不存在，可能已被删除", 404) from exc
    except StateConflict as exc:
        raise _fault("state_conflict", "该条状态已变化，请刷新后重试", 409) from exc
    except StoreUnavailable as exc:
        raise _fault("store_unavailable", _STORE_MESSAGES["unavailable"], 503) from exc
    except ValueError as exc:
        raise _fault("invalid_request", str(exc) or "请求参数不合法", 400) from exc


def _classify_store_error(raw: Optional[str]) -> Optional[dict]:
    if not raw:
        return None
    lowered = str(raw).lower()
    code = "unavailable" if any(
        token in lowered for token in (
            "oserror", "permission", "ioerror", "sqlite",
            "fileexist", "filenotfound", "isadirectory", "notadirectory",
        )
    ) else "internal"
    return {"code": code, "message": _STORE_MESSAGES[code]}


def recent_ledger_sessions(limit: int = DEFAULT_EXTRACT_SESSIONS) -> list[str]:
    """按台账文件的修改时间倒序取最近的会话，供「立即提炼」使用。"""
    root = Path(SESSIONS_DIR)
    found: list[tuple[float, str]] = []
    try:
        entries = list(root.glob("*/ledger.json"))
    except OSError:
        return []
    for path in entries:
        try:
            found.append((path.stat().st_mtime, path.parent.name))
        except OSError:
            continue
    found.sort(reverse=True)
    return [session_id for _, session_id in found[: max(1, int(limit))]]


class MemoryService:
    """记忆子系统的唯一入口。"""

    def __init__(
        self,
        store: Optional[MemoryStore] = None,
        *,
        extractor: Optional[ExperienceExtractor] = None,
        queue: Optional[ExtractionQueue] = None,
    ) -> None:
        self.store = store if store is not None else MemoryStore()
        self.extractor = extractor if extractor is not None else ExperienceExtractor(
            self.store
        )
        self.queue = queue if queue is not None else ExtractionQueue(self.extractor)
        self._config = MemoryConfig()
        self._config_reader: Optional[Callable[[], MemoryConfig]] = None
        self._config_writer: Optional[Callable[[Mapping[str, bool]], MemoryConfig]] = None
        self._maintenance: dict[str, Any] = {
            "state": "idle",
            "step": None,
            "started_at": None,
            "finished_at": None,
            "last_result": None,
            "last_error": None,
        }
        self._maintenance_task: Optional[asyncio.Task] = None

    # ------------------------------------------------------------ 生命周期

    def bind_config(
        self,
        reader: Callable[[], MemoryConfig],
        writer: Callable[[Mapping[str, bool]], MemoryConfig],
    ) -> None:
        """接上 config.json 的读写口。app.py 负责真正的持久化与版本号维护。"""
        self._config_reader = reader
        self._config_writer = writer
        self._refresh_config()

    def _refresh_config(self) -> MemoryConfig:
        if self._config_reader is not None:
            try:
                self._config = self._config_reader()
            except Exception as exc:  # noqa: BLE001 - 配置读不到就退回默认
                logger.warning("memory config read failed: %s", type(exc).__name__)
        self._sync_extractor()
        return self._config

    def _sync_extractor(self) -> None:
        config = self._config
        self.extractor.configure(
            extract_experience=bool(config.enabled and config.experience_extract),
            extract_fact=bool(config.enabled and config.fact_extract),
        )

    def start(self) -> None:
        self.queue.start()

    async def stop(self) -> None:
        task, self._maintenance_task = self._maintenance_task, None
        if task is not None and not task.done():
            task.cancel()
            try:
                await task
            except (asyncio.CancelledError, Exception):  # noqa: B014
                pass
        await self.queue.stop()
        self.store.close()

    def attach_runtime(self, runtime: Any, model_key: str = "") -> None:
        """模型运行时每次重建配置后都要重新挂上；没有它提炼直接跳过。"""
        self.extractor.configure(runtime=runtime, model_key=model_key or "")

    # ------------------------------------------------------------ 查询

    @property
    def config(self) -> MemoryConfig:
        return self._config

    @property
    def enabled(self) -> bool:
        return bool(self._config.enabled)

    def summary(self) -> dict:
        health = self.store.health()
        try:
            counts = self.store.counts()
        except StoreUnavailable:
            # 库坏了页面还得能打开：开关和参数记忆都不依赖它。
            counts = {}
        experience = {str(key): int(value) for key, value in
                      (counts.get("experience") or {}).items()}
        experience["possible_duplicate"] = int(counts.get("possible_duplicate", 0))
        facts = {str(key): int(value) for key, value in
                 (counts.get("fact") or {}).items()}
        parameters = tool_memory.counts()
        config = self._config
        return {
            "enabled": bool(config.enabled),
            "stage": STAGE,
            "notice": STAGE_NOTICE,
            "config": {
                "enabled": bool(config.enabled),
                "experience_extract": bool(config.experience_extract),
                "experience_inject": bool(config.experience_inject),
                "fact_extract": bool(config.fact_extract),
                "fact_inject": bool(config.fact_inject),
            },
            "experience": experience,
            "fact": facts,
            "total": int(counts.get("total", 0)),
            "tool_parameters": {
                "tools": int(parameters["tools"]),
                "fields": int(parameters["fields"]),
            },
            "store": {
                "status": str(health.get("status", "unavailable")),
                "fts": bool(health.get("fts", False)),
                "writable": bool(health.get("writable", False)),
                "path_hint": "用户数据目录",
                "last_error": _classify_store_error(health.get("last_error")),
            },
            "maintenance": self.maintenance_status()["maintenance"],
        }

    def list_experiences(
        self,
        *,
        kind: Optional[str] = None,
        status: Optional[str] = None,
        scope: Optional[str] = None,
        task_type: Optional[str] = None,
        q: Optional[str] = None,
        page: int = 1,
        page_size: int = 20,
    ) -> dict:
        result = _guard(
            self.store.list_items,
            kind=kind or None,
            status=status or None,
            scope=scope or None,
            task_type=task_type or None,
            q=q or None,
            page=page,
            page_size=page_size,
        )
        result["facets"] = _guard(self.store.facets)
        return result

    def get_experience(self, item_id: str) -> dict:
        return _guard(self.store.get, item_id)

    def list_tool_parameters(self) -> dict:
        return tool_memory.catalog_all()

    def audit(self, limit: int = 50) -> list:
        return _guard(self.store.audit_tail, limit)

    def maintenance_status(self) -> dict:
        queue = self.queue.stats
        extraction = {
            "pending_jobs": int(queue.get("pending", 0)),
            "running": bool(queue.get("running", False)),
            "queued": int(queue.get("queued", 0)),
            "completed": int(queue.get("completed", 0)),
            "failed": int(queue.get("failed", 0)),
            "dropped": int(queue.get("dropped", 0)),
            "last_job_result": queue.get("last_result"),
            "last_job_error": queue.get("last_error"),
        }
        return {"maintenance": dict(self._maintenance), "extraction": extraction}

    # ------------------------------------------------------------ 写入

    def patch_config(self, updates: Mapping[str, Any]) -> dict:
        allowed = {
            "enabled", "extract", "inject",
            "experience_extract", "experience_inject",
            "fact_extract", "fact_inject",
        }
        unknown = sorted(set(updates) - allowed)
        if unknown:
            raise _fault("invalid_request", "不支持的配置项: %s" % ", ".join(unknown), 400)
        if not updates:
            raise _fault("invalid_request", "至少需要一个配置项", 400)
        normalized: dict[str, bool] = {}
        for key, value in updates.items():
            if not isinstance(value, bool):
                raise _fault("invalid_request", "配置项 %s 必须是布尔值" % key, 400)
            normalized[str(key)] = value
        # 短名映射到 config.json 里的长名；enabled / extract / inject 是旧契约的简写
        mapping = {
            "enabled": ("enabled",),
            "experience_extract": ("experience_extract",),
            "experience_inject": ("experience_inject",),
            "fact_extract": ("fact_extract",),
            "fact_inject": ("fact_inject",),
        }
        flat: dict[str, bool] = {}
        for key, value in normalized.items():
            if key == "enabled":
                flat["enabled"] = value
            elif key == "extract":
                flat["experience_extract"] = value
                flat["fact_extract"] = value
            elif key == "inject":
                flat["experience_inject"] = value
                flat["fact_inject"] = value
            else:
                flat[key] = value
        if self._config_writer is None:
            raise _fault("store_unavailable", "记忆配置暂时不可写", 503)
        try:
            self._config = self._config_writer(flat)
        except Exception as exc:  # noqa: BLE001
            logger.warning("memory config write failed: %s", type(exc).__name__)
            raise _fault("store_unavailable", "记忆配置写入失败", 503) from exc
        self._sync_extractor()
        return self.summary()

    def transition_experience(
        self,
        item_id: str,
        action: str,
        *,
        reason: str = "",
        expected_updated_at: Optional[str] = None,
    ) -> dict:
        item = _guard(
            self.store.transition, item_id, action,
            reason=str(reason or "")[:200],
            expected_updated_at=expected_updated_at or None,
        )
        return item

    def bulk_experience(
        self, ids: Sequence[str], action: str, *, reason: str = ""
    ) -> dict:
        if action not in BULK_ACTIONS:
            raise _fault(
                "invalid_request",
                "批量操作只支持 %s" % "/".join(BULK_ACTIONS),
                400,
            )
        unique = [str(item) for item in dict.fromkeys(ids or []) if str(item)]
        if not unique:
            raise _fault("invalid_request", "至少需要一条记录 id", 400)
        if len(unique) > MAX_BULK_IDS:
            raise _fault(
                "invalid_request", "单次最多处理 %d 条" % MAX_BULK_IDS, 400
            )
        results = _guard(
            self.store.bulk_transition, unique, action, reason=str(reason or "")[:200]
        )
        # 存储层对成功项只给 {"id","status","ok"}，这里统一补上 code，
        # 让前端不必为「成功」和「没变化」写两套判断。
        normalized = [
            {**entry, "code": entry.get("code") or ("ok" if entry.get("ok") else "failed")}
            for entry in results
        ]
        return {
            "action": action,
            "results": normalized,
            "succeeded": sum(1 for entry in normalized if entry.get("ok")),
            "failed": sum(1 for entry in normalized if not entry.get("ok")),
        }

    def delete_experience(self, item_id: str) -> bool:
        return bool(_guard(self.store.delete, item_id))

    def delete_tool(self, tool: str) -> int:
        removed = tool_memory.clear(str(tool))
        if not removed:
            raise _fault("not_found", "该工具没有记住的参数", 404)
        return removed

    def delete_tool_field(self, tool: str, field_name: str) -> None:
        removed = tool_memory.delete_fields(str(tool), [str(field_name)])
        if not removed:
            raise _fault("not_found", "该字段已不存在", 404)

    def clear_tool_parameters(self) -> dict:
        """清空全部工具参数记忆，返回被删掉的工具数与字段数。"""
        before = tool_memory.counts()
        removed = tool_memory.clear()
        return {
            "tools": int(before["tools"]),
            "fields": int(removed),
        }

    # ------------------------------------------------------------ 维护

    def start_maintenance(self, kind: str = "governance") -> dict:
        if kind not in MAINTENANCE_KINDS:
            raise _fault(
                "invalid_request",
                "维护类型只支持 %s" % "/".join(MAINTENANCE_KINDS),
                400,
            )
        if self._maintenance["state"] == "running":
            raise _fault("state_conflict", "已有维护任务正在运行", 409)
        self._maintenance.update({
            "state": "running",
            "step": kind,
            "started_at": utcnow(),
            "finished_at": None,
            "last_error": None,
        })
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            # 没有事件循环时同步跑完，方便脚本和测试直接调用
            self._run_maintenance_sync(kind)
            return self.maintenance_status()
        self._maintenance_task = loop.create_task(self._run_maintenance(kind))
        return self.maintenance_status()

    async def _run_maintenance(self, kind: str) -> None:
        try:
            if kind == "governance":
                self._maintenance["step"] = "governance"
                result = _guard(self.store.run_governance)
                self._maintenance["last_result"] = result
            else:
                self._maintenance["step"] = "extract"
                submitted = self.submit_recent_extractions()
                self._maintenance["last_result"] = {
                    "submitted": submitted,
                    "candidates": "见候选区",
                }
        except Exception as exc:  # noqa: BLE001 - 维护失败不该影响主流程
            self._maintenance["last_error"] = type(exc).__name__
            logger.warning("memory maintenance failed: %s", type(exc).__name__)
        finally:
            self._maintenance["state"] = "idle"
            self._maintenance["step"] = None
            self._maintenance["finished_at"] = utcnow()

    def _run_maintenance_sync(self, kind: str) -> None:
        try:
            if kind == "governance":
                self._maintenance["last_result"] = _guard(self.store.run_governance)
            else:
                self._maintenance["last_result"] = {
                    "submitted": self.submit_recent_extractions()
                }
        except Exception as exc:  # noqa: BLE001
            self._maintenance["last_error"] = type(exc).__name__
        finally:
            self._maintenance["state"] = "idle"
            self._maintenance["step"] = None
            self._maintenance["finished_at"] = utcnow()

    # ------------------------------------------------------------ 提炼投递

    def submit_session(
        self,
        session_id: str,
        *,
        trigger: str = TRIGGER_MANUAL,
        focus: str = "",
        skills: Iterable[str] = (),
        watermark: int = 0,
    ) -> bool:
        """投一条提炼任务。绝不等待模型，队列满就丢。"""
        if not self.enabled:
            return False
        try:
            ledger = TaskLedger(session_id)
        except Exception as exc:  # noqa: BLE001 - 会话不存在或台账损坏都只是跳过
            logger.info("experience ledger unreadable for %s: %s",
                        session_id[:8], type(exc).__name__)
            return False
        calls = list(getattr(ledger, "calls", []) or [])
        if not calls:
            return False
        snapshot = {"plan": dict(getattr(ledger, "plan", {}) or {}), "calls": calls}
        return self.queue.submit(
            session_id, snapshot, trigger=trigger, focus=focus,
            skills=tuple(skills), watermark=watermark,
        )

    def submit_recent_extractions(self, limit: int = DEFAULT_EXTRACT_SESSIONS) -> int:
        submitted = 0
        for session_id in recent_ledger_sessions(limit):
            if self.submit_session(session_id, trigger=TRIGGER_MANUAL):
                submitted += 1
        return submitted

    def schedule_after_turn(self, session_id: str, ledger: Any, *, turn_count: int = 0) -> bool:
        """一轮对话结束后排一次提炼。绝不阻塞、绝不等待模型。

        触发源只有三种客观事件（计划进终态 / 跨过轮次水位 / 用户明确纠错），
        刻意不含「模型自评」「用户语气不满」：靠感觉写入的长期记忆一旦错了，
        用户连它是从哪来的都查不到。
        """
        if not self.enabled:
            return False
        calls = list((ledger or {}).get("calls") or [])
        if not calls:
            return False
        try:
            state = load_state(session_id, root=self.extractor.state_root)
        except Exception as exc:  # noqa: BLE001 - 断点读不到就当作没跑过
            logger.info("experience state unreadable for %s: %s",
                        str(session_id)[:8], type(exc).__name__)
            state = {}
        last_watermark = int(state.get("last_watermark") or 0)
        trigger = detect_trigger(
            ledger, turn_count=int(turn_count or 0), last_watermark=last_watermark
        )
        if trigger is None:
            return False
        snapshot = {"plan": dict((ledger or {}).get("plan") or {}), "calls": calls}
        return self.queue.submit(
            session_id, snapshot, trigger=trigger, watermark=int(turn_count or 0)
        )