"""回归测试：坐标工具的「点前预演」与「点后回拍」。

现场背景：模型控制 GridStar 只能走坐标点击（它的 UIA 控件树是空的），而点错一下
就是真实操作——生成网格、删除几何都不可逆。以前这条链路没有任何自我校验：模型
看不到自己点了哪里，也拿不到任何回执，点飞了要等用户发现。这里钉住两件事：

1. dry_run=True 必须一个鼠标事件都不注入，只回报「会落到哪个像素、那个像素上是谁」；
2. 真正执行之后必须自动回拍点击点周围的局部图，且图上真的画了标记。
"""
import base64
import io
import json

import pytest

from tools import screen_automation as sa


def _png(width: int = 40, height: int = 30, color=(30, 30, 30)) -> bytes:
    """造一张真 PNG。回拍图要过 Pillow 解码，假字节会走降级分支。"""
    PILImage = pytest.importorskip("PIL.Image")
    buf = io.BytesIO()
    PILImage.new("RGB", (width, height), color).save(buf, format="PNG")
    return buf.getvalue()


INFO = {
    "hwnd": 777, "pid": 28848, "exe": r"D:\gs\GridStar.exe", "title": "NNW-GridStar_2026R1",
    "class_name": "Qt5QWindowIcon", "process_name": "GridStar.exe",
    "frame_rect": {"left": 0, "top": 0, "right": 1920, "bottom": 1392},
    "is_visible": True, "is_iconic": False, "is_foreground": True,
}

REGION = {"left": 0, "top": 0, "width": 1920, "height": 1392}


def _patch(monkeypatch, *, region=None, info=None):
    """把坐标前置条件全部短路，只留下被测的那段逻辑。"""
    region = dict(region or REGION)
    state = {"clicks": [], "drags": [], "wheels": [], "shots": 0}

    monkeypatch.setattr(sa, "_pywin32_error", lambda tool: None)
    monkeypatch.setattr(sa, "_find_gridstar_window",
                        lambda window_title=None: dict(info or INFO))
    monkeypatch.setattr(sa, "_input_gate", lambda info: (True, ""))
    monkeypatch.setattr(sa, "_is_foreground_and_visible", lambda hwnd: True)
    monkeypatch.setattr(sa, "_bring_to_foreground", lambda hwnd, timeout_s=1.0: True)
    monkeypatch.setattr(sa, "_clamp_to_virtual_screen", lambda rect: dict(region))

    def fake_click(px, py, button="left", clicks=1):
        state["clicks"].append((px, py, button, clicks))
        return {"injected": 2, "expected": 2, "cursor": (px, py),
                "button": button, "clicks": clicks}

    def fake_drag(px0, py0, px1, py1, button="left", duration_ms=400):
        state["drags"].append((px0, py0, px1, py1))
        return {"injected": 14, "expected": 14, "cursor": (px1, py1),
                "button": button, "steps": 12, "duration_ms": duration_ms}

    def fake_wheel(px, py, notches):
        state["wheels"].append((px, py, notches))
        return {"injected": abs(notches), "expected": abs(notches),
                "cursor": (px, py), "amount": notches, "notches": abs(notches)}

    def fake_grab(grab_region):
        state["shots"] += 1
        return _png(grab_region["width"], grab_region["height"])

    monkeypatch.setattr(sa, "_mouse_click", fake_click)
    monkeypatch.setattr(sa, "_mouse_drag", fake_drag)
    monkeypatch.setattr(sa, "_mouse_wheel", fake_wheel)
    monkeypatch.setattr(sa, "_grab_png", fake_grab)
    monkeypatch.setattr(sa, "_hit_test_error", lambda info, px, py: None)
    monkeypatch.setattr(sa, "_pointer_evidence",
                        lambda px, py: {"ok": True, "hwnd_under_point": INFO["hwnd"]})
    monkeypatch.setattr(sa.time, "sleep", lambda seconds: None)
    monkeypatch.setattr(sa, "_LAST_BASIS", None)
    return state


def _payload(raw: str) -> dict:
    parsed = json.loads(raw)
    assert parsed["status"] == "success", parsed["result"]
    return parsed["result"]


# ── dry_run：一个事件都不许注入 ────────────────────────────────────

