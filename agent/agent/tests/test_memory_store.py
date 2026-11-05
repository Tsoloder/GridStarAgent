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
from experience_extractor import (
    ExperienceExtractor,
    ExtractionQueue,
    load_state,
    save_state,
)
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


class FakeQueue:
    """只记录投递、由测试决定成败的队列替身。

    真的 ``ExtractionQueue`` 没 ``start`` 时一律拒收，那样观察不到水位推进；
    这里把“收不收”变成一个显式开关。
    """

    def __init__(self, *, accept=True):
        self.accept = accept
        self.submissions = []
        self.stats = {}

    def submit(self, session_id, ledger, *, trigger="", focus="", skills=(), watermark=0):
        self.submissions.append({
            "session_id": session_id,
            "trigger": trigger,
            "watermark": watermark,
            "calls": len(list((ledger or {}).get("calls") or [])),
        })
        return self.accept


# --------------------------------------------------------------- 摘要与状态


def test_summary_reports_live_stage_and_counts(service, store):
    """注入期必须在返回值里说清楚：只有审核通过的记录会进请求。"""
    _candidate(store)
    summary = service.summary()
    assert summary["stage"] == "live"
    assert "候选区的记录不会被注入" in summary["notice"]
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


def test_set_tool_field_returns_the_new_value(service):
    tool_memory.remember("create_mesh", {"size": 2.0},
                         {"type": "object", "properties": {"size": {}}})
    assert service.set_tool_field("create_mesh", "size", 4.5) == {
        "tool": "create_mesh", "field": "size", "value": 4.5,
    }
    assert service.list_tool_parameters()["tools"][0]["fields"] == {"size": 4.5}


def test_set_tool_field_faults_carry_actionable_messages(service):
    """错误文案要能直接显示给用户，所以得说清"为什么不行"，而不是回一个码。"""
    tool_memory.remember("create_mesh", {"size": 2.0, "clean": False},
                         {"type": "object", "properties": {"size": {}, "clean": {}}})
    with pytest.raises(MemoryFault) as excinfo:
        service.set_tool_field("create_mesh", "missing", 1)
    assert (excinfo.value.code, excinfo.value.status) == ("not_found", 404)
    assert "字段已不存在" in excinfo.value.message

    with pytest.raises(MemoryFault) as excinfo:
        service.set_tool_field("create_mesh", "size", "很大")
    assert (excinfo.value.code, excinfo.value.status) == ("invalid_request", 400)
    assert "数字" in excinfo.value.message

    with pytest.raises(MemoryFault) as excinfo:
        service.set_tool_field("create_mesh", "size", [1])
    assert excinfo.value.status == 400
    assert "文本、数字或布尔" in excinfo.value.message


def test_set_tool_field_works_while_memory_is_switched_off(service):
    """关掉记忆后仍要能收拾它留下的数据，否则用户只能去翻数据目录。"""
    tool_memory.remember("create_mesh", {"size": 2.0},
                         {"type": "object", "properties": {"size": {}}})
    service.patch_config({"enabled": False})
    assert service.enabled is False
    assert service.set_tool_field("create_mesh", "size", 3)["value"] == 3
    service.delete_tool_field("create_mesh", "size")


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


# --------------------------------------------------- 轮次水位与参数记忆开关


def _service_with_state_root(tmp_path, store, queue):
    """把带断点目录的提炼器接进门面，好观察水位落到哪个文件。"""

    def root(session_id):
        path = tmp_path / "sessions" / session_id
        path.mkdir(parents=True, exist_ok=True)
        return path

    extractor = ExperienceExtractor(store, state_root=root)
    service = MemoryService(store, extractor=extractor, queue=queue)
    _, reader, writer = _config_io()
    service.bind_config(reader, writer)
    return service, root


def test_schedule_after_turn_advances_the_watermark_once(tmp_path, store, tool_dir):
    queue = FakeQueue()
    service, root = _service_with_state_root(tmp_path, store, queue)
    save_state("s1", {"last_watermark": 0}, root=root)
    ledger = {"calls": [{"tool": "DealWatertight"}], "plan": {}}

    assert service.schedule_after_turn("s1", ledger, turn_count=10) is True
    assert int(load_state("s1", root=root)["last_watermark"]) == 10
    # 水位不落盘的后果：11 轮之后每一轮都会再提炼一次，job_key 里带
    # watermark=turn_count，幂等层根本拦不住。
    assert service.schedule_after_turn("s1", ledger, turn_count=11) is False
    assert len(queue.submissions) == 1
    assert queue.submissions[0]["watermark"] == 10


