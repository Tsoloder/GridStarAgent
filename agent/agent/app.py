import argparse
import asyncio
import json
import logging
import secrets
import sys
from contextlib import asynccontextmanager
from dataclasses import replace
from datetime import datetime
from pathlib import Path
from typing import Optional

import uvicorn
from fastapi import FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, RedirectResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles

from agent_loop import classify_error, run_agent_loop
from task_ledger import TaskLedger
from config import (
    API_KEY_MASK,
    ApiConfig,
    ConfigError,
    _parse_provider,
    config_from_dict,
    config_revision,
    load_config,
    preserve_masked_api_keys,
    redacted_config,
    save_config,
)
from context import ContextManager
from document_loader import (
    IMAGE_MEDIA_TYPES,
    IMAGE_SUFFIXES,
    MAX_FILE_BYTES,
    SUPPORTED,
    load_attachments,
)
from llm_client.catalog import ModelCatalog as DiscoveryCatalog
from llm_client.providers import AnthropicProvider, OpenAICompatibleProvider, OpenAIProvider
from llm_client.registry import ProviderRegistry, default_adapter_registry
from llm_client.runtime import ModelCatalog as RuntimeCatalog, ModelRuntime
from llm_client.types import ModelConfig as RuntimeModelConfig
from llm_client.types import ProviderConfig as RuntimeProviderConfig
from logging_setup import setup_logging
from mcp_bridge import McpBridge
from paths import UPLOADS_DIR
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
_model_catalog: Optional[DiscoveryCatalog] = None
_model_runtime: Optional[ModelRuntime] = None
_config_lock: Optional[asyncio.Lock] = None
_session_async_locks = {}
_pending_approvals = {}


def _runtime_provider_config(provider) -> RuntimeProviderConfig:
    return RuntimeProviderConfig(
        id=provider.id,
        name=provider.name,
        base_url=provider.base_url,
        api_key=provider.api_key,
        api_key_env=provider.api_key_env,
        headers=dict(provider.headers),
        enabled=provider.enabled,
    )


def _provider_client(provider):
    config = _runtime_provider_config(provider)
    if provider.discovery_api == "anthropic":
        return AnthropicProvider(config)
    if provider.discovery_api == "openai":
        return OpenAIProvider(config)
    return OpenAICompatibleProvider(config)


def _discoverers(config: ApiConfig):
    clients = {}

    async def discover(provider):
        client = _provider_client(provider)
        clients[provider.id] = client
        try:
            return await client.discover_models()
        finally:
            await client.aclose()
            clients.pop(provider.id, None)

    return {"openai": discover, "anthropic": discover}


def _runtime_model(model, config: ApiConfig) -> RuntimeModelConfig:
    provider = config.provider(model.provider)
    return RuntimeModelConfig(
        id=model.id,
        provider=model.provider,
        api=model.api or provider.default_api,
        name=model.name,
        enabled=model.enabled,
        context_window=model.context_window,
        max_output_tokens=model.max_output_tokens,
        capabilities=model.capabilities,
        compat=dict(model.compat),
    )


def _build_runtime(config: ApiConfig):
    providers = ProviderRegistry()
    for provider in config.providers:
        if provider.enabled:
            providers.register(provider.id, _provider_client(provider))
    runtime = ModelRuntime(
        RuntimeCatalog([
            _runtime_model(model, config) for model in config.models
            if model.enabled and config.provider(model.provider).enabled
        ]),
        providers,
        default_adapter_registry(),
    )
    catalog = DiscoveryCatalog(config, _discoverers(config))
    return catalog, runtime


async def _close_runtime(runtime):
    if runtime is not None:
        await runtime.aclose()


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
    global _mcp, _model_catalog, _model_runtime, _config_lock
    _config_lock = asyncio.Lock()
    if current_config is not None:
        _model_catalog, _model_runtime = _build_runtime(current_config)
        await _model_catalog.refresh()

    # SSE 方式：server.py 独立运行（SSE 模式监听 5656），app.py 通过 SSE 连接
    import os
    _mcp_url = os.environ.get("MCP_SERVER_URL", "http://127.0.0.1:5656/sse")
    _mcp = McpBridge(_mcp_url)
    try:
        await _mcp.connect()
    except Exception as e:
        logger.error(f"mcp connect failed: {e}")
    try:
        yield
    finally:
        if _mcp:
            await _mcp.disconnect()
        await _close_runtime(_model_runtime)
        _model_runtime = None
        _model_catalog = None
        _config_lock = None


