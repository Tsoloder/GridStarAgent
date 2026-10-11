"""记忆门面（``memory_store.MemoryService``）的测试。

门面存在的理由是「路由层不该猜异常类型」和「两个存储只该有一套写入口」。
所以这里测的重点不是存储行为（那由 test_experience_memory 覆盖），而是：
门面有没有把错误翻成正确的状态码、开关有没有真的传到提炼器、维护任务
有没有并发保护、以及「影子期不注入」这个承诺有没有体现在返回值里。
"""
from __future__ import annotations

import dataclasses
import asyncio
import json

import pytest

import memory_store
import tool_memory
from config import MemoryConfig
from experience_memory import MemoryStore
from memory_store import MemoryFault, MemoryService


@pytest.fixture()
def store(tmp_path):
    return MemoryStore(tmp_path / "memory.db")


@pytest.fixture()
def tool_dir(tmp_path, monkeypatch):
    """把参数记忆导向临时目录 —— 门面的删除接口绝不能碰真实用户数据。"""
    target = tmp_path / "tool_parameter_memory.json"
    monkeypatch.setattr(tool_memory, "MEMORY_PATH", target)
    return target


def _config_io(initial=None):
    """一个最小的 config.json 替身：读写都在内存里。"""
    state = {"memory": initial or MemoryConfig()}

    def reader():
        return state["memory"]

    def writer(updates):
        state["memory"] = dataclasses.replace(state["memory"], **dict(updates))
        return state["memory"]

    return state, reader, writer


@pytest.fixture()
def service(store, tool_dir):
    svc = MemoryService(store)
    _, reader, writer = _config_io()
    svc.bind_config(reader, writer)
    return svc


def _candidate(store, title="质量检查超标时先做局部加密处理", **overrides):
    record = {
        "kind": "experience",
        "scope": "global",
        "task_type": "mesh_quality",
        "title": title,
        "trigger_cues": ["网格质量检查不达标"],
        "preconditions": ["已完成表面网格"],
        "strategy_steps": ["先定位超标单元", "做局部加密后复检"],
        "tool_sequence": [],
        "source_kind": "auto_extraction",
    }
    record.update(overrides)
    return store.add_item(record)["id"]


class FakeLedger:
    def __init__(self, session_id, *, calls=None, plan=None):
        self.session_id = session_id
        self.calls = list(calls or [])
        self.plan = plan or {}


# --------------------------------------------------------------- 摘要与状态


def test_summary_reports_shadow_stage_and_counts(service, store):
    """影子期必须在返回值里说清楚，否则界面会以为开关已经生效。"""
    _candidate(store)
    summary = service.summary()
    assert summary["stage"] == "shadow"
    assert "不会自动注入" in summary["notice"]
    assert summary["enabled"] is True
    assert summary["experience"]["candidate"] == 1
    assert summary["store"]["status"] == "healthy"
    assert summary["store"]["path_hint"] == "用户数据目录"
    # 绝不泄漏绝对路径
    assert ":" not in summary["store"]["path_hint"]


def test_summary_config_exposes_all_five_switches(service):
    config = service.summary()["config"]
    assert set(config) == {
        "enabled", "experience_extract", "experience_inject",
        "fact_extract", "fact_inject",
    }
    assert all(isinstance(value, bool) for value in config.values())


def test_summary_surfaces_store_degradation_without_raw_error(tmp_path, tool_dir):
    """库不可读时页面还要能显示开关和参数记忆，且错误文案不能带路径。"""
    broken = tmp_path / "blocked" / "memory.db"
    (tmp_path / "blocked").write_text("not a directory", encoding="utf-8")
    service = MemoryService(MemoryStore(broken))
    summary = service.summary()
    assert summary["store"]["status"] == "unavailable"
    assert summary["store"]["writable"] is False
    assert summary["store"]["last_error"]["code"] == "unavailable"
    assert "\\" not in summary["store"]["last_error"]["message"]


def test_maintenance_status_merges_queue_stats(service):
    status = service.maintenance_status()
    assert status["maintenance"]["state"] == "idle"
    assert set(status["extraction"]) >= {
        "pending_jobs", "running", "queued", "completed", "failed",
        "dropped", "last_job_result", "last_job_error",
    }


# --------------------------------------------------------------- 配置


def test_patch_config_uses_long_names_and_persists(service):
    _, _, writer = _config_io()
    state = {}
    service.bind_config(lambda: MemoryConfig(), lambda updates: state.update(updates)
                        or MemoryConfig(**{**dataclasses.asdict(MemoryConfig()), **updates}))
    service.patch_config({"extract": False})
    assert state == {"experience_extract": False, "fact_extract": False}


