import json
import os
import shutil
import tempfile
import threading
import uuid
import logging
from contextlib import contextmanager
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

from paths import SESSIONS_DIR

logger = logging.getLogger(__name__)

_index_lock = threading.RLock()
_session_locks = {}
_session_locks_guard = threading.Lock()


class InvalidSessionId(ValueError):
    pass


def validate_session_id(session_id: str) -> str:
    try:
        return str(uuid.UUID(str(session_id)))
    except (ValueError, AttributeError, TypeError):
        raise InvalidSessionId("invalid session id")


def session_dir(session_id: str) -> Path:
    normalized = validate_session_id(session_id)
    root = SESSIONS_DIR.resolve()
    target = (root / normalized).resolve()
    try:
        target.relative_to(root)
    except ValueError:
        raise InvalidSessionId("session path escapes data directory")
    return target


def _lock_for(session_id: str):
    normalized = validate_session_id(session_id)
    with _session_locks_guard:
        lock = _session_locks.get(normalized)
        if lock is None:
            lock = threading.RLock()
            _session_locks[normalized] = lock
        return lock


@contextmanager
def locked_session(session_id: str):
    lock = _lock_for(session_id)
    lock.acquire()
    try:
        yield
    finally:
        lock.release()


def atomic_write(path: str, data: str):
    dir_ = os.path.dirname(path)
    os.makedirs(dir_, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=dir_, suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(data)
        os.replace(tmp, path)
    except Exception:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def _append_jsonl(path: str, obj: dict):
    """追加一行 JSON 到 JSONL 文件。每行一个 JSON 对象，用换行符分隔。"""
    dir_ = os.path.dirname(path)
    os.makedirs(dir_, exist_ok=True)
    line = json.dumps(obj, ensure_ascii=False) + "\n"
    with open(path, "a", encoding="utf-8") as f:
        f.write(line)


def _read_jsonl(path: str) -> list:
    """读取 JSONL 文件，返回每行解析后的 dict 列表。跳过空行和损坏行。"""
    result = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                result.append(json.loads(line))
            except json.JSONDecodeError:
                logger.warning("jsonl line parse failed, skipped")
    return result


def _write_jsonl(path: str, messages: list):
    """全量写入 JSONL 文件（用于原地修改后的重写）。"""
    dir_ = os.path.dirname(path)
    os.makedirs(dir_, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=dir_, suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            for msg in messages:
                f.write(json.dumps(msg, ensure_ascii=False) + "\n")
        os.replace(tmp, path)
    except Exception:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


@dataclass
class Session:
    id: str
    title: str
    created_at: str
    updated_at: str
    messages: list = field(default_factory=list)
    archived: bool = False
    model_id: str = ""  # 该对话使用的模型 ID，空字符串表示用全局默认
    _dirty_full_write: bool = False  # 标记是否需要全量重写（原地修改触发）

    def ResolvedModelId(self, default_model_id: str = "") -> str:
        """返回该 session 使用的完整 provider/model 标识，空时取 fallback。"""
        return self.model_id if self.model_id else default_model_id

    @property
    def provider_id(self) -> str:
        return self.model_id.split("/", 1)[0] if "/" in self.model_id else ""

    def append_user(self, content: str, active_skills=None, attachments=None, display_content: str = ""):
        message = {"role": "user", "content": content}
        if display_content:
            message["display_content"] = display_content
        if active_skills:
            message["active_skills"] = list(active_skills)
        if attachments:
            message["attachments"] = [
                {"name": item.get("name", ""), "text": item.get("text", "")}
                for item in attachments
            ]
        self.messages.append(message)
        if len(self.messages) == 1 and (self.title == "New Session" or not self.title):
            title_source = content.strip()
            if not title_source and attachments:
                title_source = attachments[0].get("name", "New Session")
            self.title = title_source[:10]
        self.updated_at = datetime.now().isoformat()
        # 追加写：只写这一条消息到 JSONL
        _append_jsonl(str(session_dir(self.id) / "messages.jsonl"), message)

    def append_assistant(self, content: str, active_skills=None, reasoning_content: str = ""):
        message = {"role": "assistant", "content": content}
        if reasoning_content:
            message["reasoning_content"] = reasoning_content
        if active_skills:
            message["active_skills"] = sorted(set(active_skills))
        self.messages.append(message)
        self.updated_at = datetime.now().isoformat()
        _append_jsonl(str(session_dir(self.id) / "messages.jsonl"), message)

    def append_assistant_with_tool_calls(self, text: str, tool_call_events: list, reasoning_content: str = ""):
        tool_calls_stored = [
            {
                "id": tc["id"],
                "type": "function",
                "function": {
                    "name": tc["name"],
                    "arguments": json.dumps(tc["args"], ensure_ascii=False),
                },
            }
            for tc in tool_call_events
        ]
        message = {"role": "assistant", "content": text or None, "tool_calls": tool_calls_stored}
        if reasoning_content:
            message["reasoning_content"] = reasoning_content
        self.messages.append(message)
        self.updated_at = datetime.now().isoformat()
        _append_jsonl(str(session_dir(self.id) / "messages.jsonl"), message)

    def update_tool_call_args(self, tool_call_id: str, args: dict):
        for message in reversed(self.messages):
            for call in message.get("tool_calls", []):
                if call.get("id") == tool_call_id:
                    call["function"]["arguments"] = json.dumps(args, ensure_ascii=False)
                    self.updated_at = datetime.now().isoformat()
                    self._dirty_full_write = True  # 原地修改，标记需要全量重写
                    return

    def append_tool_result(self, tool_call_id: str, result: str, tool_name: str = ""):
        message = {"role": "tool", "tool_call_id": tool_call_id, "content": result}
        if tool_name:
            message["tool_name"] = tool_name
        self.messages.append(message)
        self.updated_at = datetime.now().isoformat()
        _append_jsonl(str(session_dir(self.id) / "messages.jsonl"), message)

    def begin_workflow_run(self, run_id: str, steps: list):
        message = {
            "role": "workflow",
            "run_id": run_id,
            "status": "running",
            "message": "workflow started",
            "steps": steps,
        }
        self.messages.append(message)
        self.updated_at = datetime.now().isoformat()
        _append_jsonl(str(session_dir(self.id) / "messages.jsonl"), message)

    def update_workflow_run(self, run_id: str, steps: list,
                            status: str = "running", message: str = ""):
        for item in reversed(self.messages):
            if item.get("role") == "workflow" and item.get("run_id") == run_id:
                item["steps"] = steps
                item["status"] = status
                item["message"] = message
                self.updated_at = datetime.now().isoformat()
                self._dirty_full_write = True  # 原地修改，标记需要全量重写
                return
        self.begin_workflow_run(run_id, steps)
        self.update_workflow_run(run_id, steps, status, message)

    def append_workflow_run(self, steps: list, status: str, message: str = ""):
        run_id = "legacy-%s" % uuid.uuid4().hex
        self.begin_workflow_run(run_id, steps)
        self.update_workflow_run(run_id, steps, status, message)


def create_session(title: str = "New Session", model_id: str = "") -> Session:
    sid = str(uuid.uuid4())
    now = datetime.now().isoformat()
    s = Session(id=sid, title=title, created_at=now, updated_at=now, model_id=model_id)
    session_dir(sid).joinpath("results").mkdir(parents=True, exist_ok=True)
    save_session(s)
    update_index(s)
    logger.info("session created: %s", sid)
    return s


def save_session(s: Session):
    validate_session_id(s.id)
    target = session_dir(s.id)
    meta = {
        "id": s.id,
        "title": s.title,
        "created_at": s.created_at,
        "updated_at": s.updated_at,
        "archived": bool(s.archived),
        "model_id": s.model_id,
        "provider_id": s.provider_id,
    }
    atomic_write(str(target / "meta.json"), json.dumps(meta, ensure_ascii=False, indent=2))
    # 混合策略：如果有原地修改（_dirty_full_write），全量重写 JSONL；
    # 否则 append 操作已经在各自方法中追加写了，不需要全量重写
    if s._dirty_full_write or not (target / "messages.jsonl").exists():
        _write_jsonl(str(target / "messages.jsonl"), s.messages)
        s._dirty_full_write = False


def load_session(sid: str):
    try:
        target = session_dir(sid)
    except InvalidSessionId:
        raise
    meta_path = target / "meta.json"
    if not meta_path.exists():
        return None
    try:
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
        # 优先读 JSONL，兼容旧版 messages.json
        jsonl_path = target / "messages.jsonl"
        json_path = target / "messages.json"
        if jsonl_path.exists():
            msgs = _read_jsonl(str(jsonl_path))
        elif json_path.exists():
            # 旧版兼容：读取 messages.json 并迁移为 JSONL
            msgs = json.loads(json_path.read_text(encoding="utf-8"))
            if isinstance(msgs, list) and msgs:
                _write_jsonl(str(jsonl_path), msgs)
                logger.info("session %s migrated: messages.json -> messages.jsonl (%d messages)", sid, len(msgs))
        else:
            msgs = []
        return Session(
            id=meta["id"], title=meta.get("title", "New Session"),
            created_at=meta["created_at"], updated_at=meta["updated_at"],
            messages=msgs, archived=bool(meta.get("archived", False)),
            model_id=str(meta.get("model_id", "")),
        )
    except Exception as e:
        logger.error("load_session %s failed: %s", sid, e)
        return None


def _read_index() -> list:
    index_path = SESSIONS_DIR / "index.json"
    if not index_path.exists():
        return []
    try:
        data = json.loads(index_path.read_text(encoding="utf-8"))
        return data if isinstance(data, list) else []
    except Exception:
        return []


def list_sessions(query: str = "", archived: bool = False) -> list:
    query_lower = (query or "").strip().lower()
    result = []
    for item in _read_index():
        if bool(item.get("archived", False)) != bool(archived):
            continue
        if query_lower and query_lower not in str(item.get("title", "")).lower():
            try:
                session = load_session(item.get("id", ""))
            except InvalidSessionId:
                continue
            haystack = " ".join(
                str(msg.get("content", "")) for msg in (session.messages if session else [])
            ).lower()
            if query_lower not in haystack:
                continue
        result.append(item)
    return result


def update_index(s: Session):
    with _index_lock:
        index = [item for item in _read_index() if item.get("id") != s.id]
        index.insert(0, {
            "id": s.id, "title": s.title, "updated_at": s.updated_at,
            "archived": bool(s.archived),
        })
        atomic_write(str(SESSIONS_DIR / "index.json"), json.dumps(index, ensure_ascii=False, indent=2))


def set_archived(sid: str, archived: bool) -> bool:
    with locked_session(sid):
        s = load_session(sid)
        if s is None:
            return False
        s.archived = bool(archived)
        s.updated_at = datetime.now().isoformat()
        save_session(s)
        update_index(s)
        return True


def delete_session(sid: str):
    with locked_session(sid):
        target = session_dir(sid)
        existed = target.exists()
        if existed:
            trash_dir = SESSIONS_DIR / ".trash"
            trash_dir.mkdir(parents=True, exist_ok=True)
            dest = trash_dir / validate_session_id(sid)
            if dest.exists():
                shutil.rmtree(str(dest))
            shutil.move(str(target), str(dest))

        # 即使目录已经缺失，也清理可能残留的索引项，避免"幽灵会话"永远删不掉。
        with _index_lock:
            old_index = _read_index()
            index = [item for item in old_index if item.get("id") != sid]
            index_changed = len(index) != len(old_index)
            if index_changed:
                atomic_write(str(SESSIONS_DIR / "index.json"), json.dumps(index, ensure_ascii=False, indent=2))

        deleted = existed or index_changed
        if deleted:
            logger.info("session deleted: %s", sid)
        return deleted


def clear_session(sid: str):
    with locked_session(sid):
        s = load_session(sid)
        if s is None:
            return False
        s.messages = []
        s._dirty_full_write = True
        s.updated_at = datetime.now().isoformat()
        save_session(s)
        update_index(s)
        logger.info("session cleared: %s", sid)
        return True
