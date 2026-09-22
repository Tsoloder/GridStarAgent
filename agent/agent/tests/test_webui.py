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

    # 工具调用仍是逐条可折叠的 details，只是外层改挂到卡片底部的「工具调用」过程行
    assert 'document.createElement("details")' in script
    assert 'className = "tool-item"' in script
    assert 'processRail(parent, "tools")' in script
    assert 'data-proc="tools"' in script
    assert ".proc-row{" in stylesheet
    assert ".tool-item>summary{" in stylesheet


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
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # 审批参数按工具 schema 展开成可编辑条目，与工具参数询问共用输入框上方的浮层卡
    assert "function coerceSchemaValue(" in script
    assert "event.schema && event.schema.properties" in script
    assert "function approvalEntries(" in script
    assert "function openApprovalOverlay(" in script
    assert "function queueApproval(" in script
    assert "else if (type === \"tool_approval_required\") { queueApproval(id, event); }" in script
    # 审批不再在对话流里单独成卡
    assert 'className = "approval-card"' not in script
    assert ".choice-card.approval" in stylesheet


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
    assert "finishAssistant(turn, true)" in close
    assert "setBubbleUsage(turn" in close
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


def test_webui_reconnects_background_stream_after_switching_sessions():
    """切走再切回（或刷新页面）时，后台仍在跑的回复必须能接回实时输出。"""
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    backend = (Path(__file__).resolve().parents[1] / "app.py").read_text(encoding="utf-8")

    # 服务端：本轮事件带序号存档，重连时整体回放并按序号去重，_seq 不外泄
    assert "async def _bg_put(bg: BackgroundSession, event: dict)" in backend
    assert 'stamped["_seq"] = bg.seq' in backend
    assert "def _sse_payload(event: dict) -> dict" in backend
    assert '@app.get("/sessions/{session_id}/background")' in backend

    # 前端：loadSession 末尾探测后台状态，活跃则重建本轮气泡并重连
    assert "function handleStreamEvent(id, type, event, assistant)" in script
    assert "async function reconnectStream(sessionId, info, turnTs)" in script
    assert "async function maybeReconnect(id)" in script
    assert "maybeReconnect(id);" in script
    assert 'request(`/sessions/${encodeURIComponent(id)}/background`)' in script
    reconnect = script[script.index("async function reconnectStream"):]
    # 重连要恢复"停止"按钮状态（按当前会话计算），并原样带回本轮消息标识，避免被当成新消息再跑一轮
    assert "message:info.last_message" in reconnect
    # 重连必须显式带 resume：后端只认这个标志区分「回放」和「新一轮」，否则停止后
    # 重发同一句话会被当成重连，上一轮内容灌进新气泡
    assert "resume:true" in reconnect
    assert "state.controllers.set(sessionId, controller)" in reconnect
    assert "syncComposer();" in reconnect
    # 用户主动停止过的会话不自动重连，否则切回时又粘回后台流
    assert 'const stopped = error.name === "AbortError";' in script
    assert 'setStatus(sessionId, stopped ? "stopped" : "error");' in script
    assert 'state.status.get(id) === "stopped"' in script
    # 停止标记实时流与重建历史共用一套逻辑，避免实时多一条「已停止」提示、刷新后又消失
    assert "function markStopped(item)" in script
    assert "if (message.interrupted) markStopped(item);" in script
    assert "markStopped(assistant)" in script


