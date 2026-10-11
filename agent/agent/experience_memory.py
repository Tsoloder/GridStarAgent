"""长期记忆存储层：策略经验 + 工程事实。

设计要点，每一条都对应一个已经付过代价的教训：

* **候选与可用隔离**。自动提炼只写 ``candidate``，检索只认 ``approved``。历史上
  已经出现过「参数记忆把废弃参数注回调用、服务端一刀砍掉」的事故，错误记忆比
  没有记忆更危险。
* **不存来源会话 id**。长期记录里出现会话 id 等于给自己建了一条反查用户历史的
  索引；抽取任务只留不可逆的 job 指纹，状态落在会话目录里随会话一起清掉。
* **落盘前二次脱敏**。任何字段命中敏感模式整条丢弃，只加 ``rejected_by_sanitizer``
  计数。
* **曝光不等于正确**。``exposure_count`` 只用于观测，不参与 ``confidence``；
  只有明确的 helpful/harmful 与验证成败才动置信度。
* **降级不阻断主流程**。FTS5 不可用时退回 LIKE 扫描，库损坏时记忆链路整体降级，
  主任务照跑。

中文检索走「应用层生成 2/3-gram 写进冗余列」：SQLite 的 unicode61 分词器会把
一整段中文当成一个 token，等于搜不了；自己切 gram 就不必引入分词依赖。
"""
from __future__ import annotations

import hashlib
import json
import re
import secrets
import sqlite3
import threading
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Mapping, Optional, Sequence

from memory_sanitize import assert_clean
from paths import MEMORY_DB_PATH

SCHEMA_VERSION = 1

KIND_EXPERIENCE = "experience"
KIND_FACT = "fact"
KINDS: tuple[str, ...] = (KIND_EXPERIENCE, KIND_FACT)

STATUS_CANDIDATE = "candidate"
STATUS_APPROVED = "approved"
STATUS_REJECTED = "rejected"
STATUS_DEPRECATED = "deprecated"
STATUSES: tuple[str, ...] = (
    STATUS_CANDIDATE, STATUS_APPROVED, STATUS_REJECTED, STATUS_DEPRECATED,
)

# 状态流转白名单。非法流转一律拒绝，不做「就近解释」：审核动作必须可预测。
TRANSITIONS: Mapping[str, Mapping[str, str]] = {
    "approve": {STATUS_CANDIDATE: STATUS_APPROVED},
    "reject": {STATUS_CANDIDATE: STATUS_REJECTED},
    "deprecate": {STATUS_APPROVED: STATUS_DEPRECATED},
    "restore": {STATUS_DEPRECATED: STATUS_APPROVED},
    "reopen": {STATUS_REJECTED: STATUS_CANDIDATE},
}
BULK_ACTIONS: tuple[str, ...] = ("approve", "reject")

SOURCE_KINDS: tuple[str, ...] = (
    "manual_curated", "user_correction", "validated_workflow", "auto_extraction",
)
# 初始先验，常数待真实样本校准。曝光次数绝不参与这里的计算。
SOURCE_PRIORS: Mapping[str, float] = {
    "manual_curated": 0.85,
    "user_correction": 0.65,
    "validated_workflow": 0.55,
    "auto_extraction": 0.35,
}
PRIOR_WEIGHT = 4.0

FACT_TYPES: tuple[str, ...] = ("user_preference", "environment", "project_trait")

CANDIDATE_CAP_PER_SCOPE = 200
CANDIDATE_MAX_AGE_DAYS = 30
MERGE_THRESHOLD = 0.85
DUPLICATE_HINT_THRESHOLD = 0.65
RECALL_LIMIT = 3
RECALL_POOL = 40
TITLE_LIMIT = 200

_SCOPE_RE = re.compile(r"^(global|skill:[A-Za-z0-9][A-Za-z0-9_.\-]{0,63})$")
_CJK_RE = re.compile(
    r"[\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff\u3040-\u30ff\uac00-\ud7af]+"
)
_NORMALIZE_RE = re.compile(r"[\s\W_]+", re.UNICODE)
_ID_KEY_RE = re.compile(r"(?i)(^|_)id(entifier)?s?$")


class MemoryStoreError(RuntimeError):
    pass


class StoreUnavailable(MemoryStoreError):
    """经验库不可用：文件损坏、目录不可写、SQLite 报错。"""


class StateConflict(MemoryStoreError):
    """状态流转非法或并发写入冲突。"""


class RecordNotFound(MemoryStoreError):
    pass


def utcnow() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="microseconds")


def days_since(stamp: Optional[str]) -> float:
    if not stamp:
        return 3650.0
    try:
        moment = datetime.fromisoformat(stamp)
    except ValueError:
        return 3650.0
    if moment.tzinfo is None:
        moment = moment.replace(tzinfo=timezone.utc)
    return max(0.0, (datetime.now(timezone.utc) - moment).total_seconds() / 86400.0)


def normalize_scope(scope: Any) -> str:
    """作用域只允许 global 与 skill:<id>。

    项目级作用域被刻意排除：当前没有可信的项目标识，用文件路径充当 scope 会把
    工程数据的关联关系写进长期存储，比不记还糟。
    """
    text = "" if scope is None else str(scope).strip()
    if not _SCOPE_RE.match(text):
        raise ValueError("invalid scope: %r" % (text[:64],))
    return text


def normalize_text(value: Any) -> str:
    """去空白与标点并小写化，用于指纹与相似度比较。"""
    text = "" if value is None else str(value)
    return _NORMALIZE_RE.sub("", text).lower()


# --------------------------------------------------------------------------
# 中文 n-gram
# --------------------------------------------------------------------------


def cjk_ngrams(text: str, sizes: Sequence[int] = (2, 3)) -> list[str]:
    grams: list[str] = []
    for run in _CJK_RE.findall(text or ""):
        for size in sizes:
            if len(run) < size:
                continue
            for index in range(len(run) - size + 1):
                grams.append(run[index:index + size])
    return grams