app = FastAPI(lifespan=lifespan)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["http://localhost:*", "http://127.0.0.1:*"],
    allow_methods=["*"],
    allow_headers=["*"],
)
WEBUI_DIR = Path(__file__).resolve().parents[2] / "webui"


@app.get("/", include_in_schema=False)
async def root_redirect():
    return RedirectResponse(url="/ui/")


@app.get("/ui", include_in_schema=False)
async def webui_redirect():
    return RedirectResponse(url="/ui/")


app.mount("/ui", StaticFiles(directory=str(WEBUI_DIR), html=True), name="webui")
app.mount("/uploads", StaticFiles(directory=str(UPLOADS_DIR)), name="uploads")


@app.post("/upload")
async def upload_file(request: Request, name: str = ""):
    """接收前端拖拽/选择的文件：原始字节落盘到 uploads 目录，返回可引用的路径。"""
    raw_name = (name or "").strip().replace("\\", "/")
    display_name = Path(raw_name).name or "attachment"
    suffix = Path(display_name).suffix.lower()
    if suffix not in SUPPORTED:
        return JSONResponse(
            {"error": "不支持的文件类型 %s" % (suffix or "(无扩展名)")}, status_code=400
        )

    data = await request.body()
    if not data:
        return JSONResponse({"error": "文件内容为空"}, status_code=400)
    if len(data) > MAX_FILE_BYTES:
        return JSONResponse({"error": "单个文件不能超过 10 MB"}, status_code=400)

    stored_name = "%s-%s%s" % (
        datetime.now().strftime("%Y%m%d%H%M%S"), secrets.token_hex(4), suffix
    )
    target = UPLOADS_DIR / stored_name
    try:
        target.write_bytes(data)
    except OSError as exc:
        logger.error("upload write failed: %s", exc)
        return JSONResponse({"error": "写入上传目录失败"}, status_code=500)

    return {
        "name": display_name,
        "path": str(target),
        "kind": "image" if suffix in IMAGE_SUFFIXES else "text",
        "media_type": IMAGE_MEDIA_TYPES.get(suffix, ""),
        "url": "/uploads/%s" % stored_name,
        "size": len(data),
    }


@app.get("/health")
async def health():
    snapshot = _model_catalog.snapshot if _model_catalog is not None else None
    return {
        "status": "ok",
        "config_loaded": current_config is not None,
        "runtime_ready": _model_runtime is not None,
        "catalog_generation": snapshot.generation if snapshot is not None else 0,
        "catalog_models": len(snapshot.models) if snapshot is not None else 0,
        "catalog_errors": dict(snapshot.errors) if snapshot is not None else {},
    }


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


@app.get("/mcp/tools")
async def get_mcp_tools(refresh: bool = False):
    """设置页 MCP 工具列表：默认返回启动时缓存的工具清单，refresh=1 时重新向 MCP 服务读取。"""
    if _mcp is None:
        return JSONResponse({"connected": False, "tools": [], "error": "MCP 未启动"}, status_code=503)
    error = ""
    if refresh:
        try:
            await _mcp.list_tools()
        except Exception as exc:
            # 重新读取失败时保留上一次的缓存清单，只把错误反馈给前端
            logger.warning("mcp list_tools failed: %s", exc)
            error = f"{type(exc).__name__}: {exc}"
    tools = sorted(
        (
            {
                "name": str(tool.name),
                "description": getattr(tool, "description", "") or "",
                "input_schema": _mcp.tool_schema(tool.name),
            }
            for tool in _mcp.available_tools()
        ),
        key=lambda item: item["name"].lower(),
    )
    return {"connected": _mcp.connected, "error": error, "tools": tools}


@app.get("/config")
async def get_config():
    if current_config is None:
        return {"revision": None, "config": None}
    return {
        "revision": config_revision(current_config),
        "config": redacted_config(current_config),
    }