def test_webui_per_session_stream_state_and_badges():
    """每个会话有独立的流状态与列表徽标，发送按钮只跟随当前所在会话。"""
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")
    backend = (Path(__file__).resolve().parents[1] / "app.py").read_text(encoding="utf-8")
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")

    # 全局单槽 busy/controller/stream 已移除，流状态按会话隔离成 Map
    assert "state.busy" not in script
    assert "setBusy(" not in script
    assert "controllers: new Map(), streams: new Map(), status: new Map()" in script
    # 发送按钮的"停止/发送"形态只根据当前会话计算
    assert "function isBusy() { const id = currentId(); return Boolean(id && state.controllers.has(id)); }" in script
    assert "function syncComposer()" in script
    # 点停止：断开 SSE 之外还要真正取消后端任务，否则会继续消耗 token
    assert "function stopSession(id)" in script
    assert "encodeURIComponent(id)}/cancel" in script
    assert '@app.post("/sessions/{session_id}/cancel")' in backend
    assert "bg.task.cancel()" in backend
    # 会话列表五态徽标；刷新页面后靠服务端 active/waiting 字段兜底标出"进行中/待确认"
    assert 'const STATUS_TEXT = {running:"进行中", done:"已完成", stopped:"已停止", error:"异常", waiting:"待确认"}' in script
    assert '<i class="session-badge ${status}">' in script
    assert 'session.active ? "running"' in script
    assert 'item["active"]' in backend
    for cls in (".session-badge.running", ".session-badge.done", ".session-badge.stopped", ".session-badge.error", ".session-badge.waiting"):
        assert cls in stylesheet
    assert "@keyframes badgePulse" in stylesheet
    # 待确认：选项卡片/工具参数面板收尾或审批浮层挂起时进入 waiting
    assert 'session.waiting ? "waiting"' in script
    assert "message.awaitingInput" in script
    assert "stream.awaiting = true" in script
    assert 'setStatus(id, "waiting")' in script
    assert "function queueApproval(id, event)" in script
    assert 'item["waiting"]' in backend
    # 缓存版本随本次前端改动升级
    assert "app.js?v=73" in index
    assert "style.css?v=58" in index


def test_webui_voice_input_contract():
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # 语音按钮位于发送按钮之前，处于同一输入区
    assert 'id="voice-btn"' in index
    assert 'class="voice"' in index
    assert 'aria-label="语音输入"' in index
    assert index.index('id="voice-btn"') < index.index('id="send"')
    # 缓存版本随本次前端改动升级
    assert "style.css?v=58" in index
    assert "app.js?v=73" in index

    # 录音 → 浏览器端 WAV 编码 → POST /asr → 回填，全链路契约
    for contract in (
        "getUserMedia", "createScriptProcessor", "function encodeWav(",
        "function startRecording(", "function stopRecording(",
        'fetch("/asr"', 'request("/asr/health")', "FormData",
        "function appendTranscript(", "function toggleVoice(",
    ):
        assert contract in script

    # 按钮四态样式（常态/录音脉冲/加载 spinner/禁用）
    for rule in (".voice{", ".voice.recording{", ".voice.loading{", ".voice:disabled{",
                 "@keyframes voicePulse", "@keyframes voiceSpin"):
        assert rule in stylesheet


def test_webui_attachment_drag_and_upload_contract():
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # 输入区附件控件：选择按钮、隐藏 file input、芯片条、拖拽遮罩
    assert 'id="attach-btn"' in index
    assert 'id="file-input"' in index
    assert 'id="attach-bar"' in index
    assert 'id="drop-overlay"' in index
    assert 'type="file" multiple' in index
    # 附件按钮位于语音/发送按钮之前，同属输入区
    assert index.index('id="attach-btn"') < index.index('id="voice-btn"')

    # 上传 → 芯片 → 随消息发送 → 历史渲染 全链路契约
    for contract in (
        '"/upload?name="', "function uploadFile(", "function addFiles(",
        "function renderAttachments(", "function renderMessageAttachments(",
        "attachments:sentAttachments", 'type === "notice"',
        "function dragHasFiles(", 'addEventListener("dragenter"', 'addEventListener("drop"',
        "attachments: [], uploading: 0",
    ):
        assert contract in script

    # 附件相关样式（芯片条、拖拽遮罩、气泡内附件）
    for rule in (".attach-bar{", ".attach-chip{", ".drop-overlay{", ".bubble-attachments{"):
        assert rule in stylesheet


