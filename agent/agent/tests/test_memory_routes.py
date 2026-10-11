"""``/memory/*`` 路由的契约测试。

门面和存储各自有单元测试；这里只管一件事：**HTTP 层有没有把契约守住**。
重点是错误码（404/409/400/503 各自对应什么）、204 空响应、以及一个容易
被忽略的承诺 —— 库坏掉时 ``/memory/summary`` 仍要能打开页面。
"""
from __future__ import annotations

import dataclasses

import pytest
from fastapi.testclient import TestClient

import app as server
import tool_memory
from config import MemoryConfig
from experience_memory import MemoryStore
from memory_store import MemoryService


def _record(title="质量检查超标时先做局部加密处理", **overrides):
    record = {
        "kind": "experience",
        "scope": "global",
        "task_type": "mesh_quality",
        "title": title,
        "trigger_cues": ["网格质量检查不达标"],
        "preconditions": ["已完成表面网格"],
        "strategy_steps": ["先定位超标单元", "做局部加密后复检"],
        "tool_sequence": ["ExamineMeshQuality"],
        "source_kind": "auto_extraction",
    }
    record.update(overrides)
    return record


@pytest.fixture()
def service(tmp_path, monkeypatch):
    monkeypatch.setattr(tool_memory, "MEMORY_PATH", tmp_path / "tool_parameter_memory.json")
    store = MemoryStore(tmp_path / "memory.db")
    svc = MemoryService(store)
    state = {"memory": MemoryConfig()}

    def writer(updates):
        state["memory"] = dataclasses.replace(state["memory"], **dict(updates))
        return state["memory"]

    svc.bind_config(lambda: state["memory"], writer)
    monkeypatch.setattr(server, "_memory", svc)
    monkeypatch.setattr(server, "_config_lock", None)
    return svc


@pytest.fixture()
def client(service):
    return TestClient(server.app)


# --------------------------------------------------------------- 摘要与开关


def test_summary_is_503_when_service_is_absent(monkeypatch):
    monkeypatch.setattr(server, "_memory", None)
    response = TestClient(server.app).get("/memory/summary")
    assert response.status_code == 503
    assert response.json()["code"] == "store_unavailable"


def test_summary_reports_shadow_stage(client):
    payload = client.get("/memory/summary").json()
    assert payload["stage"] == "shadow"
    assert payload["enabled"] is True
    assert payload["store"]["path_hint"] == "用户数据目录"


def test_summary_still_opens_when_store_is_broken(tmp_path, monkeypatch):
    """库坏了页面还得能显示开关和参数记忆 —— 这是 §7 明确要求的降级行为。"""
    monkeypatch.setattr(tool_memory, "MEMORY_PATH", tmp_path / "tool_parameter_memory.json")
    (tmp_path / "blocked").write_text("not a directory", encoding="utf-8")
    svc = MemoryService(MemoryStore(tmp_path / "blocked" / "memory.db"))
    svc.bind_config(lambda: MemoryConfig(), lambda updates: MemoryConfig())
    monkeypatch.setattr(server, "_memory", svc)

    response = TestClient(server.app).get("/memory/summary")
    assert response.status_code == 200
    payload = response.json()
    assert payload["store"]["status"] == "unavailable"
    assert payload["store"]["last_error"]["code"] == "unavailable"
    # 错误文案里绝不能出现数据目录
    assert str(tmp_path) not in response.text


def test_config_patch_is_immediate_and_returns_summary(client, service):
    response = client.patch("/memory/config", json={"extract": False})
    assert response.status_code == 200
    payload = response.json()
    assert payload["config"]["experience_extract"] is False
    assert payload["config"]["fact_extract"] is False
    assert service.config.experience_extract is False


def test_config_patch_rejects_unknown_key_and_non_boolean(client):
    assert client.patch("/memory/config", json={"nope": True}).status_code == 400
    assert client.patch("/memory/config", json={"enabled": "yes"}).status_code == 400
    assert client.patch("/memory/config", json={}).status_code == 400


