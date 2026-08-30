import argparse
import asyncio
import json
import logging
from contextlib import asynccontextmanager
from datetime import datetime
from pathlib import Path
from typing import Optional

import uvicorn
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, StreamingResponse

from agent_loop import run_agent_loop
from config import ApiConfig, load_config, save_config, api_config_to_dict
from context import ContextManager
from logging_setup import setup_logging
from mcp_bridge import McpBridge
from session import (
    InvalidSessionId,
    clear_session,
    create_session,
    delete_session,
    list_sessions,
    load_session,
    locked_session,
    save_session,
    set_archived,
    update_index,
    validate_session_id,
)
from skill_runtime import SkillError, SkillRegistry, tool_is_allowed
from workflow_runner import run_workflow

setup_logging()
logger = logging.getLogger(__name__)

current_config: Optional[ApiConfig] = load_config()
ctx_mgr = ContextManager()
skill_registry = SkillRegistry(SkillRegistry.default_roots())

_mcp: Optional[McpBridge] = None
_session_async_locks = {}
_pending_approvals = {}


def _session_async_lock(session_id: str):
    validate_session_id(session_id)
    lock = _session_async_locks.get(session_id)
    if lock is None:
        lock = asyncio.Lock()
        _session_async_locks[session_id] = lock
    return lock


async def _request_tool_approval(session_id, call_id, name, args):
    loop = asyncio.get_running_loop()
    future = loop.create_future()
    key = "%s:%s" % (session_id, call_id)
    _pending_approvals[key] = future
    logger.info("tool approval pending: %s %s", session_id, name)
    try:
        return await future
    finally:
        _pending_approvals.pop(key, None)


@asynccontextmanager
async def lifespan(app: FastAPI):
    global _mcp
    # SSE 方式：server.py 独立运行（SSE 模式监听 5656），app.py 通过 SSE 连接
    import os
    _mcp_url = os.environ.get("MCP_SERVER_URL", "http://127.0.0.1:5656/sse")
    _mcp = McpBridge(_mcp_url)
    try:
        await _mcp.connect()
    except Exception as e:
        logger.error(f"mcp connect failed: {e}")
    yield
    if _mcp:
        await _mcp.disconnect()


