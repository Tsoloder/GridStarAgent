"""task_ledger 单元测试：ID 全量记录/截断、ok 判定、快照渲染、持久化。"""
import json
import os
import uuid

import pytest

import session as session_mod
from task_ledger import (
    ARGS_LIST_CAP,
    ARGS_TEXT_LIMIT,
    TaskLedger,
    digest_args,
    digest_result,
    digest_value,
    normalize_phase,
    result_failed,
)


@pytest.fixture
def tmp_sessions(monkeypatch, tmp_path):
    """隔离会话数据目录，避免污染真实台账文件。"""
    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    return tmp_path


@pytest.fixture
def ledger(tmp_sessions):
    return TaskLedger(str(uuid.uuid4()))


# ---------- 参数摘要：ID 全量记录 ----------

def test_id_list_fully_recorded():
    ids = ["edge_%d" % i for i in range(120)]
    assert digest_value(ids) == ids
    args = digest_args({"surface_ids": ids, "name": "x"})
    assert args["surface_ids"] == ids


def test_id_list_over_cap_truncated():
    ids = list(range(ARGS_LIST_CAP + 80))
    digested = digest_value(ids)
    assert len(digested) == ARGS_LIST_CAP + 1
    assert digested[:ARGS_LIST_CAP] == ids[:ARGS_LIST_CAP]
    assert "80" in digested[-1]


def test_non_scalar_list_collapsed():
    assert digest_value([{"id": 1}, {"id": 2}]) == "[2 项]"
    # bool 是 int 子类，但不视为 ID 列表元素
    assert digest_value([True, False]) == "[2 项]"


def test_long_string_truncated():
    digested = digest_value("p" * (ARGS_TEXT_LIMIT + 50))
    assert len(digested) == ARGS_TEXT_LIMIT + 1
    assert digested.endswith("…")


def test_scalar_kept_and_nested_dict_digested():
    assert digest_value(3.5) == 3.5
    assert digest_value(True) is True
    assert digest_value(None) is None
    digested = digest_value({"config": {"surface_ids": ["s1", "s2"], "path": "x" * 200}})
    assert digested["config"]["surface_ids"] == ["s1", "s2"]
    assert len(digested["config"]["path"]) == ARGS_TEXT_LIMIT + 1


# ---------- 结果摘要 ----------

def test_digest_result_keeps_id_fields():
    result = json.dumps({"status": "ok", "created_ids": ["b1", "b2"], "detail": "y" * 300})
    digested = digest_result(result)
    assert "b1" in digested and "b2" in digested
    assert "y" not in digested


def test_digest_result_message_field():
    assert digest_result(json.dumps({"message": "done", "other": "x"})) == "done"


def test_digest_result_text_fallback():
    assert digest_result("plain text result") == "plain text result"
    assert digest_result("r" * 500).endswith("…")
    assert digest_result("") == ""


# ---------- 失败判定 ----------

@pytest.mark.parametrize(("result", "failed"), [
    ("false", True),
    (json.dumps({"status": "error", "message": "x"}), True),
    (json.dumps({"result": False}), True),
    (json.dumps({"result": "false"}), True),
    (json.dumps({"status": "ok"}), False),
    (json.dumps({"result": True}), False),
    ("not json at all", False),
    ("", False),
])
def test_result_failed(result, failed):
    assert result_failed(result) is failed


# ---------- 阶段规范化 ----------

def test_normalize_phase_defaults_and_validation():
    assert normalize_phase({"id": "import", "title": "导入"}) == {
        "id": "import", "title": "导入", "status": "pending", "note": "",
    }
    # 未知 status 回退 pending（如旧协议的 active）
    assert normalize_phase({"id": "a", "title": "A", "status": "active"})["status"] == "pending"
    assert normalize_phase({"id": "a", "title": "A", "status": "failed"})["status"] == "failed"
    with pytest.raises(ValueError):
        normalize_phase({"id": "", "title": "A"})
    with pytest.raises(ValueError):
        normalize_phase({"id": "a", "title": ""})
    with pytest.raises(ValueError):
        normalize_phase("not-a-dict")


# ---------- update_plan ----------

def test_update_plan_requires_fields(ledger):
    with pytest.raises(ValueError):
        ledger.update_plan("", "T", [{"id": "a", "title": "A"}])
    with pytest.raises(ValueError):
        ledger.update_plan("p", "", [{"id": "a", "title": "A"}])
    with pytest.raises(ValueError):
        ledger.update_plan("p", "T", [])
    with pytest.raises(ValueError):
        ledger.update_plan("p", "T", [{"id": "a"}])


