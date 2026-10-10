"""回归测试：窗口被隐藏/最小化时，叫回可见这条回退路必须存在。

现场背景：GridStar 最小化到托盘后 IsWindowVisible=False 且 IsIconic=False，
仅可见候选的定位直接返回 None，激活与截图都会报「找不到窗口」——用户看到的
是"截图工具明明在，却说没窗口"。回退顺序固定为「先可见，再隐藏」，
并且必须如实标出 was_hidden，不能装作窗口本来就在那儿。
"""
import pytest

from tools import screen_automation as sa


HIDDEN_INFO = {
    "hwnd": 42, "pid": 28848, "exe": r"D:\gs\GridStar.exe", "title": "NNW-GridStar_2026R1",
    "class_name": "Qt5QWindowIcon", "frame_rect": {"left": 0, "top": 0, "right": 100, "bottom": 100},
    "is_visible": False, "is_iconic": False, "is_foreground": False,
}


def test_find_any_prefers_visible_window(monkeypatch):
    visible = dict(HIDDEN_INFO, is_visible=True)
    monkeypatch.setattr(sa, "_find_gridstar_window",
                        lambda window_title=None, include_hidden=False: dict(visible))

    info = sa._find_gridstar_window_any()

    assert info["hwnd"] == 42
    assert "was_hidden" not in info


def test_find_any_falls_back_to_hidden_candidates(monkeypatch):
    calls = []

    def fake(window_title=None, include_hidden=False):
        calls.append((window_title, include_hidden))
        return dict(HIDDEN_INFO) if include_hidden else None

    monkeypatch.setattr(sa, "_find_gridstar_window", fake)

    info = sa._find_gridstar_window_any("NNW")

    assert info["hwnd"] == 42
    assert info["was_hidden"] is True
    assert calls == [("NNW", False), ("NNW", True)]


def test_find_any_returns_none_without_any_candidate(monkeypatch):
    monkeypatch.setattr(sa, "_find_gridstar_window",
                        lambda window_title=None, include_hidden=False: None)

    assert sa._find_gridstar_window_any() is None


def test_show_window_leaves_visible_window_untouched(monkeypatch):
    shown = []
    monkeypatch.setattr(sa.win32gui, "IsWindowVisible", lambda hwnd: True)
    monkeypatch.setattr(sa.win32gui, "IsIconic", lambda hwnd: False)
    monkeypatch.setattr(sa.win32gui, "ShowWindow", lambda hwnd, cmd: shown.append(cmd))

    assert sa._show_window(1, settle_s=0) is True
    assert shown == []


def test_show_window_displays_hidden_window(monkeypatch):
    state = {"visible": False, "iconic": False}
    commands = []

    def show(hwnd, cmd):
        commands.append(cmd)
        if cmd in (sa.win32con.SW_SHOW, sa.win32con.SW_RESTORE):
            state["visible"], state["iconic"] = True, False

    monkeypatch.setattr(sa.win32gui, "IsWindowVisible", lambda hwnd: state["visible"])
    monkeypatch.setattr(sa.win32gui, "IsIconic", lambda hwnd: state["iconic"])
    monkeypatch.setattr(sa.win32gui, "ShowWindow", show)

    assert sa._show_window(1, settle_s=0) is True
    assert sa.win32con.SW_SHOW in commands


def test_capture_refuses_when_window_stays_hidden(monkeypatch):
    monkeypatch.setattr(sa, "_missing_deps_error", lambda tool, needs=(): None)
    monkeypatch.setattr(sa, "_find_gridstar_window_any",
                        lambda window_title=None: dict(HIDDEN_INFO, was_hidden=True))
    monkeypatch.setattr(sa, "_show_window", lambda hwnd, settle_s=0.0: False)

    payload = sa._capture_gridstar(activate=False)

    assert '"status": "error"' in payload.replace("'", '"')
    assert "隐藏状态" in payload
    assert "陈画面" in payload


def test_capture_restores_hidden_window_before_grabbing(monkeypatch):
    """隐藏窗口要先被叫回可见，截图矩形才可用。"""
    visible = dict(HIDDEN_INFO, is_visible=True)
    order = []

    def find_any(window_title=None):
        order.append("find")
        return dict(visible) if order.count("find") > 1 else dict(HIDDEN_INFO, was_hidden=True)

    monkeypatch.setattr(sa, "_missing_deps_error", lambda tool, needs=(): None)
    monkeypatch.setattr(sa, "_find_gridstar_window_any", find_any)
    monkeypatch.setattr(sa, "_show_window",
                        lambda hwnd, settle_s=0.0: order.append("show") or True)
    monkeypatch.setattr(sa, "_clamp_to_virtual_screen",
                        lambda rect: {"left": 0, "top": 0, "width": 100, "height": 100})
    monkeypatch.setattr(sa, "_grab_png", lambda region: b"\x89PNG-fake")

    payload = sa._capture_gridstar(activate=False)

    assert order[:2] == ["find", "show"]
    assert '"was_hidden": true' in payload
    assert '"restored_from_hidden": true' in payload
    assert "restore_note" in payload