def test_webui_choice_card_contract():
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # 待作答的询问浮在输入框上方、把输入框盖住；卡片不落进对话流
    assert '<div id="choice-overlay" class="choice-overlay hidden"' in index
    assert ".choice-overlay{position:absolute;" in stylesheet
    assert ".choice-overlay.open{" in stylesheet
    assert ".choice-overlay .choice-card{width:100%;margin:0}" in stylesheet
    # 面板展示期间彻底藏掉输入框（卡片圆角更大，不藏会露出输入框圆角与投影）
    assert ".composer.choice-open .input-wrap{opacity:0;visibility:hidden" in stylesheet
    assert 'if (el.composer) el.composer.classList.add("choice-open");' in script
    assert 'if (el.composer) el.composer.classList.remove("choice-open");' in script
    assert "function openChoiceOverlay(payload)" in script
    assert "function closeChoiceOverlay()" in script
    assert 'requestAnimationFrame(() => host.classList.add("open"))' in script
    assert 'host.classList.remove("open");' in script

    # 模型询问：ask_user_question 工具事件与旧 options 文本块都走同一套浮层
    assert 'const ASK_USER_TOOL = "ask_user_question"' in script
    assert "function renderChoiceCard(payload, parent)" in script
    assert 'else if (type === "options_offered") { assistant.pendingAsk = event; }' in script
    assert 'if (!deferred && message.pendingAsk && message.node.isConnected) openChoiceOverlay(message.pendingAsk);' in script
    assert "if (last && last.awaitingInput && last.pendingAsk) openChoiceOverlay(last.pendingAsk);" in script
    assert "finishAssistant(turn, true);" in script
    # 询问类调用不落成工具条目：实时流与历史重放保持一致
    assert 'if (event.name !== ASK_USER_TOOL) renderToolCall(event,assistant.node);' in script
    assert 'if (name === ASK_USER_TOOL) { item.pendingAsk = args; return; }' in script
    assert 'if (message.tool_name === ASK_USER_TOOL) return turn;' in script
    # 交互契约：普通选项点一下即确认；「其他」先放开输入框、写完再点提交答案；折叠、关闭、Esc 收起
    assert 'entries.push({item, option: null, label: "其他", other: true});' in script
    assert "const activate = () => { if (entry.other) openAnswer(index); else { pick(index); submit(); } };" in script
    assert 'placeholder="点击「其他」后在此输入答案"' in script
    assert '<button class="choice-submit" type="button" disabled>提交答案</button>' in script
    assert "submitBtn.onclick = submit;" in script
    assert 'if (!text) { input.classList.add("invalid"); input.focus(); return; }' in script
    assert 'input.onkeydown = event => { if (event.key === "Enter") { event.preventDefault(); submit(); } };' in script
    assert 'card.classList.toggle("collapsed")' in script
    assert '$(".choice-close", card).onclick = event => { event.stopPropagation(); closeChoiceOverlay(); };' in script
    assert "按 Esc 取消" in script
    # Esc 只在浮层打开时响应，且让位给设置弹窗
    assert 'if (!el.choiceOverlay || el.choiceOverlay.classList.contains("hidden")) return;' in script
    # 卡片样式（三主题共用变量）
    for rule in (".choice-card{", ".choice-head{", ".choice-close{", ".choice-item{",
                 ".choice-item.selected .choice-dot:after{", ".choice-input:focus{",
                 ".choice-input.invalid{", ".choice-submit{", ".choice-hint{"):
        assert rule in stylesheet