app = FastAPI(lifespan=lifespan)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["http://localhost:*", "http://127.0.0.1:*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.get("/health")
async def health():
    return {"status": "ok", "config_loaded": current_config is not None}


@app.get("/skills")
async def get_skills():
    skill_registry.reload()
    return {"skills": [
        {
            "id": skill.id, "name": skill.name, "description": skill.description,
            "version": skill.version, "source": skill.source,
            "content_hash": skill.content_hash, "allowed_tools": skill.allowed_tools,
            "shadowed": [
                {"source": item.source, "version": item.version, "content_hash": item.content_hash}
                for item in skill_registry.shadowed(skill.id)
            ],
        }
        for skill in skill_registry.all()
    ]}


@app.post("/config")
async def update_config(cfg: dict):
    global current_config
    try:
        # 使用 api_config_to_dict 的逆操作构造
        models_raw = cfg.get("models", [])
        from config import ModelEntry
        models = []
        for m in models_raw:
            if isinstance(m, dict):
                models.append(ModelEntry(
                    provider=str(m.get("provider", "")),
                    model_id=str(m.get("model_id", "")),
                ))
        current_config = ApiConfig(
            api_type=str(cfg.get("api_type", "openai")),
            api_url=str(cfg.get("api_url", "")),
            api_key=str(cfg.get("api_key", "")),
            models=models,
            default_model_index=int(cfg.get("default_model_index", 0)),
        )
        save_config(current_config)
        return {"ok": True}
    except Exception as e:
        return JSONResponse({"error": str(e)}, status_code=400)


@app.get("/config/models")
async def get_models():
    if current_config is None:
        return {"models": [], "default_index": 0}
    return {
        "models": [
            {"provider": m.provider, "model_id": m.model_id}
            for m in current_config.models
        ],
        "default_index": current_config.default_model_index,
    }


@app.post("/chat/stream")
async def chat_stream(body: dict):
    if current_config is None:
        return JSONResponse(
            {"error": "no config, POST /config first"}, status_code=400
        )
    if _mcp is None:
        return JSONResponse({"error": "mcp not ready"}, status_code=503)

    session_id = body.get("session_id", "") or ""
    message = body.get("message", "") or ""
    system_prompt = body.get("system_prompt", "") or ""
    selected_skills = body.get("selected_skills", []) or []
    skill_roots = body.get("skill_roots", []) or []
    writable_skill_root = body.get("writable_skill_root", "") or ""
    model_id = body.get("model_id", "") or ""

    if skill_roots:
        # Qt sends authoritative local roots so deployed and development layouts work alike.
        skill_registry.set_roots(
            [str(path) for path in SkillRegistry.default_roots()] +
            [str(path) for path in skill_roots],
            writable_skill_root,
        )
    else:
        skill_registry.reload()

    if not isinstance(selected_skills, list):
        return JSONResponse({"error": "selected_skills must be an array"}, status_code=400)

    interaction_mode = body.get("interaction_mode", "manual")
    attachments = body.get("attachments", [])
    display_content = body.get("display_content", "")

    if not session_id or not message:
        return JSONResponse({"error": "missing session_id or message"}, status_code=400)
    try:
        session_id = validate_session_id(session_id)
    except InvalidSessionId as e:
        return JSONResponse({"error": str(e)}, status_code=400)

    save_events = {"tool_call", "tool_result", "done"}

    async def gen():
        bg = _get_or_create_background(session_id)
        bg.subscriber_count += 1

        # 取消待执行的清理定时器（有订阅者了）
        _cleanup_timer = getattr(bg, '_cleanup_timer', None)
        if _cleanup_timer is not None:
            _cleanup_timer.cancel()
            bg._cleanup_timer = None

        try:
            # 启动后台 task（如果未运行或已结束）
            if bg.task is None or bg.task.done():
                if bg.task and bg.task.done() and not bg.cancelled:
                    # task 已自然结束，需要区分：SSE 重连 vs 新的用户消息
                    if message and message != bg.last_message:
                        # 新的用户消息 — 重置 bg 状态，走下方启动新 task
                        bg.queue = asyncio.Queue()
                        bg.done_event = asyncio.Event()
                        bg.cancelled = False
                        bg.started_at = None
                        bg.last_message = message
                    else:
                        # SSE 重连 — 回放最后一条 assistant 消息
                        session = load_session(session_id)
                        if session:
                            for msg in reversed(session.messages):
                                if msg.get("role") == "assistant":
                                    content = msg.get("content", "") or ""
                                    yield f"event: text_chunk\ndata: {json.dumps({'delta': content}, ensure_ascii=False)}\n\n"
                                    break
                        yield f"event: done\ndata: {json.dumps({}, ensure_ascii=False)}\n\n"
                        return
                else:
                    # task 为 None（首次请求），记录消息
                    bg.last_message = message

                # 启动新的后台 task
                async with _session_async_lock(session_id):
                    bg.last_message = message
                    bg.task = asyncio.create_task(
                        _run_background_loop(
                            bg, session_id, message, system_prompt,
                            selected_skills, attachments, display_content,
                            interaction_mode, model_id,
                        )
                    )

            # 消费 queue 事件直到完成
            while not bg.done_event.is_set() or not bg.queue.empty():
                try:
                    event = await asyncio.wait_for(bg.queue.get(), timeout=0.5)
                    yield f"event: {event['type']}\ndata: {json.dumps(event, ensure_ascii=False)}\n\n"
                    if event["type"] == "done":
                        break
                except asyncio.TimeoutError:
                    continue

            # drain 剩余事件
            async for event in _drain_queue(bg):
                yield f"event: {event['type']}\ndata: {json.dumps(event, ensure_ascii=False)}\n\n"

        except asyncio.CancelledError:
            # 只断开 SSE 连接，不 cancel 后台 task
            logger.info(
                f"SSE client disconnected from session {session_id}, "
                f"background task continues"
            )
        finally:
            bg.subscriber_count -= 1
            if bg.subscriber_count <= 0 and bg.done_event.is_set():
                _schedule_cleanup(bg)

    return StreamingResponse(gen(), media_type="text/event-stream")


def _latest_active_skills(session) -> list:
    for message in reversed(session.messages):
        if message.get("role") == "assistant" and message.get("active_skills"):
            return list(message.get("active_skills", []))
    return []


def _workflow_tool_policy(selected_skills: list):
    selected_ids = []
    for item in selected_skills or []:
        skill_id = str(item.get("id", "") if isinstance(item, dict) else item).strip().lower()
        if not skill_id or skill_id in selected_ids:
            continue
        try:
            skill_registry.get(skill_id)
        except SkillError:
            continue
        selected_ids.append(skill_id)

    restrictive = [
        skill_registry.get(skill_id).allowed_tools
        for skill_id in selected_ids
        if skill_registry.get(skill_id).allowed_tools
    ]

    def allowed(tool_name: str) -> bool:
        return all(tool_is_allowed(tool_name, patterns) for patterns in restrictive)

    return allowed


@app.post("/workflows/run")
async def workflow_stream(body: dict):
    if _mcp is None:
        return JSONResponse({"error": "mcp not ready"}, status_code=503)
    body = body or {}
    try:
        session_id = validate_session_id(body.get("session_id", ""))
    except InvalidSessionId as e:
        return JSONResponse({"error": str(e)}, status_code=400)
    steps = body.get("steps", [])
    if not isinstance(steps, list):
        return JSONResponse({"error": "steps must be an array"}, status_code=400)
    selected_skills = body.get("selected_skills", [])
    if not isinstance(selected_skills, list):
        return JSONResponse({"error": "selected_skills must be an array"}, status_code=400)
    skill_roots = body.get("skill_roots", [])
    writable_skill_root = body.get("writable_skill_root", "")
    if skill_roots:
        skill_registry.set_roots(
            [str(path) for path in SkillRegistry.default_roots()] +
            [str(path) for path in skill_roots],
            writable_skill_root,
        )
    else:
        skill_registry.reload()

    async def gen():
        lock = _session_async_lock(session_id)
        await lock.acquire()
        session = load_session(session_id)
        if session is None:
            lock.release()
            yield "event: error\ndata: %s\n\n" % json.dumps({"message": "session not found"})
            return
        try:
            policy_skills = list(selected_skills) + _latest_active_skills(session)
            tool_allowed = _workflow_tool_policy(policy_skills)
            async for event in run_workflow(
                session, steps, _mcp, _request_tool_approval,
                tool_allowed=tool_allowed,
                persist_result=ctx_mgr.persist_large_result,
            ):
                if event["type"] in {"workflow_started", "workflow_step", "workflow_done"}:
                    with locked_session(session_id):
                        save_session(session)
                        update_index(session)
                yield "event: %s\ndata: %s\n\n" % (
                    event["type"], json.dumps(event, ensure_ascii=False)
                )
            yield "event: done\ndata: %s\n\n" % json.dumps({"session_id": session_id})
        except asyncio.CancelledError:
            with locked_session(session_id):
                save_session(session)
                update_index(session)
            raise
        except Exception as exc:
            logger.exception("workflow failed")
            with locked_session(session_id):
                save_session(session)
                update_index(session)
            yield "event: workflow_done\ndata: %s\n\n" % json.dumps(
                {"status": "failed", "message": str(exc)}, ensure_ascii=False
            )
            yield "event: done\ndata: %s\n\n" % json.dumps({"session_id": session_id})
        finally:
            if lock.locked():
                lock.release()

    return StreamingResponse(gen(), media_type="text/event-stream")


@app.get("/sessions")
async def get_sessions(query: str = "", archived: bool = False):
    return {"sessions": list_sessions(query=query, archived=archived)}


@app.post("/sessions")
async def new_session(body: dict = None):
    body = body or {}
    title = body.get("title", "New Session")
    model_id = body.get("model_id", "") or ""
    s = create_session(title, model_id=model_id)
    return {"id": s.id, "title": s.title, "created_at": s.created_at}


@app.get("/sessions/{session_id}")
async def get_session(session_id: str):
    try:
        s = load_session(session_id)
    except InvalidSessionId as e:
        return JSONResponse({"error": str(e)}, status_code=400)
    if s is None:
        return JSONResponse({"error": "not found"}, status_code=404)
    return {
        "meta": {
            "id": s.id,
            "title": s.title,
            "created_at": s.created_at,
            "updated_at": s.updated_at,
            "model_id": s.model_id,
        },
        "messages": s.messages,
    }


@app.put("/sessions/{session_id}/rename")
async def rename_session(session_id: str, body: dict = None):
    body = body or {}
    try:
        session_id = validate_session_id(session_id)
    except InvalidSessionId as e:
        return JSONResponse({"error": str(e)}, status_code=400)
    new_title = body.get("title", "").strip()
    if not new_title:
        return JSONResponse({"error": "title is required"}, status_code=400)
    async with _session_async_lock(session_id):
        with locked_session(session_id):
            s = load_session(session_id)
            if s is None:
                return JSONResponse({"error": "not found"}, status_code=404)
            old_title = s.title
            s.title = new_title
            s.updated_at = datetime.now().isoformat()
            save_session(s)
            update_index(s)
    logger.info(f"session renamed: {session_id} | {old_title} -> {new_title}")
    return {"ok": True, "id": s.id, "title": s.title}


@app.delete("/sessions/{session_id}")
async def remove_session(session_id: str):
    try:
        session_id = validate_session_id(session_id)
        async with _session_async_lock(session_id):
            deleted = delete_session(session_id)
    except InvalidSessionId as e:
        return JSONResponse({"error": str(e)}, status_code=400)
    return {"ok": deleted}


@app.post("/sessions/{session_id}/clear")
async def clear_session_endpoint(session_id: str):
    try:
        session_id = validate_session_id(session_id)
        async with _session_async_lock(session_id):
            cleared = clear_session(session_id)
    except InvalidSessionId as e:
        return JSONResponse({"error": str(e)}, status_code=400)
    return {"ok": cleared}


@app.put("/sessions/{session_id}/archive")
async def archive_session(session_id: str, body: dict = None):
    try:
        session_id = validate_session_id(session_id)
        async with _session_async_lock(session_id):
            changed = set_archived(session_id, bool((body or {}).get("archived", True)))
    except InvalidSessionId as e:
        return JSONResponse({"error": str(e)}, status_code=400)
    if not changed:
        return JSONResponse({"error": "not found"}, status_code=404)
    return {"ok": True}


@app.post("/sessions/{session_id}/tool-approvals/{call_id}")
async def resolve_tool_approval(session_id: str, call_id: str, body: dict = None):
    try:
        session_id = validate_session_id(session_id)
    except InvalidSessionId as e:
        return JSONResponse({"error": str(e)}, status_code=400)
    key = "%s:%s" % (session_id, call_id)
    future = _pending_approvals.get(key)
    if future is None or future.done():
        return JSONResponse({"error": "approval not found"}, status_code=404)
    future.set_result(body or {"approved": False})
    return {"ok": True}


# ============================================================
# Background agent session management
# 使 agent_loop 脱离 SSE 连接生命周期，切换对话不断后台回复
# ============================================================


class BackgroundSession:
    """管理一个 session 的后台 agent_loop 任务。

    - task: 独立运行的 asyncio.Task，不受 SSE 连接生命周期约束
    - queue: asyncio.Queue，task 生产事件，SSE 消费事件
    - done_event: task 结束后设置，通知消费者 drain 剩余事件
    - subscriber_count: 当前 SSE 连接数，用于清理判断
    """

    def __init__(self, session_id: str):
        self.session_id = session_id
        self.task = None
        self.queue = asyncio.Queue()
        self.done_event = asyncio.Event()
        self.subscriber_count = 0
        self.started_at = None
        self.cancelled = False
        self.last_message = None  # 记录上次处理的消息，用于区分 SSE 重连与新消息


_background_sessions = {}
_BG_CLEANUP_SECONDS = 300  # 后台任务结束后，保留 5 分钟等重连


def _get_or_create_background(session_id: str) -> BackgroundSession:
    """获取或创建 session 对应的 BackgroundSession。"""
    bg = _background_sessions.get(session_id)
    if bg is None:
        bg = BackgroundSession(session_id)
        _background_sessions[session_id] = bg
    return bg


async def _run_background_loop(
    bg: BackgroundSession,
    session_id: str,
    message: str,
    system_prompt: str,
    selected_skills: list,
    attachments: list,
    display_content: str,
    interaction_mode: str,
    model_id: str,
):
    """后台独立运行 agent_loop，事件写入 queue。

    与 SSE 连接完全解耦，连接断开后 task 继续运行。
    """
    session = load_session(session_id)
    if session is None:
        await bg.queue.put({
            "type": "error",
            "message": "session not found",
            "retryable": False,
        })
        bg.done_event.set()
        return

    bg.started_at = datetime.now().isoformat()

    try:
        async for event in run_agent_loop(
            session, message, system_prompt, current_config, _mcp, ctx_mgr,
            skill_registry, selected_skills, _request_tool_approval,
            attachments=attachments, display_content=display_content,
            interaction_mode=interaction_mode,
            model_override=model_id if model_id else None,
        ):
            # 工具调用与返回值日志
            if event["type"] == "tool_call":
                logger.info("[tool_call] session=%s name=%s args=%s", session_id,
                            event.get("name", ""),
                            json.dumps(event.get("args", {}), ensure_ascii=False)[:500])
            elif event["type"] == "tool_result":
                _result_preview = str(event.get("result", ""))[:500]
                logger.info("[tool_result] session=%s name=%s result=%s", session_id,
                            event.get("name", ""), _result_preview)
            elif event["type"] == "error":
                logger.warning("[error] session=%s message=%s", session_id,
                               event.get("message", ""))
            elif event["type"] == "skill_loaded":
                logger.info("[skill_loaded] session=%s skill_id=%s", session_id,
                            event.get("skill_id", ""))

            # 持久化关键事件
            if event["type"] in {"tool_call", "tool_result", "done"}:
                if model_id and session.model_id != model_id:
                    session.model_id = model_id
                with locked_session(session_id):
                    save_session(session)

            await bg.queue.put(event)
    except asyncio.CancelledError:
        bg.cancelled = True
        with locked_session(session_id):
            save_session(session)
        logger.info(f"background task cancelled: {session_id}")
    except Exception as e:
        logger.exception("background agent loop failed")
        await bg.queue.put({
            "type": "error",
            "message": str(e),
            "retryable": False,
        })
    finally:
        await bg.queue.put({"type": "done"})
        bg.done_event.set()


async def _drain_queue(bg: BackgroundSession):
    """drain task 结束后 queue 中剩余的事件。"""
    while not bg.queue.empty():
        event = bg.queue.get_nowait()
        yield event
        if event["type"] == "done":
            break


def _schedule_cleanup(bg: BackgroundSession):
    """调度后台 session 清理（无订阅者且 task 已完成时）。"""
    loop = asyncio.get_running_loop()
    bg._cleanup_timer = loop.call_later(
        _BG_CLEANUP_SECONDS,
        lambda: _do_cleanup(bg),
    )


def _do_cleanup(bg: BackgroundSession):
    """清理后台 session。"""
    if bg.subscriber_count > 0:
        return
    if bg.task and not bg.task.done():
        bg.task.cancel()
    _background_sessions.pop(bg.session_id, None)
    logger.info(f"background session cleaned up: {bg.session_id}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=1231)
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args()
    uvicorn.run(app, host=args.host, port=args.port, log_level="info")