def build_search_text(*parts: Any) -> str:
    """原文 + 中文 2/3-gram 冗余列，写入时生成一次。"""
    raw = " ".join(str(part) for part in parts if part)
    grams = cjk_ngrams(raw)
    if not grams:
        return raw
    unique: dict[str, None] = {}
    for gram in grams:
        unique.setdefault(gram, None)
    return raw + " " + " ".join(unique)


def build_fts_query(query: str) -> str:
    """查询侧同样切 gram，每个词元包双引号后 OR 连接。

    不做 AND：记忆文本是「参考」而非「约束」，召回宽一点交给排序层收窄更合适。
    """
    text = (query or "").strip()
    if not text:
        return ""
    terms: list[str] = []
    for token in re.split(r"\s+", text):
        if token:
            terms.append('"%s"' % token.replace('"', '""'))
    for gram in cjk_ngrams(text):
        terms.append('"%s"' % gram)
    if not terms:
        return ""
    unique: dict[str, None] = {}
    for term in terms:
        unique.setdefault(term, None)
    return " OR ".join(unique)


def _shingles(text: str) -> set[str]:
    return set(cjk_ngrams(text, (2, 3))) | set(re.findall(r"[a-z0-9]{3,}", (text or "").lower()))


def jaccard(left: str, right: str) -> float:
    a = _shingles(left)
    b = _shingles(right)
    if not a or not b:
        return 0.0
    return len(a & b) / len(a | b)


# --------------------------------------------------------------------------
# 建表
# --------------------------------------------------------------------------

_SCHEMA = """
CREATE TABLE IF NOT EXISTS memory_item (
    id TEXT PRIMARY KEY,
    kind TEXT NOT NULL,
    schema_version INTEGER NOT NULL DEFAULT 1,
    status TEXT NOT NULL,
    scope TEXT NOT NULL DEFAULT 'global',
    task_type TEXT NOT NULL DEFAULT '',
    title TEXT NOT NULL,
    payload TEXT NOT NULL DEFAULT '{}',
    search_text TEXT NOT NULL DEFAULT '',
    fingerprint TEXT NOT NULL UNIQUE,
    confidence REAL NOT NULL DEFAULT 0.5,
    evidence_count INTEGER NOT NULL DEFAULT 1,
    source_kind TEXT NOT NULL DEFAULT 'auto_extraction',
    source_version TEXT NOT NULL DEFAULT '{}',
    flags TEXT NOT NULL DEFAULT '[]',
    exposure_count INTEGER NOT NULL DEFAULT 0,
    helpful_count INTEGER NOT NULL DEFAULT 0,
    harmful_count INTEGER NOT NULL DEFAULT 0,
    verified_success_count INTEGER NOT NULL DEFAULT 0,
    verified_failure_count INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    last_exposed_at TEXT,
    last_verified_at TEXT
);
CREATE INDEX IF NOT EXISTS idx_memory_item_status ON memory_item(status);
CREATE INDEX IF NOT EXISTS idx_memory_item_kind ON memory_item(kind, status);
CREATE INDEX IF NOT EXISTS idx_memory_item_scope ON memory_item(scope);
CREATE INDEX IF NOT EXISTS idx_memory_item_updated ON memory_item(updated_at);

CREATE TABLE IF NOT EXISTS memory_audit (
    seq INTEGER PRIMARY KEY AUTOINCREMENT,
    ts TEXT NOT NULL,
    item_kind TEXT NOT NULL,
    item_id TEXT NOT NULL,
    action TEXT NOT NULL,
    reason TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_memory_audit_item ON memory_audit(item_id);

CREATE TABLE IF NOT EXISTS memory_counter (
    name TEXT PRIMARY KEY,
    value INTEGER NOT NULL DEFAULT 0
);
"""

_FTS_SCHEMA = """
CREATE VIRTUAL TABLE IF NOT EXISTS memory_item_fts USING fts5(
    title, search_text, content='memory_item', content_rowid='rowid',
    tokenize='unicode61'
);
CREATE TRIGGER IF NOT EXISTS memory_item_ai AFTER INSERT ON memory_item BEGIN
    INSERT INTO memory_item_fts(rowid, title, search_text)
    VALUES (new.rowid, new.title, new.search_text);
END;
CREATE TRIGGER IF NOT EXISTS memory_item_ad AFTER DELETE ON memory_item BEGIN
    INSERT INTO memory_item_fts(memory_item_fts, rowid, title, search_text)
    VALUES ('delete', old.rowid, old.title, old.search_text);
END;
CREATE TRIGGER IF NOT EXISTS memory_item_au AFTER UPDATE ON memory_item BEGIN
    INSERT INTO memory_item_fts(memory_item_fts, rowid, title, search_text)
    VALUES ('delete', old.rowid, old.title, old.search_text);
    INSERT INTO memory_item_fts(rowid, title, search_text)
    VALUES (new.rowid, new.title, new.search_text);
END;
"""

COUNTER_DUPLICATE = "duplicate_suppressed"
COUNTER_SANITIZER = "rejected_by_sanitizer"
COUNTER_INVALID = "rejected_by_validation"

# 批量操作的目标状态，用于判断"已经在这个状态上"从而重放安全
_BULK_TARGETS = {"approve": STATUS_APPROVED, "reject": STATUS_REJECTED}


def _new_id(prefix: str) -> str:
    millis = int(time.time() * 1000)
    return "%s%011x%s" % (prefix, millis, secrets.token_hex(4))


def _decode_json(text: Any, fallback: Any) -> Any:
    if not text:
        return fallback
    try:
        value = json.loads(text)
    except (TypeError, ValueError):
        return fallback
    return value if isinstance(value, type(fallback)) else fallback


def _clean_list(values: Any, limit: int = 20, item_limit: int = 300) -> list[str]:
    if values is None:
        return []
    if isinstance(values, str):
        values = [values]
    if not isinstance(values, (list, tuple)):
        return []
    out: list[str] = []
    for value in values:
        text = str(value).strip()
        if text:
            out.append(text[:item_limit])
        if len(out) >= limit:
            break
    return out