@app.post("/config")
async def update_config(body: dict):
    global current_config, _model_catalog, _model_runtime, _config_lock
    body = body or {}
    expected_revision = body.get("revision")
    raw_config = body.get("config")
    if raw_config is None:
        raw_config = {key: value for key, value in body.items() if key != "revision"}
    if _config_lock is None:
        _config_lock = asyncio.Lock()
    async with _config_lock:
        actual_revision = config_revision(current_config) if current_config is not None else None
        if expected_revision != actual_revision:
            return JSONResponse(
                {"error": "config revision conflict", "revision": actual_revision},
                status_code=409,
            )
        try:
            candidate = config_from_dict(raw_config)
            if current_config is not None:
                candidate = preserve_masked_api_keys(candidate, current_config)
            candidate_catalog, candidate_runtime = _build_runtime(candidate)
            save_config(candidate)
        except (ConfigError, OSError, TypeError, ValueError) as exc:
            if "candidate_runtime" in locals():
                await _close_runtime(candidate_runtime)
            return JSONResponse({"error": str(exc)}, status_code=400)

        previous_runtime = _model_runtime
        current_config = candidate
        _model_catalog = candidate_catalog
        _model_runtime = candidate_runtime
        await _close_runtime(previous_runtime)
        return {
            "ok": True,
            "revision": config_revision(candidate),
            "config": redacted_config(candidate),
        }


def _provider_from_body(body: dict):
    raw = (body or {}).get("provider", body or {})
    if not isinstance(raw, dict):
        raise ConfigError("provider must be an object")
    provider = _parse_provider(raw, 0)
    if provider.api_key == API_KEY_MASK:
        if current_config is None:
            raise ConfigError("masked API key has no saved credential")
        try:
            saved_key = current_config.provider(provider.id).api_key
        except KeyError:
            raise ConfigError("masked API key has no saved credential") from None
        provider = replace(provider, api_key=saved_key)
    return provider


def _safe_provider_error(exc: Exception, provider) -> str:
    message = str(exc)
    for secret in (provider.api_key, provider.resolved_api_key()):
        if secret and secret != API_KEY_MASK:
            message = message.replace(secret, API_KEY_MASK)
    return message


@app.post("/config/providers/test")
async def test_provider(body: dict):
    client = None
    try:
        provider = _provider_from_body(body)
        client = _provider_client(provider)
        response = await client.client().get("")
        response.raise_for_status()
        return {"ok": True, "status_code": response.status_code}
    except (ConfigError, ValueError) as exc:
        return JSONResponse({"error": str(exc)}, status_code=400)
    except Exception as exc:
        return JSONResponse({"error": _safe_provider_error(exc, provider)}, status_code=502)
    finally:
        if client is not None:
            await client.aclose()


@app.post("/config/providers/models")
async def provider_models(body: dict):
    client = None
    try:
        provider = _provider_from_body(body)
        if provider.discovery_api == "none":
            return {"models": []}
        client = _provider_client(provider)
        discovered = await client.discover_models()
        by_id = {}
        for item in discovered:
            model_id = str(item.get("id", "")).strip()
            if model_id and model_id not in by_id:
                by_id[model_id] = {
                    "id": model_id,
                    "name": str(item.get("name") or model_id),
                    "created": item.get("created"),
                    "owned_by": item.get("owned_by"),
                }
        return {"models": [by_id[key] for key in sorted(by_id, key=str.casefold)]}
    except (ConfigError, ValueError) as exc:
        return JSONResponse({"error": str(exc)}, status_code=400)
    except Exception as exc:
        return JSONResponse({"error": _safe_provider_error(exc, provider)}, status_code=502)
    finally:
        if client is not None:
            await client.aclose()


def _catalog_models():
    if _model_catalog is None:
        return []
    return [
        {
            "key": item.key,
            "id": item.config.id,
            "name": item.config.name or item.config.id,
            "provider": item.config.provider,
            "provider_name": current_config.provider(item.config.provider).name or item.config.provider,
            "api": item.config.api or current_config.provider(item.config.provider).default_api,
            "enabled": item.config.enabled,
            "context_window": item.config.context_window,
            "max_output_tokens": item.config.max_output_tokens,
            "capabilities": vars(item.config.capabilities),
            "status": item.status,
            "created": item.created,
            "owned_by": item.owned_by,
        }
        for item in _model_catalog.snapshot.models.values()
        if (
            item.config.enabled
            and current_config.provider(item.config.provider).enabled
        )
    ]