def test_update_plan_full_replacement(ledger):
    confirmation = ledger.update_plan("mesh", "网格划分", [
        {"id": "import", "title": "导入", "status": "done"},
        {"id": "mesh", "title": "网格生成", "status": "in_progress"},
    ])
    assert "1/2" in confirmation
    assert "网格生成" in confirmation
    # 全量替换：第二次更新后旧阶段被整体替换
    ledger.update_plan("mesh", "网格划分", [{"id": "export", "title": "导出", "status": "pending"}])
    assert [phase["id"] for phase in ledger.plan["phases"]] == ["export"]


def test_active_phase(ledger):
    assert ledger.active_phase() is None
    ledger.update_plan("p", "T", [
        {"id": "a", "title": "A", "status": "done"},
        {"id": "b", "title": "B", "status": "in_progress"},
    ])
    assert ledger.active_phase()["id"] == "b"


# ---------- 工具执行账本 ----------

def test_record_call_attributes_phase_and_ids(ledger):
    ledger.update_plan("p", "T", [{"id": "a", "title": "A", "status": "in_progress"}])
    ids = ["obj_%d" % i for i in range(50)]
    ledger.record_call(
        "SetBoundary", {"object_ids": ids, "value": 1},
        result=json.dumps({"status": "ok"}), ok=True,
    )
    call = ledger.calls[-1]
    assert call["phase"] == "A"
    assert call["tool"] == "SetBoundary"
    assert call["args_digest"]["object_ids"] == ids
    assert call["ok"] is True
    assert call["ts"]


def test_success_calls_flush_on_threshold(ledger):
    for _ in range(4):
        ledger.record_call("GetModelTree", {}, result="ok", ok=True)
    assert not os.path.exists(ledger.path)
    ledger.record_call("GetModelTree", {}, result="ok", ok=True)
    assert os.path.exists(ledger.path)


def test_failed_call_flushes_immediately(ledger):
    ledger.record_call(
        "ImportCAD", {"file": "a.igs"},
        result=json.dumps({"status": "error"}), ok=False,
    )
    assert os.path.exists(ledger.path)


# ---------- 注入快照 ----------

def test_render_empty_ledger(ledger):
    assert ledger.render_task_progress() == ""


def test_render_snapshot_folds_old_calls(ledger):
    ledger.update_plan("p", "网格划分", [
        {"id": "a", "title": "导入", "status": "done", "note": "导入 f6.igs"},
        {"id": "b", "title": "网格生成", "status": "in_progress"},
        {"id": "c", "title": "导出", "status": "pending"},
    ])
    for i in range(10):
        ledger.record_call("GetModelTree", {"i": i}, result="ok", ok=(i != 3))
    snapshot = ledger.render_task_progress()

    assert snapshot.startswith("<task_progress>")
    assert snapshot.rstrip().endswith("</task_progress>")
    assert "【当前计划】" in snapshot
    assert "1/3 完成" in snapshot
    assert "导入 f6.igs" in snapshot
    assert "网格生成（进行中）" in snapshot
    assert "【执行记录】最近 8 条" in snapshot
    assert "另有 2 条调用" in snapshot
    assert "【差距】" in snapshot
    assert "上次失败：GetModelTree" in snapshot


# ---------- 持久化 ----------

def test_persistence_reload(tmp_sessions):
    sid = str(uuid.uuid4())
    ledger = TaskLedger(sid)
    ledger.update_plan("p", "T", [{"id": "a", "title": "A", "status": "done"}])
    ledger.record_call("GetModelTree", {"object_ids": ["o1"]}, result="ok", ok=True)
    ledger.flush()

    reloaded = TaskLedger(sid)
    assert reloaded.plan == ledger.plan
    assert len(reloaded.calls) == 1
    assert reloaded.calls[0]["args_digest"]["object_ids"] == ["o1"]


def test_clear_removes_state_and_file(tmp_sessions):
    sid = str(uuid.uuid4())
    ledger = TaskLedger(sid)
    ledger.update_plan("p", "T", [{"id": "a", "title": "A"}])
    assert os.path.exists(ledger.path)
    ledger.clear()
    assert ledger.plan is None
    assert ledger.calls == []
    assert ledger.is_empty()
    assert not os.path.exists(ledger.path)
