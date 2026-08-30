from pathlib import Path

from fastapi.testclient import TestClient

from app import WEBUI_DIR, app


client = TestClient(app)


def test_ui_redirects_to_trailing_slash():
    response = client.get("/ui", follow_redirects=False)

    assert response.status_code in {307, 308}
    assert response.headers["location"] == "/ui/"


def test_ui_serves_index_and_static_assets():
    index = client.get("/ui/")
    css = client.get("/ui/style.css")
    javascript = client.get("/ui/app.js")

    assert index.status_code == 200
    assert 'href="style.css"' in index.text
    assert 'src="app.js"' in index.text
    assert css.status_code == 200
    assert "text/css" in css.headers["content-type"]
    assert javascript.status_code == 200
    assert "javascript" in javascript.headers["content-type"]


def test_webui_contains_existing_api_and_sse_contracts():
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")

    for endpoint in (
        '"/health"', '"/config/models"', '"/skills"', '"/sessions"',
        '"/chat/stream"', '"/workflows/run"', 'tool-approvals',
    ):
        assert endpoint in script
    for event in (
        "text_chunk", "reasoning_chunk", "phase_plan", "tool_call",
        "tool_result", "tool_approval_required", "skill_loaded", "error", "done",
        "workflow_started", "workflow_step", "workflow_done",
    ):
        assert event in script
    for interaction in ("options", "tool_params", "workflow", "phase_plan"):
        assert interaction in script
