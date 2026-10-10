"""会话分类（set_active_category）的持久化与接口契约。

守两件事：

1. `Session.category` 必须由 save_session 落进 meta.json。此前 set_active_category
   只往 meta.json 塞一个游离的 category 键，而 save_session 用固定的键集合整份重建
   meta.json，那个键在同一个回合内就被抹掉，后端「回落读 meta」永远读不到东西。
2. GET /sessions/{id} 要把 category 带出来，前端切会话才能把下拉框恢复成服务端真值。
"""
import json

import pytest
from fastapi.testclient import TestClient

import app as server
import session as session_mod


@pytest.fixture
def sessions_root(tmp_path, monkeypatch):
    root = tmp_path / "sessions"
    root.mkdir()
    monkeypatch.setattr(session_mod, "SESSIONS_DIR", root)
    monkeypatch.setattr(session_mod, "_storage_issues", [])
    return root


def _meta(sessions_root, sid):
    return json.loads((sessions_root / sid / "meta.json").read_text(encoding="utf-8"))


def test_category_round_trips_through_save_and_load(sessions_root):
    s = session_mod.create_session("分类会话")
    s.category = "missile"
    session_mod.save_session(s)

    assert _meta(sessions_root, s.id)["category"] == "missile"
    assert session_mod.load_session(s.id).category == "missile"


def test_repeated_save_does_not_wipe_category(sessions_root):
    """回归守卫：同一回合内再保存一次，分类不能被整份重建的 meta 抹掉。"""
    s = session_mod.create_session("分类会话")
    s.category = "aircraft"
    session_mod.save_session(s)

    # 模拟 app.py 在 tool_result / done 之后那次保存：会话内容变了，分类不变
    s.messages.append({"role": "user", "content": "再来一次"})
    session_mod.save_session(s)

    assert _meta(sessions_root, s.id)["category"] == "aircraft"
    assert session_mod.load_session(s.id).category == "aircraft"


def test_category_can_be_cleared_back_to_all(sessions_root):
    s = session_mod.create_session("分类会话")
    s.category = "missile"
    session_mod.save_session(s)

    s.category = ""
    session_mod.save_session(s)

    assert _meta(sessions_root, s.id)["category"] == ""
    assert session_mod.load_session(s.id).category == ""


def test_old_session_without_category_loads_as_empty(sessions_root):
    """老会话的 meta.json 里没有这个键，必须读成空串而不是抛异常。"""
    s = session_mod.create_session("老会话")
    session_mod.save_session(s)
    meta_path = sessions_root / s.id / "meta.json"
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    del meta["category"]
    meta_path.write_text(json.dumps(meta, ensure_ascii=False), encoding="utf-8")

    loaded = session_mod.load_session(s.id)
    assert loaded is not None
    assert loaded.category == ""


def test_get_session_returns_category(sessions_root):
    s = session_mod.create_session("分类会话")
    s.category = "missile"
    session_mod.save_session(s)

    response = TestClient(server.app).get("/sessions/%s" % s.id)

    assert response.status_code == 200
    assert response.json()["meta"]["category"] == "missile"


def test_get_session_category_is_empty_for_unclassified_session(sessions_root):
    s = session_mod.create_session("未分类会话")
    session_mod.save_session(s)

    response = TestClient(server.app).get("/sessions/%s" % s.id)

    assert response.status_code == 200
    assert response.json()["meta"]["category"] == ""