def test_config_writer_updates_current_config_and_saves(monkeypatch, tmp_path):
    """开关必须并回 config.json，不另建文件 —— 否则模型 Tab 保存时会撞 409。"""
    from config import config_from_dict

    config = config_from_dict({
        "version": 1,
        "default_model": "alpha/model-b",
        "providers": [{
            "id": "alpha", "name": "Alpha", "base_url": "https://alpha.test/v1",
            "api_key": "", "headers": {}, "discover_models": False,
            "discovery_api": "openai", "default_api": "openai-chat", "enabled": True,
        }],
        "models": [{
            "id": "model-b", "provider": "alpha", "name": "Model B",
            "capabilities": {"tools": True},
        }],
    })
    saved = []
    monkeypatch.setattr(server, "current_config", config)
    monkeypatch.setattr(server, "save_config", lambda cfg: saved.append(cfg))
    monkeypatch.setattr(server, "_memory", None)

    result = server._memory_config_writer({"enabled": False, "fact_inject": False})
    assert result.enabled is False
    assert result.fact_inject is False
    # 模型设置本身没有被顺手改掉
    assert server.current_config.default_model == "alpha/model-b"
    assert saved and saved[0].memory.enabled is False


# --------------------------------------------------------------- 经验审核


def test_experience_list_and_detail(client, service):
    item_id = service.store.add_item(_record())["id"]

    listing = client.get("/memory/experiences", params={"status": "candidate"}).json()
    assert listing["total"] == 1
    summary = listing["items"][0]
    assert summary["id"] == item_id
    assert summary["status"] == "candidate"
    # 列表不该把正文全塞回来，详情才给
    assert "strategy_steps" not in summary

    detail = client.get("/memory/experiences/%s" % item_id).json()
    assert detail["strategy_steps"] == ["先定位超标单元", "做局部加密后复检"]


def test_experience_detail_404_for_unknown_id(client):
    response = client.get("/memory/experiences/exp_missing")
    assert response.status_code == 404
    assert response.json()["code"] == "not_found"


def test_transition_approve_then_conflict_is_409(client, service):
    item_id = service.store.add_item(_record())["id"]
    approved = client.patch(
        "/memory/experiences/%s" % item_id, json={"action": "approve", "reason": "已复现"}
    )
    assert approved.status_code == 200
    assert approved.json()["status"] == "approved"

    again = client.patch("/memory/experiences/%s" % item_id, json={"action": "approve"})
    assert again.status_code == 409
    assert again.json()["code"] == "state_conflict"


def test_transition_requires_action(client, service):
    item_id = service.store.add_item(_record())["id"]
    response = client.patch("/memory/experiences/%s" % item_id, json={})
    assert response.status_code == 400


def test_candidate_is_invisible_to_recall_until_approved(client, service):
    """人工审核这道闸门是整个设计的核心承诺，路由层也要能验证它。"""
    item_id = service.store.add_item(_record())["id"]
    assert client.get("/memory/experiences/%s" % item_id).json()["status"] == "candidate"
    assert service.store.recall(
        query="网格质量检查不达标", scopes=("global",), task_type="mesh_quality"
    ) == []
    client.patch("/memory/experiences/%s" % item_id, json={"action": "approve"})
    assert service.store.recall(
        query="网格质量检查不达标", scopes=("global",), task_type="mesh_quality"
    )


def test_delete_returns_204_then_404(client, service):
    item_id = service.store.add_item(_record())["id"]
    response = client.delete("/memory/experiences/%s" % item_id)
    assert response.status_code == 204
    assert response.content == b""
    assert client.delete("/memory/experiences/%s" % item_id).status_code == 404


def test_bulk_route_validates_and_reports_partial_success(client, service):
    first = service.store.add_item(_record())["id"]
    second = service.store.add_item(_record(title="部件分割前先修复自由边"))["id"]

    assert client.post("/memory/experiences/bulk", json={"action": "approve"}).status_code == 400
    assert client.post(
        "/memory/experiences/bulk", json={"ids": "not-a-list", "action": "approve"}
    ).status_code == 400
    assert client.post(
        "/memory/experiences/bulk", json={"ids": [first], "action": "deprecate"}
    ).status_code == 400

    response = client.post(
        "/memory/experiences/bulk",
        json={"ids": [first, "exp_missing", second], "action": "approve", "reason": "批量通过"},
    )
    assert response.status_code == 200
    payload = response.json()
    assert (payload["succeeded"], payload["failed"]) == (2, 1)
    assert [entry["code"] for entry in payload["results"]] == ["ok", "not_found", "ok"]