@app.get("/config/models")
async def get_models():
    return {
        "models": _catalog_models(),
        "default_model": current_config.default_model if current_config is not None else "",
        "generation": _model_catalog.snapshot.generation if _model_catalog is not None else 0,
        "errors": dict(_model_catalog.snapshot.errors) if _model_catalog is not None else {},
    }


@app.post("/config/models/refresh")
async def refresh_models():
    if _model_catalog is None:
        return JSONResponse({"error": "no config"}, status_code=400)
    await _model_catalog.refresh()
    return await get_models()


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
    attachments = body.get("attachments", []) or []
    display_content = body.get("display_content", "")

    if not isinstance(attachments, list):
        return JSONResponse({"error": "attachments must be an array"}, status_code=400)

    if not session_id or (not message and not attachments):
        return JSONResponse({"error": "missing session_id or message"}, status_code=400)
    try:
        session_id = validate_session_id(session_id)
    except InvalidSessionId as e:
        return JSONResponse({"error": str(e)}, status_code=400)

    # 纯附件消息没有文本，用附件名参与轮次标识，避免被误判为 SSE 重连
    turn_marker = message or "|".join(
        str(item.get("name", "")) for item in attachments if isinstance(item, dict)
    )

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
                    if turn_marker and turn_marker != bg.last_message:
                        # 新的用户消息 — 重置 bg 状态，走下方启动新 task
                        bg.queue = asyncio.Queue()
                        bg.done_event = asyncio.Event()
                        bg.cancelled = False
                        bg.started_at = None
                        bg.last_message = turn_marker
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
                    bg.last_message = turn_marker

                # 启动新的后台 task
                async with _session_async_lock(session_id):
                    bg.last_message = turn_marker
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
    try:
        plan = TaskLedger(session_id).plan
    except InvalidSessionId:
        plan = None
    return {
        "meta": {
            "id": s.id,
            "title": s.title,
            "created_at": s.created_at,
            "updated_at": s.updated_at,
            "model_id": s.model_id,
        },
        "messages": s.messages,
        "plan": plan,
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
            if cleared:
                TaskLedger(session_id).clear()
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
    ledger = TaskLedger(session_id)

    try:
        stored_attachments = []
        if attachments:
            documents, images, failures = await asyncio.to_thread(
                load_attachments, attachments
            )
            for notice in failures:
                logger.warning("[attachment] session=%s %s", session_id, notice)
                await bg.queue.put({"type": "notice", "message": notice})
            stored_attachments = [
                {
                    "kind": "text", "name": item["name"],
                    "text": item["text"], "path": item.get("path", ""),
                }
                for item in documents
            ] + [
                {
                    "kind": "image", "name": item["name"],
                    "media_type": item["media_type"], "path": item["path"],
                    "url": item.get("url", ""),
                }
                for item in images
            ]
            if not message and not stored_attachments:
                await bg.queue.put({
                    "type": "error",
                    "message": "附件读取失败，请更换文件后重试",
                    "retryable": False,
                })
                return

        async for event in run_agent_loop(
            session, message, system_prompt, current_config, _mcp, ctx_mgr,
            skill_registry, selected_skills, _request_tool_approval,
            attachments=stored_attachments, display_content=display_content,
            interaction_mode=interaction_mode,
            model_override=model_id if model_id else None,
            model_runtime=_model_runtime,
            ledger=ledger,
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
                    update_index(session)

            await bg.queue.put(event)
    except asyncio.CancelledError:
        bg.cancelled = True
        with locked_session(session_id):
            save_session(session)
            update_index(session)
        logger.info(f"background task cancelled: {session_id}")
    except Exception as e:
        logger.exception("background agent loop failed")
        await bg.queue.put(classify_error(e))
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


# --- 语音转文字（voice_asr）模块挂载 ---
# voice_asr 位于项目根，需将 PROJECT_ROOT 加入 sys.path 才能导入。
# 守卫式导入：挂载失败仅告警，不影响主应用启动（沿用 mcp_bridge/litellm 风格）。
PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))
try:
    from voice_asr.router import router as voice_router

    app.include_router(voice_router)
    logger.info("voice_asr mounted: POST /asr, GET /asr/health")
except Exception as exc:  # noqa: BLE001 - 挂载失败不应阻断主应用
    logger.warning("voice_asr not mounted: %s", exc)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=1231)
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args()
    uvicorn.run(app, host=args.host, port=args.port, log_level="info")