class MemoryStore:
    """经验库与事实库的统一存储。

    这里只负责存储与状态机；提炼调度在 experience_extractor，对外门面在
    memory_store。所有写操作即时生效并留审计。
    """

    def __init__(self, path: Optional[Path] = None, *, enabled: bool = True) -> None:
        self.path = Path(path) if path is not None else Path(MEMORY_DB_PATH)
        self.enabled = enabled
        self._lock = threading.RLock()
        self._conn: Optional[sqlite3.Connection] = None
        self._fts = False
        self._writable = True
        self._last_error: Optional[str] = None

    # ------------------------------------------------------------- 连接

    def _connect(self) -> sqlite3.Connection:
        if self._conn is not None:
            return self._conn
        if not self.enabled:
            raise StoreUnavailable("memory disabled")
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            conn = sqlite3.connect(str(self.path), check_same_thread=False, timeout=10.0)
            conn.row_factory = sqlite3.Row
            conn.execute("PRAGMA journal_mode=WAL")
            conn.execute("PRAGMA synchronous=NORMAL")
            conn.executescript(_SCHEMA)
            try:
                conn.executescript(_FTS_SCHEMA)
                self._fts = True
            except sqlite3.Error:
                # 少数 Python 发行版编不到 FTS5，退化为 LIKE 扫描，功能不消失。
                self._fts = False
            conn.execute("PRAGMA user_version=%d" % SCHEMA_VERSION)
            conn.commit()
            self._conn = conn
            self._writable = True
            self._last_error = None
        except (OSError, sqlite3.Error) as exc:
            self._writable = False
            self._last_error = type(exc).__name__
            raise StoreUnavailable("memory store unavailable") from exc
        return self._conn

    def close(self) -> None:
        with self._lock:
            if self._conn is not None:
                try:
                    self._conn.close()
                except sqlite3.Error:
                    pass
                self._conn = None

    @property
    def fts_available(self) -> bool:
        with self._lock:
            try:
                self._connect()
            except StoreUnavailable:
                return False
            return self._fts

    def health(self) -> dict[str, Any]:
        with self._lock:
            try:
                self._connect()
                healthy = True
            except StoreUnavailable:
                healthy = False
            return {
                "status": "healthy" if healthy else "unavailable",
                "fts": bool(self._fts),
                "writable": bool(self._writable),
                "last_error": self._last_error,
            }

    def _execute(self, sql: str, params: Sequence[Any] = ()) -> sqlite3.Cursor:
        try:
            return self._connect().execute(sql, params)
        except StoreUnavailable:
            raise
        except sqlite3.Error as exc:
            self._last_error = type(exc).__name__
            raise StoreUnavailable("memory store error") from exc

    def _bump(self, conn: sqlite3.Connection, name: str, delta: int = 1) -> None:
        conn.execute(
            "INSERT INTO memory_counter(name, value) VALUES(?, ?)"
            " ON CONFLICT(name) DO UPDATE SET value = value + excluded.value",
            (name, delta),
        )

    def counter(self, name: str) -> int:
        with self._lock:
            row = self._execute(
                "SELECT value FROM memory_counter WHERE name = ?", (name,)
            ).fetchone()
            return int(row["value"]) if row else 0

    # ------------------------------------------------------------- 写入

    def _fingerprint(self, kind: str, scope: str, task_type: str, title: str,
                     texts: Sequence[str]) -> str:
        payload = "\x1f".join([
            kind, scope, task_type, normalize_text(title),
            normalize_text(" ".join(texts)),
        ])
        return hashlib.sha256(payload.encode("utf-8")).hexdigest()

    def _shape_record(self, record: Mapping[str, Any]) -> dict[str, Any]:
        """校验并规范化一条待写入记录，返回 ``(item, texts)``。"""
        kind = str(record.get("kind") or KIND_EXPERIENCE).strip()
        if kind not in KINDS:
            raise ValueError("invalid kind: %r" % (kind[:32],))
        scope = normalize_scope(record.get("scope") or "global")
        source_kind = str(record.get("source_kind") or "auto_extraction").strip()
        if source_kind not in SOURCE_KINDS:
            raise ValueError("invalid source_kind: %r" % (source_kind[:32],))
        # 自动提炼永远只能产出候选；只有人工录入才有资格直接落成已启用。
        status = STATUS_CANDIDATE
        if source_kind == "manual_curated" and record.get("status") in STATUSES:
            status = str(record["status"])
        task_type = str(record.get("task_type") or "").strip()[:64]

        if kind == KIND_EXPERIENCE:
            title = str(record.get("title") or "").strip()
            payload = {
                "trigger_cues": _clean_list(record.get("trigger_cues"), 8, 120),
                "preconditions": _clean_list(record.get("preconditions"), 8, 200),
                "strategy_steps": _clean_list(record.get("strategy_steps"), 12, 300),
                "tool_sequence": _clean_list(record.get("tool_sequence"), 24, 64),
                "failure_patterns": _clean_list(record.get("failure_patterns"), 8, 300),
            }
            texts = (
                payload["trigger_cues"] + payload["preconditions"]
                + payload["strategy_steps"] + payload["failure_patterns"]
                + payload["tool_sequence"]
            )
        else:
            fact_text = str(record.get("fact_text") or record.get("title") or "").strip()
            if not fact_text:
                raise ValueError("fact_text must not be empty")
            fact_type = str(record.get("fact_type") or "project_trait").strip()
            if fact_type not in FACT_TYPES:
                fact_type = "project_trait"
            title = fact_text
            payload = {
                "fact_text": fact_text,
                "fact_type": fact_type,
                "tags": _clean_list(record.get("tags"), 8, 48),
            }
            texts = payload["tags"]

        if not title:
            raise ValueError("title must not be empty")
        title = title[:TITLE_LIMIT]

        evidence = record.get("evidence_count", 1)
        try:
            evidence = max(1, int(evidence))
        except (TypeError, ValueError):
            evidence = 1

        confidence = record.get("confidence")
        if not isinstance(confidence, (int, float)):
            confidence = SOURCE_PRIORS[source_kind]
        confidence = min(1.0, max(0.0, float(confidence)))

        source_version = record.get("source_version")
        if not isinstance(source_version, Mapping):
            source_version = {}

        item = {
            "kind": kind,
            "status": status,
            "scope": scope,
            "task_type": task_type,
            "title": title,
            "payload": payload,
            "confidence": confidence,
            "evidence_count": evidence,
            "source_kind": source_kind,
            "source_version": {str(k)[:32]: str(v)[:128] for k, v in source_version.items()},
            "flags": _clean_list(record.get("flags"), 8, 48),
        }
        return item, texts

    def add_item(self, record: Mapping[str, Any]) -> dict[str, Any]:
        """写入一条记忆。

        返回 ``{"id", "created", "reason"}``。``reason="duplicate"`` 表示指纹已存在：
        候选会累加证据，已拒绝与已停用的记录**原样保留、不复活**，只加抑制计数。
        校验失败与脱敏命中都返回 ``created=False``，不抛异常，让提炼任务继续跑。
        """
        try:
            item, texts = self._shape_record(record)
        except ValueError as exc:
            with self._lock:
                conn = self._connect()
                self._bump(conn, COUNTER_INVALID)
                conn.commit()
            return {"id": None, "created": False, "reason": "invalid", "detail": str(exc)[:120]}

        record_for_scan = dict(item)
        record_for_scan.pop("payload", None)
        record_for_scan.update(item["payload"])
        try:
            assert_clean(record_for_scan)
        except Exception as exc:  # SensitiveContentError 及其子类
            with self._lock:
                conn = self._connect()
                self._bump(conn, COUNTER_SANITIZER)
                conn.commit()
            return {
                "id": None, "created": False, "reason": "sanitized",
                "detail": getattr(exc, "field", "") or "",
            }

        fingerprint = self._fingerprint(
            item["kind"], item["scope"], item["task_type"], item["title"], texts
        )
        search_text = build_search_text(
            item["title"], item["task_type"], *texts
        )
        now = utcnow()
        prefix = "exp_" if item["kind"] == KIND_EXPERIENCE else "fact_"

        with self._lock:
            conn = self._connect()
            try:
                existing = conn.execute(
                    "SELECT id, status FROM memory_item WHERE fingerprint = ?",
                    (fingerprint,),
                ).fetchone()
                if existing is not None:
                    if existing["status"] == STATUS_CANDIDATE:
                        conn.execute(
                            "UPDATE memory_item SET evidence_count = evidence_count + ?,"
                            " updated_at = ? WHERE id = ?",
                            (item["evidence_count"], now, existing["id"]),
                        )
                    else:
                        self._bump(conn, COUNTER_DUPLICATE)
                    conn.commit()
                    return {"id": existing["id"], "created": False, "reason": "duplicate"}

                item_id = _new_id(prefix)
                conn.execute(
                    "INSERT INTO memory_item (id, kind, schema_version, status, scope,"
                    " task_type, title, payload, search_text, fingerprint, confidence,"
                    " evidence_count, source_kind, source_version, flags, created_at,"
                    " updated_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    (
                        item_id, item["kind"], SCHEMA_VERSION, item["status"], item["scope"],
                        item["task_type"], item["title"],
                        json.dumps(item["payload"], ensure_ascii=False),
                        search_text, fingerprint, item["confidence"], item["evidence_count"],
                        item["source_kind"],
                        json.dumps(item["source_version"], ensure_ascii=False),
                        json.dumps(item["flags"], ensure_ascii=False), now, now,
                    ),
                )
                conn.execute(
                    "INSERT INTO memory_audit (ts, item_kind, item_id, action, reason)"
                    " VALUES (?,?,?,?,?)",
                    (now, item["kind"], item_id, "create", item["source_kind"]),
                )
                conn.commit()
                return {"id": item_id, "created": True, "reason": "created"}
            except sqlite3.Error as exc:
                conn.rollback()
                self._last_error = type(exc).__name__
                raise StoreUnavailable("memory store error") from exc

    # ------------------------------------------------------------- 读取

    def get(self, item_id: str) -> dict[str, Any]:
        with self._lock:
            row = self._execute(
                "SELECT * FROM memory_item WHERE id = ?", (item_id,)
            ).fetchone()
        if row is None:
            raise RecordNotFound(item_id)
        return _row_to_item(row)

    def _search_ids(self, query: str, pool: int) -> list[str]:
        """按关键词取候选 id。FTS5 不可用或中文空召回时退回 LIKE。"""
        fts_query = build_fts_query(query)
        conn = self._connect()
        if self._fts and fts_query:
            try:
                rows = conn.execute(
                    "SELECT m.id AS id FROM memory_item_fts f"
                    " JOIN memory_item m ON m.rowid = f.rowid"
                    " WHERE memory_item_fts MATCH ?"
                    " ORDER BY bm25(memory_item_fts) LIMIT ?",
                    (fts_query, pool),
                ).fetchall()
                if rows:
                    return [row["id"] for row in rows]
            except sqlite3.Error:
                self._fts = False
        like = "%%%s%%" % query.replace("%", "").replace("_", "")
        rows = conn.execute(
            "SELECT id FROM memory_item WHERE title LIKE ? OR search_text LIKE ?"
            " ORDER BY updated_at DESC LIMIT ?",
            (like, like, pool),
        ).fetchall()
        return [row["id"] for row in rows]

    def list_items(
        self,
        *,
        kind: Optional[str] = None,
        status: Optional[str] = None,
        scope: Optional[str] = None,
        task_type: Optional[str] = None,
        q: Optional[str] = None,
        page: int = 1,
        page_size: int = 20,
    ) -> dict[str, Any]:
        page = max(1, int(page))
        page_size = min(100, max(1, int(page_size)))
        clauses: list[str] = []
        params: list[Any] = []
        if kind in KINDS:
            clauses.append("kind = ?")
            params.append(kind)
        if status in STATUSES:
            clauses.append("status = ?")
            params.append(status)
        if scope:
            clauses.append("scope = ?")
            params.append(scope)
        if task_type:
            clauses.append("task_type = ?")
            params.append(task_type)

        ids: Optional[list[str]] = None
        text = (q or "").strip()
        if text:
            ids = self._search_ids(text, 500)
            if not ids:
                return {
                    "items": [], "total": 0, "page": page, "page_size": page_size,
                }
            clauses.append("id IN (%s)" % ",".join("?" for _ in ids))
            params.extend(ids)

        where = (" WHERE " + " AND ".join(clauses)) if clauses else ""
        with self._lock:
            total = int(
                self._execute(
                    "SELECT COUNT(*) AS n FROM memory_item" + where, params
                ).fetchone()["n"]
            )
            order = "created_at DESC" if status == STATUS_CANDIDATE else "updated_at DESC"
            rows = self._execute(
                "SELECT * FROM memory_item" + where + " ORDER BY " + order + " LIMIT ? OFFSET ?",
                params + [page_size, (page - 1) * page_size],
            ).fetchall()
        items = [_row_to_summary(row) for row in rows]
        if ids is not None:
            order_index = {item_id: index for index, item_id in enumerate(ids)}
            items.sort(key=lambda item: order_index.get(item["id"], 1 << 30))
        return {"items": items, "total": total, "page": page, "page_size": page_size}

    def facets(self) -> dict[str, list[dict[str, Any]]]:
        with self._lock:
            scopes = self._execute(
                "SELECT scope AS value, COUNT(*) AS n FROM memory_item"
                " GROUP BY scope ORDER BY n DESC LIMIT 50"
            ).fetchall()
            task_types = self._execute(
                "SELECT task_type AS value, COUNT(*) AS n FROM memory_item"
                " WHERE task_type <> '' GROUP BY task_type ORDER BY n DESC LIMIT 50"
            ).fetchall()
        return {
            "scopes": [{"value": row["value"], "count": int(row["n"])} for row in scopes],
            "task_types": [
                {"value": row["value"], "count": int(row["n"])} for row in task_types
            ],
        }

    def counts(self) -> dict[str, Any]:
        result: dict[str, Any] = {kind: {status: 0 for status in STATUSES} for kind in KINDS}
        with self._lock:
            rows = self._execute(
                "SELECT kind, status, COUNT(*) AS n FROM memory_item GROUP BY kind, status"
            ).fetchall()
            duplicate = self._execute(
                "SELECT COUNT(*) AS n FROM memory_item WHERE flags LIKE '%possible_duplicate%'"
            ).fetchone()["n"]
        for row in rows:
            kind = row["kind"]
            status = row["status"]
            if kind in result and status in result[kind]:
                result[kind][status] = int(row["n"])
        total = sum(sum(bucket.values()) for bucket in result.values())
        return {
            "experience": result[KIND_EXPERIENCE],
            "fact": result[KIND_FACT],
            "possible_duplicate": int(duplicate),
            "total": total,
        }

    def audit_tail(self, limit: int = 50) -> list[dict[str, Any]]:
        with self._lock:
            rows = self._execute(
                "SELECT ts, item_kind, item_id, action, reason FROM memory_audit"
                " ORDER BY seq DESC LIMIT ?",
                (max(1, min(500, int(limit))),),
            ).fetchall()
        return [dict(row) for row in rows]

    # ------------------------------------------------------------- 状态流转

    def _transition_locked(
        self, conn: sqlite3.Connection, item_id: str, action: str, reason: str,
        expected_updated_at: Optional[str],
    ) -> dict[str, Any]:
        row = conn.execute(
            "SELECT status, updated_at, kind FROM memory_item WHERE id = ?", (item_id,)
        ).fetchone()
        if row is None:
            raise RecordNotFound(item_id)
        allowed = TRANSITIONS.get(action)
        if allowed is None:
            raise ValueError("invalid action: %r" % (action[:32],))
        target = allowed.get(row["status"])
        if target is None:
            raise StateConflict(
                "cannot %s from %s" % (action, row["status"])
            )
        if expected_updated_at and expected_updated_at != row["updated_at"]:
            raise StateConflict("stale revision")
        now = utcnow()
        conn.execute(
            "UPDATE memory_item SET status = ?, updated_at = ? WHERE id = ?",
            (target, now, item_id),
        )
        conn.execute(
            "INSERT INTO memory_audit (ts, item_kind, item_id, action, reason) VALUES (?,?,?,?,?)",
            (now, row["kind"], item_id, action, reason[:200]),
        )
        return {"id": item_id, "status": target, "ok": True}

    def transition(
        self, item_id: str, action: str, *, reason: str = "",
        expected_updated_at: Optional[str] = None,
    ) -> dict[str, Any]:
        with self._lock:
            conn = self._connect()
            try:
                result = self._transition_locked(
                    conn, item_id, action, reason, expected_updated_at
                )
                conn.commit()
                return result
            except (RecordNotFound, StateConflict, ValueError):
                conn.rollback()
                raise
            except sqlite3.Error as exc:
                conn.rollback()
                raise StoreUnavailable("memory store error") from exc

    def bulk_transition(
        self, ids: Sequence[str], action: str, *, reason: str = ""
    ) -> list[dict[str, Any]]:
        """一次事务批量流转，允许部分成功。

        已处于目标状态的记录计为成功且不重复写审计，重放安全。
        """
        if action not in BULK_ACTIONS:
            raise ValueError("bulk action not allowed: %r" % (action[:32],))
        results: list[dict[str, Any]] = []
        with self._lock:
            conn = self._connect()
            try:
                for item_id in ids:
                    row = conn.execute(
                        "SELECT status FROM memory_item WHERE id = ?", (item_id,)
                    ).fetchone()
                    if row is None:
                        results.append({"id": item_id, "ok": False, "code": "not_found"})
                        continue
                    if row["status"] == _BULK_TARGETS[action]:
                        # 已处于目标状态：计为成功，不重复写审计（重放安全）
                        results.append({"id": item_id, "ok": True, "code": "unchanged"})
                        continue
                    try:
                        results.append(self._transition_locked(conn, item_id, action, reason, None))
                    except (RecordNotFound, StateConflict, ValueError) as exc:
                        results.append({
                            "id": item_id, "ok": False, "code": "state_conflict",
                            "detail": str(exc)[:120],
                        })
                conn.commit()
            except sqlite3.Error as exc:
                conn.rollback()
                raise StoreUnavailable("memory store error") from exc
        return results

    def delete(self, item_id: str) -> bool:
        with self._lock:
            conn = self._connect()
            row = conn.execute(
                "SELECT kind FROM memory_item WHERE id = ?", (item_id,)
            ).fetchone()
            if row is None:
                return False
            conn.execute("DELETE FROM memory_item WHERE id = ?", (item_id,))
            conn.execute("DELETE FROM memory_audit WHERE item_id = ?", (item_id,))
            conn.execute(
                "INSERT INTO memory_audit (ts, item_kind, item_id, action, reason) VALUES (?,?,?,?,?)",
                (utcnow(), row["kind"], item_id, "delete", ""),
            )
            conn.commit()
            return True

    def set_flag(self, item_id: str, flag: str, enabled: bool = True) -> None:
        with self._lock:
            conn = self._connect()
            row = conn.execute(
                "SELECT flags FROM memory_item WHERE id = ?", (item_id,)
            ).fetchone()
            if row is None:
                raise RecordNotFound(item_id)
            flags = _decode_json(row["flags"], [])
            current = set(flags)
            if enabled:
                current.add(flag)
            else:
                current.discard(flag)
            conn.execute(
                "UPDATE memory_item SET flags = ?, updated_at = ? WHERE id = ?",
                (json.dumps(sorted(current), ensure_ascii=False), utcnow(), item_id),
            )
            conn.commit()

    # ------------------------------------------------------------- 反馈

    def _recompute_confidence(self, conn: sqlite3.Connection, item_id: str) -> float:
        row = conn.execute(
            "SELECT source_kind, helpful_count, harmful_count, verified_success_count,"
            " verified_failure_count FROM memory_item WHERE id = ?", (item_id,)
        ).fetchone()
        base = SOURCE_PRIORS.get(row["source_kind"], 0.35)
        alpha = base * PRIOR_WEIGHT + row["helpful_count"] + row["verified_success_count"]
        beta = (1.0 - base) * PRIOR_WEIGHT + row["harmful_count"] + row["verified_failure_count"]
        return round(alpha / (alpha + beta), 4)

    def record_exposure(self, ids: Sequence[str]) -> None:
        """只增加曝光计数。命中次数不构成「这条经验是对的」的任何证据。"""
        if not ids:
            return
        now = utcnow()
        # 同一次注入里重复传同一条不该被算成两次曝光，否则调用方的重试会把
        # 计数灌水，观测数据就废了。
        unique = list(dict.fromkeys(str(item_id) for item_id in ids))
        with self._lock:
            conn = self._connect()
            for item_id in unique:
                conn.execute(
                    "UPDATE memory_item SET exposure_count = exposure_count + 1,"
                    " last_exposed_at = ? WHERE id = ?",
                    (now, item_id),
                )
            conn.commit()

    FEEDBACK_COLUMNS: Mapping[str, tuple[str, bool]] = {
        "helpful": ("helpful_count", False),
        "harmful": ("harmful_count", False),
        "verified_success": ("verified_success_count", True),
        "verified_failure": ("verified_failure_count", True),
    }

    def record_feedback(self, item_id: str, feedback: str) -> dict[str, Any]:
        column, verified = self.FEEDBACK_COLUMNS.get(feedback, (None, False))
        if column is None:
            raise ValueError("invalid feedback: %r" % (feedback[:32],))
        with self._lock:
            conn = self._connect()
            row = conn.execute(
                "SELECT id FROM memory_item WHERE id = ?", (item_id,)
            ).fetchone()
            if row is None:
                raise RecordNotFound(item_id)
            now = utcnow()
            if verified:
                conn.execute(
                    "UPDATE memory_item SET %s = %s + 1, last_verified_at = ?,"
                    " updated_at = ? WHERE id = ?" % (column, column),
                    (now, now, item_id),
                )
            else:
                conn.execute(
                    "UPDATE memory_item SET %s = %s + 1, updated_at = ? WHERE id = ?"
                    % (column, column),
                    (now, item_id),
                )
            confidence = self._recompute_confidence(conn, item_id)
            conn.execute(
                "UPDATE memory_item SET confidence = ? WHERE id = ?", (confidence, item_id)
            )
            conn.execute(
                "INSERT INTO memory_audit (ts, item_kind, item_id, action, reason)"
                " VALUES (?,?,?,?,?)",
                (now, "memory_item", item_id, "feedback", feedback),
            )
            conn.commit()
        return {"id": item_id, "feedback": feedback, "confidence": confidence}

    # ------------------------------------------------------------- 检索

    def recall(
        self,
        *,
        query: str,
        scopes: Sequence[str],
        task_type: str = "",
        tool_names: Sequence[str] = (),
        skill_hashes: Optional[Mapping[str, str]] = None,
        exclude_ids: Sequence[str] = (),
        limit: int = RECALL_LIMIT,
    ) -> list[dict[str, Any]]:
        """检索可用于注入的经验。

        硬过滤：只有 ``approved``、作用域命中、Skill 版本兼容。至少要有任务类型
        或工具序列的实质重叠，纯词法命中一律丢弃——那多半只是措辞像而已。
        """
        text = (query or "").strip()
        if not text:
            return []
        scope_set = {normalize_scope(scope) for scope in scopes}
        if not scope_set:
            return []
        exclude = set(exclude_ids)
        ids = self._search_ids(text, RECALL_POOL)
        if not ids:
            return []
        placeholders = ",".join("?" for _ in ids)
        with self._lock:
            rows = self._execute(
                "SELECT * FROM memory_item WHERE kind = ? AND status = ? AND id IN (%s)"
                % placeholders,
                [KIND_EXPERIENCE, STATUS_APPROVED] + ids,
            ).fetchall()
        wanted_tools = {str(name) for name in tool_names if name}
        scored: list[dict[str, Any]] = []
        for row in rows:
            item = _row_to_item(row)
            if item["id"] in exclude or item["scope"] not in scope_set:
                continue
            if not _skill_version_compatible(item, skill_hashes):
                continue
            overlap = 0.0
            if task_type and item["task_type"] and task_type == item["task_type"]:
                overlap = 1.0
            elif wanted_tools and item.get("tool_sequence"):
                shared = wanted_tools & set(item["tool_sequence"])
                if shared:
                    overlap = min(1.0, 0.5 + 0.5 * len(shared) / max(1, len(item["tool_sequence"])))
            if overlap <= 0.0:
                continue
            scored.append({"item": item, "overlap": overlap})
        if not scored:
            return []
        raw = self._raw_ranks(ids)
        best = min(raw.values()) if raw else 0.0
        worst = max(raw.values()) if raw else 0.0
        span = (worst - best) or 1.0
        for entry in scored:
            item = entry["item"]
            rank = raw.get(item["id"])
            lexical = 1.0 if rank is None else 1.0 - (rank - best) / span
            recency = 1.0 / (1.0 + days_since(item["updated_at"]) / 30.0)
            entry["score"] = round(
                0.55 * lexical + 0.20 * entry["overlap"]
                + 0.15 * float(item["confidence"]) + 0.10 * recency,
                4,
            )
            entry["parts"] = {
                "lexical": round(lexical, 4),
                "metadata": round(entry["overlap"], 4),
                "confidence": float(item["confidence"]),
                "recency": round(recency, 4),
            }
        scored.sort(key=lambda entry: entry["score"], reverse=True)
        return scored[: max(1, limit)]

    def _raw_ranks(self, ids: Sequence[str]) -> dict[str, float]:
        fts_query = None
        if self._fts:
            with self._lock:
                row = self._execute(
                    "SELECT title, search_text FROM memory_item WHERE id = ?", (ids[0],)
                ).fetchone()
            if row is not None:
                fts_query = build_fts_query(row["search_text"][:200])
        if not self._fts or not fts_query:
            return {}
        placeholders = ",".join("?" for _ in ids)
        with self._lock:
            try:
                rows = self._execute(
                    "SELECT m.id AS id, bm25(memory_item_fts) AS rank FROM memory_item_fts f"
                    " JOIN memory_item m ON m.rowid = f.rowid"
                    " WHERE memory_item_fts MATCH ? AND m.id IN (%s)" % placeholders,
                    [fts_query] + list(ids),
                ).fetchall()
            except StoreUnavailable:
                return {}
        return {row["id"]: float(row["rank"]) for row in rows}

    # ------------------------------------------------------------- 治理

    def run_governance(self) -> dict[str, int]:
        """候选合并、标记疑似重复、超龄归档、按作用域限流。

        只输出数字，不输出经验正文：维护报告的职责是告诉人「有多少东西等着看」，
        不是把可疑内容再抄一遍。
        """
        merged = 0
        flagged = 0
        archived = 0
        with self._lock:
            conn = self._connect()
            try:
                rows = conn.execute(
                    "SELECT id, kind, scope, task_type, title, payload, evidence_count,"
                    " updated_at FROM memory_item WHERE status = ? ORDER BY created_at",
                    (STATUS_CANDIDATE,),
                ).fetchall()
                groups: dict[tuple[str, str, str], list[sqlite3.Row]] = {}
                for row in rows:
                    groups.setdefault(
                        (row["kind"], row["scope"], row["task_type"]), []
                    ).append(row)

                for _key, members in groups.items():
                    merged += self._merge_group(conn, members)
                    flagged += self._flag_group(conn, members)

                cutoff = (
                    datetime.now(timezone.utc) - timedelta(days=CANDIDATE_MAX_AGE_DAYS)
                ).isoformat(timespec="microseconds")
                stale = conn.execute(
                    "SELECT id, kind FROM memory_item WHERE status = ? AND updated_at < ?",
                    (STATUS_CANDIDATE, cutoff),
                ).fetchall()
                for row in stale:
                    conn.execute(
                        "UPDATE memory_item SET status = ?, updated_at = ? WHERE id = ?",
                        (STATUS_REJECTED, utcnow(), row["id"]),
                    )
                    conn.execute(
                        "INSERT INTO memory_audit (ts, item_kind, item_id, action, reason)"
                        " VALUES (?,?,?,?,?)",
                        (utcnow(), row["kind"], row["id"], "reject", "aged_out"),
                    )
                    archived += 1

                archived += self._enforce_candidate_cap(conn)
                conn.commit()
            except sqlite3.Error as exc:
                conn.rollback()
                raise StoreUnavailable("memory store error") from exc
        return {"merged": merged, "flagged": flagged, "archived": archived}

    def _merge_group(self, conn: sqlite3.Connection, members: Sequence[sqlite3.Row]) -> int:
        if len(members) < 2 or len(members) > 300:
            return 0
        texts = [_candidate_text(row) for row in members]
        dropped: set[int] = set()
        merged = 0
        for i in range(len(members)):
            if i in dropped:
                continue
            for j in range(i + 1, len(members)):
                if j in dropped:
                    continue
                if jaccard(texts[i], texts[j]) < MERGE_THRESHOLD:
                    continue
                keeper, victim = members[i], members[j]
                conn.execute(
                    "UPDATE memory_item SET evidence_count = evidence_count + ?,"
                    " updated_at = ? WHERE id = ?",
                    (victim["evidence_count"], utcnow(), keeper["id"]),
                )
                conn.execute("DELETE FROM memory_item WHERE id = ?", (victim["id"],))
                conn.execute(
                    "INSERT INTO memory_audit (ts, item_kind, item_id, action, reason)"
                    " VALUES (?,?,?,?,?)",
                    (utcnow(), victim["kind"], victim["id"], "merge", keeper["id"]),
                )
                dropped.add(j)
                merged += 1
        return merged

    def _flag_group(self, conn: sqlite3.Connection, members: Sequence[sqlite3.Row]) -> int:
        if len(members) < 2 or len(members) > 300:
            return 0
        texts = [_candidate_text(row) for row in members]
        flagged = 0
        for i in range(len(members)):
            for j in range(i + 1, len(members)):
                score = jaccard(texts[i], texts[j])
                if not (DUPLICATE_HINT_THRESHOLD <= score < MERGE_THRESHOLD):
                    continue
                for row in (members[i], members[j]):
                    if "possible_duplicate" in _decode_json(row["payload"], {}):
                        continue
                    current = conn.execute(
                        "SELECT flags FROM memory_item WHERE id = ?", (row["id"],)
                    ).fetchone()
                    if current is None:
                        continue
                    flags = set(_decode_json(current["flags"], []))
                    if "possible_duplicate" in flags:
                        continue
                    flags.add("possible_duplicate")
                    conn.execute(
                        "UPDATE memory_item SET flags = ? WHERE id = ?",
                        (json.dumps(sorted(flags), ensure_ascii=False), row["id"]),
                    )
                    flagged += 1
        return flagged

    def _enforce_candidate_cap(self, conn: sqlite3.Connection) -> int:
        rows = conn.execute(
            "SELECT id, kind, scope FROM memory_item WHERE status = ?"
            " ORDER BY evidence_count ASC, created_at ASC",
            (STATUS_CANDIDATE,),
        ).fetchall()
        per_scope: dict[tuple[str, str], int] = {}
        for row in rows:
            per_scope[(row["kind"], row["scope"])] = per_scope.get(
                (row["kind"], row["scope"]), 0
            ) + 1
        overflow = {
            key: count - CANDIDATE_CAP_PER_SCOPE
            for key, count in per_scope.items()
            if count > CANDIDATE_CAP_PER_SCOPE
        }
        if not overflow:
            return 0
        archived = 0
        for row in rows:
            key = (row["kind"], row["scope"])
            if overflow.get(key, 0) <= 0:
                continue
            overflow[key] -= 1
            conn.execute(
                "UPDATE memory_item SET status = ?, updated_at = ? WHERE id = ?",
                (STATUS_REJECTED, utcnow(), row["id"]),
            )
            conn.execute(
                "INSERT INTO memory_audit (ts, item_kind, item_id, action, reason)"
                " VALUES (?,?,?,?,?)",
                (utcnow(), row["kind"], row["id"], "reject", "candidate_cap"),
            )
            archived += 1
        return archived

    # ------------------------------------------------------------- 维护

    def wipe(self) -> dict[str, int]:
        """清空全部长期记忆。只给测试与将来的「危险区」用，接口层单独包确认。"""
        with self._lock:
            conn = self._connect()
            counts = {
                row["kind"]: int(row["n"])
                for row in conn.execute(
                    "SELECT kind, COUNT(*) AS n FROM memory_item GROUP BY kind"
                ).fetchall()
            }
            conn.execute("DELETE FROM memory_item")
            conn.execute("DELETE FROM memory_audit")
            conn.commit()
            return counts