def test_dropped_job_does_not_advance_the_watermark(tmp_path, store, tool_dir):
    """队列满被丢掉的 job 必须下一轮还能重试，不能先记水位。"""
    queue = FakeQueue(accept=False)
    service, root = _service_with_state_root(tmp_path, store, queue)
    save_state("s1", {"last_watermark": 0}, root=root)

    assert service.schedule_after_turn(
        "s1", {"calls": [{"tool": "DealWatertight"}], "plan": {}}, turn_count=10
    ) is False
    assert int(load_state("s1", root=root)["last_watermark"]) == 0


def test_master_switch_silences_parameter_memory(tool_dir):
    """总开关必须盖住唯一真正进提示词的那部分记忆。"""
    schema = {"type": "object", "properties": {"tol": {"type": "number", "default": 0.5}}}
    tool_memory.configure(True)
    try:
        tool_memory.remember("DealWatertight", {"tol": 0.1})
        assert "tol" in tool_memory.merge("DealWatertight", {}, schema)
        assert "DealWatertight" in tool_memory.catalog()

        tool_memory.configure(False)
        assert tool_memory.enabled() is False
        assert tool_memory.catalog() == ""
        # schema 默认值照旧应用，只是记忆层不再参与
        assert tool_memory.merge("DealWatertight", {}, schema) == {"tol": 0.5}
        assert tool_memory.remember("DealWatertight", {"tol": 0.2}) == {}
        # 旧数据保持可读可改，否则设置页关掉之后就清理不了
        assert tool_memory.get("DealWatertight") == {"tol": 0.1}
        assert tool_memory.catalog_all()["field_count"] == 1
        assert tool_memory.set_field("DealWatertight", "tol", 0.3)["ok"] is True
    finally:
        tool_memory.configure(True)
    assert "DealWatertight" in tool_memory.catalog()


def test_patch_config_drives_the_parameter_memory_switch(service):
    try:
        assert tool_memory.enabled() is True
        service.patch_config({"enabled": False})
        assert tool_memory.enabled() is False
        service.patch_config({"enabled": True})
        assert tool_memory.enabled() is True
    finally:
        tool_memory.configure(True)


def test_refresh_config_follows_a_replaced_config(store, tool_dir):
    """``POST /config`` 整体换掉配置对象，门面必须重新读一遍。"""
    state, reader, writer = _config_io()
    service = MemoryService(store)
    service.bind_config(reader, writer)
    try:
        assert tool_memory.enabled() is True
        # 绕过 patch_config，直接换掉配置对象：这正是 update_config 的做法
        state["memory"] = dataclasses.replace(state["memory"], enabled=False)

        config = service.refresh_config()

        assert config.enabled is False
        assert service.config.enabled is False
        assert tool_memory.enabled() is False
        assert service.extractor.extract_experience is False
    finally:
        tool_memory.configure(True)


# --------------------------------------------------------------- 注入与直授


def test_prompt_block_injects_only_approved(service, store):
    """候选与已停用的一律不进块；只有真正进了文本的才计一次曝光。"""
    approved = _candidate(store, title="质量检查超标时先做局部加密处理")
    store.transition(approved, "approve")
    candidate = _candidate(
        store,
        title="导出前先确认导出目录存在",
        trigger_cues=["准备导出网格"],
        strategy_steps=["先确认目录"],
    )
    deprecated = _candidate(
        store,
        title="旧版的全量重划做法",
        trigger_cues=["网格质量差"],
        strategy_steps=["全量重划"],
    )
    store.transition(deprecated, "approve")
    store.transition(deprecated, "deprecate")

    block = service.build_prompt_block(query="质量检查", task_type="mesh_quality")

    assert "<experience_memory>" in block
    assert "质量检查超标时先做局部加密处理" in block
    assert "导出前先确认导出目录存在" not in block
    assert "旧版的全量重划做法" not in block
    assert store.get(approved)["exposure_count"] == 1
    assert store.get(candidate)["exposure_count"] == 0
    assert store.get(deprecated)["exposure_count"] == 0


def test_prompt_block_respects_the_char_budget(service, store):
    for index in range(3):
        item_id = _candidate(
            store, title="第%d条经验：质量检查超标后的局部处理顺序说明" % index
        )
        store.transition(item_id, "approve")

    block = service.build_prompt_block(
        query="质量检查", task_type="mesh_quality", budget_chars=420
    )

    assert block
    assert len(block) <= 420
    # 超预算按整条丢，绝不能留下半个标签
    assert block.count("<item ") == block.count("</item>")