def test_click_dry_run_injects_nothing(monkeypatch):
    state = _patch(monkeypatch)

    result = _payload(sa.ClickAtPoint(50, 50, dry_run=True))

    assert state["clicks"] == [], "预演绝不能真的点下去"
    assert state["shots"] == 0, "预演没有发生操作，不该回拍"
    assert result["dry_run"] is True
    assert result["injected"] == 0 and result["events_expected"] == 0
    assert result["verified"] == "dry_run"
    assert result["point"]["screen_pixel"] == {"x": 96, "y": 70}
    assert result["pointer_evidence"]["hwnd_under_point"] == INFO["hwnd"]


def test_click_dry_run_accepts_string_false(monkeypatch):
    """模型常把 false 写成字符串；bool("false") 是 True，方向正好反了。"""
    state = _patch(monkeypatch)

    result = _payload(sa.ClickAtPoint(10, 10, dry_run="false"))

    assert result["dry_run"] is False
    assert len(state["clicks"]) == 1


def test_drag_dry_run_injects_nothing(monkeypatch):
    state = _patch(monkeypatch)

    result = _payload(sa.DragAtPoint(20, 20, 60, 60, dry_run="true"))

    assert state["drags"] == []
    assert result["dry_run"] is True
    assert result["from_screen_pixel"] == {"x": 38, "y": 28}
    assert result["to_screen_pixel"] == {"x": 115, "y": 83}
    assert result["injected"] == 0


def test_scroll_dry_run_injects_nothing(monkeypatch):
    state = _patch(monkeypatch)

    result = _payload(sa.ScrollAtPoint(50, 50, 3, dry_run=True))

    assert state["wheels"] == []
    assert result["dry_run"] is True
    assert result["notches"] == 3


# ── 点后回拍：图上必须真的有标记 ──────────────────────────────────

def test_click_post_shot_carries_marked_image(monkeypatch):
    state = _patch(monkeypatch)

    result = _payload(sa.ClickAtPoint(50, 50))

    assert len(state["clicks"]) == 1 and state["shots"] == 1
    assert result["verified"] == "post_shot"
    shot = result["post_shot"]
    assert shot["view"] == "detail_around_point"
    assert "红十字符号就是本次操作点" in shot["note"]
    assert (result["width"], result["height"]) == (shot["region"]["width"], shot["region"]["height"])

    PILImage = pytest.importorskip("PIL.Image")
    image = PILImage.open(io.BytesIO(base64.b64decode(result["image"]))).convert("RGB")
    cx = shot["marked_pixel_in_image"]["x"]
    cy = shot["marked_pixel_in_image"]["y"]
    # 十字的横臂：命中点上必须是标记色，而不是原来的灰底
    assert image.getpixel((cx, cy))[0] > 150, "回拍图上没有画出操作点标记"
    assert image.getpixel((max(0, cx - 10), cy))[0] > 150


def test_drag_and_scroll_also_post_shot(monkeypatch):
    state = _patch(monkeypatch)

    drag = _payload(sa.DragAtPoint(20, 20, 60, 60))
    scroll = _payload(sa.ScrollAtPoint(50, 50, -2))

    assert state["shots"] == 2
    assert drag["verified"] == "post_shot" and "image" in drag
    assert scroll["verified"] == "post_shot" and "image" in scroll
    assert "红十字符号是松手的位置" in drag["verification_note"]


def test_post_shot_can_be_switched_off(monkeypatch):
    state = _patch(monkeypatch)

    result = _payload(sa.ClickAtPoint(50, 50, post_shot=False))

    assert len(state["clicks"]) == 1
    assert state["shots"] == 0
    assert "image" not in result
    assert result["verified"] is False, "没有回拍就不能声称验证过"


def test_post_shot_failure_is_not_reported_as_operation_failure(monkeypatch):
    """点击已经注入了；回拍失败只是回拍失败，不能把整次操作报成失败。"""
    _patch(monkeypatch)

    def boom(region):
        raise OSError("mss 抓图失败")

    monkeypatch.setattr(sa, "_grab_png", boom)

    result = _payload(sa.ClickAtPoint(50, 50))

    assert result["injected"] == 2
    assert "post_shot_error" in result
    assert "操作已经执行" in result["post_shot_error"]
    assert "CaptureGridStarWindow" in result["post_shot_error"]
    assert result["verified"] is False, "回拍失败时不能声称验证过"


# ── 坐标基准漂移：必须说破，不能静默按新矩形点 ────────────────────

