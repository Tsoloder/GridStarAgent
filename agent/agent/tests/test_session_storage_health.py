"""会话写入与列表读取在存储不可用时的行为。

要守住两件事：
1. 原子写不再用可能在「目录拒绝写入」时卡死的 tempfile.mkstemp；
2. 会话列表读不出来时留痕，不能静默返回空列表冒充「没有会话」。
"""
import json
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import session as session_mod  # noqa: E402


@pytest.fixture
def sessions_root(tmp_path, monkeypatch):
    root = tmp_path / "sessions"
    root.mkdir()
    monkeypatch.setattr(session_mod, "SESSIONS_DIR", root)
    monkeypatch.setattr(session_mod, "_storage_issues", [])
    return root


def test_open_exclusive_creates_distinct_files(tmp_path):
    first, path_a = session_mod._open_exclusive(str(tmp_path))
    second, path_b = session_mod._open_exclusive(str(tmp_path))
    try:
        assert path_a != path_b
        assert os.path.exists(path_a) and os.path.exists(path_b)
    finally:
        os.close(first)
        os.close(second)


def test_open_exclusive_raises_fast_when_dir_missing(tmp_path):
    """目录不存在立刻报错，不重试；这是与 mkstemp 卡死的区别所在。"""
    missing = tmp_path / "nope"
    with pytest.raises(OSError):
        session_mod._open_exclusive(str(missing))


def test_atomic_write_round_trip(sessions_root):
    target = sessions_root / "index.json"
    session_mod.atomic_write(str(target), json.dumps([{"id": "a"}]))
    assert json.loads(target.read_text(encoding="utf-8")) == [{"id": "a"}]
    # 不留临时文件
    assert [p.name for p in sessions_root.iterdir()] == ["index.json"]


def test_atomic_write_reports_error_and_cleans_up(sessions_root, monkeypatch):
    def boom(path, flags):
        raise PermissionError(13, "Permission denied", path)

    monkeypatch.setattr(session_mod.os, "open", boom)
    with pytest.raises(PermissionError):
        session_mod.atomic_write(str(sessions_root / "index.json"), "[]")
    assert list(sessions_root.iterdir()) == []


def test_read_index_missing_is_normal(sessions_root):
    assert session_mod._read_index() == []
    assert session_mod.storage_issues() == []


def test_read_index_corrupt_is_reported(sessions_root):
    (sessions_root / "index.json").write_text("{not json", encoding="utf-8")
    assert session_mod._read_index() == []
    issues = session_mod.storage_issues()
    assert len(issues) == 1
    assert "损坏" in issues[0]


def test_read_index_unreadable_is_reported(sessions_root, monkeypatch):
    index = sessions_root / "index.json"
    index.write_text("[]", encoding="utf-8")
    real_read_text = session_mod.Path.read_text

    def deny(self, *args, **kwargs):
        if self.name == "index.json":
            raise PermissionError(13, "Permission denied", str(self))
        return real_read_text(self, *args, **kwargs)

    monkeypatch.setattr(session_mod.Path, "read_text", deny)
    assert session_mod._read_index() == []
    issues = session_mod.storage_issues()
    assert len(issues) == 1
    assert "不可读取" in issues[0]


def test_storage_issue_recorded_once(sessions_root):
    (sessions_root / "index.json").write_text("bad", encoding="utf-8")
    session_mod._read_index()
    session_mod._read_index()
    assert len(session_mod.storage_issues()) == 1


def test_storage_issues_returns_a_copy(sessions_root):
    (sessions_root / "index.json").write_text("bad", encoding="utf-8")
    session_mod._read_index()
    snapshot = session_mod.storage_issues()
    snapshot.append("tampered")
    assert "tampered" not in session_mod.storage_issues()


def test_list_sessions_survives_corrupt_index(sessions_root):
    """索引损坏时列表为空但不抛异常，问题留给 storage_issues 说明。"""
    (sessions_root / "index.json").write_text("bad", encoding="utf-8")
    assert session_mod.list_sessions() == []
    assert session_mod.storage_issues()