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
    assert 'style.css?v=' in index.text
    assert 'app.js?v=' in index.text
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
        "text_chunk", "reasoning_chunk", "plan_updated", "tool_call",
        "tool_result", "tool_approval_required", "skill_loaded", "error", "done",
        "workflow_started", "workflow_step", "workflow_done",
    ):
        assert event in script
    # phase_plan 保留仅用于渲染旧会话历史（向后兼容，不再是协议）
    for interaction in ("options", "tool_params", "workflow", "phase_plan"):
        assert interaction in script


def test_webui_groups_tool_calls_in_collapsible_details():
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    assert 'document.createElement("details")' in script
    assert 'className = "tool-group"' in script
    assert 'className = "tool-item"' in script
    assert 'class="tool-count"' in script
    assert 'toolGroup(parent)' in script
    assert '.tool-group{' in stylesheet
    assert '.tool-item>summary{' in stylesheet


def test_webui_contains_model_settings_center_contract():
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")

    assert 'id="open-settings"' in index
    assert 'aria-label="打开设置"' in index
    assert index.index('id="open-settings"') > index.index('id="skill-select"')
    for tab in ('data-settings-tab="models"', 'data-settings-tab="skills"', 'data-settings-tab="mcp"'):
        assert tab in index
    assert 'class="provider-sidebar"' in index
    for endpoint in ('request("/config")', 'request("/config/providers/test"', 'request("/config/providers/models"'):
        assert endpoint in script
    for contract in ("discoveredModels", "testingProviderId", "readingProviderId", "revision", "validateSettings", "addModel", "manual-model-id", "data-model-field=\"api\""):
        assert contract in script


def test_webui_uses_config_response_contract_and_preserves_saved_credentials():
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")

    assert 'const payload = {revision:state.settings.revision,config:state.settings.draft}' in script
    assert 'data.config && data.config.default_model' in script
    assert 'defaultModel = data.default_model || defaultModel' in script
    assert 'selectModel(models.value.default_model || "")' in script
    assert "default_index" not in script
    assert "refreshModels(data.models, data.default_model)" not in script
    assert 'return {provider:{...provider' in script
    assert 'provider.api_key !== "********"' in script
    assert 'provider.api_key === "********" ? ""' in script
    assert 'provider.api_key = ""' in script


def test_webui_uses_in_page_dialogs_instead_of_native_blocks():
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    assert "confirm(" not in script
    assert "prompt(" not in script
    assert "alert(" not in script
    assert "function showDialog(" in script
    assert 'className = "modal-backdrop dialog-backdrop"' in script
    assert ".confirm-dialog{" in stylesheet
    assert ".dialog-actions{" in stylesheet


def test_webui_approval_card_renders_editable_params_from_schema():
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")

    assert "function coerceSchemaValue(" in script
    assert "event.schema && event.schema.properties" in script
    assert "approval-param-" in script
    assert 'className = "approval-card"' in script


def test_webui_model_listbox_has_static_provider_groups():
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    assert 'role="combobox"' in index
    assert 'role="listbox"' in index
    assert 'className = "model-group"' in script
    assert 'setAttribute("role","group")' in script
    assert 'setAttribute("role","option")' in script
    # 分组标题展示供应商名称，而不是供应商 ID（ID 仅作无名称时的回退）
    group = script[script.index("function renderModelList"):script.index("function setConnection")]
    assert "items[0].provider_name || provider" in group
    assert "escapeHtml(label)" in group
    assert 'className = "tool-group"' in script
    assert 'document.createElement("details")' not in script[script.index("function renderModelList"):script.index("function setConnection")]
    assert ".model-listbox{" in stylesheet
    assert ".model-group-label{" in stylesheet


def test_webui_mcp_panel_lists_backend_tools():
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # MCP 面板从"即将支持"占位改为工具列表容器
    assert 'id="panel-mcp"' in index
    assert 'class="settings-panel mcp-panel hidden"' in index
    assert 'id="mcp-tools"' in index
    assert 'id="refresh-mcp"' in index
    assert 'id="mcp-count"' in index
    mcp_panel = index[index.index('id="panel-mcp"'):]
    mcp_panel = mcp_panel[: mcp_panel.index("</footer>")]
    assert "即将支持" not in mcp_panel
    # 前端拉取后端工具清单并按 tab 惰性加载
    for contract in ('"/mcp/tools"', '"/mcp/tools?refresh=1"', "function renderMcpTools(", "function loadMcpTools(",
                     "function schemaParams(", 'tab === "mcp" && !state.mcp.loaded'):
        assert contract in script
    assert ".mcp-panel{" in stylesheet
    assert ".mcp-tool{" in stylesheet


def test_webui_skills_panel_lists_registered_skills():
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # 技能面板从"即将支持"占位改为已注册技能列表容器
    assert 'id="panel-skills"' in index
    assert 'class="settings-panel skills-panel hidden"' in index
    assert 'id="skills-list"' in index
    assert 'id="refresh-skills"' in index
    assert 'id="skill-count"' in index
    skills_panel = index[index.index('id="panel-skills"'):]
    skills_panel = skills_panel[: skills_panel.index("</footer>")]
    assert "即将支持" not in skills_panel
    # 前端渲染 state.skills 并按 tab 惰性渲染，刷新按钮重新拉取 /skills
    for contract in ("function renderSkills(", "function loadSkills(", 'tab === "skills"',
                     'el.refreshSkills.onclick = () => loadSkills()', 'await request("/skills")'):
        assert contract in script
    assert ".skills-panel{" in stylesheet
    assert ".skill-card{" in stylesheet


def test_webui_history_render_matches_live_stream():
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")

    # 历史渲染必须还原对话中持久化的思考过程
    assert "appendReasoning(item, message.reasoning_content)" in script
    body = script[script.index("function renderHistoryMessage"):]
    body = body[: body.index("function showWelcome")]
    render, close = body[: body.index("function finishHistoryTurn")], body[body.index("function finishHistoryTurn"):]
    # 工具调用必须先挂到消息节点，收尾统一交给 finishHistoryTurn。
    # 否则"无正文、仅工具调用"的消息（自动模式常见）会被当空气泡移除，
    # 后续 tool 结果找不到 call-id，退化为独立 TOOL RESULT 气泡
    assert "(message.tool_calls || []).forEach" in render
    assert "finishAssistant(" not in render
    assert "finishAssistant(turn)" in close
    assert "renderTokenUsage(turn" in close
    # 实时流里一轮对话只有一个气泡，而持久化会按迭代拆成多条 assistant/tool 消息，
    # 历史渲染必须按轮次合并，否则重开会话后变成"一条消息一个工具调用"
    assert "function renderHistory(messages)" in close
    assert 'message.role === "assistant" || message.role === "tool"' in close
    assert "renderHistoryMessage(message, turn)" in close
    assert "item.text += " in render
    assert "renderHistory(state.session.messages)" in script


def test_webui_surfaces_upstream_error_classification():
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")

    # 后端透传的分类字段必须真的被用上，而不是只剩一句 message
    for field in ("event.category", "event.retryable", "event.retry_after"):
        assert field in script
    assert "function streamFailure(" in script
    assert "function renderFailure(" in script
    assert 'retryable ? "RETRY" : "ERROR"' in script
    assert "throw new Error(event.message" not in script
    # 只有连不上后端才把顶栏标成离线，上游故障不该说"连接异常"
    assert "if (!error.stream) setConnection(" in script
    assert 'sendMessage(retry.message, retry.display)' in script