def test_patch_config_short_names_map_to_both_lanes(service):
    """旧契约的 extract/inject 是两条链路的合写，不能只改一条。"""
    _, reader, writer = _config_io()
    service.bind_config(reader, writer)
    service.patch_config({"extract": False})
    config = service.config
    assert config.experience_extract is False
    assert config.fact_extract is False
    assert config.experience_inject is True


def test_patch_config_rejects_unknown_key(service):
    with pytest.raises(MemoryFault) as excinfo:
        service.patch_config({"expirience_extract": True})
    assert excinfo.value.code == "invalid_request"
    assert excinfo.value.status == 400
    assert "expirience_extract" in excinfo.value.message


def test_patch_config_rejects_non_boolean(service):
    with pytest.raises(MemoryFault) as excinfo:
        service.patch_config({"enabled": "yes"})
    assert excinfo.value.code == "invalid_request"


def test_patch_config_rejects_empty_body(service):
    with pytest.raises(MemoryFault) as excinfo:
        service.patch_config({})
    assert excinfo.value.status == 400


def test_patch_config_without_writer_is_unavailable(store, tool_dir):
    service = MemoryService(store)
    with pytest.raises(MemoryFault) as excinfo:
        service.patch_config({"enabled": True})
    assert excinfo.value.status == 503


def test_config_switches_reach_the_extractor(service, store):
    """开关没传到提炼器就等于没关，这是最容易被忽略的一环。"""
    service.patch_config({"experience_extract": False})
    assert service.extractor.extract_experience is False
    assert service.extractor.extract_fact is True
    service.patch_config({"enabled": False})
    assert service.extractor.extract_experience is False
    assert service.extractor.extract_fact is False


def test_config_write_failure_is_reported_not_raised_raw(service):
    def broken_writer(updates):
        raise OSError("C:/Users/someone/secret/path")

    service.bind_config(lambda: MemoryConfig(), broken_writer)
    with pytest.raises(MemoryFault) as excinfo:
        service.patch_config({"enabled": True})
    assert excinfo.value.code == "store_unavailable"
    assert "secret" not in excinfo.value.message


# --------------------------------------------------------------- 经验审核


def test_transition_codes_map_to_http_semantics(service, store):
    item_id = _candidate(store)
    assert service.transition_experience(item_id, "approve")["status"] == "approved"

    # 非法流转 → 409
    with pytest.raises(MemoryFault) as excinfo:
        service.transition_experience(item_id, "approve")
    assert (excinfo.value.code, excinfo.value.status) == ("state_conflict", 409)

    # 找不到 → 404
    with pytest.raises(MemoryFault) as excinfo:
        service.transition_experience("exp_missing", "approve")
    assert (excinfo.value.code, excinfo.value.status) == ("not_found", 404)

    # 非法 action → 400
    with pytest.raises(MemoryFault) as excinfo:
        service.transition_experience(item_id, "promote")
    assert (excinfo.value.code, excinfo.value.status) == ("invalid_request", 400)


def test_transition_reason_is_truncated_to_200(service, store):
    item_id = _candidate(store)
    service.transition_experience(item_id, "approve", reason="x" * 500)
    audit = service.audit(limit=10)
    assert max(len(entry["reason"]) for entry in audit) <= 200


def test_bulk_rejects_bad_action_empty_and_oversize(service, store):
    item_id = _candidate(store)
    with pytest.raises(MemoryFault) as excinfo:
        service.bulk_experience([item_id], "deprecate")
    assert excinfo.value.status == 400

    with pytest.raises(MemoryFault):
        service.bulk_experience([], "approve")

    with pytest.raises(MemoryFault) as excinfo:
        service.bulk_experience(["exp_%d" % n for n in range(101)], "approve")
    assert "100" in excinfo.value.message
    assert item_id  # 保持引用，避免被误认为死代码


def test_bulk_allows_partial_success_and_is_replay_safe(service, store):
    first = _candidate(store, title="质量检查超标时先做局部加密处理")
    second = _candidate(store, title="部件分割前先修复自由边")
    result = service.bulk_experience([first, "exp_missing", second], "approve")
    assert result["succeeded"] == 2
    assert result["failed"] == 1
    codes = {entry["id"]: entry["code"] for entry in result["results"]}
    assert codes[first] == "ok"
    assert codes["exp_missing"] == "not_found"
    assert codes[second] == "ok"

    replay = service.bulk_experience([first], "approve")
    assert replay["results"][0]["ok"] is True
    assert replay["results"][0]["code"] == "unchanged"


def test_bulk_deduplicates_ids_before_counting(service, store):
    """同一 id 传两次不该被算成两条，否则上限与结果计数都会虚高。"""
    item_id = _candidate(store)
    result = service.bulk_experience([item_id, item_id], "approve")
    assert len(result["results"]) == 1