def _skill_version_compatible(
    item: Mapping[str, Any], skill_hashes: Optional[Mapping[str, str]]
) -> bool:
    """Skill 变了，绑在旧 Skill 上的经验不能再自动注入，等人工复核。"""
    scope = str(item.get("scope") or "")
    if not scope.startswith("skill:") or not skill_hashes:
        return True
    recorded = str((item.get("source_version") or {}).get("skill_hash") or "")
    if not recorded:
        return True
    current = skill_hashes.get(scope.split(":", 1)[1])
    if not current:
        return True
    return recorded == current


def _candidate_text(row: sqlite3.Row) -> str:
    payload = _decode_json(row["payload"], {})
    chunks = [str(row["title"])]
    for key in ("strategy_steps", "trigger_cues", "failure_patterns", "tags"):
        value = payload.get(key)
        if isinstance(value, list):
            chunks.extend(str(item) for item in value)
    fact_text = payload.get("fact_text")
    if fact_text:
        chunks.append(str(fact_text))
    return " ".join(chunks)


def _row_to_summary(row: sqlite3.Row) -> dict[str, Any]:
    flags = _decode_json(row["flags"], [])
    return {
        "id": row["id"],
        "kind": row["kind"],
        "status": row["status"],
        "scope": row["scope"],
        "task_type": row["task_type"],
        "title": row["title"],
        "confidence": float(row["confidence"]),
        "evidence_count": int(row["evidence_count"]),
        "possible_duplicate": "possible_duplicate" in flags,
        "created_at": row["created_at"],
        "updated_at": row["updated_at"],
    }


def _row_to_item(row: sqlite3.Row) -> dict[str, Any]:
    item = _row_to_summary(row)
    item.update({
        "schema_version": int(row["schema_version"]),
        "source_kind": row["source_kind"],
        "source_version": _decode_json(row["source_version"], {}),
        "fingerprint": row["fingerprint"],
        "flags": _decode_json(row["flags"], []),
        "exposure_count": int(row["exposure_count"]),
        "helpful_count": int(row["helpful_count"]),
        "harmful_count": int(row["harmful_count"]),
        "verified_success_count": int(row["verified_success_count"]),
        "verified_failure_count": int(row["verified_failure_count"]),
        "last_exposed_at": row["last_exposed_at"],
        "last_verified_at": row["last_verified_at"],
    })
    item.update(_decode_json(row["payload"], {}))
    return item