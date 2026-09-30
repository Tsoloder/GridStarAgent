"""数据目录策略测试：固定 .gridstar、旧目录迁移、提权钉目录。

权限修复（icacls/takeown/UAC）依赖真实 ACL 与提权，不适合放进单元测试，
由 agent/agent/check_data_dir.py 与手工验证覆盖；这里锁住可离线的路径策略。
"""
import importlib
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import data_dir_guard  # noqa: E402
import paths  # noqa: E402


@pytest.fixture
def fake_profile(tmp_path, monkeypatch):
    """把 Path.home() 指到沙箱，避免测试碰真实用户目录。"""
    monkeypatch.setattr(paths.Path, "home", classmethod(lambda cls: tmp_path))
    monkeypatch.delenv(data_dir_guard.PIN_ENV, raising=False)
    return tmp_path


def test_default_dir_is_gridstar_under_roaming_appdata(fake_profile):
    """默认位置就应该是 <profile>\\AppData\\Roaming\\.gridstar，逐段钉住。"""
    assert paths.DATA_DIR_NAME == ".gridstar"
    parent = paths._user_data_parent()
    if os.name == "nt":
        assert parent == fake_profile / "AppData" / "Roaming"
    else:
        assert parent == fake_profile / ".local" / "share"
    assert parent / paths.DATA_DIR_NAME == parent / ".gridstar"


def test_legacy_env_var_no_longer_selects_the_dir(fake_profile, monkeypatch, tmp_path):
    """自定义路径已取消：设了旧变量也必须落到固定目录。"""
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    monkeypatch.setenv("CLINELIKECHAT_DATA_DIR", str(elsewhere))
    module = importlib.reload(paths)
    try:
        assert "elsewhere" not in str(module.DATA_DIR)
        assert module.DATA_DIR.name == ".gridstar"
    finally:
        monkeypatch.undo()
        importlib.reload(paths)


def test_pinned_dir_wins(fake_profile, monkeypatch, tmp_path):
    """提权重启传下来的钉住目录优先，且不参与旧目录迁移。"""
    pinned = tmp_path / "pinned-data"
    monkeypatch.setenv(data_dir_guard.PIN_ENV, str(pinned))
    module = importlib.reload(paths)
    try:
        assert module.DATA_DIR == pinned.resolve()
    finally:
        monkeypatch.undo()
        importlib.reload(paths)


def test_migrates_legacy_dir_when_target_absent(tmp_path):
    root = tmp_path / ".gridstar"
    legacy = tmp_path / "ClineLikeChat"
    (legacy / "sessions").mkdir(parents=True)
    (legacy / "config.json").write_text('{"marker": 1}', encoding="utf-8")

    paths.migrate_legacy_dir(root)

    assert root.is_dir()
    assert not legacy.exists()
    assert (root / "config.json").read_text(encoding="utf-8") == '{"marker": 1}'
    assert any("搬迁" in item for item in paths.STARTUP_NOTICES)


def test_keeps_both_when_target_already_exists(tmp_path):
    """两边都在时不合并、不删除，只提示，避免程序替使用者做取舍。"""
    root = tmp_path / ".gridstar"
    root.mkdir()
    legacy = tmp_path / "ClineLikeChat"
    legacy.mkdir()
    (legacy / "config.json").write_text("{}", encoding="utf-8")

    paths.STARTUP_NOTICES.clear()
    paths.migrate_legacy_dir(root)

    assert legacy.is_dir()
    assert not (root / "config.json").exists()
    assert any("同时存在" in item for item in paths.STARTUP_NOTICES)


def test_ensure_subdirs_reports_failures(tmp_path, monkeypatch):
    """子目录建不出来时返回描述而不是抛异常。"""
    blocker = tmp_path / "blocked"
    blocker.write_text("not a directory", encoding="utf-8")
    monkeypatch.setattr(paths, "SESSIONS_DIR", blocker / "sessions")
    monkeypatch.setattr(paths, "LOG_DIR", tmp_path / "logs")
    monkeypatch.setattr(paths, "SKILLS_DIR", tmp_path / "skills")
    monkeypatch.setattr(paths, "UPLOADS_DIR", tmp_path / "uploads")

    failures = paths.ensure_subdirs()

    assert len(failures) == 1
    assert "blocked" in failures[0]
    assert (tmp_path / "logs").is_dir()
    assert (tmp_path / "uploads").is_dir()


def test_probe_writable_detects_writable_dir(tmp_path):
    assert data_dir_guard.probe_writable(tmp_path) is None
    assert list(tmp_path.iterdir()) == []


def test_probe_writable_reports_uncreatable_dir(tmp_path):
    blocker = tmp_path / "file_not_dir"
    blocker.write_text("x", encoding="utf-8")
    error = data_dir_guard.probe_writable(blocker / "child")
    assert error is not None
    assert "不可创建" in error or "不可写" in error


def test_rejects_removed_env_var_name_anywhere_in_paths_module():
    """旧变量名不得再出现在 paths 里，否则等于又把自定义路径放回来了。"""
    source = (paths.Path(paths.__file__)).read_text(encoding="utf-8")
    assert "CLINELIKECHAT_DATA_DIR" not in source