def test_delete_experience_reports_missing_as_false(service, store):
    item_id = _candidate(store)
    assert service.delete_experience(item_id) is True
    assert service.delete_experience(item_id) is False


def test_list_experiences_paginates_and_adds_facets(service, store):
    for index in range(3):
        _candidate(store, title="经验主题%d" % index)
    page = service.list_experiences(kind="experience", status="candidate", page_size=2)
    assert page["total"] == 3
    assert len(page["items"]) == 2
    assert page["page_size"] == 2
    assert any(entry["value"] == "global" for entry in page["facets"]["scopes"])


# --------------------------------------------------------------- 参数记忆


def test_tool_parameter_facade_lists_sorted_scalars(service):
    tool_memory.remember("create_mesh", {"size": 2.0, "algorithm": "delaunay"},
                         {"type": "object", "properties": {"size": {}, "algorithm": {}}})
    tool_memory.remember("UGBlockCreate", {"name": "blk"}, {"type": "object",
                         "properties": {"name": {}}})
    payload = service.list_tool_parameters()
    assert [entry["tool"] for entry in payload["tools"]] == [
        "UGBlockCreate", "create_mesh",
    ]
    create = [e for e in payload["tools"] if e["tool"] == "create_mesh"][0]
    assert list(create["fields"]) == ["algorithm", "size"]
    assert payload["field_count"] == 3


def test_delete_tool_field_then_tool(service):
    tool_memory.remember("create_mesh", {"size": 2.0, "algorithm": "delaunay"},
                         {"type": "object", "properties": {"size": {}, "algorithm": {}}})
    service.delete_tool_field("create_mesh", "algorithm")
    tool = service.list_tool_parameters()["tools"][0]
    assert list(tool["fields"]) == ["size"]

    # 删掉最后一个字段后，工具条目整体消失
    service.delete_tool_field("create_mesh", "size")
    assert service.list_tool_parameters()["tools"] == []


def test_delete_missing_tool_or_field_is_404(service):
    with pytest.raises(MemoryFault) as excinfo:
        service.delete_tool("nope")
    assert (excinfo.value.code, excinfo.value.status) == ("not_found", 404)

    tool_memory.remember("create_mesh", {"size": 2.0},
                         {"type": "object", "properties": {"size": {}}})
    with pytest.raises(MemoryFault) as excinfo:
        service.delete_tool_field("create_mesh", "missing")
    assert excinfo.value.status == 404


def test_clear_tool_parameters_reports_before_counts(service):
    tool_memory.remember("create_mesh", {"size": 2.0, "algorithm": "delaunay"},
                         {"type": "object", "properties": {"size": {}, "algorithm": {}}})
    tool_memory.remember("DealWatertight", {"merge": True},
                         {"type": "object", "properties": {"merge": {}}})
    result = service.clear_tool_parameters()
    assert result == {"tools": 2, "fields": 3}
    assert service.list_tool_parameters()["tools"] == []


# --------------------------------------------------------------- 维护


def test_maintenance_governance_runs_synchronously_without_loop(service, store):
    """没有事件循环时（脚本/测试）也要能跑完，否则治理只能靠 Web 触发。"""
    _candidate(store, title="质量检查超标时先做局部加密处理")
    _candidate(store, title="质量检查超标时先做局部加密修复")
    status = service.start_maintenance("governance")
    assert status["maintenance"]["state"] == "idle"
    assert status["maintenance"]["last_result"]["merged"] >= 1
    assert status["maintenance"]["finished_at"]


def test_maintenance_rejects_unknown_kind(service):
    with pytest.raises(MemoryFault) as excinfo:
        service.start_maintenance("reindex")
    assert excinfo.value.status == 400


@pytest.mark.asyncio
async def test_maintenance_rejects_concurrent_run(service):
    """已有任务在跑时必须 409 而不是排队，避免连点堆出一串重复治理。"""
    service._maintenance["state"] = "running"
    with pytest.raises(MemoryFault) as excinfo:
        service.start_maintenance("governance")
    assert (excinfo.value.code, excinfo.value.status) == ("state_conflict", 409)


@pytest.mark.asyncio
async def test_maintenance_resets_state_after_a_real_run(service, store):
    service.start_maintenance("governance")
    await service._maintenance_task
    assert service.maintenance_status()["maintenance"]["state"] == "idle"


def test_maintenance_records_failure_without_raising(service, store, monkeypatch):
    def broken():
        raise RuntimeError("boom")

    monkeypatch.setattr(store, "run_governance", broken)
    status = service.start_maintenance("governance")
    assert status["maintenance"]["last_error"] == "RuntimeError"
    assert status["maintenance"]["state"] == "idle"