def test_webui_turn_rail_navigates_conversation_turns():
    """对话界面最左侧竖排白点：一轮一个点，鼠标移入展开整轮列表，点击行/点跳转，当前轮高亮。"""
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # 轨道与轮次面板是对话区旁的独立节点，面板不放在轨道内（轨道可滚动，放进去会被 overflow 裁掉）
    assert 'id="turn-rail"' in index
    assert 'id="turn-rail-panel"' in index
    assert 'id="turn-rail-list"' in index
    assert index.index('id="turn-rail-panel"') > index.index('id="turn-rail"')

    # 轮次以用户消息为界：一个点对应一条 user 消息，列表一行摘要同一轮
    assert 'function renderTurnRail()' in script
    assert 'el.messages.querySelectorAll(".message.user")' in script
    # 消息区增删统一走 MutationObserver + 下一帧合并刷新，点集未变不重建
    assert "new MutationObserver(syncTurnRail).observe(el.messages, {childList: true});" in script
    assert "function syncTurnRail()" in script
    # 移入轨道展开整轮列表、点击定位到该轮、滚动时重算当前轮高亮
    assert "function showTurnRailPanel(" in script
    assert "function hideTurnRailPanel()" in script
    assert "function jumpToTurn(" in script
    assert 'el.turnRail.addEventListener("mouseenter", showTurnRailPanel);' in script
    assert 'node.scrollIntoView({block: "start", behavior: "smooth"})' in script
    assert "function updateTurnRailActive()" in script
    assert "updateTurnRailActive(); }, {passive: true});" in script
    assert "syncTurnRail();" in script[script.index("function switchViewTab"):]

    # 固定在最左边缘垂直居中，点/当前点的样式 + 右侧列表行样式
    for rule in (".turn-rail{position:fixed;left:0;top:50%;transform:translateY(-50%)",
                 ".turn-rail-dot{", ".turn-rail-dot:before{", ".turn-rail-dot.active:before{",
                 ".turn-rail-panel{position:fixed;", ".turn-rail-panel.show{",
                 ".turn-rail-row{", ".turn-rail-index{", ".turn-rail-row.active .turn-rail-index{"):
        assert rule in stylesheet