def test_bulk_rejects_more_than_hundred_ids(client):
    response = client.post(
        "/memory/experiences/bulk",
        json={"ids": ["exp_%d" % n for n in range(101)], "action": "approve"},
    )
    assert response.status_code == 400


# --------------------------------------------------------------- 参数记忆


def test_tool_parameter_routes_round_trip(client):
    tool_memory.remember(
        "create_mesh", {"size": 2.0, "algorithm": "delaunay"},
        {"type": "object", "properties": {"size": {}, "algorithm": {}}},
    )
    listing = client.get("/memory/tool-parameters").json()
    assert listing["tool_count"] == 1
    assert listing["tools"][0]["fields"] == {"algorithm": "delaunay", "size": 2.0}

    assert client.delete("/memory/tool-parameters/create_mesh/algorithm").status_code == 204
    assert client.delete("/memory/tool-parameters/create_mesh/algorithm").status_code == 404
    assert client.delete("/memory/tool-parameters/nope").status_code == 404
    assert client.delete("/memory/tool-parameters/create_mesh").status_code == 204
    assert client.get("/memory/tool-parameters").json()["tools"] == []


def test_tool_parameter_clear_reports_counts(client):
    tool_memory.remember(
        "create_mesh", {"size": 2.0, "algorithm": "delaunay"},
        {"type": "object", "properties": {"size": {}, "algorithm": {}}},
    )
    tool_memory.remember(
        "DealWatertight", {"merge": True}, {"type": "object", "properties": {"merge": {}}}
    )
    response = client.delete("/memory/tool-parameters")
    assert response.status_code == 200
    assert response.json() == {"tools": 2, "fields": 3}


def test_tool_names_and_fields_are_url_encoded(client):
    """工具名与字段名来自持久化文件，前端用 encodeURIComponent 传，后端要能解。"""
    tool_memory.remember(
        "create mesh", {"a b": 1}, {"type": "object", "properties": {"a b": {}}}
    )
    listing = client.get("/memory/tool-parameters").json()
    assert listing["tools"][0]["tool"] == "create mesh"
    assert client.delete("/memory/tool-parameters/create%20mesh/a%20b").status_code == 204


def test_slash_in_name_is_rejected_not_silently_missing(client):
    """带斜杠的名字在路径式接口里无法寻址：要明确回 400，别让前端对着 404 猜。

    注意 `%2F` 在路由匹配前就被解码成路径分隔符，所以那条路径根本进不了
    视图，星形路由只会给 404。真正能进到视图的是反斜杠，这里一并守住。
    """
    tool_memory.remember(
        "create_mesh", {"a\\b": 1}, {"type": "object", "properties": {"a\\b": {}}}
    )
    response = client.delete("/memory/tool-parameters/create_mesh/a%5Cb")
    assert response.status_code == 400
    assert response.json()["code"] == "invalid_request"

    assert client.delete("/memory/tool-parameters/create%2Fmesh/a").status_code == 404


# --------------------------------------------------------------- 维护


def test_maintenance_run_returns_202_and_is_visible_in_status(client):
    response = client.post("/memory/maintenance/runs", json={"kind": "governance"})
    assert response.status_code == 202
    assert "maintenance" in response.json()

    status = client.get("/memory/status").json()
    assert status["maintenance"]["state"] == "idle"
    assert "extraction" in status


def test_maintenance_rejects_unknown_kind(client):
    response = client.post("/memory/maintenance/runs", json={"kind": "reindex"})
    assert response.status_code == 400
    assert response.json()["code"] == "invalid_request"


def test_maintenance_defaults_to_governance(client):
    response = client.post("/memory/maintenance/runs", json={})
    assert response.status_code == 202


def test_status_reports_queue_counters(client):
    payload = client.get("/memory/status").json()["extraction"]
    assert payload["pending_jobs"] == 0
    assert payload["running"] is False
    assert payload["dropped"] == 0