def test_click_warns_when_basis_changed_since_capture(monkeypatch):
    _patch(monkeypatch)
    sa._mark_basis(INFO, REGION)  # 截图时窗口铺满全屏
    moved = {"left": 120, "top": 60, "width": 1280, "height": 900}
    monkeypatch.setattr(sa, "_clamp_to_virtual_screen", lambda rect: dict(moved))

    result = _payload(sa.ClickAtPoint(50, 50))

    basis = result["basis_check"]
    assert basis["status"] == "stale"
    assert basis["captured_rect"]["width"] == 1920
    assert basis["current_rect"]["width"] == 1280
    assert "坐标基准已经漂了" in basis["note"]


def test_click_reports_fresh_basis_after_capture(monkeypatch):
    _patch(monkeypatch)
    sa._mark_basis(INFO, REGION)

    result = _payload(sa.ClickAtPoint(50, 50))

    assert result["basis_check"]["status"] == "fresh"


def test_click_reports_missing_basis_without_capture(monkeypatch):
    _patch(monkeypatch)

    result = _payload(sa.ClickAtPoint(50, 50))

    assert result["basis_check"]["status"] == "no_capture"
    assert "请先用 CaptureGridStarWindow 截一张图" in result["basis_check"]["note"]


def test_capture_records_basis_for_later_checks(monkeypatch):
    monkeypatch.setattr(sa, "_missing_deps_error", lambda tool, needs=(): None)
    monkeypatch.setattr(sa, "_find_gridstar_window_any", lambda window_title=None: dict(INFO))
    monkeypatch.setattr(sa, "_bring_to_foreground", lambda hwnd, timeout_s=1.0: True)
    monkeypatch.setattr(sa, "_clamp_to_virtual_screen", lambda rect: dict(REGION))
    monkeypatch.setattr(sa, "_grab_png", lambda region: _png(4, 4))
    monkeypatch.setattr(sa, "_LAST_BASIS", None)

    raw = json.loads(sa._capture_gridstar())
    assert raw["status"] == "success"
    assert sa._LAST_BASIS == (INFO["hwnd"], (0, 0, 1920, 1392))


# ── 局部裁剪：必须夹回窗口区域，不能越界 ──────────────────────────

def test_point_crop_is_clamped_inside_window(monkeypatch):
    region = {"left": 100, "top": 50, "width": 1600, "height": 1000}

    top_left = sa._point_crop(105, 55, region)
    bottom_right = sa._point_crop(1690, 1040, region)

    for crop in (top_left, bottom_right):
        assert crop["left"] >= region["left"]
        assert crop["top"] >= region["top"]
        assert crop["left"] + crop["width"] <= region["left"] + region["width"]
        assert crop["top"] + crop["height"] <= region["top"] + region["height"]
    assert top_left["left"] == region["left"] and top_left["top"] == region["top"]


def test_point_crop_caps_size_for_token_budget(monkeypatch):
    """整窗图会把上下文吃光：裁剪必须以 _POST_SHOT_MAX_* 封顶。"""
    region = {"left": 0, "top": 0, "width": 1920, "height": 1392}

    crop = sa._point_crop(960, 696, region)

    assert crop["width"] <= sa._POST_SHOT_MAX_W
    assert crop["height"] <= sa._POST_SHOT_MAX_H
    assert crop["width"] < region["width"], "1920 宽的窗口不该整窗回拍"


def test_point_crop_keeps_small_window_uncropped():
    region = {"left": 0, "top": 0, "width": 400, "height": 300}

    crop = sa._point_crop(200, 150, region)

    assert (crop["width"], crop["height"]) == (400, 300)
    assert (crop["left"], crop["top"]) == (0, 0)


# ── 目标点被别的窗口压住：一个事件都不许注入 ───────────────────────

def test_click_refuses_when_another_window_covers_the_point(monkeypatch):
    """注入的鼠标消息按命中测试派发，像素上压着别人的窗口时事件就落到别人身上。"""
    state = _patch(monkeypatch)
    monkeypatch.setattr(
        sa, "_hit_test_error",
        lambda info, px, py: sa._err("目标像素 (%s, %s) 上压着别的窗口，已拒绝执行。" % (px, py)))

    raw = json.loads(sa.ClickAtPoint(50, 50))

    assert raw["status"] == "error"
    assert "已拒绝执行" in raw["result"]
    assert state["clicks"] == [], "命中测试不通过时绝不能点下去"
    assert state["shots"] == 0