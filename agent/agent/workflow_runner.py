"""Deterministic sequential workflow execution through explicit policy and approval gates."""
import asyncio
import uuid
from typing import AsyncIterator, Callable, Optional

from tool_memory import merge as merge_tool_memory, normalize as normalize_tool_args


def _available_tool_names(mcp) -> set:
    return {str(tool.name) for tool in mcp.available_tools()}


def _snapshot(steps: list) -> list:
    return [dict(step) for step in steps]


async def run_workflow(
    session,
    steps: list,
    mcp,
    request_tool_approval,
    tool_allowed: Optional[Callable[[str], bool]] = None,
    persist_result: Optional[Callable[[str, str], str]] = None,
) -> AsyncIterator[dict]:
    run_id = "workflow-%s" % uuid.uuid4().hex
    run_steps = []
    session.begin_workflow_run(run_id, run_steps)
    yield {
        "type": "workflow_started",
        "run_id": run_id,
        "status": "running",
        "steps": [],
    }

    if not isinstance(steps, list) or not steps:
        message = "workflow has no steps"
        session.update_workflow_run(run_id, run_steps, "failed", message)
        yield {"type": "workflow_done", "run_id": run_id, "status": "failed", "message": message}
        return

    available = _available_tool_names(mcp)
    for index, raw in enumerate(steps):
        if not isinstance(raw, dict):
            message = "invalid step at index %d" % index
            session.update_workflow_run(run_id, _snapshot(run_steps), "failed", message)
            yield {"type": "workflow_done", "run_id": run_id, "status": "failed", "message": message}
            return
        tool = str(raw.get("tool", "")).strip()
        params = raw.get("params", {})
        if not tool or not isinstance(params, dict):
            message = "invalid workflow step %d" % (index + 1)
            session.update_workflow_run(run_id, _snapshot(run_steps), "failed", message)
            yield {"type": "workflow_done", "run_id": run_id, "status": "failed", "message": message}
            return
        run_steps.append({
            "tool": tool,
            "desc": str(raw.get("desc", "")),
            "params": dict(params),
            "status": "pending",
            "result": "",
        })
    session.update_workflow_run(run_id, _snapshot(run_steps), "running", "workflow started")

    final_status = "succeeded"
    final_message = "workflow completed"
    try:
        for index, step in enumerate(run_steps):
            tool = step["tool"]
            if tool not in available:
                step["status"] = "failed"
                step["result"] = "Tool not found: %s" % tool
                session.update_workflow_run(run_id, _snapshot(run_steps), "running", step["result"])
                yield {"type": "workflow_step", "run_id": run_id, "index": index, **step}
                final_status, final_message = "failed", step["result"]
                break
            if tool_allowed is not None and not tool_allowed(tool):
                step["status"] = "failed"
                step["result"] = "Tool blocked by active Skill policy: %s" % tool
                session.update_workflow_run(run_id, _snapshot(run_steps), "running", step["result"])
                yield {"type": "workflow_step", "run_id": run_id, "index": index, **step}
                final_status, final_message = "failed", step["result"]
                break

            schema = mcp.tool_schema(tool) if hasattr(mcp, "tool_schema") else {}
            args = merge_tool_memory(tool, step["params"], schema)
            call_id = "%s-%d-%s" % (run_id, index, uuid.uuid4().hex[:8])

            try:
                if request_tool_approval:
                    step["status"] = "awaiting_approval"
                    session.update_workflow_run(run_id, _snapshot(run_steps), "running", "awaiting approval")
                    yield {"type": "workflow_step", "run_id": run_id, "index": index, **step}
                    yield {
                        "type": "tool_approval_required",
                        "call_id": call_id,
                        "name": tool,
                        "args": args,
                        "schema": schema,
                    }
                    approval = await request_tool_approval(session.id, call_id, tool, args)
                    if not bool(approval.get("approved", False)):
                        step["status"] = "cancelled"
                        step["result"] = "Tool execution denied by user: %s" % tool
                        session.update_workflow_run(run_id, _snapshot(run_steps), "running", step["result"])
                        yield {"type": "workflow_step", "run_id": run_id, "index": index, **step}
                        final_status, final_message = "cancelled", step["result"]
                        break
                    args = normalize_tool_args(tool, approval.get("args", {}) or args, schema)
                    args = merge_tool_memory(tool, args, schema)

                step["status"] = "running"
                step["params"] = args
                session.update_workflow_run(run_id, _snapshot(run_steps), "running", "running %s" % tool)
                yield {"type": "workflow_step", "run_id": run_id, "index": index, **step}
                result = str(await asyncio.wait_for(mcp.call_tool(tool, args), timeout=60.0))
                if persist_result is not None:
                    result = persist_result(result, session.id)
                step["status"] = "succeeded"
                step["result"] = result
                session.update_workflow_run(run_id, _snapshot(run_steps), "running", "completed %s" % tool)
                yield {"type": "workflow_step", "run_id": run_id, "index": index, **step}
            except asyncio.TimeoutError:
                step["status"] = "failed"
                step["result"] = "Tool error: timeout after 60 seconds"
                session.update_workflow_run(run_id, _snapshot(run_steps), "running", step["result"])
                yield {"type": "workflow_step", "run_id": run_id, "index": index, **step}
                final_status, final_message = "failed", step["result"]
                break
            except Exception as exc:
                step["status"] = "failed"
                step["result"] = "Tool error: %s" % exc
                session.update_workflow_run(run_id, _snapshot(run_steps), "running", step["result"])
                yield {"type": "workflow_step", "run_id": run_id, "index": index, **step}
                final_status, final_message = "failed", step["result"]
                break
    except asyncio.CancelledError:
        final_status = "interrupted"
        final_message = "workflow interrupted because the client disconnected"
        for step in run_steps:
            if step["status"] in {"pending", "awaiting_approval", "running"}:
                step["status"] = "interrupted"
                step["result"] = final_message
        session.update_workflow_run(run_id, _snapshot(run_steps), final_status, final_message)
        raise

    if final_status != "succeeded":
        for index, step in enumerate(run_steps):
            if step["status"] == "pending":
                step["status"] = "cancelled"
                step["result"] = "Not executed because the workflow stopped earlier"
                session.update_workflow_run(run_id, _snapshot(run_steps), "running", final_message)
                yield {"type": "workflow_step", "run_id": run_id, "index": index, **step}

    session.update_workflow_run(run_id, _snapshot(run_steps), final_status, final_message)
    yield {
        "type": "workflow_done",
        "run_id": run_id,
        "status": final_status,
        "message": final_message,
    }