def test_prompt_block_is_silent_when_switched_off(service, store):
    item_id = _candidate(store)
    store.transition(item_id, "approve")

    service.patch_config({"enabled": False})
    try:
        assert service.build_prompt_block(query="质量检查", task_type="mesh_quality") == ""
    finally:
        service.patch_config({"enabled": True})

    service.patch_config({"experience_inject": False, "fact_inject": False})
    try:
        assert service.build_prompt_block(query="质量检查", task_type="mesh_quality") == ""
    finally:
        service.patch_config({"experience_inject": True, "fact_inject": True})


def test_prompt_block_carries_facts_in_their_own_block(service, store):
    fact_id = store.add_item({
        "kind": "fact",
        "scope": "global",
        "task_type": "environment",
        "fact_text": "导出目录习惯放在工程根目录下的 exports",
        "fact_type": "user_preference",
        "source_kind": "user_correction",
    })["id"]
    store.transition(fact_id, "approve")

    block = service.build_prompt_block(query="导出目录")

    assert "<memory_facts>" in block
    assert "<experience_memory>" not in block
    assert 'type="user_preference"' in block
    assert "exports" in block


def test_create_experience_lands_approved_and_recallable(service, store):
    record = {
        "title": "水密检查失败时先看自由边计数",
        "scope": "global",
        "task_type": "watertight_repair",
        "trigger_cues": ["水密工具报错"],
        "strategy_steps": ["先查自由边", "修复后复审"],
    }
    created = service.create_experience(record)

    assert created["status"] == "approved"
    assert created["source_kind"] == "manual_curated"
    hits = store.recall(
        query="水密检查", scopes=["global"], task_type="watertight_repair"
    )
    assert [hit["item"]["id"] for hit in hits] == [created["id"]]

    # 同一条再提一次：指纹已存在，返回已有记录并标明 duplicate
    again = service.create_experience(dict(record))
    assert again["duplicate"] is True
    assert again["id"] == created["id"]


def test_create_experience_rejects_illegal_input(service):
    with pytest.raises(MemoryFault) as scope_error:
        service.create_experience({"title": "非法作用域", "scope": "team:x"})
    assert scope_error.value.code == "invalid_request"

    with pytest.raises(MemoryFault) as status_error:
        service.create_experience({"title": "非法状态", "status": "deprecated"})
    assert status_error.value.code == "invalid_request"

    with pytest.raises(MemoryFault) as title_error:
        service.create_experience({"scope": "global"})
    assert title_error.value.code == "invalid_request"


# ------------------------------------------------------------ 运行时引用计数


def test_bind_runtime_guard_forwards_to_the_queue(service):
    def acquire(runtime):
        return None

    async def release(runtime):
        return None

    service.bind_runtime_guard(acquire, release)

    assert service.queue._acquire is acquire
    assert service.queue._release is release


def test_bind_runtime_guard_tolerates_a_queue_stand_in(store, tool_dir):
    """测试里的队列替身没有这个钩子，门面不能因此报错。"""
    service = MemoryService(store, queue=FakeQueue())
    service.bind_runtime_guard(lambda runtime: None, lambda runtime: None)


def test_queue_guards_the_runtime_on_both_paths():
    """成功与异常两条路径都必须 acquire 一次、release 一次。"""

    class _ScriptedExtractor:
        def __init__(self, outcome):
            self.runtime = object()
            self.outcome = outcome

        async def extract_session(self, session_id, ledger, **kwargs):
            if isinstance(self.outcome, BaseException):
                raise self.outcome
            return dict(self.outcome)

    async def scenario(outcome):
        calls = []

        def acquire(runtime):
            calls.append(("acquire", runtime))

        async def release(runtime):
            calls.append(("release", runtime))

        extractor = _ScriptedExtractor(outcome)
        queue = ExtractionQueue(extractor)
        queue.bind_runtime_guard(acquire, release)
        queue.start()
        queue.submit("sess-g1", {"calls": []})
        await asyncio.wait_for(queue._queue.join(), timeout=5)
        await queue.stop()
        return calls, extractor.runtime

    calls, runtime = asyncio.run(scenario({"status": "completed", "attempts": 1}))
    assert calls == [("acquire", runtime), ("release", runtime)]

    failing_calls, failing_runtime = asyncio.run(scenario(RuntimeError("boom")))
    assert failing_calls == [("acquire", failing_runtime), ("release", failing_runtime)]