def test_webui_usage_panel_contract():
    """设置中心第四个 Tab「用量」：供应商/模型维度、按天/按小时、日历选区间、三图 + 明细表。"""
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    charts = (Path(WEBUI_DIR) / "charts.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")
    backend_dir = Path(__file__).resolve().parents[1]
    stats = (backend_dir / "usage_stats.py").read_text(encoding="utf-8")

    # Tab 排在 MCP 之后，面板与筛选控件齐备
    assert 'data-settings-tab="usage"' in index
    assert 'id="panel-usage"' in index
    assert index.index('data-settings-tab="usage"') > index.index('data-settings-tab="mcp"')
    for anchor in ('id="usage-group"', 'id="usage-provider"', 'id="usage-model"', 'id="usage-granularity"',
                   'id="usage-range"', 'id="usage-range-panel"', 'id="usage-calendar"', 'id="usage-overview"',
                   'id="usage-line"', 'id="usage-pie"', 'id="usage-bar"', 'id="usage-table"'):
        assert anchor in index
    # 筛选顺序：分组 → 供应商 → 模型 → 粒度 → 日期
    assert index.index('id="usage-group"') < index.index('id="usage-provider"')
    assert index.index('id="usage-provider"') < index.index('id="usage-model"')
    assert index.index('id="usage-model"') < index.index('id="usage-granularity"')
    # 日历面板挂在筛选行下而不是日期触发器内：它宽约 560，
    # 跟着触发器左缘向右展开会顶出弹窗右缘被裁掉
    assert index.index('id="usage-range-panel"') > index.index('id="usage-refresh"')
    assert index.index('id="usage-range-panel"') < index.index('id="usage-status"')
    assert ".usage-filters{position:relative;" in stylesheet
    assert ".usage-range-panel{position:absolute;z-index:26;top:calc(100% + 5px);right:0;left:auto;max-width:100%;" in stylesheet
    # 图表模块先于 app.js 加载，且不引任何外部资源（内网离线）
    assert 'charts.js?v=' in index
    assert index.index('charts.js?v=') < index.index('app.js?v=')
    assert '<script src="http' not in index

    # 接口与聚合口径
    assert "/usage/stats" in script
    assert "/usage/stats" in (backend_dir / "app.py").read_text(encoding="utf-8")
    assert "function loadUsageStats(" in script
    assert "function usageRollupDays(" in script
    assert "function usageDrilled(" in script
    assert 'if (tab === "usage")' in script
    # 供应商/模型筛选项只列「有调用记录」的模型：candidates 按时间窗算出、不随筛选缩水。
    # 列出从没调用过的配置模型只会让用户选到空结果。
    assert "function usageSyncCatalog(" in script
    assert "data.candidates" in script
    assert '"candidates"' in stats
    assert "function usageProviderName(" in script
    assert "function usageModelName(" in script
    assert "&provider=" in script
    assert '"provider"' in stats
    # 配置目录只用于名称映射，不再作为选项来源
    assert "(state.models || []).filter(item => item.enabled !== false)" in script
    # 选中项在当前时间窗无记录时仍保留下拉里，不静默清空
    assert "usage.modelOptions.push({value: usage.model" in script
    # 结果缓存有上限：key 精确到分钟，跨分钟后旧条目再也命中不了，不设上限会一直堆积
    assert "USAGE_CACHE_LIMIT" in script
    assert "if (Object.keys(usage.cache).length >= USAGE_CACHE_LIMIT) usage.cache = {};" in script
    # 命中缓存时也要作废在飞请求，否则它返回时会把这份缓存结果覆盖成上一个筛选的数据
    assert "usage.token += 1;" in script
    # 分组只决定饼图与明细表的拆分维度，不再置灰模型下拉、也不再清空已选筛选
    assert "el.usageModel.disabled" not in script
    for label in ("最近 12 小时", "最近 24 小时", "最近 3 天", "最近 7 天", "最近 30 天"):
        assert label in script
    for label in ("模型用量", "供应商用量", "按小时", "按天"):
        assert label in script
    # 12/24 小时档在按天下只剩一两个点，选择后自动切到按小时
    assert "if (usagePreset(id).hours) usage.granularity = \"hour\";" in script
    # 单个模型下饼图退化为该模型的 token 构成，避免只剩一个满圆
    assert "缓存读" in script and "缓存写" in script
    # 缓存命中率口径不假设 input 与 cache_read 的包含关系
    assert "totals.cache_read / base" in script

    # 图表模块自带折线/饼/柱与时间轴缩放，零依赖
    for name in ("function line(", "function pie(", "function bar(", "function bindTimeZoom(",
                 "function clampView(", "function arcPath("):
        assert name in charts
    assert "https://" not in charts and "http://" not in charts.replace("http://www.w3.org/2000/svg", "")
    assert 'svg.addEventListener("dblclick"' in charts
    assert 'svg.addEventListener("wheel"' in charts
    assert 'svg.addEventListener("mousedown"' in charts

    # 用量一律以 M 为单位：同一页混用 K 与 M 无法横向比较，故不再有 K 档
    assert "function formatMillions(" in charts
    assert "/ 102.4" not in charts
    assert 'return "0M";' in charts
    # 不足 1M 时按数量级补足小数位，否则 8.8K 会被压成 0.00M，看起来像没有消耗
    assert "Math.min(8, 1 - Math.floor(Math.log(abs) / Math.LN10))" in charts
    # 用量面板统一走 M 格式化
    for call in ("Charts.formatMillions(totals.total)", "Charts.formatMillions(totals.input)",
                 "Charts.formatMillions(totals.output)", "Charts.formatMillions(row[item.key])"):
        assert call in script
    # 模型设置的上下文窗口仍用 K/M 自适应：128K 换成 0.13M 反而难读
    assert "function formatTokens(value) { return value >= 1048576" in script
    assert "formatTokens(totals" not in script and "formatTokens(row[item" not in script
    # 轮次是计数不是用量：只给 token 列打 tokens 标记，否则 33 轮会写成 0.000033M
    assert '{key:"turns",label:"轮次",numeric:true},' in script
    assert 'escapeHtml(item.tokens ? Charts.formatMillions(row[item.key]) : row[item.key])' in script

    for rule in (".usage-panel{", ".usage-filters{", ".usage-select{", ".usage-listbox{",
                 ".usage-range-panel{", ".usage-cal-day{", ".usage-overview{", ".usage-stat{",
                 ".usage-table{", ".chart-legend{", ".chart-tip{", ".chart-empty{", ".pie-legend{"):
        assert rule in stylesheet


def test_webui_serves_charts_module():
    response = client.get("/ui/charts.js")

    assert response.status_code == 200
    assert "javascript" in response.headers["content-type"]