# --------------------------------------------------------------- 提炼投递


def test_submit_session_skips_when_disabled(service, monkeypatch):
    monkeypatch.setattr(memory_store, "TaskLedger",
                        lambda sid: FakeLedger(sid, calls=[{"tool": "DealWatertight"}]))
    service.patch_config({"enabled": False})
    assert service.submit_session("s1") is False


def test_submit_session_skips_empty_ledger(service, monkeypatch):
    monkeypatch.setattr(memory_store, "TaskLedger", lambda sid: FakeLedger(sid))
    assert service.submit_session("s1") is False


def test_submit_session_skips_unreadable_ledger(service, monkeypatch):
    def explode(sid):
        raise OSError("ledger.json is a directory")

    monkeypatch.setattr(memory_store, "TaskLedger", explode)
    assert service.submit_session("s1") is False


@pytest.mark.asyncio
async def test_submit_session_submits_snapshot_without_touching_ledger(service, monkeypatch):
    ledger = FakeLedger("s1", calls=[{"tool": "DealWatertight"}],
                        plan={"id": "p1", "phases": []})
    monkeypatch.setattr(memory_store, "TaskLedger", lambda sid: ledger)
    service.start()
    assert service.submit_session("s1") is True
    # 快照是拷贝：后续台账变动不该影响已入队的任务
    assert service.queue.stats["pending"] == 1
    ledger.calls.append({"tool": "UGReDimension"})
    queued = service.queue._queue.get_nowait()
    assert len(queued[1]["calls"]) == 1


def test_submit_session_without_started_queue_is_rejected(service, monkeypatch):
    """队列没启动时必须返回 False 而不是假装入队 —— 静默成功比报错更难查。"""
    monkeypatch.setattr(memory_store, "TaskLedger",
                        lambda sid: FakeLedger(sid, calls=[{"tool": "DealWatertight"}]))
    assert service.submit_session("s1") is False


@pytest.mark.asyncio
async def test_submit_recent_extractions_counts_only_submitted(service, monkeypatch):
    monkeypatch.setattr(memory_store, "recent_ledger_sessions",
                        lambda limit=3: ["s1", "s2", "s3"])
    with_calls = {"s1", "s3"}
    monkeypatch.setattr(
        memory_store, "TaskLedger",
        lambda sid: FakeLedger(sid, calls=[{"tool": "DealWatertight"}] if sid in with_calls else []),
    )
    service.start()
    assert service.submit_recent_extractions() == 2


def test_recent_ledger_sessions_orders_by_mtime(tmp_path, monkeypatch):
    root = tmp_path / "sessions"
    for index, name in enumerate(["old", "middle", "new"]):
        directory = root / name
        directory.mkdir(parents=True)
        (directory / "ledger.json").write_text("{}", encoding="utf-8")
        import os
        os.utime(directory / "ledger.json", (index * 100, index * 100))
    (root / "no-ledger").mkdir()
    monkeypatch.setattr(memory_store, "SESSIONS_DIR", root)
    assert memory_store.recent_ledger_sessions(2) == ["new", "middle"]
    assert memory_store.recent_ledger_sessions(10) == ["new", "middle", "old"]


def test_recent_ledger_sessions_tolerates_missing_root(tmp_path, monkeypatch):
    monkeypatch.setattr(memory_store, "SESSIONS_DIR", tmp_path / "gone")
    assert memory_store.recent_ledger_sessions() == []


# --------------------------------------------------------------- 生命周期


@pytest.mark.asyncio
async def test_stop_is_safe_before_start(service):
    await service.stop()
    await service.stop()


@pytest.mark.asyncio
async def test_stop_cancels_running_maintenance(service, store, monkeypatch):
    async def never_finishes():
        await asyncio.Event().wait()

    async def slow_governance():
        await never_finishes()

    monkeypatch.setattr(store, "run_governance", lambda: None)
    service.start_maintenance("governance")
    await asyncio.sleep(0)
    await service.stop()
    assert service._maintenance_task is None


def test_attach_runtime_reaches_extractor(service):
    marker = object()
    service.attach_runtime(marker, "openai/gpt-4o-mini")
    assert service.extractor.runtime is marker
    assert service.extractor.model_key == "openai/gpt-4o-mini"


def test_audit_tail_is_exposed_without_payloads(service, store):
    item_id = _candidate(store)
    service.transition_experience(item_id, "approve", reason="已复现")
    entries = service.audit(limit=5)
    assert entries
    assert {"ts", "item_kind", "item_id", "action", "reason"} <= set(entries[0])
    assert "质量检查" not in json.dumps(entries, ensure_ascii=False)